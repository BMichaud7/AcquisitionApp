#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace acq {

inline constexpr const char* SCHEMA_VERSION = "1.2";  // base64 iq_snapshot, snr_db (2025-Q2)

struct Detection {
    std::chrono::system_clock::time_point timestamp;
    uint64_t center_freq_hz{0};
    uint32_t bandwidth_hz{0};
    float    power_db{0.f};
    std::string scanner_id;
    int      channel{0};
    float    snr_db{0.f};   // PAPR of detected signal group (peak − mean, dB)

    // 1 024-sample IQ snapshot taken directly from the detecting dwell.
    // Shared across all detections from the same dwell (zero-copy shared_ptr).
    // Interleaved float32 I,Q,I,Q,... at snapshot_sample_rate_sps.
    std::shared_ptr<const std::vector<float>> iq_snapshot;
    double                                    snapshot_sample_rate_sps{0.0};
};

} // namespace acq
