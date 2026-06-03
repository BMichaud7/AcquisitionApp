#pragma once
/**
 * @file GpsMonitor.hpp
 * @brief Monitors GPS L1/L2/L5 frequencies for jamming and spoofing.
 *
 * Jamming signature:  broadband noise or CW tone at GPS frequency,
 *                     power > baseline + JAMMER_THRESHOLD_DB.
 * Spoofing signature: BPSK-shaped spectrum (~2 MHz BW) at GPS frequency
 *                     at power > SPOOF_DETECTABLE_DBM (normally GPS is
 *                     ~20 dB below the noise floor — any detectable signal
 *                     at L1/L2/L5 is suspicious).
 */
#include "RfAlert.hpp"
#include "AlertStore.hpp"
#include <au/units/hertz.hh>
#include <functional>
#include <string>
#include <vector>
#include <deque>

namespace acq {

class GpsMonitor {
public:
    using NarrowbandCb = std::function<std::vector<float>(
        au::QuantityD<au::Hertz> freq,
        au::QuantityD<au::Hertz> sr,
        double duration_s)>;

    struct Config {
        double jammer_threshold_db{20.0};  ///< dB above baseline to declare jamming
        double spoof_detectable_dbm{-110.0};///< Any GPS-band signal above this = spoofing
        double baseline_alpha{0.05};       ///< EMA coefficient for baseline update
        double check_interval_s{30.0};    ///< Seconds between L1/L2/L5 checks
        std::string scanner_id;
    };

    GpsMonitor(const Config& cfg, NarrowbandCb fetch_iq, AlertStore& store);

    /// Call periodically from the sweep loop (non-blocking check).
    void tick(double now_s);

private:
    struct BandState {
        double   freq_hz;
        const char* name;
        float    baseline_db{-120.0f};  ///< EMA-smoothed noise floor at this band
        double   last_check_s{-9999.0};
    };

    void checkBand(BandState& band);
    float computePower(const std::vector<float>& iq) const;
    float computeBandwidthMHz(const std::vector<float>& iq, double sr_hz) const;

    Config         cfg_;
    NarrowbandCb   fetch_iq_;
    AlertStore&    store_;
    std::vector<BandState> bands_ = {
        {1'575'420'000.0, "GPS-L1"},
        {1'227'600'000.0, "GPS-L2"},
        {1'176'450'000.0, "GPS-L5"},
    };
};

} // namespace acq
