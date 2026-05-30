#pragma once
/**
 * @file SpectrumScanner.hpp
 * @brief Frequency sweep loop with persistence filter and noise floor EMA.
 *
 * SpectrumScanner drives an IqSource in a background thread, running
 * FftProcessor on each dwell and applying a persistence filter before
 * invoking the DetectionCallback.
 *
 * ## Persistence filter
 * A candidate detection must appear at the same quantised frequency
 * (±PERSIST_FREQ_QUANT_HZ) in at least PERSIST_MIN_HITS consecutive sweeps.
 * Signals with PAPR ≥ PERSIST_BYPASS_PAPR_DB (strong, obvious signals such
 * as FM broadcast or LTE) bypass persistence and emit on the first sweep.
 *
 * ## Per-frequency noise floor EMA
 * An asymmetric EMA tracks the observed power at each centre frequency across
 * sweeps.  Rising noise (new interference) adapts at 4 × alpha; falling noise
 * adapts at alpha (slower, avoids immediate re-detection).
 */
#include "SweepConfig.hpp"
#include "FftProcessor.hpp"
#include "IqSource.hpp"
#include "Types.hpp"
#include <au/units/hertz.hh>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>

namespace acq {

/**
 * @brief Drives an IqSource through a frequency sweep and fires detections.
 *
 * Non-copyable.  Call start() to launch the sweep thread and stop() to
 * join it.  The DetectionCallback is invoked from the sweep thread; keep
 * it fast or hand off to a queue.
 */
class SpectrumScanner {
public:
    /// @brief Callback fired for each confirmed detection.
    using DetectionCallback = std::function<void(const Detection&)>;

    /**
     * @brief Construct the scanner.
     * @param cfg     Sweep configuration (frequency range, FFT params, etc.).
     * @param source  IQ dwell source (must outlive this scanner).
     * @param cb      Detection callback (invoked from the sweep thread).
     */
    SpectrumScanner(SweepConfig cfg, IqSource* source, DetectionCallback cb);
    ~SpectrumScanner();

    SpectrumScanner(const SpectrumScanner&)            = delete;
    SpectrumScanner& operator=(const SpectrumScanner&) = delete;

    /// @brief Open the IqSource and launch the sweep thread.
    void start();

    /// @brief Signal stop, close the IqSource, and join the sweep thread.
    void stop();

private:
    SweepConfig       cfg_;
    IqSource*         source_;
    DetectionCallback cb_;

    std::vector<std::unique_ptr<FftProcessor>> processors_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;

    /// Per-channel, per-centre-frequency asymmetric-EMA noise floor.
    /// Rises at 4 × alpha (new interference) / falls at alpha (slow decay).
    std::vector<std::unordered_map<uint64_t, std::vector<float>>> ch_noise_floor_;

    /// Persistence filter state: track hit/miss counts per (channel, centre_hz, freq).
    struct PersistEntry {
        int   hits{0};     ///< Consecutive sweeps the signal was detected.
        int   misses{0};   ///< Consecutive sweeps the signal was absent.
        float peak_db{-300.f}; ///< Highest observed peak power (dBFS).
    };
    std::vector<std::unordered_map<uint64_t,
        std::unordered_map<uint64_t, PersistEntry>>> ch_persist_;

    /// Frequencies quantised to this step before the persistence lookup.
    /// Coarse enough to tolerate sub-bin interpolation jitter across sweeps.
    static constexpr uint64_t PERSIST_FREQ_QUANT_HZ  = 10'000;
    /// Minimum confirmed hits before a detection callback fires.
    static constexpr int      PERSIST_MIN_HITS        = 2;
    /// Max consecutive misses before a persistence entry is evicted.
    static constexpr int      PERSIST_MAX_MISSES      = 4;
    /// Signals with PAPR ≥ this threshold bypass persistence (emit on first sweep).
    static constexpr float    PERSIST_BYPASS_PAPR_DB  = 15.0f;

    void sweepLoop();

    /**
     * @brief Process one dwell and return confirmed detections.
     * @param ch         Channel index.
     * @param center_hz  LO centre frequency for this dwell.
     * @param samples    IQ samples for this dwell.
     * @return Detections that passed the persistence filter.
     */
    std::vector<Detection> processDwell(int ch, au::QuantityD<au::Hertz> center_hz,
                                         const std::vector<std::complex<float>>& samples);
};

} // namespace acq
