# AcquisitionApp

Spectrum scanning application for the SDR Radio Resource Task Manager ecosystem. Submits frequency-sweep tasks to `SdrResourceManager` via AMQP, receives IQ samples over UDP, runs FFT-based signal detection on each dwell, and publishes detections.

## Architecture

```
SweepConfig (XML)
     │
     ▼
TaskManagerIqSource ──AMQP TASK_REQUEST_SCAN──► SdrResourceManager
                    ◄──UDP CF32 IQ packets─────
     │
     ▼
SoapyIqSource (unit tests only — bypasses AMQP, drives hardware directly)
     │
     ▼
SpectrumScanner
     │  per dwell:
     ▼
FftProcessor ──► Detection callback ──► AMQP rf.detections topic
```

## Configuration

`SweepConfig` is loaded from XML. A minimal config:

```xml
<sdr_acquisition>
  <scanner_id>scanner-0</scanner_id>
  <rank>1</rank>                        <!-- required: preemption tier (0 = lowest) -->

  <amqp>
    <url>amqp://activemq-service.sdr-system:5672</url>
    <username>sdr_ctrl</username>
    <password>your_password</password>
    <task_request_queue>sdr.task.request</task_request_queue>
    <task_response_queue>sdr.task.response</task_response_queue>
    <detection_topic>rf.detections</detection_topic>
  </amqp>

  <device>
    <rx_channels>1</rx_channels>
    <sample_rate_sps>10000000</sample_rate_sps>
    <rx_gain_db>40</rx_gain_db>
    <bandwidth_hz>10000000</bandwidth_hz>
  </device>

  <sweep>
    <start_hz>70000000</start_hz>
    <stop_hz>1000000000</stop_hz>
    <dwell_samples>4096</dwell_samples>
    <fft_size>4096</fft_size>
    <usable_bw_fraction>0.80</usable_bw_fraction>
    <threshold_db>10.0</threshold_db>
    <min_signal_bw_hz>1000</min_signal_bw_hz>
    <settle_samples>512</settle_samples>
  </sweep>
</sdr_acquisition>
```

### `<rank>` field

**Required at runtime** (the controller rejects requests without it). Sets the preemption tier for the scan task. A scan at `rank=2` will displace any running task at `rank=0` or `rank=1` on the target device if spectrum is unavailable. Default in the struct is `0` (lowest priority — can be preempted by anything with `rank > 0`).

### Sweep parameters

| Parameter | Description |
|-----------|-------------|
| `start_hz` / `stop_hz` | Frequency sweep range |
| `dwell_samples` | Samples collected per dwell (must be ≥ `fft_size`) |
| `fft_size` | FFT size (power of 2, ≥ 64) |
| `usable_bw_fraction` | Fraction of FFT bins examined (discard roll-off edges) |
| `threshold_db` | Detection threshold above estimated per-dwell noise floor (dB) |
| `min_signal_bw_hz` | Minimum contiguous bandwidth for a detection to be reported |
| `settle_samples` | Samples discarded after each retune before collecting dwell |

## Building

```bash
# Unit tests (CentOS 10 container, no hardware or broker needed)
podman build -f Containerfile.test -t sdr-acq:test .
podman run --rm sdr-acq:test          # exits 0 on pass
podman run --rm sdr-acq:test /build/AcquisitionApp/build/tests/acq_tests --gtest_filter='*' -V
```

Test suite covers 48 cases across `FftProcessor`, `SpectrumScanner`, and `SweepConfig`.

The production binary (`sdr_acquisition`) requires qpid-proton and libpqxx (PostgreSQL) and is built inside the full stack image, not the test container.

## Detection output

Each detection published to `rf.detections` contains:

| Field | Description |
|-------|-------------|
| `scanner_id` | From config `<scanner_id>` |
| `channel` | RX channel index (0-based) |
| `center_freq_hz` | Estimated signal center frequency |
| `bandwidth_hz` | Estimated signal bandwidth |
| `power_db` | Peak power in detection run (dBFS) |
| `timestamp` | Detection time (system clock) |

## Repository

https://github.com/BMichaud7/AcquisitionApp
