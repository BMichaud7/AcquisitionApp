#pragma once
#include <complex>
#include <cstdint>
#include <vector>

namespace acq {

// One dwell position's worth of IQ data, one sample buffer per channel.
struct Dwell {
    uint64_t center_hz{0};
    std::vector<std::vector<std::complex<float>>> ch_samples;  // [channel][sample]
};

// Abstract source of IQ dwells.
// Production implementation: TaskManagerIqSource (AMQP task request + UDP receive).
// Test implementation: SoapyIqSource (direct SoapySDR device — test binary only).
class IqSource {
public:
    virtual ~IqSource() = default;
    IqSource(const IqSource&)            = delete;
    IqSource& operator=(const IqSource&) = delete;

    // Connect to the source (open device or submit task). Throws on failure.
    virtual void open() = 0;

    // Release all resources and cancel any in-flight task.
    virtual void close() = 0;

    // Signal stop from another thread. next() will return false after this.
    virtual void stop() = 0;

    // Block until the next dwell is ready.
    // Fills d.center_hz and d.ch_samples.
    // Returns false when stopped or the source is exhausted.
    virtual bool next(Dwell& d) = 0;

    virtual int numChannels() const = 0;

protected:
    IqSource() = default;
};

} // namespace acq
