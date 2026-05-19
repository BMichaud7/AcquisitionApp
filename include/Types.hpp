#pragma once
#include <chrono>
#include <string>
#include <cstdint>
#include <vector>

namespace acq {

inline constexpr const char* SCHEMA_VERSION = "1.1";  // added iq_snapshot (2024-Q4)

struct Detection {
    std::chrono::system_clock::time_point timestamp;
    uint64_t center_freq_hz{0};
    uint32_t bandwidth_hz{0};
    float    power_db{0.f};
    std::string scanner_id;
    int      channel{0};

    // 1 024-sample IQ snapshot taken directly from the detecting dwell.
    // Interleaved float32 I,Q,I,Q,... at snapshot_sample_rate_sps.
    // AnalysisApp uses this for zero-acquisition ONNX fast-path classification.
    std::vector<float> iq_snapshot;
    double             snapshot_sample_rate_sps{0.0};
};

} // namespace acq
