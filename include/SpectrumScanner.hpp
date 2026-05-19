#pragma once
#include "SweepConfig.hpp"
#include "FftProcessor.hpp"
#include "IqSource.hpp"
#include "Types.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>

namespace acq {

class SpectrumScanner {
public:
    using DetectionCallback = std::function<void(const Detection&)>;

    SpectrumScanner(SweepConfig cfg, IqSource* source, DetectionCallback cb);
    ~SpectrumScanner();

    SpectrumScanner(const SpectrumScanner&)            = delete;
    SpectrumScanner& operator=(const SpectrumScanner&) = delete;

    void start();
    void stop();

private:
    SweepConfig       cfg_;
    IqSource*         source_;
    DetectionCallback cb_;

    std::vector<std::unique_ptr<FftProcessor>> processors_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;

    // Per-channel, per-center-frequency asymmetric-EMA noise floor.
    // Rises fast (new interference) / falls slow (avoid immediate false alarms).
    std::vector<std::unordered_map<uint64_t, std::vector<float>>> ch_noise_floor_;

    // Persistence filter: track hit/miss counts per (channel, center_hz, freq).
    // A detection is emitted only after PERSIST_MIN_HITS confirmations, unless
    // PAPR ≥ PERSIST_BYPASS_PAPR (strong/obvious signal → emit immediately).
    struct PersistEntry {
        int   hits{0};
        int   misses{0};
        float peak_db{-300.f};
    };
    std::vector<std::unordered_map<uint64_t,
        std::unordered_map<uint64_t, PersistEntry>>> ch_persist_;

    // Frequencies are quantised to this step before persistence lookup.
    // Coarse enough to tolerate sub-bin interpolation jitter across sweeps.
    static constexpr uint64_t PERSIST_FREQ_QUANT_HZ = 10'000;
    static constexpr int      PERSIST_MIN_HITS       = 2;
    static constexpr int      PERSIST_MAX_MISSES      = 4;
    // Signals with PAPR above this threshold bypass persistence (emit on first sweep).
    static constexpr float    PERSIST_BYPASS_PAPR_DB  = 15.0f;

    void sweepLoop();
    std::vector<Detection> processDwell(int ch, uint64_t center_hz,
                                         const std::vector<std::complex<float>>& samples);
};

} // namespace acq
