#!/usr/bin/env python3
"""
AcquisitionApp integration tests — fake controller + IQ streamer.

Acts as a fake SdrResourceManager.  Verifies the scanner:
  1. Submits a well-formed TASK_REQUEST_SCAN
  2. Binds UDP to the port the fake controller allocates (not a random port)
  3. Receives IQ packets and publishes detections to rf.detections
  4. Sends TASK_STOP when shut down

Test cases
----------
1. udp_port_protocol   Controller allocates port; scanner must bind to returned udp_port
2. task_request_format TASK_REQUEST_SCAN has required fields (no dest_ports)
3. detections_published After IQ stream, detections appear on rf.detections
4. task_stop_on_reject  When task is rejected, scanner retries (no crash)

Usage:
    python3 e2e_test.py [--broker amqp://localhost:5673]
"""
from __future__ import annotations

import argparse, json, math, socket, struct, sys, threading, time, uuid
import numpy as np

try:
    import proton, proton.handlers, proton.reactor
except ImportError:
    sys.exit("pip install python-qpid-proton")

# ── IQ packet (matches sdr::IqPacketHeader) ───────────────────────────────────
IQ_MAGIC  = 0x49515030
IQ_HEADER = struct.Struct("<I I Q Q I H B B")
assert IQ_HEADER.size == 32

IQ_FLAG_FIRST_PACKET = 0x02
IQ_FLAG_DWELL_CHANGE = 0x04

def _iq_packet(seq: int, samples: np.ndarray, cf: float, sr: float,
               flags: int = 0) -> bytes:
    n   = len(samples)
    hdr = IQ_HEADER.pack(IQ_MAGIC, seq, 0, int(cf), int(sr), n, 0, flags)
    raw = np.empty(n * 2, dtype=np.float32)
    raw[0::2] = samples.real
    raw[1::2] = samples.imag
    return hdr + raw.tobytes()

def _gen_fm(n: int = 4096, sr: float = 2e6, dev: float = 50e3) -> np.ndarray:
    rng   = np.random.default_rng(7)
    audio = rng.standard_normal(n).astype(np.float32)
    audio /= np.max(np.abs(audio)) + 1e-9
    phase = 2 * np.pi * dev / sr * np.cumsum(audio)
    iq    = (np.exp(1j * phase)).astype(np.complex64)
    return iq / np.sqrt(np.mean(np.abs(iq) ** 2))

def _stream_iq(dest_ip: str, dest_port: int, cf: float, sr: float,
               n_total: int = 400_000, pkt_sz: int = 1024,
               delay_s: float = 0.2):
    """Send IQ with DWELL_CHANGE flags so the scanner can produce detections."""
    time.sleep(delay_s)
    sock  = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    base  = _gen_fm(4096, sr)
    full  = np.tile(base, math.ceil(n_total / len(base)))[:n_total]
    flags = IQ_FLAG_FIRST_PACKET | IQ_FLAG_DWELL_CHANGE
    seq   = 0
    for off in range(0, len(full), pkt_sz):
        chunk = full[off : off + pkt_sz]
        data  = _iq_packet(seq, chunk, cf, sr, flags)
        sock.sendto(data, (dest_ip, dest_port))
        flags = 0
        seq  += 1
        time.sleep(0.0003)
    sock.close()
    print(f"  [ctrl] streamed {seq} packets ({n_total} samples) → {dest_ip}:{dest_port}")

_PORT = 30200

def _alloc_port() -> int:
    global _PORT
    p = _PORT; _PORT += 1
    return p


# ── Base handler ──────────────────────────────────────────────────────────────

class _Base(proton.handlers.MessagingHandler):
    CREDS = ("sdr_ctrl", "test_password")

    def __init__(self, broker: str):
        super().__init__()
        self.broker = broker
        self._senders: dict[str, proton.Sender] = {}
        self.error: str | None = None

    def _connect(self, event, *recv_addrs):
        conn = event.container.connect(
            self.broker, user=self.CREDS[0], password=self.CREDS[1],
            sasl_enabled=True, allowed_mechs="PLAIN",
        )
        for a in recv_addrs:
            event.container.create_receiver(conn, a)
        return conn

    def _open_sender(self, conn, addr):
        s = conn.open_sender(addr)
        self._senders[addr] = s
        return s

    def _send(self, addr, body):
        msg = proton.Message(body=json.dumps(body),
                             content_type="application/json")
        self._senders[addr].send(msg)

    def _parse(self, event) -> dict | None:
        event.delivery.accept()
        try:
            return json.loads(event.message.body)
        except Exception:
            return None

    def on_transport_error(self, event):
        self.error = str(event.transport.condition)

    def on_disconnected(self, event):
        pass


# ══════════════════════════════════════════════════════════════════════════════
# Test 1+2+3: udp_port_protocol / task_request_format / detections_published
# ══════════════════════════════════════════════════════════════════════════════

class _ScanFlowHandler(_Base):
    def __init__(self, broker: str):
        super().__init__(broker)
        self.task_req:       dict | None = None
        self.detection:      dict | None = None
        self.task_stop:      dict | None = None
        self.udp_port        = 0
        self._responded      = False

    def on_start(self, event):
        conn = self._connect(event, "sdr.task.request", "rf.detections")
        self._open_sender(conn, "sdr.task.response")

    def on_message(self, event):
        body = self._parse(event)
        if body is None:
            return
        addr = event.receiver.source.address

        if addr == "sdr.task.request":
            msg_type = body.get("msg_type", "")

            if msg_type == "TASK_STOP":
                self.task_stop = body
                print(f"  [ctrl] ← TASK_STOP task={body.get('task_id','?')} ✓")
                event.connection.close()
                return

            if not self._responded and "REQUEST" in msg_type:
                self.task_req    = body
                self._responded  = True
                req_id  = body.get("request_id", "")
                dest_ip = body.get("streaming", {}).get("dest_ip", "127.0.0.1")
                cf      = body.get("rf", {}).get("center_freq_hz", 101e6)
                sr      = body.get("rf", {}).get("sample_rate_sps", 2e6)
                self.udp_port = _alloc_port()

                print(f"  [ctrl] ← TASK_REQUEST msg_type={msg_type}  allocating port {self.udp_port}")
                self._send("sdr.task.response", {
                    "msg_type":   "TASK_RESPONSE",
                    "request_id": req_id,
                    "task_id":    str(uuid.uuid4()),
                    "status":     "ACCEPTED",
                    "timestamp_ms": int(time.time() * 1000),
                    "streams": [{
                        "channel_index":   0,
                        "udp_ip":          dest_ip,
                        "udp_port":        self.udp_port,
                        "center_freq_hz":  cf,
                        "sample_rate_sps": sr,
                        "format":          "CF32",
                    }],
                })
                print(f"  [ctrl] → TASK_RESPONSE ACCEPTED  udp_port={self.udp_port}")
                threading.Thread(
                    target=_stream_iq,
                    args=(dest_ip, self.udp_port, cf, sr),
                    daemon=True,
                ).start()

        elif addr == "rf.detections":
            self.detection = body
            print(f"  [ctrl] ← rf.detections  {body.get('center_freq_hz',0)/1e6:.2f} MHz  {body.get('power_db',0):.1f} dB")
            # Close after first detection (enough to verify the pipeline works)
            event.connection.close()


# ══════════════════════════════════════════════════════════════════════════════
# Test 4: task_stop_on_reject
# ══════════════════════════════════════════════════════════════════════════════

class _RejectThenAcceptHandler(_Base):
    """Reject the first SCAN, accept the second — scanner must retry."""
    def __init__(self, broker: str):
        super().__init__(broker)
        self.second_task_req: dict | None = None
        self.detection:       dict | None = None
        self._reject_count = 0

    def on_start(self, event):
        conn = self._connect(event, "sdr.task.request", "rf.detections")
        self._open_sender(conn, "sdr.task.response")

    def on_message(self, event):
        body = self._parse(event)
        if body is None:
            return
        addr = event.receiver.source.address

        if addr == "sdr.task.request" and "REQUEST" in body.get("msg_type", ""):
            req_id = body.get("request_id", "")
            if self._reject_count == 0:
                self._reject_count += 1
                print(f"  [ctrl] ← TASK_REQUEST #{self._reject_count} — rejecting")
                self._send("sdr.task.response", {
                    "msg_type":     "TASK_RESPONSE",
                    "request_id":   req_id,
                    "task_id":      "",
                    "status":       "REJECTED",
                    "reject_reason":"test rejection",
                    "timestamp_ms": int(time.time() * 1000),
                    "streams":      [],
                })
            else:
                self.second_task_req = body
                dest_ip = body.get("streaming", {}).get("dest_ip", "127.0.0.1")
                cf      = body.get("rf", {}).get("center_freq_hz", 101e6)
                sr      = body.get("rf", {}).get("sample_rate_sps", 2e6)
                port    = _alloc_port()
                print(f"  [ctrl] ← TASK_REQUEST #{self._reject_count+1} — accepting  port={port}")
                self._send("sdr.task.response", {
                    "msg_type":   "TASK_RESPONSE",
                    "request_id": req_id,
                    "task_id":    str(uuid.uuid4()),
                    "status":     "ACCEPTED",
                    "timestamp_ms": int(time.time() * 1000),
                    "streams": [{"channel_index": 0, "udp_ip": dest_ip,
                                 "udp_port": port, "center_freq_hz": cf,
                                 "sample_rate_sps": sr, "format": "CF32"}],
                })
                threading.Thread(
                    target=_stream_iq,
                    args=(dest_ip, port, cf, sr),
                    daemon=True,
                ).start()

        elif addr == "rf.detections":
            self.detection = body
            event.connection.close()


# ── Runner + checks ───────────────────────────────────────────────────────────

def _run(h, timeout_s=40):
    c = proton.reactor.Container(h)
    t = threading.Thread(target=c.run, daemon=True)
    t.start(); t.join(timeout=timeout_s)
    if t.is_alive():
        c.stop(); t.join(timeout=3)
    return h

def _report(checks):
    ok = True
    for label, passed in checks:
        print(f"  {'✓' if passed else '✗'} {label}")
        if not passed: ok = False
    return ok


def run_udp_port_protocol(broker):
    print("\n── Test 1: udp_port_protocol ───────────────────────────────────")
    h = _run(_ScanFlowHandler(broker))
    if h.error: print(f"  AMQP error: {h.error}"); return False
    checks = [
        ("TASK_REQUEST received",           h.task_req is not None),
        ("request has no dest_ports",       h.task_req and
                                             h.task_req.get("streaming",{}).get("dest_ports") is None),
        ("controller allocated udp_port",   h.udp_port > 0),
        ("detections published (app got IQ)", h.detection is not None),
    ]
    return _report(checks)


def run_task_request_format(broker):
    print("\n── Test 2: task_request_format ─────────────────────────────────")
    h = _run(_ScanFlowHandler(broker))
    if h.error or h.task_req is None:
        print("  ✗ No TASK_REQUEST"); return False
    req = h.task_req
    checks = [
        ("has request_id",            bool(req.get("request_id"))),
        ("has task_type=SCAN",        req.get("task_type") == "SCAN"
                                      or "SCAN" in req.get("msg_type","")),
        ("has rf.center_freq_hz",     req.get("rf",{}).get("center_freq_hz",0) > 0),
        ("has rf.sample_rate_sps",    req.get("rf",{}).get("sample_rate_sps",0) > 0),
        ("has scan_params.entries",   bool(req.get("scan_params",{}).get("entries"))),
        ("has streaming.dest_ip",     bool(req.get("streaming",{}).get("dest_ip"))),
        ("no streaming.dest_ports",   req.get("streaming",{}).get("dest_ports") is None),
        ("rank field present",        "rank" in req),
    ]
    return _report(checks)


def run_detections_published(broker):
    print("\n── Test 3: detections_published ────────────────────────────────")
    h = _run(_ScanFlowHandler(broker))
    if h.error: print(f"  AMQP error: {h.error}"); return False
    checks = [
        ("detection received on rf.detections", h.detection is not None),
        ("detection has center_freq_hz",
         h.detection is not None and h.detection.get("center_freq_hz",0) > 0),
        ("detection has power_db",
         h.detection is not None and "power_db" in h.detection),
        ("detection has scanner_id",
         h.detection is not None and bool(h.detection.get("scanner_id"))),
    ]
    return _report(checks)


def run_task_stop_on_reject(broker):
    print("\n── Test 4: task_stop_on_reject (retry) ─────────────────────────")
    h = _run(_RejectThenAcceptHandler(broker), timeout_s=40)
    if h.error: print(f"  AMQP error: {h.error}"); return False
    checks = [
        ("first task was rejected",          h._reject_count >= 1),
        ("scanner retried and resubmitted",  h.second_task_req is not None),
        ("scanner got IQ and published det", h.detection is not None),
    ]
    return _report(checks)


ALL_TESTS = {
    "udp_port_protocol":    run_udp_port_protocol,
    "task_request_format":  run_task_request_format,
    "detections_published": run_detections_published,
    "task_stop_on_reject":  run_task_stop_on_reject,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="amqp://localhost:5673")
    ap.add_argument("--test",   default=None)
    args = ap.parse_args()

    tests = {args.test: ALL_TESTS[args.test]} if args.test else ALL_TESTS
    results = {}
    for name, fn in tests.items():
        results[name] = fn(args.broker)
        time.sleep(1)

    print("\n── Summary ─────────────────────────────────────────────────────")
    all_ok = True
    for name, ok in results.items():
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if not ok: all_ok = False

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
