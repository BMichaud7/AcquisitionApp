// FftProcessor micro-benchmark
// Measures wall-clock time for the hot path: computeSpectrum + detectFromSpectrum.
// Uses realistic inputs (20 MSPS, 131 072-sample dwell, 4096-pt FFT = 63 Welch frames).
// Build via CMake target `acq_bench`; run from any working directory.

#include "FftProcessor.hpp"
#include <au/units/hertz.hh>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace acq;
using namespace std::chrono;

// ── Signal generators ─────────────────────────────────────────────────────────

static std::vector<std::complex<float>>
makeNoise(int n, float sigma = 0.05f, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.f, sigma);
    std::vector<std::complex<float>> s(n);
    for (auto& c : s) c = {dist(rng), dist(rng)};
    return s;
}

// AWGN + 4 tones at different power levels, simulating a busy scan band.
static std::vector<std::complex<float>>
makeRealisticRf(int n, float noise_sigma = 0.05f) {
    auto s = makeNoise(n, noise_sigma);
    struct Tone { float freq_frac; float amp; };
    constexpr int NT = 4;
    float ffs[NT] = {0.08f, 0.22f, 0.41f, 0.49f};
    float amps[NT] = {0.80f, 0.35f, 0.55f, 0.90f};
    for (int t = 0; t < NT; ++t)
        for (int i = 0; i < n; ++i) {
            float phi = 2.f * static_cast<float>(M_PI) * ffs[t] * i;
            s[i] += amps[t] * std::complex<float>(std::cos(phi), std::sin(phi));
        }
    return s;
}

// Imbalanced IQ: amplitude 10%, phase 5°. Tests IQ correction path.
static std::vector<std::complex<float>>
makeImbalancedRf(int n, float noise_sigma = 0.05f) {
    constexpr float ALPHA = 1.10f;
    constexpr float PHI   = 0.0873f;  // 5 deg
    auto s = makeNoise(n, noise_sigma);
    for (int i = 0; i < n; ++i) {
        float angle = 2.f * static_cast<float>(M_PI) * 0.25f * i;
        float I_val =       std::cos(angle);
        float Q_val = ALPHA * std::cos(angle + PHI);
        s[i] += std::complex<float>(I_val, Q_val);
    }
    return s;
}

// ── Timing helper ─────────────────────────────────────────────────────────────

struct Result { double mean_ms; double min_ms; };

static Result timeit(int warmup, int reps, const std::function<void()>& fn) {
    for (int i = 0; i < warmup; ++i) fn();

    double sum = 0, min_t = 1e18;
    for (int i = 0; i < reps; ++i) {
        auto t0 = high_resolution_clock::now();
        fn();
        double ms = duration_cast<nanoseconds>(
            high_resolution_clock::now() - t0).count() / 1e6;
        sum += ms;
        if (ms < min_t) min_t = ms;
    }
    return {sum / reps, min_t};
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    constexpr int    FFT_SIZE   = 4096;
    constexpr int    N_SAMPLES  = 131072;   // 20 MSPS × 6.55 ms
    constexpr double SR         = 20e6;
    constexpr int    N_FRAMES   = (N_SAMPLES - FFT_SIZE) / (FFT_SIZE / 2) + 1;
    constexpr int    WARMUP     = 3;
    constexpr int    REPS       = 30;

    double receive_ms = N_SAMPLES / SR * 1e3;  // real-time cost at 20 MSPS

    printf("======================================================\n");
    printf("  AcquisitionApp — FftProcessor benchmark\n");
    printf("======================================================\n");
    printf("  FFT size    : %d\n", FFT_SIZE);
    printf("  Dwell       : %d samples  (%.1f ms at %.0f MSPS)\n",
           N_SAMPLES, receive_ms, SR / 1e6);
    printf("  Welch frames: %d  (50%% overlap)\n", N_FRAMES);
    printf("  Repetitions : %d  (after %d warmup)\n\n", REPS, WARMUP);

    auto rf_signal   = makeRealisticRf(N_SAMPLES);
    auto iq_imbal    = makeImbalancedRf(N_SAMPLES);
    auto noise_only  = makeNoise(N_SAMPLES);

    FftProcessor proc(FFT_SIZE);

    auto bench = [&](const char* label, const std::vector<std::complex<float>>& sig) {
        // computeSpectrum alone
        auto rs = timeit(WARMUP, REPS,
            [&]{ proc.computeSpectrum(sig.data(), N_SAMPLES); });

        // detectFromSpectrum alone (spectrum already computed)
        proc.computeSpectrum(sig.data(), N_SAMPLES);
        auto rd = timeit(WARMUP, REPS, [&]{
            proc.detectFromSpectrum(10.f, 0.80f,
                au::hertz(SR), au::hertz(1000), au::hertz(50000), 3.f);
        });

        // Full pipeline
        auto rt = timeit(WARMUP, REPS, [&]{
            proc.computeSpectrum(sig.data(), N_SAMPLES);
            proc.detectFromSpectrum(10.f, 0.80f,
                au::hertz(SR), au::hertz(1000), au::hertz(50000), 3.f);
        });

        double overhead_pct  = rt.mean_ms / receive_ms * 100.0;
        double realtime_factor = receive_ms / rt.mean_ms;

        printf("  [%s]\n", label);
        printf("    computeSpectrum  : %6.3f ms avg  %6.3f ms min\n",
               rs.mean_ms, rs.min_ms);
        printf("    detectFromSpectrum: %6.3f ms avg  %6.3f ms min\n",
               rd.mean_ms, rd.min_ms);
        printf("    Full pipeline    : %6.3f ms avg  %6.3f ms min\n",
               rt.mean_ms, rt.min_ms);
        printf("    Overhead vs SDR  : %.1f%%  (%.1fx real-time headroom)\n\n",
               overhead_pct, realtime_factor);
    };

    bench("Realistic RF (4 tones + AWGN)",   rf_signal);
    bench("IQ-imbalanced RF (IQ correction)", iq_imbal);
    bench("Noise only",                       noise_only);

    // Per-frame cost breakdown
    {
        proc.computeSpectrum(rf_signal.data(), N_SAMPLES);
        auto rs = timeit(WARMUP, REPS,
            [&]{ proc.computeSpectrum(rf_signal.data(), N_SAMPLES); });
        printf("  Per-frame cost: %.3f ms / %d frames = %.4f ms/frame\n",
               rs.mean_ms, N_FRAMES, rs.mean_ms / N_FRAMES);
        printf("  Samples/sec   : %.1f M  (%.1fx of %.0f MSPS SDR rate)\n",
               N_SAMPLES / rs.mean_ms / 1e3,
               receive_ms / rs.mean_ms,
               SR / 1e6);
    }

    printf("\n======================================================\n");
    return 0;
}
