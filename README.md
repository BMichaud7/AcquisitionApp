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
     │   1. Welch-average IQ → power spectrum (FftProcessor::computeSpectrum)
     │   2. CA-CFAR per-bin detection          (FftProcessor::detectFromSpectrum)
     │   3. Persistence filter                 (SpectrumScanner::processDwell)
     ▼
Detection callback ──► AMQP rf.detections topic
                   ──► PostgreSQL detections table
```

## Detection pipeline

Each dwell goes through four stages:

### 1. Welch spectrum averaging
`dwell_samples` of CF32 IQ are split into 50%-overlapping frames of `fft_size` samples. Each frame is DC-removed and Blackman-Harris windowed before FFT. All frame power spectra are averaged linearly, then converted to dBFS.

**Blackman-Harris window** — ~92 dB sidelobe rejection vs ~31 dB for Hann. Prevents strong broadcast stations (FM, LTE) from producing ghost detections in adjacent bins.

**Welch averaging** — 63 frames at 131 072 samples (20 MSPS × 6.6 ms dwell) reduces the noise floor variance by ~18 dB vs a single-shot FFT, enabling detection of signals just above the noise floor.

### 2. CA-CFAR detection
For each bin, the local noise floor is estimated by averaging 32 reference bins on each side (skipping 8 guard bins to exclude the signal itself). This is computed in O(N) via a prefix sum, the same cost as the previous global-median approach.

A bin enters a candidate run when its power exceeds `local_noise + threshold_db`. Runs are then filtered by two quality gates:

- **Minimum bandwidth** — runs narrower than `min_signal_bw_hz` are discarded (removes single-bin spurs)
- **PAPR filter** — multi-bin runs with peak-to-mean power ratio < `min_papr_db` are discarded (removes flat noise bumps with no spectral peak)

The peak bin of each surviving run is sub-bin interpolated using a 3-point parabola, giving the reported `center_freq_hz` sub-bin accuracy (~1–2 kHz at 20 MSPS / 4096 bins).

### 3. Persistence filter
Each candidate must be detected at the same quantised frequency (±10 kHz) in at least 2 consecutive sweeps before the detection callback fires. This eliminates single-dwell noise spurs and random false alarms.

Bypass: signals with PAPR ≥ 15 dB (strong, obvious signals — FM broadcast, LTE, cellular) emit immediately on the first sweep without waiting for confirmation.

### 4. Per-frequency noise floor EMA
An asymmetric exponential moving average tracks the observed spectrum at each center frequency across sweeps. Rising noise uses `4 × alpha` (new interference suppressed quickly); falling noise uses `alpha` (slower decay, prevents immediate re-detection). This floor is maintained for monitoring; it is not used in the detection path to avoid suppressing persistent signals.

## Benchmark results

Measured inside the build container (Ubuntu 24.04, Release build):

| Input | computeSpectrum | detectFromSpectrum | Full pipeline |
|---|---|---|---|
| Realistic RF (4 tones + AWGN) | 0.664 ms | 0.020 ms | 0.691 ms |
| Noise only | 0.649 ms | 0.020 ms | 0.696 ms |

**Config**: FFT size 4096, dwell 131 072 samples at 20 MSPS (6.6 ms real time), 63 Welch frames.

- **SDR receive time**: 6.6 ms/dwell at 20 MSPS  
- **Processing overhead**: ~0.7 ms/dwell = **10.5% of dwell time**  
- **Real-time headroom**: **9.5× faster than real time** (processes one dwell while the SDR delivers the next)  
- **Throughput**: 195 M samples/sec sustained

The bottleneck is the SDR hardware (IQ receive rate), not the CPU. Even a 2× improvement in FFT speed would only reduce end-to-end scan time by ~5%.

## Configuration

`SweepConfig` is loaded from XML. A minimal config:

```xml
<sdr_acquisition>
  <scanner_id>scanner-0</scanner_id>
  <rank>2</rank>  <!-- higher rank preempts AnalysisApp (rank 1) when re-submitting -->

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
    <sample_rate_sps>20000000</sample_rate_sps>
    <rx_gain_db>40</rx_gain_db>
    <bandwidth_hz>20000000</bandwidth_hz>
  </device>

  <sweep>
    <start_hz>80000000</start_hz>
    <stop_hz>3000000000</stop_hz>
    <dwell_samples>131072</dwell_samples>
    <fft_size>4096</fft_size>
    <usable_bw_fraction>0.80</usable_bw_fraction>
    <threshold_db>10.0</threshold_db>
    <min_signal_bw_hz>1000</min_signal_bw_hz>
    <dc_guard_hz>50000</dc_guard_hz>
    <cfar_guard_bins>8</cfar_guard_bins>
    <cfar_ref_bins>32</cfar_ref_bins>
    <min_papr_db>3.0</min_papr_db>
    <noise_floor_alpha>0.08</noise_floor_alpha>
    <settle_samples>512</settle_samples>
    <analysis_pause_ms>15000</analysis_pause_ms>
  </sweep>
</sdr_acquisition>
```

### `<rank>` field

**Required at runtime** (the controller rejects requests without it). Sets the preemption tier for the scan task. Default: `2` (above AnalysisApp at rank 1, preempted by nothing at rank 3+).

### Sweep parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `start_hz` / `stop_hz` | 70 MHz / 1 GHz | Frequency sweep range |
| `dwell_samples` | 4096 | Samples collected per dwell (≥ `fft_size`) |
| `fft_size` | 4096 | FFT size (power of 2, ≥ 64) |
| `usable_bw_fraction` | 0.80 | Fraction of bins examined (discards roll-off edges) |
| `threshold_db` | 10.0 | Detection threshold above local CA-CFAR noise estimate (dB) |
| `min_signal_bw_hz` | 1000 | Minimum run width to be reported as a detection |
| `dc_guard_hz` | 50 000 | Bins within this range of DC are blanked (LO leakage suppression) |
| `cfar_guard_bins` | 8 | CA-CFAR guard cells each side of test cell |
| `cfar_ref_bins` | 32 | CA-CFAR reference cells each side for local noise average |
| `min_papr_db` | 3.0 | Minimum peak-to-mean power ratio for multi-bin detections |
| `noise_floor_alpha` | 0.08 | EMA coefficient for per-frequency noise floor (~12-sweep time constant) |
| `settle_samples` | 512 | Samples discarded after each retune |
| `analysis_pause_ms` | 0 | Pause after each sweep pass to yield SDR to AnalysisApp (0 = disabled) |

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

## Building

**Unit tests (container — no hardware or broker needed):**

```bash
cd AcquisitionApp
podman build --target test -t sdr-acq:test .
```

**Benchmark (run the FftProcessor timing benchmark):**

```bash
podman build --target builder -t acq-builder .
podman run --rm acq-builder /workspace/AcquisitionApp/build/tests/acq_bench
```

**Native build:**

```bash
git clone https://github.com/BMichaud7/SdrTaskApi.git ../SdrTaskApi

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
ctest --test-dir build --output-on-failure     # 61 unit tests
./build/tests/acq_bench                        # benchmark
```

Test suite covers 61 cases across `FftProcessor`, `SpectrumScanner`, and `SweepConfig`.

## Detection output

Each detection published to `rf.detections` (schema version **1.1**) contains:

| Field | Type | Description |
|-------|------|-------------|
| `msg_type` | string | Always `"RF_DETECTION"` |
| `schema_version` | string | `"1.1"` |
| `timestamp_ms` | integer | Unix epoch ms (UTC) |
| `scanner_id` | string | From config `<scanner_id>` |
| `channel` | integer | RX channel index (0-based) |
| `center_freq_hz` | number | Sub-bin interpolated center frequency (Hz) |
| `bandwidth_hz` | integer | Estimated occupied bandwidth (Hz) |
| `power_db` | number | Peak power in the detection run (dBFS) |
| `iq_snapshot` | float array | **v1.1+** 1 024 complex samples (2 048 interleaved I,Q floats) from the detecting dwell. Used by AnalysisApp for zero-acquisition ONNX classification. |
| `snapshot_sample_rate_sps` | number | **v1.1+** Sample rate of `iq_snapshot` (Hz, matches scan `sample_rate_sps`). Always present alongside `iq_snapshot`. |

The full JSON Schema is in [`schema/rf_detection.schema.json`](schema/rf_detection.schema.json).

### Schema version history

| Version | Change |
|---------|--------|
| 1.0 | Initial release |
| 1.1 | Added `iq_snapshot` + `snapshot_sample_rate_sps` (optional, for ONNX fast-path) |

## AnalysisApp co-existence

When `analysis_pause_ms > 0`, the scanner releases the SDR after each sweep pass and sleeps for that duration, giving AnalysisApp (lower rank) a guaranteed window to grab the device for wideband classification. On re-submission, the scanner's higher rank preempts any running analysis.

Recommended cycle with 20 MSPS PlutoSDR: 8s scan + 5s drain + 15s analysis window ≈ 28s per full cycle.

## Repository

https://github.com/BMichaud7/AcquisitionApp
