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
 * @file FftProcessor.hpp
 * @note Frequency/rate parameters use Au quantity types (au::QuantityD<au::Hertz>).
 *       Raw double values remain for threshold_db, usable_fraction, and min_papr_db.
 * @brief Welch-averaged FFT + CA-CFAR signal detector for one dwell.
 *
 * Detection pipeline applied to each dwell of CF32 IQ samples:
 * 1. **Welch averaging** — 50%-overlapping frames, each DC-removed,
 *    IQ-imbalance corrected (Gram-Schmidt), and Blackman-Harris windowed
 *    before FFT.  All frame power spectra are averaged linearly then
 *    converted to dBFS.
 * 2. **CA-CFAR** — per-bin local noise estimated via O(N) prefix sum
 *    over @p cfar_ref reference cells (skipping @p cfar_guard guard cells).
 * 3. **PAPR filter** — multi-bin runs must have peak-to-mean ratio ≥
 *    @p min_papr_db; rejects flat noise bumps.
 * 4. **Sub-bin interpolation** — 3-point parabolic fit on the peak bin
 *    gives Signal::center_bin with ~1–2 kHz accuracy at 20 MSPS / 4096 bins.
 *
 * **IQ imbalance correction** — amplitude and phase parameters are estimated
 * once per dwell using all samples, then applied per-frame via Gram-Schmidt
 * orthogonalisation.  Reduces the AD9361 mirror image from ~25 dBc to ~55 dBc.
 *
 * **Thread safety** — one instance per thread (owns FFTW plan and scratch buffers).
 */
#include <au/units/hertz.hh>
#include <complex>
#include <vector>
#include <cstdint>
#include <fftw3.h>

namespace acq {

/**
 * @brief FFT-based signal detector for one IQ dwell.
 *
 * Instantiate once per processing thread.  Call computeSpectrum() then
 * detectFromSpectrum() on each dwell, or use the convenience detect() wrapper.
 */
class FftProcessor {
public:
    /**
     * @brief Describes one detected signal group in the power spectrum.
     */
    struct Signal {
        int   start_bin;    ///< First bin of the run (fftshift coordinates).
        int   end_bin;      ///< Last bin of the run, inclusive (fftshift coordinates).
        float peak_db;      ///< Peak power in the run (dBFS).
        float mean_db;      ///< Mean power of the run (dBFS).
        float center_bin;   ///< Sub-bin interpolated peak position (fftshift coordinates).
    };

    /**
     * @brief Construct an FftProcessor.
     * @param fft_size    FFT size (must be a power of 2, ≥ 64).
     * @param cfar_guard  Guard cells each side of the test bin (excluded from
     *                    the local noise average to avoid signal self-noise).
     * @param cfar_ref    Reference cells each side for the CA-CFAR noise estimate.
     */
    explicit FftProcessor(int fft_size, int cfar_guard = 8, int cfar_ref = 32);
    ~FftProcessor();

    FftProcessor(const FftProcessor&)            = delete;
    FftProcessor& operator=(const FftProcessor&) = delete;

    /**
     * @brief Run Welch averaging on @p n_samples IQ samples.
     *
     * Populates powerDb().  Must be called before detectFromSpectrum().
     *
     * @param samples   Pointer to interleaved CF32 IQ samples.
     * @param n_samples Total sample count (must be ≥ fft_size()).
     */
    void computeSpectrum(const std::complex<float>* samples, int n_samples);

    /**
     * @brief Detect signals using the spectrum produced by computeSpectrum().
     *
     * @param threshold_db      Detection threshold above local CA-CFAR noise (dB).
     * @param usable_fraction   Fraction of bins examined; edge bins are discarded
     *                          to avoid roll-off artefacts (e.g. 0.80).
     * @param sample_rate       Sample rate of the dwell (samples/s); used for
     *                          Hz-to-bin and bin-to-Hz conversions.
     * @param min_signal_bw_hz  Minimum run width to be reported (Hz); single-bin
     *                          spurs narrower than this are discarded.
     * @param dc_guard_hz       Bins within this range of DC (0 Hz) are blanked to
     *                          suppress LO leakage.  Pass 0 to disable.
     * @param min_papr_db       Minimum peak-to-mean power ratio (dB) for multi-bin
     *                          runs.  Flat noise bumps below this threshold are
     *                          discarded.
     * @param hist_floor        Optional per-bin historical noise floor (same length
     *                          as fft_size).  If provided, the local noise estimate
     *                          is raised to max(cfar, hist_floor[bin]) to suppress
     *                          persistent interference.  Pass nullptr to disable.
     * @return Detected signal groups, sorted by peak_db descending.
     */
    std::vector<Signal> detectFromSpectrum(
        float                    threshold_db,
        float                    usable_fraction,
        au::QuantityD<au::Hertz> sample_rate,
        au::QuantityD<au::Hertz> min_signal_bw_hz,
        au::QuantityD<au::Hertz> dc_guard_hz  = au::hertz(0.0),
        float                    min_papr_db  = 3.0f,
        const std::vector<float>* hist_floor = nullptr);

    /**
     * @brief Convenience: computeSpectrum() + detectFromSpectrum() in one call.
     * @param samples       Pointer to interleaved CF32 IQ samples.
     * @param n_samples     Total sample count.
     * @param threshold_db  Detection threshold above CA-CFAR noise (dB).
     * @param usable_fraction Fraction of bins examined.
     * @param sample_rate   Sample rate (samples/s).
     * @param min_signal_bw_hz Minimum signal bandwidth (Hz).
     * @param dc_guard_hz   DC guard zone (Hz).  0 = disabled.
     * @return Detected signal groups.
     */
    std::vector<Signal> detect(
        const std::complex<float>* samples,
        int                      n_samples,
        float                    threshold_db,
        float                    usable_fraction,
        au::QuantityD<au::Hertz> sample_rate,
        au::QuantityD<au::Hertz> min_signal_bw_hz,
        au::QuantityD<au::Hertz> dc_guard_hz = au::hertz(0.0));

    /// @brief Power spectrum in dBFS after the last computeSpectrum() call.
    const std::vector<float>& powerDb() const { return power_db_; }

    /**
     * @brief Convert an integer bin index to a frequency quantity (fftshift coordinates).
     * @param bin         Bin index in fftshift layout.
     * @param sample_rate Sample rate.
     * @param center_hz   LO centre frequency.
     * @return Absolute frequency as an Au quantity.
     */
    au::QuantityD<au::Hertz> binToHz(int   bin,
                                     au::QuantityD<au::Hertz> sample_rate,
                                     au::QuantityD<au::Hertz> center_hz) const;

    /**
     * @brief Convert a fractional (sub-bin) index to a frequency quantity (fftshift coordinates).
     * @param bin         Sub-bin position from parabolic interpolation.
     * @param sample_rate Sample rate.
     * @param center_hz   LO centre frequency.
     * @return Absolute frequency as an Au quantity.
     */
    au::QuantityD<au::Hertz> binToHz(float bin,
                                     au::QuantityD<au::Hertz> sample_rate,
                                     au::QuantityD<au::Hertz> center_hz) const;

    /// @brief FFT size this processor was constructed with.
    int fft_size() const { return fft_size_; }

private:
    int            fft_size_;
    int            cfar_guard_;
    int            cfar_ref_;
    fftwf_complex* in_{nullptr};
    fftwf_complex* out_{nullptr};
    fftwf_plan     plan_{nullptr};

    std::vector<float> window_;       ///< Blackman-Harris window coefficients.
    std::vector<float> linear_acc_;   ///< Welch linear-power accumulator.
    std::vector<float> power_db_;     ///< Averaged power in dBFS (fftshift layout).
    std::vector<float> scratch_;      ///< Scratch buffer — no per-call allocation.
    std::vector<float> psum_;         ///< CFAR prefix sum — pre-allocated.

    void  accumFrame(const std::complex<float>* src);
    float estimateNoise(int start_bin, int end_bin);
};

} // namespace acq
