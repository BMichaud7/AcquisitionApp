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
#include "GpsMonitor.hpp"
#include <spdlog/spdlog.h>
#include <cmath>
#include <chrono>
#include <numeric>
#include <sstream>

namespace acq {

GpsMonitor::GpsMonitor(const Config& cfg, NarrowbandCb fetch_iq, AlertStore& store)
    : cfg_(cfg), fetch_iq_(std::move(fetch_iq)), store_(store) {}

void GpsMonitor::tick(double now_s) {
    for (auto& band : bands_) {
        if (now_s - band.last_check_s < cfg_.check_interval_s) continue;
        band.last_check_s = now_s;
        checkBand(band);
    }
}

void GpsMonitor::checkBand(BandState& band) {
    // 5 MHz capture centred on the GPS frequency — wide enough to see
    // broadband jammers and the 2.046 MHz C/A code bandwidth of a spoofer.
    constexpr double SR = 5'000'000.0;
    constexpr double DUR = 0.1;  // 100 ms capture

    auto iq = fetch_iq_(au::hertz(band.freq_hz), au::hertz(SR), DUR);
    if (iq.empty()) return;

    float power = computePower(iq);

    // Update baseline with a slow EMA — only when signal is quiet.
    // If power is already elevated, don't let the baseline drift up.
    if (power < band.baseline_db + 10.0f)
        band.baseline_db = (1.0f - cfg_.baseline_alpha) * band.baseline_db
                         +          cfg_.baseline_alpha  * power;

    float delta = power - band.baseline_db;

    spdlog::debug("[GpsMonitor] {} power={:.1f} dB baseline={:.1f} dB delta={:.1f} dB",
                  band.name, power, band.baseline_db, delta);

    if (power < static_cast<float>(cfg_.spoof_detectable_dbm) && delta < static_cast<float>(cfg_.jammer_threshold_db))
        return; // Nothing anomalous

    // Discriminate jamming from spoofing using spectral shape:
    // - Spoofer: BPSK-shaped, BW ~2 MHz, power detectable above noise floor
    // - Jammer: broadband noise or CW tone (BW >> 2 MHz or BW << 0.1 MHz)
    float bw_mhz = computeBandwidthMHz(iq, SR);

    std::ostringstream det;
    AlertType type;
    AlertSeverity sev;

    if (power > static_cast<float>(cfg_.spoof_detectable_dbm) && bw_mhz > 0.5f && bw_mhz < 5.0f) {
        // BPSK-shaped signal at detectable power at a GPS frequency.
        // Real GPS signals are always below the noise floor.
        type = AlertType::GPS_SPOOFING;
        sev  = AlertSeverity::CRITICAL;
        det << band.name << " spoofing suspect: detectable signal at "
            << std::fixed << std::setprecision(1) << power << " dBm, "
            << "BW=" << bw_mhz << " MHz (GPS C/A = 2.046 MHz). "
            << "Real GPS is always below noise floor.";
    } else if (delta >= static_cast<float>(cfg_.jammer_threshold_db)) {
        type = AlertType::GPS_JAMMING;
        sev  = (delta > 40.0f) ? AlertSeverity::CRITICAL : AlertSeverity::HIGH;
        det << band.name << " jammer detected: +" << std::setprecision(1) << delta
            << " dB above baseline, BW=" << bw_mhz << " MHz. "
            << "Baseline=" << band.baseline_db << " dBm.";
    } else {
        return;
    }

    RfAlert alert;
    alert.type        = type;
    alert.severity    = sev;
    alert.freq_hz     = band.freq_hz;
    alert.power_db    = power;
    alert.baseline_db = band.baseline_db;
    alert.details     = det.str();
    alert.scanner_id  = cfg_.scanner_id;

    store_.insert(alert);
}

// RMS power of complex IQ samples (interleaved float32 I,Q,...)
float GpsMonitor::computePower(const std::vector<float>& iq) const {
    if (iq.size() < 2) return -999.0f;
    double sum = 0.0;
    for (size_t i = 0; i + 1 < iq.size(); i += 2)
        sum += static_cast<double>(iq[i]) * iq[i] + static_cast<double>(iq[i+1]) * iq[i+1];
    double rms = std::sqrt(sum / (iq.size() / 2));
    return rms > 1e-12 ? static_cast<float>(20.0 * std::log10(rms)) : -120.0f;
}

// Estimate 3-dB bandwidth by computing the FFT power spectrum and
// counting bins above half-max. Returns bandwidth in MHz.
float GpsMonitor::computeBandwidthMHz(const std::vector<float>& iq, double sr_hz) const {
    // Simple DFT over first 512 complex samples
    const int N = std::min(512, static_cast<int>(iq.size() / 2));
    if (N < 32) return 0.0f;

    std::vector<float> psd(N, 0.0f);
    float peak = -1e9f;
    for (int k = 0; k < N; ++k) {
        float re = 0.0f, im = 0.0f;
        for (int n = 0; n < N; ++n) {
            float angle = -2.0f * 3.14159265f * k * n / N;
            re += iq[2*n]   * std::cos(angle) - iq[2*n+1] * std::sin(angle);
            im += iq[2*n]   * std::sin(angle) + iq[2*n+1] * std::cos(angle);
        }
        psd[k] = re*re + im*im;
        if (psd[k] > peak) peak = psd[k];
    }
    float half = peak / 2.0f;
    int above = 0;
    for (float v : psd) if (v >= half) ++above;
    double bin_hz = sr_hz / N;
    return static_cast<float>(above * bin_hz / 1e6);
}

} // namespace acq
