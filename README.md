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

## Dependencies

AcquisitionApp depends on [SdrTaskApi](https://github.com/BMichaud7/SdrTaskApi)
for the shared type system and AMQP message codec. All three repos must share the
same parent directory for CMake to detect SdrTaskApi automatically:

```
parent/
├── SdrTaskApi/           ← https://github.com/BMichaud7/SdrTaskApi   (required)
├── SdrResourceManager/   ← https://github.com/BMichaud7/SdrResourceManager
└── AcquisitionApp/       ← this repo
```

If SdrTaskApi is not found as a sibling, CMake falls back to an installed
`sdr_task_api` package and fails with a helpful message if neither is available.

### Dependency table

| Package | Required for | Notes |
|---------|-------------|-------|
| `SdrTaskApi` | All | Sibling dir or installed package |
| `libtinyxml2-dev` | All | Config file parser |
| `libfftw3-dev` | All | FFT-based signal detection |
| `libfmt-dev` | All | Logging formatting |
| `libspdlog-dev` | All | Structured logging |
| `libsoapysdr-dev` | Unit tests | `FakeAcqSoapyDevice` in test binary |
| `googletest` | Unit tests | Auto-fetched via FetchContent |
| `libqpid-proton-cpp12-dev` | `sdr_acquisition` binary only | AMQP broker connection |
| `libpqxx-dev` | `sdr_acquisition` binary only | PostgreSQL detection DB |

CMake prints a `FATAL_ERROR` with the exact install command for any missing
required dependency. The `sdr_acquisition` binary is silently skipped (with a
`STATUS` message) when qpid-proton or libpqxx are not found.

```bash
# Ubuntu 24.04 — unit tests + production binary
apt-get install -y \
    build-essential cmake pkg-config git \
    libtinyxml2-dev libfftw3-dev libfmt-dev libspdlog-dev \
    libsoapysdr-dev soapysdr-module-remote \
    libqpid-proton-cpp12-dev \
    libpqxx-dev

# CentOS Stream 10 — see Containerfile.test for exact build-from-source steps
```

## Building

**Unit tests (container — no hardware or broker needed):**

```bash
# Must be run from the parent directory so COPY SdrTaskApi/ works
podman build -f AcquisitionApp/Containerfile.test -t sdr-acq:test .
podman run --rm sdr-acq:test                                   # exits 0 on pass
podman run --rm sdr-acq:test ctest --output-on-failure -V      # verbose
```

**Native build (using the included build script):**

```bash
git clone https://github.com/BMichaud7/AcquisitionApp.git
cd AcquisitionApp

./build.sh             # Release build — clones SdrTaskApi automatically
./build.sh --tests     # Release build + run all 48 unit tests
./build.sh --debug     # Debug build (AddressSanitizer + UBSan)
./build.sh --clean     # Wipe build/ and rebuild from scratch
./build.sh --no-clone  # Skip git-clone (SdrTaskApi already present)
./build.sh --help      # All options
```

Or directly with CMake:

```bash
# Clone SdrTaskApi sibling first
git clone https://github.com/BMichaud7/SdrTaskApi.git ../SdrTaskApi

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
ctest --test-dir build --output-on-failure
```

Test suite covers 48 cases across `FftProcessor`, `SpectrumScanner`, and `SweepConfig`.

The production binary (`sdr_acquisition`) requires qpid-proton and libpqxx (PostgreSQL)
and is skipped automatically at configure time if those packages are absent.

## Combined-window / shared-channel IQ streams

When `SdrResourceManager` accepts two tasks at nearby frequencies on a `shared_lo=true`
device, it retuning the hardware to a combined RF window and multicasts the same wideband
IQ stream to both UDP endpoints. AcquisitionApp receives the full combined-window IQ;
the `slice_offset_hz` field in the `TASK_RESPONSE` tells it where within that wideband
capture its scan slice is located:

```
actual_center_freq = device_cf + slice_offset_hz
```

`slice_offset_hz` **can change mid-stream** if a second task joins. When it does,
`SdrResourceManager` publishes a `TASK_STATUS` update and sets `IQ_FLAG_DWELL_CHANGE`
on the next packet. `TaskManagerIqSource` surfaces this as a new dwell start and
`SpectrumScanner` discards the partial dwell automatically (same logic as a scan retune).

If the combined-window SR is wider than the requested scan `sample_rate_sps`, the extra
bandwidth is visible in the FFT but lies outside the `usable_bw_fraction` window; the
threshold search ignores it unless signals alias into the scan band.

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
