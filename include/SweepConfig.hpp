#pragma once
#include <string>
#include <cstdint>
#include <stdexcept>

namespace acq {

struct AmqpConfig {
    std::string url{"amqp://localhost:5672"};
    std::string username;
    std::string password;
    std::string detection_topic{"rf.detections"};
    std::string task_request_queue{"sdr.task.request"};
    std::string task_response_queue{"sdr.task.response"};
    int reconnect_interval_sec{5};
};

struct DbConfig {
    std::string host{"localhost"};
    int         port{5432};
    std::string name{"sdr_scanner"};
    std::string user;
    std::string password;
    std::string connection_string() const;
};

struct DeviceConfig {
    int    rx_channels{1};
    double sample_rate{10e6};
    double rx_gain_db{40.0};
    double bandwidth_hz{10e6};  // requested bandwidth per channel
};

struct SweepParams {
    uint64_t start_hz{70'000'000};
    uint64_t stop_hz{1'000'000'000};
    int      dwell_samples{4096};
    int      fft_size{4096};
    double   usable_bw_fraction{0.80};
    double   threshold_db{10.0};
    uint32_t min_signal_bw_hz{1'000};
    int      settle_samples{512};
    // Bins within this many Hz of the tuned center are blanked before peak
    // detection to suppress AD9361 LO leakage. Set 0 to disable.
    uint32_t dc_guard_hz{50'000};
    // CA-CFAR: guard cells each side of test cell (exclude signal from noise ref).
    // Reference cells each side used to estimate local noise floor.
    int      cfar_guard_bins{8};
    int      cfar_ref_bins{32};
    // Minimum peak-to-mean power ratio (dB) within a candidate signal group.
    // Multi-bin noise bumps with no clear spectral peak are rejected.
    float    min_papr_db{3.0f};
    // Exponential moving average coefficient for per-frequency noise floor.
    // Smaller = slower adaptation (more smoothing). Range (0, 1).
    float    noise_floor_alpha{0.08f};
};

// Where the acquisition should bind to receive IQ data from the controller.
struct ReceiverConfig {
    std::string local_ip{"0.0.0.0"};  // must be routable from the controller
    int         port{0};              // 0 = OS-assigned
};

struct SweepConfig {
    std::string    scanner_id{"scanner-0"};
    int            rank{0};
    // After each sweep pass the scanner sleeps this long (ms) before re-submitting,
    // giving AnalysisApp a guaranteed window to grab the SDR. 0 = disabled.
    int            analysis_pause_ms{0};
    AmqpConfig     amqp;
    DbConfig       db;
    DeviceConfig   device;
    SweepParams    sweep;
    ReceiverConfig receiver;

    static SweepConfig from_file(const std::string& path);
};

} // namespace acq
