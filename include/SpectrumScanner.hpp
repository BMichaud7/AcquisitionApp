#pragma once
#include "SweepConfig.hpp"
#include "FftProcessor.hpp"
#include "IqSource.hpp"
#include "Types.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

namespace acq {

// Drives the sweep by pulling dwells from an IqSource, running FFT+detection
// on each, and firing the callback for every signal found.
// IqSource is injected so the same scanner works with both the task manager
// (production) and a direct SoapySDR source (unit tests).
class SpectrumScanner {
public:
    using DetectionCallback = std::function<void(const Detection&)>;

    // source must outlive the scanner.
    SpectrumScanner(SweepConfig cfg, IqSource* source, DetectionCallback cb);
    ~SpectrumScanner();

    SpectrumScanner(const SpectrumScanner&)            = delete;
    SpectrumScanner& operator=(const SpectrumScanner&) = delete;

    void start();
    void stop();  // blocks until the sweep thread exits

private:
    SweepConfig       cfg_;
    IqSource*         source_;
    DetectionCallback cb_;

    std::vector<FftProcessor> processors_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;

    void sweepLoop();
    std::vector<Detection> processDwell(int ch, uint64_t center_hz,
                                         const std::vector<std::complex<float>>& samples);
};

} // namespace acq
