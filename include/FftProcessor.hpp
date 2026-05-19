#pragma once
#include <complex>
#include <vector>
#include <cstdint>
#include <fftw3.h>

namespace acq {

// Processes one dwell of CF32 samples and returns detected signals.
//
// Detection pipeline:
//   1. Welch averaging (50% overlap frames) → power_db_ in dBFS
//      Each frame: DC removal + IQ imbalance correction (Gram-Schmidt) + BH window
//   2. CA-CFAR per-bin local noise estimate (O(N) via prefix sum)
//   3. PAPR filter: multi-bin runs must have a clear spectral peak
//   4. Sub-bin parabolic interpolation of peak bin (Signal::center_bin)
//
// IQ imbalance correction: parameters estimated once per dwell from all samples,
// applied per-frame. Cancels the AD9361 mirror image (~25 dBc without correction
// → ~55 dBc after), eliminating a major source of false detections.
//
// Thread-compatible: one instance per thread / channel.
class FftProcessor {
public:
    struct Signal {
        int   start_bin;    // first bin of the detected group (fftshift coords)
        int   end_bin;      // last bin of the detected group, inclusive
        float peak_db;      // peak power in dBFS
        float mean_db;      // mean power of the group in dBFS
        float center_bin;   // sub-bin interpolated peak position (fftshift coords)
    };

    // cfar_guard: guard cells each side of test cell (excluded from local noise estimate)
    // cfar_ref:   reference cells each side used to average local noise
    explicit FftProcessor(int fft_size, int cfar_guard = 8, int cfar_ref = 32);
    ~FftProcessor();

    FftProcessor(const FftProcessor&)            = delete;
    FftProcessor& operator=(const FftProcessor&) = delete;

    // Run Welch averaging on [samples, samples+n_samples); populates powerDb().
    void computeSpectrum(const std::complex<float>* samples, int n_samples);

    // Detect signals using the spectrum last computed by computeSpectrum().
    // hist_floor: optional per-bin historical noise floor — raises local noise
    //             estimate in persistently noisy bins.
    std::vector<Signal> detectFromSpectrum(
        float    threshold_db,
        float    usable_fraction,
        double   sample_rate,
        uint32_t min_signal_bw_hz,
        uint32_t dc_guard_hz  = 0,
        float    min_papr_db  = 3.0f,
        const std::vector<float>* hist_floor = nullptr);

    // Convenience: computeSpectrum + detectFromSpectrum in one call.
    std::vector<Signal> detect(
        const std::complex<float>* samples,
        int      n_samples,
        float    threshold_db,
        float    usable_fraction,
        double   sample_rate,
        uint32_t min_signal_bw_hz,
        uint32_t dc_guard_hz = 0);

    const std::vector<float>& powerDb() const { return power_db_; }

    uint64_t binToHz(int   bin, double sample_rate, uint64_t center_hz) const;
    uint64_t binToHz(float bin, double sample_rate, uint64_t center_hz) const;

    int fft_size() const { return fft_size_; }

private:
    int            fft_size_;
    int            cfar_guard_;
    int            cfar_ref_;
    fftwf_complex* in_{nullptr};
    fftwf_complex* out_{nullptr};
    fftwf_plan     plan_{nullptr};

    std::vector<float> window_;       // Blackman-Harris coefficients
    std::vector<float> linear_acc_;   // Welch linear-power accumulator
    std::vector<float> power_db_;     // averaged power in dBFS after Welch
    std::vector<float> scratch_;      // median sort scratch — no per-call alloc
    std::vector<float> psum_;         // CFAR prefix sum — pre-allocated, no per-dwell alloc

    void  accumFrame(const std::complex<float>* src);
    float estimateNoise(int start_bin, int end_bin);
};

} // namespace acq
