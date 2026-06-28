#!/usr/bin/env python3
# ========================================================================
# Project: OpenRFStack
# Author:  Brendan Michaud
# Year:    2026
# Part of OpenRFStack (https://github.com/OpenRFStack)
#
# Licensed under the Personal Use License.
# Do not use for commercial, organizational, or military purposes.
# ========================================================================
"""
sdr_recon.py — Passive recon recorder.

Subscribes to the rf.detections AMQP topic.  When a signal above the
SNR threshold is detected, requests a full-bandwidth NARROWBAND IQ capture
(default 10 s) from the sdr_controller and saves it to disk as:

  <output_dir>/<ISO-timestamp>_<freq_mhz>MHz_<bw_khz>kHz.<format>
  <output_dir>/<ISO-timestamp>_<freq_mhz>MHz_<bw_khz>kHz.json   ← metadata

Formats:
  --format cf32   interleaved float32 I/Q  (default, numpy-compatible)
  --format npz    numpy .npz {iq: complex64, meta: str}
  --format wav    WAV (16-bit PCM, 2-channel I/Q) — opens in audacity/inspectrum

Designed to run continuously alongside AcquisitionApp.  Uses a cooldown
per frequency bin to avoid triggering multiple captures of the same signal.

Usage (inside container or dev env):
    python3 sdr_recon.py \\
        --out /recon \\
        --snr-min 15 \\
        --capture-s 10 \\
        --gain 40 \\
        --cooldown 60

Usage (standalone test):
    python3 sdr_recon.py --broker amqp://localhost:5672 --dry-run
"""
from __future__ import annotations

import argparse
import json
import math
import os
import socket
import struct
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import proton
import proton.handlers
import proton.reactor

# ── Constants ─────────────────────────────────────────────────────────────────
IQ_HDR   = struct.Struct("<I I Q Q I H B B")
IQ_MAGIC = 0x49515030
TUNE_OVERHEAD_S = 0.10   # extra time for PLL settle after retune


# ── AMQP session (shared request/response + detection subscription) ────────────

class _Handler(proton.handlers.MessagingHandler):
    def __init__(self, sess: "ReconSession", detection_topic: str,
                 req_queue: str, resp_queue: str, gps_topic: str) -> None:
        super().__init__()
        self._sess   = sess
        self._det    = detection_topic
        self._req_q  = req_queue
        self._resp_q = resp_queue
        self._gps    = gps_topic

    def on_start(self, ev) -> None:
        c = ev.container.connect(
            self._sess.broker,
            user=self._sess.user, password=self._sess.password,
            sasl_enabled=True, allowed_mechs="PLAIN ANONYMOUS",
        )
        ev.container.create_receiver(c, self._resp_q)
        ev.container.create_receiver(c, self._det)
        ev.container.create_receiver(c, self._gps)
        self._sender = ev.container.create_sender(c, self._req_q)
        self._sess._handler = self

    def on_sendable(self, ev) -> None:
        self._sess._ready.set()

    def on_message(self, ev) -> None:
        try:
            msg = json.loads(ev.message.body)
        except Exception:
            return
        # GPS fix from GpsApp — cache it
        if "latitude_deg" in msg:
            self._sess._on_gps(msg)
            return
        # Task response
        rid = msg.get("request_id", "")
        if rid:
            with self._sess._lock:
                entry = self._sess._pending.get(rid)
            if entry:
                entry[1].append(msg)
                entry[0].set()
                return
        # Detection event
        self._sess._on_detection(msg)

    def send(self, d: dict) -> None:
        self._sender.send(proton.Message(body=json.dumps(d),
                                         content_type="application/json"))


class ReconSession:
    def __init__(self, broker: str, user: str, password: str,
                 detection_topic: str, req_queue: str, resp_queue: str,
                 gps_topic: str, on_detection_cb) -> None:
        self.broker   = broker
        self.user     = user
        self.password = password
        self._pending:  dict = {}
        self._lock      = threading.Lock()
        self._ready     = threading.Event()
        self._handler   = None
        self._on_detection_cb = on_detection_cb
        self._gps_fix: dict | None = None   # latest fix from GpsApp
        self._gps_lock = threading.Lock()

        h = _Handler(self, detection_topic, req_queue, resp_queue, gps_topic)
        self._ctr = proton.reactor.Container(h)
        threading.Thread(target=self._ctr.run, daemon=True).start()
        # Artemis can take several minutes to finish a cold start (seen up to
        # ~4 min on a Raspberry Pi). Unlike the C++ services' AmqpClient,
        # which logs a warning and keeps retrying forever in the background,
        # raising here after a short fixed timeout used to crash the whole
        # process — which the entrypoint's 3s-respawn supervisor would then
        # restart from scratch, repeating the same short timeout and
        # crash-looping for minutes instead of just waiting. Poll patiently
        # with periodic warnings instead; only give up if it's truly stuck.
        waited = 0.0
        while not self._ready.wait(20):
            waited += 20
            print(f"[recon] AMQP session not ready after {waited:.0f}s — "
                  f"still waiting for broker...", flush=True)
            if waited >= 300:
                raise RuntimeError("AMQP session did not become ready within 300 s")

    def _on_detection(self, msg: dict) -> None:
        self._on_detection_cb(msg)

    def _on_gps(self, msg: dict) -> None:
        with self._gps_lock:
            self._gps_fix = msg

    def get_gps(self) -> dict | None:
        """Return latest GPS fix from GpsApp, or None if not available."""
        with self._gps_lock:
            return self._gps_fix

    def rpc(self, req: dict, timeout: float = 30.0) -> dict | None:
        rid = req["request_id"]
        ev, box = threading.Event(), []
        with self._lock:
            self._pending[rid] = (ev, box)
        self._handler.send(req)
        ev.wait(timeout)
        with self._lock:
            self._pending.pop(rid, None)
        return box[0] if box else None

    def close(self) -> None:
        try:
            self._ctr.stop()
        except Exception:
            pass


# ── IQ collection ──────────────────────────────────────────────────────────────

def collect_iq(sock: socket.socket, capture_s: float) -> np.ndarray:
    """Receive UDP IQ stream for capture_s seconds.  Returns complex64 array.

    Takes ownership of sock and closes it when done.  The caller must have
    already bound sock to the target port before sending the task request,
    so no packets are missed during the bind-gap window.
    """
    chunks: list[np.ndarray] = []
    deadline = time.time() + capture_s + TUNE_OVERHEAD_S
    try:
        while time.time() < deadline:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            if len(data) < IQ_HDR.size:
                continue
            if IQ_HDR.unpack_from(data)[0] != IQ_MAGIC:
                continue
            n   = IQ_HDR.unpack_from(data)[5]
            raw = np.frombuffer(data[IQ_HDR.size: IQ_HDR.size + n * 8],
                                dtype=np.float32)
            if len(raw) == n * 2:
                chunks.append(raw[0::2] + 1j * raw[1::2])
    finally:
        sock.close()
    return np.concatenate(chunks).astype(np.complex64) if chunks else np.array([], dtype=np.complex64)


# ── IQ save ───────────────────────────────────────────────────────────────────

def save_iq(iq: np.ndarray, meta: dict, out_dir: Path, fmt: str) -> Path:
    ts  = datetime.fromtimestamp(meta["timestamp_unix"], tz=timezone.utc)
    ts_str   = ts.strftime("%Y%m%dT%H%M%SZ")
    freq_mhz = meta["center_freq_hz"] / 1e6
    bw_khz   = meta["bandwidth_hz"] / 1e3
    stem     = f"{ts_str}_{freq_mhz:.4f}MHz_{bw_khz:.0f}kHz"

    out_dir.mkdir(parents=True, exist_ok=True)

    if fmt == "cf32":
        iq_path = out_dir / f"{stem}.cf32"
        iq.tofile(iq_path)
    elif fmt == "npz":
        iq_path = out_dir / f"{stem}.npz"
        np.savez_compressed(iq_path, iq=iq)
    elif fmt == "wav":
        import wave, struct as st
        iq_path = out_dir / f"{stem}.wav"
        sr = int(meta["sample_rate_sps"])
        samples_i = np.clip(iq.real * 32767, -32768, 32767).astype(np.int16)
        samples_q = np.clip(iq.imag * 32767, -32768, 32767).astype(np.int16)
        interleaved = np.empty(len(samples_i) * 2, dtype=np.int16)
        interleaved[0::2] = samples_i
        interleaved[1::2] = samples_q
        with wave.open(str(iq_path), "wb") as wf:
            wf.setnchannels(2)
            wf.setsampwidth(2)
            wf.setframerate(sr)
            wf.writeframes(interleaved.tobytes())
    else:
        raise ValueError(f"Unknown format: {fmt}")

    # Sidecar JSON
    meta_path = out_dir / f"{stem}.json"
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)

    return iq_path


# ── Recon recorder ────────────────────────────────────────────────────────────

class ReconRecorder:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args        = args
        self.out_dir     = Path(args.out)
        self._cooldown: dict[int, float] = {}  # freq_bin → last_capture_time
        self._record_lock = threading.Lock()    # one capture at a time
        self._stats = {"detections": 0, "captures": 0, "samples_saved": 0}

    def _freq_bin(self, freq_hz: float) -> int:
        """Quantise to cooldown_bin_hz for dedup."""
        return int(freq_hz // (self.args.cooldown_bin_hz))

    def _is_cooled_down(self, freq_hz: float) -> bool:
        b = self._freq_bin(freq_hz)
        last = self._cooldown.get(b, 0.0)
        return (time.time() - last) >= self.args.cooldown

    def _mark_captured(self, freq_hz: float) -> None:
        self._cooldown[self._freq_bin(freq_hz)] = time.time()

    def on_detection(self, msg: dict, sess: ReconSession) -> None:
        self._stats["detections"] += 1

        freq_hz = float(msg.get("center_freq_hz", 0))
        bw_hz   = float(msg.get("bandwidth_hz",   100e3))
        snr_db  = float(msg.get("snr_db",         0.0))
        power   = float(msg.get("power_db",        -999.0))
        sr_sps  = self.args.sample_rate

        if snr_db < self.args.snr_min:
            return

        if not self._is_cooled_down(freq_hz):
            return

        # One capture at a time — drop concurrent triggers for other freqs
        if not self._record_lock.acquire(blocking=False):
            print(f"[recon] capture in progress — skipping {freq_hz/1e6:.4f} MHz", flush=True)
            return

        # on_detection is called from the proton reactor thread.  _capture
        # blocks for capture_s seconds and issues an rpc() that waits for
        # an on_message response — which can never arrive while the reactor
        # thread is stuck here.  Offload to a daemon thread so the reactor
        # stays live.  The lock is released inside _capture_thread.
        threading.Thread(
            target=self._capture_thread,
            args=(sess, freq_hz, bw_hz, snr_db, power, sr_sps, msg),
            daemon=True,
        ).start()

    def _capture_thread(self, sess: ReconSession,
                        freq_hz: float, bw_hz: float,
                        snr_db: float, power_db: float,
                        sr_sps: float, trigger_msg: dict) -> None:
        try:
            self._capture(sess, freq_hz, bw_hz, snr_db, power_db, sr_sps, trigger_msg)
        finally:
            self._record_lock.release()

    def _capture(self, sess: ReconSession,
                 freq_hz: float, bw_hz: float,
                 snr_db: float, power_db: float,
                 sr_sps: float, trigger_msg: dict) -> None:

        cap_bw  = max(bw_hz * self.args.bw_margin, self.args.min_bw_hz)
        cap_sr  = max(cap_bw * 1.25, sr_sps)  # at least 1.25× occupied BW
        cap_bw  = min(cap_bw,  self.args.max_bw_hz)
        cap_sr  = min(cap_sr, self.args.max_bw_hz)

        now_ts  = time.time()
        rid     = str(uuid.uuid4())

        print(f"[recon] {datetime.utcnow().strftime('%H:%M:%S')}  "
              f"TRIGGER  {freq_hz/1e6:.4f} MHz  BW={cap_bw/1e3:.0f} kHz  "
              f"SNR={snr_db:.1f} dB  cap={self.args.capture_s}s", flush=True)

        if self.args.dry_run:
            print(f"[recon] dry-run — would capture {self.args.capture_s}s at "
                  f"{freq_hz/1e6:.4f} MHz", flush=True)
            self._mark_captured(freq_hz)
            return

        # Bind UDP port before sending task so we don't miss early packets.
        # Keep the socket open — closing and rebinding in collect_iq would
        # create a window where another process could steal the port.
        # collect_iq takes ownership and closes; we close it ourselves on
        # early-return paths (reject/timeout) via the finally block.
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024 * 1024)
        udp_sock.settimeout(0.3)
        udp_sock.bind(("", 0))
        udp_port = udp_sock.getsockname()[1]
        sock_ref = [udp_sock]  # mutable cell — cleared once collect_iq takes ownership

        try:
            resp = sess.rpc({
                "msg_type": "TASK_REQUEST", "schema_version": "2.0",
                "request_id": rid,
                "timestamp_ms": int(now_ts * 1000),
                "task_type": "NARROWBAND",
                "rank": self.args.rank,
                "schedule": {
                    "mode": "IMMEDIATE",
                    "duration_ms": int((self.args.capture_s + TUNE_OVERHEAD_S + 2) * 1000),
                },
                "rf": {
                    "center_freq_hz": freq_hz,
                    "bandwidth_hz":   cap_bw,
                    "sample_rate_sps": cap_sr,
                    "rx_count": 1,
                    "rx_gain_db": self.args.gain,
                },
                "stream": {
                    "dest_ip":   "127.0.0.1",
                    "dest_port": udp_port,
                },
            }, timeout=15.0)

            if resp is None or resp.get("status") != "ACCEPTED":
                reason = resp.get("reason", "timeout") if resp else "timeout"
                print(f"[recon] task rejected: {reason}", flush=True)
                return

            actual_sr = float(resp.get("sample_rate_sps", cap_sr))
            print(f"[recon] task ACCEPTED — collecting IQ on UDP :{udp_port} "
                  f"sr={actual_sr/1e6:.3f} MSPS ...", flush=True)

            sock_ref[0] = None  # collect_iq now owns and will close the socket
            iq = collect_iq(udp_sock, self.args.capture_s)
        finally:
            if sock_ref[0] is not None:
                sock_ref[0].close()

        if len(iq) == 0:
            print("[recon] WARNING: no IQ received", flush=True)
            return

        # GPS from GpsApp — attach if available
        gps = sess.get_gps()
        gps_meta: dict = {}
        if gps:
            gps_meta = {
                "lat":   gps.get("latitude_deg"),
                "lon":   gps.get("longitude_deg"),
                "alt_m": gps.get("altitude_m"),
            }

        meta = {
            "timestamp_unix":    now_ts,
            "timestamp_iso":     datetime.fromtimestamp(now_ts, tz=timezone.utc).isoformat(),
            "center_freq_hz":    freq_hz,
            "bandwidth_hz":      cap_bw,
            "sample_rate_sps":   actual_sr,
            "capture_s":         self.args.capture_s,
            "snr_db_trigger":    snr_db,
            "power_db_trigger":  power_db,
            "rx_gain_db":        self.args.gain,
            "num_samples":       len(iq),
            "trigger_detection": trigger_msg,
            **gps_meta,
        }

        iq_path = save_iq(iq, meta, self.out_dir, self.args.format)
        self._mark_captured(freq_hz)
        self._stats["captures"] += 1
        self._stats["samples_saved"] += len(iq)

        self._write_db(meta, iq_path)

        gps_str = f" @ {gps_meta['lat']:.5f},{gps_meta['lon']:.5f}" if gps_meta else ""
        print(f"[recon] saved {len(iq):,} samples → {iq_path.name}  "
              f"({iq_path.stat().st_size / 1e6:.1f} MB){gps_str}", flush=True)

    def _write_db(self, meta: dict, iq_path) -> None:
        if not self.args.db:
            return
        try:
            import psycopg2
            conn = psycopg2.connect(self.args.db)
            with conn, conn.cursor() as cur:
                cur.execute("""
                    INSERT INTO recon_captures
                        (captured_at, center_freq_hz, bandwidth_hz, sample_rate_sps,
                         snr_db_trigger, power_db, capture_s, num_samples,
                         file_path, format, lat, lon, alt_m)
                    VALUES
                        (to_timestamp(%s), %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                """, (
                    meta["timestamp_unix"],
                    meta["center_freq_hz"],
                    meta.get("bandwidth_hz"),
                    meta.get("sample_rate_sps"),
                    meta.get("snr_db_trigger"),
                    meta.get("power_db_trigger"),
                    meta.get("capture_s"),
                    meta.get("num_samples"),
                    str(iq_path),
                    self.args.format,
                    meta.get("lat"),
                    meta.get("lon"),
                    meta.get("alt_m"),
                ))
            conn.close()
        except Exception as e:
            print(f"[recon] DB write failed: {e}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="SDR passive recon recorder")
    ap.add_argument("--broker",       default="amqp://localhost:5672")
    ap.add_argument("--user",         default="sdr_ctrl")
    ap.add_argument("--password",     default="sdr_hw_test")
    ap.add_argument("--detection-topic", default="rf.detections")
    ap.add_argument("--req-queue",    default="sdr.task.request")
    ap.add_argument("--resp-queue",   default="sdr.task.response")
    ap.add_argument("--out",          default="/recon",
                    help="Output directory for captured IQ files")
    ap.add_argument("--format",       default="cf32",
                    choices=["cf32", "npz", "wav"],
                    help="Output format (default: cf32)")
    ap.add_argument("--snr-min",      type=float, default=15.0,
                    help="Minimum SNR (dB) to trigger a capture (default 15)")
    ap.add_argument("--capture-s",    type=float, default=10.0,
                    help="Capture duration in seconds (default 10)")
    ap.add_argument("--gain",         type=float, default=40.0,
                    help="RX gain dB for capture task (default 40)")
    ap.add_argument("--rank",         type=int,   default=4,
                    help="Task priority rank (1=low … 5=high, default 4)")
    ap.add_argument("--cooldown",     type=float, default=60.0,
                    help="Seconds before re-capturing same frequency (default 60)")
    ap.add_argument("--cooldown-bin-hz", type=float, default=50e3,
                    help="Frequency bin size for cooldown dedup (default 50 kHz)")
    ap.add_argument("--bw-margin",    type=float, default=2.0,
                    help="Capture BW = detected BW × margin (default 2.0)")
    ap.add_argument("--min-bw-hz",    type=float, default=25e3,
                    help="Minimum capture bandwidth Hz (default 25 kHz)")
    ap.add_argument("--max-bw-hz",    type=float, default=3.2e6,
                    help="Maximum capture bandwidth Hz (default 3.2 MHz = RTL-SDR max)")
    ap.add_argument("--sample-rate",  type=float, default=2.4e6,
                    help="Fallback sample rate if not in detection (default 2.4 MSPS)")
    ap.add_argument("--gps-topic",    default="gps.location",
                    help="AMQP topic GpsApp publishes fixes to (default: gps.location)")
    ap.add_argument("--db",           default="",
                    help="PostgreSQL connection string for recon_captures table "
                         "(e.g. host=localhost dbname=sdr_scanner user=sdr)")
    ap.add_argument("--dry-run",      action="store_true",
                    help="Subscribe and log detections without actually capturing")
    args = ap.parse_args()

    recorder = ReconRecorder(args)

    print(f"[recon] Starting — broker={args.broker}  snr_min={args.snr_min} dB  "
          f"capture={args.capture_s}s  out={args.out}  format={args.format}",
          flush=True)
    if args.dry_run:
        print("[recon] DRY-RUN mode — no captures will be made", flush=True)

    Path(args.out).mkdir(parents=True, exist_ok=True)

    sess = ReconSession(
        broker=args.broker, user=args.user, password=args.password,
        detection_topic=args.detection_topic,
        req_queue=args.req_queue, resp_queue=args.resp_queue,
        gps_topic=args.gps_topic,
        on_detection_cb=lambda msg: recorder.on_detection(msg, sess),
    )

    print(f"[recon] Listening on {args.detection_topic} ...", flush=True)

    try:
        while True:
            time.sleep(30)
            s = recorder._stats
            print(f"[recon] stats: detections={s['detections']}  "
                  f"captures={s['captures']}  "
                  f"samples_saved={s['samples_saved']:,}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        sess.close()
        s = recorder._stats
        print(f"[recon] shutdown — captures={s['captures']}  "
              f"samples_saved={s['samples_saved']:,}", flush=True)


if __name__ == "__main__":
    main()
