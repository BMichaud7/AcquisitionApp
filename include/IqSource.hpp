/*
========================================================================
Project: OpenRFStack
Author:  Brendan Michaud
Year:    2026
Part of OpenRFStack (https://github.com/OpenRFStack)

Licensed under the Personal Use License.
Do not use for commercial, organizational, or military purposes.
Contact author for permission: https://github.com/OpenRFStack
========================================================================
*/
#pragma once
/**
 * @file IqSource.hpp
 * @brief Abstract IQ dwell source interface.
 *
 * SpectrumScanner calls IqSource::next() in a loop to obtain successive dwells.
 * Two implementations exist:
 * - **TaskManagerIqSource** (production) — submits SCAN tasks via AMQP and
 *   receives CF32 IQ packets over UDP from SdrResourceManager.
 * - **SoapyIqSource** (unit tests only) — drives a SoapySDR device directly,
 *   bypassing AMQP entirely.
 */
#include <au/units/hertz.hh>
#include <complex>
#include <cstdint>
#include <vector>

namespace acq {

/**
 * @brief One dwell's worth of IQ data from a single frequency position.
 *
 * ch_samples[c] holds all samples for channel @p c.
 * For a single-channel PlutoSDR, ch_samples has one element.
 */
struct Dwell {
    au::QuantityD<au::Hertz> center_hz{au::hertz(0.0)}; ///< LO centre frequency for this dwell.
    /// Samples per channel: ch_samples[channel][sample_index].
    /// Each sample is a CF32 complex<float> (I + jQ).
    std::vector<std::vector<std::complex<float>>> ch_samples;
};

/**
 * @brief Abstract source of IQ dwells consumed by SpectrumScanner.
 *
 * Non-copyable. Implementations must be thread-safe with respect to stop()
 * being called from a different thread than next().
 */
class IqSource {
public:
    virtual ~IqSource() = default;
    IqSource(const IqSource&)            = delete;
    IqSource& operator=(const IqSource&) = delete;

    /**
     * @brief Connect to the source (open device or submit task).
     * @throws std::runtime_error on connection failure.
     */
    virtual void open() = 0;

    /// @brief Release all resources and cancel any in-flight task.
    virtual void close() = 0;

    /**
     * @brief Signal stop from another thread.
     *
     * After stop() is called, the next call to next() will return false
     * as soon as the current dwell completes.  Thread-safe.
     */
    virtual void stop() = 0;

    /**
     * @brief Block until the next dwell is ready.
     *
     * Fills @p d.center_hz and @p d.ch_samples on success.
     *
     * @param d  Output dwell (overwritten on return).
     * @return   true if a dwell was delivered; false if stopped or exhausted.
     */
    virtual bool next(Dwell& d) = 0;

    /// @brief Number of RX channels this source delivers per dwell.
    virtual int numChannels() const = 0;

protected:
    IqSource() = default;
};

} // namespace acq
