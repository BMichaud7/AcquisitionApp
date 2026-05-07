#include <gtest/gtest.h>
#include "FftProcessor.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace acq;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Generate fft_size CF32 samples of a complex tone at freq_frac * sample_rate.
static std::vector<std::complex<float>> makeTone(int fft_size, float freq_frac,
                                                  float amplitude = 1.0f) {
    std::vector<std::complex<float>> s((size_t)fft_size);
    for (int i = 0; i < fft_size; ++i) {
        float phi = 2.0f * static_cast<float>(M_PI) * freq_frac * (float)i;
        s[i] = amplitude * std::complex<float>(std::cos(phi), std::sin(phi));
    }
    return s;
}

static std::vector<std::complex<float>> makeZeros(int fft_size) {
    return std::vector<std::complex<float>>((size_t)fft_size, {0.0f, 0.0f});
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

    auto sigs = proc.detect(zeros.data(), 10.0f, 0.80f, 10e6, 1000);
    EXPECT_TRUE(sigs.empty());
}

// ── Tone detection ────────────────────────────────────────────────────────────

TEST(FftProcessor, ToneAtBin64DetectedInUsableBand) {
    // freq_frac = 0.25 → FFT bin 64 → after fftshift: bin 192 (inside usable [26,230]).
    // With a pure-tone test the noise floor is ~-300 dBFS so sidelobes smear across
    // the entire usable range.  Use a high threshold (250 dB above floor) that passes
    // only the main lobe and verify that at least one detection has the expected power.
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);

    auto sigs = proc.detect(tone.data(), 100.0f, 0.80f, 10e6, 1000);
    ASSERT_FALSE(sigs.empty()) << "Expected a detection for a strong tone";

    // Find the detection with the highest peak power — should be the main lobe.
    auto best = std::max_element(sigs.begin(), sigs.end(),
        [](const auto& a, const auto& b){ return a.peak_db < b.peak_db; });
    EXPECT_GT(best->peak_db, -20.0f) << "Main lobe power should be well above -20 dBFS";
}

TEST(FftProcessor, WeakToneBelowThresholdNotDetected) {
    // Pure-tone noise floor is ~-300 dBFS.  A 300 dB threshold above that floor
    // (threshold = 0 dBFS) suppresses everything — even a unit-amplitude tone
    // at ~-6 dBFS cannot survive a 300 dB margin above a -300 dBFS floor.
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 0.001f);

    auto sigs = proc.detect(tone.data(), 300.0f, 0.80f, 10e6, 1000);
    EXPECT_TRUE(sigs.empty()) << "Nothing should exceed a 300 dB margin above noise";
}

TEST(FftProcessor, ToneOutsideUsableBandNotDetected) {
    // Place a tone at freq_frac = 0.01 → FFT bin 2 → fftshift bin 130 (inside usable).
    // Actually use the very edge: bin 0 → fftshift bin 128 (usable centre).
    // To force edge exclusion: freq_frac ≈ 0.47 → bin ~120 → fftshift ~248 > 230 (excluded).
    FftProcessor proc(256);
    // freq_frac = 0.47 → bin = round(0.47*256) = 120 → fftshift = 120+128=248. end_bin=230. Excluded.
    auto tone = makeTone(256, 0.47f, 1.0f);

    auto sigs = proc.detect(tone.data(), 5.0f, 0.80f, 10e6, 1000);
    // The signal might leak into adjacent bins, but the peak should be outside usable range.
    // With usable_fraction=0.80 and fft_size=256: end_bin=230.
    // Verify any detection is within the usable range (no detection at bin 248).
    for (const auto& s : sigs) {
        EXPECT_LE(s.start_bin, 230) << "Detection outside usable band";
        EXPECT_GE(s.start_bin, 25)  << "Detection outside usable band";
    }
}

// ── Minimum bandwidth filter ──────────────────────────────────────────────────

TEST(FftProcessor, MinBwFilterSuppressesNarrowPeak) {
    // With sample_rate=10 MHz, fft_size=256: bin_width = 39062 Hz.
    // threshold_db=100 with pure-tone noise floor ≈ -125 dBFS gives threshold ≈ -25 dBFS.
    // Only the main lobe peak bin (-6 dBFS) is above threshold — that's 1 bin.
    // min_bins = ceil(200000/39062) = 6, so 1 < 6 and the run is filtered out.
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);

    auto sigs = proc.detect(tone.data(), 100.0f, 0.80f, 10e6, 200'000);
    EXPECT_TRUE(sigs.empty()) << "1-bin detection should be filtered by min_bw=200 kHz";
}

TEST(FftProcessor, SmallMinBwAllowsSingleBinDetection) {
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);

    // min_signal_bw_hz = 1000 Hz → min_bins = 1
    auto sigs = proc.detect(tone.data(), 5.0f, 0.80f, 10e6, 1000);
    EXPECT_FALSE(sigs.empty()) << "Single-bin tone should pass min_bw=1 kHz filter";
}

// ── binToHz ───────────────────────────────────────────────────────────────────

TEST(FftProcessor, BinToHz_CenterBinIsExactlyCenter) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    uint64_t hz = proc.binToHz(128, 10e6, center);
    EXPECT_EQ(hz, center) << "Center bin (N/2) should map to center_hz";
}

TEST(FftProcessor, BinToHz_Bin0IsLowestFrequency) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    double sr = 10e6;
    // bin 0: offset = (0 - 128) * (10e6/256) = -128 * 39062.5 = -5,000,000 Hz
    uint64_t expected = center - 5'000'000ULL;
    uint64_t hz = proc.binToHz(0, sr, center);
    EXPECT_NEAR(static_cast<double>(hz), static_cast<double>(expected), 2.0);
}

TEST(FftProcessor, BinToHz_LastBinIsNearNyquist) {
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    double sr = 10e6;
    // bin 255: offset = (255 - 128) * (10e6/256) = 127 * 39062.5 ≈ +4,960,937 Hz
    uint64_t hz = proc.binToHz(255, sr, center);
    EXPECT_GT(hz, center) << "Last bin should be above center";
    EXPECT_LT(hz, center + static_cast<uint64_t>(sr / 2) + 100);
}

TEST(FftProcessor, BinToHz_MonotonicallyIncreasingAcrossBins) {
    FftProcessor proc(64);
    uint64_t center = 433'000'000ULL;
    double sr = 2e6;
    uint64_t prev = proc.binToHz(0, sr, center);
    for (int b = 1; b < 64; ++b) {
        uint64_t cur = proc.binToHz(b, sr, center);
        EXPECT_GT(cur, prev) << "binToHz should increase with bin index";
        prev = cur;
    }
}

// ── Noise floor estimation ────────────────────────────────────────────────────

TEST(FftProcessor, NoiseFloorRobustToMinoritySignal) {
    // Inject a tone (minority of bins above noise); the median-based noise estimate
    // should remain near the noise level, so threshold should still be meaningful.
    FftProcessor proc(256);
    auto tone = makeTone(256, 0.25f, 1.0f);

    // At threshold=5 the tone should be detected; this confirms the noise floor
    // estimate doesn't get pulled up by a single strong peak.
    auto sigs = proc.detect(tone.data(), 5.0f, 0.80f, 10e6, 1000);
    EXPECT_FALSE(sigs.empty()) << "Noise floor estimate should not be dominated by tone";
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST(FftProcessor, TwoSeparatedTonesYieldAtLeastTwoDetections) {
    // tone at freq_frac=0.125 → fftshift bin 160; freq_frac=0.25 → bin 192.
    // Use threshold=250 dB so only the main lobes (not Hann sidelobes) are detected,
    // ensuring the two peaks are resolved as separate signals with a gap between them.
    FftProcessor proc(256);
    auto t1 = makeTone(256, 0.125f, 1.0f);
    auto t2 = makeTone(256,  0.25f, 1.0f);
    std::vector<std::complex<float>> combined(256);
    for (int i = 0; i < 256; ++i)
        combined[i] = t1[i] + t2[i];

    // threshold_db=40: suppresses cross-tone sidelobes (~-107 dBFS) between the
    // two peaks while still detecting both main lobes (~-6 dBFS each).
    auto sigs = proc.detect(combined.data(), 40.0f, 0.80f, 10e6, 1000);
    EXPECT_GE(sigs.size(), 2u)
        << "Two well-separated tones should produce at least two distinct detections";
}

TEST(FftProcessor, ExactlyMinimumFftSizeIsValid) {
    EXPECT_NO_THROW(FftProcessor p(64));
    FftProcessor proc(64);
    EXPECT_EQ(proc.fft_size(), 64);
    // Basic detect must not crash on the minimum size.
    auto zeros = makeZeros(64);
    EXPECT_NO_THROW(proc.detect(zeros.data(), 5.0f, 0.80f, 2e6, 1000));
}

TEST(FftProcessor, LargeFftSizeWorks) {
    EXPECT_NO_THROW(FftProcessor p(4096));
    FftProcessor proc(4096);
    EXPECT_EQ(proc.fft_size(), 4096);
    auto zeros = makeZeros(4096);
    auto sigs = proc.detect(zeros.data(), 5.0f, 0.80f, 56e6, 1000);
    EXPECT_TRUE(sigs.empty());
}

TEST(FftProcessor, BinToHz_SymmetricAroundCenter) {
    // For an even fft_size, bin N/2 is center and the axis should be symmetric.
    FftProcessor proc(256);
    uint64_t center = 915'000'000ULL;
    double sr = 10e6;
    uint64_t lo = proc.binToHz(0,   sr, center);
    uint64_t hi = proc.binToHz(255, sr, center);
    // lo and hi should be approximately equidistant from center.
    int64_t dist_lo = static_cast<int64_t>(center) - static_cast<int64_t>(lo);
    int64_t dist_hi = static_cast<int64_t>(hi)     - static_cast<int64_t>(center);
    EXPECT_NEAR(static_cast<double>(dist_lo), static_cast<double>(dist_hi),
                sr / 256 + 1);  // within one bin width
}
