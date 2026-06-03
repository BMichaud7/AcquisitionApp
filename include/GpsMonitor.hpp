#pragma once
/**
 * @file GpsMonitor.hpp
 * @brief Monitors GPS L1, L2, and L5 frequencies for jamming and spoofing.
 *
 * GpsMonitor periodically requests short IQ captures at each of the three GPS
 * carrier frequencies, computes the received power, and classifies any anomaly:
 *
 * @par Jamming detection
 * Power exceeds the exponential moving-average (EMA) baseline by more than
 * @c Config::jammer_threshold_db.  This catches broadband noise jammers (flat
 * elevated noise floor), sweep jammers (intermittent), and CW tone jammers
 * (single bin well above baseline).
 *
 * @par Spoofing detection
 * Power exceeds @c Config::spoof_detectable_dbm **and** the estimated 3-dB
 * spectral bandwidth falls in the range 0.5–5 MHz.  Real GPS signals are always
 * ~20 dB below the noise floor (~−130 dBm at antenna); any signal with BPSK-like
 * characteristics at a GPS carrier frequency at detectable power is a strong
 * spoofing indicator.
 *
 * @par Integration
 * GpsMonitor::tick() should be called from the AcquisitionApp sweep loop or a
 * dedicated timer.  It is rate-limited internally by @c Config::check_interval_s
 * to avoid interrupting the sweep too frequently.
 *
 * @see ThreatConfig, AlertStore, RfAlert
 */
#include "RfAlert.hpp"
#include "AlertStore.hpp"
#include <au/units/hertz.hh>
#include <functional>
#include <string>
#include <vector>

namespace acq {

/**
 * @class GpsMonitor
 * @brief Passive GPS frequency threat monitor using wideband IQ captures.
 *
 * Injecting the IQ-fetch callback makes this unit-testable without a real SDR.
 */
class GpsMonitor {
public:
    /**
     * @brief Callback type used to request a short IQ capture at a given frequency.
     *
     * @param freq      Centre frequency to tune to.
     * @param sr        Sample rate for the capture (typically 5 MHz for GPS bands).
     * @param duration_s Duration of the capture in seconds.
     * @return Interleaved float32 I,Q samples (even=I, odd=Q), empty on failure.
     */
    using NarrowbandCb = std::function<std::vector<float>(
        au::QuantityD<au::Hertz> freq,
        au::QuantityD<au::Hertz> sr,
        double duration_s)>;

    /**
     * @brief Runtime configuration for the GPS monitor.
     */
    struct Config {
        double jammer_threshold_db{20.0};    ///< Power delta (dB above EMA baseline) to declare jamming.
        double spoof_detectable_dbm{-110.0}; ///< Any GPS-band signal above this level is a spoofing suspect.
                                              ///<   Real GPS is always ~−130 dBm (buried in noise).
        double baseline_alpha{0.05};          ///< EMA coefficient for baseline noise-floor tracker.
                                              ///<   Smaller values = slower adaptation.  Range (0, 1).
        double check_interval_s{30.0};       ///< Minimum seconds between successive checks per GPS band.
        std::string scanner_id;               ///< Identifier embedded in generated RfAlert records.
    };

    /**
     * @brief Construct the GPS monitor.
     * @param cfg      Monitor configuration (thresholds, check interval).
     * @param fetch_iq IQ-fetch callback; called once per band check.
     * @param store    AlertStore to persist generated RfAlert records.
     */
    GpsMonitor(const Config& cfg, NarrowbandCb fetch_iq, AlertStore& store);

    /**
     * @brief Advance the monitor clock and trigger band checks if the interval has elapsed.
     *
     * Call this from the acquisition loop or a periodic timer.  It will trigger
     * at most one IQ capture per GPS band per @c Config::check_interval_s seconds.
     * @param now_s Current monotonic time in seconds (e.g. from @c steady_clock).
     */
    void tick(double now_s);

private:
    /**
     * @brief State machine for a single GPS frequency band.
     */
    struct BandState {
        double      freq_hz;          ///< Nominal carrier frequency (Hz).
        const char* name;             ///< Human-readable band name ("GPS-L1", etc.).
        float       baseline_db{-120.0f}; ///< EMA noise-floor estimate (dBFS).
        double      last_check_s{-9999.0};///< Monotonic time of last IQ capture.
    };

    /**
     * @brief Perform one IQ capture and threat evaluation for @p band.
     * @param band Band state to evaluate (modified in place to update baseline).
     */
    void checkBand(BandState& band);

    /**
     * @brief Compute RMS power from interleaved CF32 IQ samples.
     * @param iq Interleaved float32 I,Q samples.
     * @return Power in dBFS, or −120 dBFS if the input is too small.
     */
    float computePower(const std::vector<float>& iq) const;

    /**
     * @brief Estimate the 3-dB occupied bandwidth of the signal using a DFT.
     * @param iq   Interleaved CF32 IQ samples (first 512 complex samples used).
     * @param sr_hz Capture sample rate in Hz.
     * @return Estimated 3-dB bandwidth in MHz.
     */
    float computeBandwidthMHz(const std::vector<float>& iq, double sr_hz) const;

    Config                 cfg_;     ///< Runtime configuration.
    NarrowbandCb           fetch_iq_;///< IQ-fetch injection point.
    AlertStore&            store_;   ///< Alert persistence backend.
    std::vector<BandState> bands_;   ///< Per-band state (L1, L2, L5).
};

} // namespace acq
