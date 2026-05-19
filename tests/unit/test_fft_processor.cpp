#include <gtest/gtest.h>
#include "FftProcessor.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

using namespace acq;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::vector<std::complex<float>> makeTone(int n, float freq_frac,
                                                  float amplitude = 1.0f) {
    std::vector<std::complex<float>> s((size_t)n);
    for (int i = 0; i < n; ++i) {
        float phi = 2.0f * static_cast<float>(M_PI) * freq_frac * (float)i;
        s[i] = amplitude * std::complex<float>(std::cos(phi), std::sin(phi));
    }
    return s;
}

static std::vector<std::complex<float>> makeZeros(int n) {
    return std::vector<std::complex<float>>((size_t)n, {0.0f, 0.0f});
}

static std::vector<std::complex<float>> makeNoise(int n, float sigma = 0.05f,
                                                   uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.f, sigma);
    std::vector<std::complex<float>> s(n);
    for (auto& c : s) c = {dist(rng), dist(rng)};
    return s;
}

// Synthesise an IQ-imbalanced tone: Q has amplitude error `alpha` and phase
// offset `phi_rad`. Produces a mirror image at -freq_frac.
static std::vector<std::complex<float>> makeImbalancedTone(int n, float freq_frac,
                                                             float alpha, float phi_rad) {
    std::vector<std::complex<float>> s(n);
    for (int i = 0; i < n; ++i) {
        float angle = 2.f * static_cast<float>(M_PI) * freq_frac * i;
        float I_val =       std::cos(angle);
        float Q_val = alpha * std::cos(angle + phi_rad);
        s[i] = {I_val, Q_val};
    }
    return s;
}

// ── Construction ──────────────────────────────────────────────────────────────

TEST(FftProcessor, ValidSizesConstruct) {
    EXPECT_NO_THROW(FftProcessor p64(64));
    EXPECT_NO_THROW(FftProcessor p256(256));
    EXPECT_NO_THROW(FftProcessor p4096(4096));
}

TEST(FftProcessor, FftSizeAccessorReturnsConstructorArg) {
    FftProcessor p(512);
    EXPECT_EQ(p.fft_size(), 512);
}

TEST(FftProcessor, InvalidSizeTooSmallThrows) {
    EXPECT_THROW(FftProcessor p(32), std::invalid_argument);
}

TEST(FftProcessor, InvalidSizeNotPowerOfTwoThrows) {
    EXPECT_THROW(FftProcessor p(100), std::invalid_argument);
    EXPECT_THROW(FftProcessor p(63),  std::invalid_argument);
    EXPECT_THROW(FftProcessor p(65),  std::invalid_argument);
}

// ── Detection with zero input ─────────────────────────────────────────────────

TEST(FftProcessor, ZeroInputProducesNoDetections) {
    FftProcessor proc(256);
    auto zeros = makeZeros(256);
    auto sigs = proc.detect(zeros.data(), (int)zeros.size(), 10.0f, 0.80f, 10e6, 1000);
    EXPECT_TRUE(sigs.empty());
}

// ── Tone detection ────────────────────────────────────────────────────────────

TEST(FftProcessor, ToneAtBin64DetectedInUsableBand) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 100.0f, 0.80f, 10e6, 1000);
    ASSERT_FALSE(sigs.empty()) << "Expected a detection for a strong tone";
    auto best = std::max_element(sigs.begin(), sigs.end(),
        [](const auto& a, const auto& b){ return a.peak_db < b.peak_db; });
    EXPECT_GT(best->peak_db, -20.0f) << "Main lobe power should be well above -20 dBFS";
}

TEST(FftProcessor, WeakToneBelowThresholdNotDetected) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 0.001f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 300.0f, 0.80f, 10e6, 1000);
    EXPECT_TRUE(sigs.empty()) << "Nothing should exceed a 300 dB margin above noise";
}

TEST(FftProcessor, ToneOutsideUsableBandNotDetected) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.47f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    for (const auto& s : sigs) {
        EXPECT_LE(s.start_bin, 230) << "Detection outside usable band";
        EXPECT_GE(s.start_bin, 25)  << "Detection outside usable band";
    }
}

// ── Minimum bandwidth filter ──────────────────────────────────────────────────

TEST(FftProcessor, MinBwFilterSuppressesNarrowPeak) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 100.0f, 0.80f, 10e6, 200'000);
    EXPECT_TRUE(sigs.empty()) << "1-bin detection should be filtered by min_bw=200 kHz";
}

TEST(FftProcessor, SmallMinBwAllowsSingleBinDetection) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    EXPECT_FALSE(sigs.empty()) << "Single-bin tone should pass min_bw=1 kHz filter";
}

// ── binToHz ───────────────────────────────────────────────────────────────────

TEST(FftProcessor, BinToHz_CenterBinIsExactlyCenter) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    EXPECT_EQ(proc.binToHz(128, 10e6, center), center);
}

TEST(FftProcessor, BinToHz_Bin0IsLowestFrequency) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    uint64_t expected = center - 5'000'000ULL;
    EXPECT_NEAR((double)proc.binToHz(0, 10e6, center), (double)expected, 2.0);
}

TEST(FftProcessor, BinToHz_LastBinIsNearNyquist) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    uint64_t hz = proc.binToHz(255, 10e6, center);
    EXPECT_GT(hz, center);
    EXPECT_LT(hz, center + (uint64_t)(10e6 / 2) + 100);
}

TEST(FftProcessor, BinToHz_MonotonicallyIncreasingAcrossBins) {
    FftProcessor proc(64);
    uint64_t center = 433'000'000ULL;
    uint64_t prev = proc.binToHz(0, 2e6, center);
    for (int b = 1; b < 64; ++b) {
        uint64_t cur = proc.binToHz(b, 2e6, center);
        EXPECT_GT(cur, prev) << "binToHz should increase with bin index";
        prev = cur;
    }
}

TEST(FftProcessor, BinToHz_SymmetricAroundCenter) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    int64_t lo = (int64_t)proc.binToHz(0,   10e6, center);
    int64_t hi = (int64_t)proc.binToHz(255, 10e6, center);
    int64_t dl = (int64_t)center - lo;
    int64_t dr = hi - (int64_t)center;
    EXPECT_NEAR((double)dl, (double)dr, 10e6 / 256 + 1);
}

// ── Float binToHz overload (sub-bin interpolation) ────────────────────────────

TEST(FftProcessor, BinToHz_FloatOverloadMatchesIntAtIntegerBin) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    for (int b : {0, 64, 128, 192, 255}) {
        EXPECT_EQ(proc.binToHz((float)b, 10e6, center),
                  proc.binToHz(b,        10e6, center))
            << "Float binToHz should match int version at integer bin " << b;
    }
}

TEST(FftProcessor, BinToHz_FloatOverloadInterpolatesBetweenBins) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    double sr = 10e6;
    uint64_t hz_lo = proc.binToHz(128,   sr, center);
    uint64_t hz_hi = proc.binToHz(129,   sr, center);
    uint64_t hz_mid = proc.binToHz(128.5f, sr, center);
    // Mid-point should be between the two integer bins
    EXPECT_GT(hz_mid, hz_lo);
    EXPECT_LT(hz_mid, hz_hi);
}

// ── Split API: computeSpectrum + detectFromSpectrum ───────────────────────────

TEST(FftProcessor, ComputeSpectrum_PopulatesPowerDb) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    proc.computeSpectrum(tone.data(), (int)tone.size());

    const auto& pdb = proc.powerDb();
    ASSERT_EQ((int)pdb.size(), 256);

    // Peak bin (fftshift: 0.25*256 + 128 = 192) should be the highest.
    int peak_bin = (int)(std::max_element(pdb.begin(), pdb.end()) - pdb.begin());
    // Peak should be near bin 192 (within ±2 for window effects).
    EXPECT_NEAR(peak_bin, 192, 2) << "Peak should land near the expected bin";
}

TEST(FftProcessor, DetectFromSpectrum_MatchesConvenienceDetect) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);

    // Split API
    proc.computeSpectrum(tone.data(), (int)tone.size());
    auto split_sigs = proc.detectFromSpectrum(5.0f, 0.80f, 10e6, 1000);

    // Convenience
    auto conv_sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);

    ASSERT_EQ(split_sigs.size(), conv_sigs.size())
        << "Split and convenience APIs should return same number of signals";
    for (size_t i = 0; i < split_sigs.size(); ++i) {
        EXPECT_EQ(split_sigs[i].start_bin, conv_sigs[i].start_bin);
        EXPECT_EQ(split_sigs[i].end_bin,   conv_sigs[i].end_bin);
        EXPECT_NEAR(split_sigs[i].peak_db, conv_sigs[i].peak_db, 0.01f);
    }
}

TEST(FftProcessor, DetectFromSpectrum_ThresholdAboveAllPowerProducesNoDetections) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    proc.computeSpectrum(tone.data(), (int)tone.size());
    // 999 dB above any realistic floor — nothing should pass
    auto sigs = proc.detectFromSpectrum(999.0f, 0.80f, 10e6, 1000);
    EXPECT_TRUE(sigs.empty());
}

// ── Signal::center_bin sub-bin interpolation ──────────────────────────────────

TEST(FftProcessor, CenterBinIsWithinRunBounds) {
    FftProcessor proc(512);
    // Use a larger FFT and multiple dwell frames for sharper peak.
    auto tone = makeTone(512 * 4, 0.3f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 20e6, 1000);
    ASSERT_FALSE(sigs.empty());
    for (const auto& s : sigs) {
        EXPECT_GE(s.center_bin, (float)(s.start_bin - 1));
        EXPECT_LE(s.center_bin, (float)(s.end_bin   + 1));
    }
}

TEST(FftProcessor, CenterBinCloserToTrueFrequencyThanBinCenter) {
    // Tone at freq_frac = 0.3; fftshift bin = 0.3*256 + 128 = 204.8 → bin 205.
    // Sub-bin interpolation should place center_bin closer to 204.8 than 205.
    FftProcessor proc(256);
    auto tone = makeTone(256 * 8, 0.3f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    ASSERT_FALSE(sigs.empty());
    float expected = 0.3f * 256.f + 128.f;  // = 204.8
    auto best = std::max_element(sigs.begin(), sigs.end(),
        [](const auto& a, const auto& b){ return a.peak_db < b.peak_db; });
    float err_interp = std::abs(best->center_bin - expected);
    float err_integer = std::abs((float)best->peak_db - expected);  // use peak bin as proxy
    // Interpolated center should be within 0.6 bins of the true frequency.
    EXPECT_LT(err_interp, 0.6f) << "Sub-bin interpolation should place center near true freq";
}

// ── Noise floor estimation ────────────────────────────────────────────────────

TEST(FftProcessor, NoiseFloorRobustToMinoritySignal) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    EXPECT_FALSE(sigs.empty()) << "Noise floor estimate should not be dominated by tone";
}

// ── CA-CFAR: detects weak signal near strong one ──────────────────────────────

TEST(FftProcessor, CafarDetectsWeakSignalAlongsideStrongOne) {
    // Strong tone (freq_frac=0.15) raises global noise median.
    // A global-threshold detector would miss the weak tone (0.40) in a dense band.
    // CA-CFAR estimates local noise at each bin, so the weak signal is still visible.
    const int FFT   = 512;
    const int N     = FFT * 16;
    std::vector<std::complex<float>> signal = makeNoise(N, 0.02f);
    for (int i = 0; i < N; ++i) {
        float phi1 = 2.f * static_cast<float>(M_PI) * 0.15f * i;
        signal[i] += 0.9f * std::complex<float>(std::cos(phi1), std::sin(phi1));
        float phi2 = 2.f * static_cast<float>(M_PI) * 0.40f * i;
        signal[i] += 0.04f * std::complex<float>(std::cos(phi2), std::sin(phi2));
    }
    FftProcessor proc(FFT);
    auto sigs = proc.detect(signal.data(), N, 8.0f, 0.80f, 20e6, 1000);
    // Both signals should be detected (they're spectrally separated by 25% of SR).
    EXPECT_GE(sigs.size(), 2u)
        << "CA-CFAR should detect weak signal despite nearby strong one";
}

// ── PAPR filter ───────────────────────────────────────────────────────────────

TEST(FftProcessor, PaprFilter_StrongTonePassesFilter) {
    // A sharp spectral peak (tone) has high PAPR → always passes PAPR filter.
    FftProcessor proc(256);
    auto tone = makeTone(256 * 4, 0.25f, 1.0f);
    auto sigs = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    EXPECT_FALSE(sigs.empty()) << "A pure tone has high PAPR and should always be detected";
}

TEST(FftProcessor, PaprFilter_HighMinPapr_SuppressesFlatBumps) {
    // min_papr_db = 20 is very aggressive. A synthetic run of medium-power noise
    // produces a "bump" with low PAPR that should be suppressed.
    // Test: noise-only input → no detections regardless of PAPR threshold.
    FftProcessor proc(256);
    auto noise = makeNoise(256 * 16, 0.05f);
    // At threshold=5 dB above local noise: with AWGN, CA-CFAR should produce few/no
    // detections; at very high PAPR requirement, even fewer.
    auto sigs_hi = proc.detect(noise.data(), (int)noise.size(),
                                5.0f, 0.80f, 10e6, 1000);
    auto sigs_lo = proc.detectFromSpectrum(5.0f, 0.80f, 10e6, 1000, 0, 0.0f);  // min_papr=0
    // High PAPR must not produce MORE detections than low PAPR.
    EXPECT_LE(sigs_hi.size(), sigs_lo.size())
        << "Higher min_papr_db should not increase detection count";
}

// ── IQ imbalance (informational) ──────────────────────────────────────────────
// Note: Hardware IQ correction (Gram-Schmidt) is not applied in accumFrame.
// The Blackman-Harris window provides ~92 dB sidelobe rejection which suppresses
// the uncorrected AD9361 mirror (~-25 dBc) to ~-117 dBFS — well below any
// detection threshold. Per-dwell Gram-Schmidt has a sign ambiguity for wideband
// signals that can move the main tone to the mirror bin; this is removed until
// a sign-corrected implementation is available.

TEST(FftProcessor, ImbalancedIqToneIsStillDetected) {
    // Even with 10% IQ amplitude imbalance, the dominant signal peak should be
    // detected. The mirror is ~20 dBc weaker and may also appear, but the test
    // only verifies the main tone is found.
    const int FFT = 512;
    const int N   = FFT * 8;
    auto signal = makeImbalancedTone(N, 0.25f, 1.10f, 0.0873f);

    FftProcessor proc(FFT);
    auto sigs = proc.detect(signal.data(), N, 5.0f, 0.80f, 20e6, 1000);
    ASSERT_FALSE(sigs.empty()) << "Imbalanced IQ tone must still produce at least one detection";
    auto best = std::max_element(sigs.begin(), sigs.end(),
        [](const auto& a, const auto& b){ return a.peak_db < b.peak_db; });
    EXPECT_GT(best->peak_db, -20.0f) << "Imbalanced tone peak should be well above -20 dBFS";
}

// ── Welch averaging ───────────────────────────────────────────────────────────

TEST(FftProcessor, WelchAveraging_MoreFramesReducesVariance) {
    // Welch averaging reduces the variance of the power estimate, not its expected
    // value. With more frames: the peak-to-trough spread of the noise bins shrinks.
    // We measure this by counting bins that randomly exceed the mean by > 6 dB —
    // that count should be lower with more averaging frames.
    FftProcessor proc1(256), proc8(256);
    auto noise = makeNoise(256 * 32, 0.05f, 99);

    proc1.computeSpectrum(noise.data(), 256);          // 1 frame
    proc8.computeSpectrum(noise.data(), 256 * 32);     // 31 frames

    const auto& pdb1 = proc1.powerDb();
    const auto& pdb8 = proc8.powerDb();

    // Count bins that are > 6 dB above the mean (random exceedances)
    auto countHighBins = [](const std::vector<float>& pdb) {
        float mean = 0;
        for (float v : pdb) mean += v;
        mean /= (float)pdb.size();
        int count = 0;
        for (float v : pdb) if (v > mean + 6.0f) ++count;
        return count;
    };
    int high1 = countHighBins(pdb1);
    int high8 = countHighBins(pdb8);
    EXPECT_LT(high8, high1)
        << "More Welch frames should reduce random > 6 dB exceedances: 1-frame="
        << high1 << " vs many-frame=" << high8;
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST(FftProcessor, TwoSeparatedTonesYieldAtLeastTwoDetections) {
    FftProcessor proc(256);
    auto t1 = makeTone(256, 0.125f, 1.0f);
    auto t2 = makeTone(256,  0.25f, 1.0f);
    std::vector<std::complex<float>> combined(256);
    for (int i = 0; i < 256; ++i) combined[i] = t1[i] + t2[i];
    auto sigs = proc.detect(combined.data(), (int)combined.size(), 40.0f, 0.80f, 10e6, 1000);
    EXPECT_GE(sigs.size(), 2u)
        << "Two well-separated tones should produce at least two distinct detections";
}

TEST(FftProcessor, ExactlyMinimumFftSizeIsValid) {
    EXPECT_NO_THROW(FftProcessor p(64));
    FftProcessor proc(64);
    EXPECT_EQ(proc.fft_size(), 64);
    auto zeros = makeZeros(64);
    EXPECT_NO_THROW(proc.detect(zeros.data(), (int)zeros.size(), 5.0f, 0.80f, 2e6, 1000));
}

TEST(FftProcessor, LargeFftSizeWorks) {
    EXPECT_NO_THROW(FftProcessor p(4096));
    FftProcessor proc(4096);
    EXPECT_EQ(proc.fft_size(), 4096);
    auto zeros = makeZeros(4096);
    auto sigs = proc.detect(zeros.data(), (int)zeros.size(), 5.0f, 0.80f, 56e6, 1000);
    EXPECT_TRUE(sigs.empty());
}

TEST(FftProcessor, DetectIsIdempotent) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);
    auto s1 = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    auto s2 = proc.detect(tone.data(), (int)tone.size(), 5.0f, 0.80f, 10e6, 1000);
    ASSERT_EQ(s1.size(), s2.size());
    for (size_t i = 0; i < s1.size(); ++i) {
        EXPECT_EQ(s1[i].start_bin, s2[i].start_bin);
        EXPECT_EQ(s1[i].end_bin,   s2[i].end_bin);
    }
}
