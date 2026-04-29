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
};

// Where the acquisition should bind to receive IQ data from the controller.
struct ReceiverConfig {
    std::string local_ip{"0.0.0.0"};  // must be routable from the controller
    int         port{0};              // 0 = OS-assigned
};

struct SweepConfig {
    std::string    scanner_id{"scanner-0"};
    AmqpConfig     amqp;
    DbConfig       db;
    DeviceConfig   device;
    SweepParams    sweep;
    ReceiverConfig receiver;

    static SweepConfig from_file(const std::string& path);
};

} // namespace acq
