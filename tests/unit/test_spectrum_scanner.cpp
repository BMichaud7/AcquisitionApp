#include <gtest/gtest.h>
#include "SpectrumScanner.hpp"
#include "SoapyIqSource.hpp"
#include "FakeAcqSoapyControl.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace acq;

// ── Helpers ───────────────────────────────────────────────────────────────────

static SweepConfig makeTestConfig(bool shared_lo = false, int rx_channels = 1) {
    // shared_lo has no meaning in the task-manager path; it only matters for
    // SoapyIqSource which uses it to decide whether to tune channels independently.
    // We keep the field so both paths accept the same SweepConfig.
    (void)shared_lo;
    SweepConfig cfg;
    cfg.scanner_id                = "test-scanner";
    cfg.device.rx_channels        = rx_channels;
    cfg.device.sample_rate        = 10e6;
    cfg.device.rx_gain_db         = 40.0;
    cfg.device.bandwidth_hz       = 10e6;
    cfg.sweep.start_hz            = 100'000'000;   // 100 MHz
    cfg.sweep.stop_hz             = 108'000'000;   // 108 MHz — exactly one dwell step (step = sr*usable = 8 MHz)
    cfg.sweep.dwell_samples       = 256;
    cfg.sweep.fft_size            = 256;
    cfg.sweep.usable_bw_fraction  = 0.80;
    cfg.sweep.threshold_db        = 15.0;  // suppress Hann sidelobe fragments while keeping main lobe
    cfg.sweep.min_signal_bw_hz    = 1000;
    cfg.sweep.settle_samples      = 0;
    return cfg;
}

static bool waitFor(std::function<bool()> pred, int timeout_ms = 500) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return pred();
}

// ── Fixture ───────────────────────────────────────────────────────────────────

struct SpectrumScannerTest : ::testing::Test {
    void SetUp()    override { FakeAcqSoapy::reset(); }
    void TearDown() override { FakeAcqSoapy::reset(); }
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TEST_F(SpectrumScannerTest, ConstructsWithoutOpeningDevice) {
    // Construction must not call open() on the source.
    // Setting fail_open means any Device::make() call would throw.
    FakeAcqSoapy::fail_open.store(true);
    SoapyIqSource source(makeTestConfig());
    EXPECT_NO_THROW({
        SpectrumScanner scanner(makeTestConfig(), &source, [](const Detection&){});
    });
}

TEST_F(SpectrumScannerTest, StartAndStopCleanly) {
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source, [](const Detection&){});
    ASSERT_NO_THROW(scanner.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_NO_THROW(scanner.stop());  // must not hang
}

TEST_F(SpectrumScannerTest, DoubleStopIsHarmless) {
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source, [](const Detection&){});
    scanner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scanner.stop();
    EXPECT_NO_THROW(scanner.stop());
}

// ── Detection behaviour ───────────────────────────────────────────────────────

TEST_F(SpectrumScannerTest, NoDetectionsWithZeroInput) {
    FakeAcqSoapy::tone_enabled.store(false);  // all-zero samples

    std::atomic<int> count{0};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(),
        &source, [&count](const Detection&){ count++; });
    scanner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    scanner.stop();

    EXPECT_EQ(count.load(), 0) << "Zero-valued samples should produce no detections";
}

TEST_F(SpectrumScannerTest, CallbackFiredWhenToneInjected) {
    // Inject a strong complex tone at 25 % of sample rate.
    // After fftshift that lands at bin 192, well inside usable band [25, 230].
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    std::atomic<int> count{0};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(),
        &source, [&count](const Detection&){ count++; });
    scanner.start();

    bool fired = waitFor([&]{ return count.load() > 0; }, 500);
    scanner.stop();

    EXPECT_TRUE(fired) << "Detection callback should fire for a 1.0-amplitude tone";
    EXPECT_GT(count.load(), 0);
}

TEST_F(SpectrumScannerTest, DetectionHasReasonableMetadata) {
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    Detection captured{};
    std::atomic<bool> got{false};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source,
        [&](const Detection& d){
            if (!got.exchange(true)) captured = d;
        });
    scanner.start();
    waitFor([&]{ return got.load(); }, 500);
    scanner.stop();

    ASSERT_TRUE(got.load()) << "Did not receive a detection in time";
    EXPECT_EQ(captured.scanner_id, "test-scanner");
    EXPECT_EQ(captured.channel, 0);
    EXPECT_GT(captured.bandwidth_hz, 0u);
    EXPECT_GT(captured.power_db, -50.0f);

    const double fc = static_cast<double>(captured.center_freq_hz);
    EXPECT_GT(fc, 100e6);
    EXPECT_LT(fc, 108e6);
}

// ── IQ snapshot ───────────────────────────────────────────────────────────────

TEST_F(SpectrumScannerTest, DetectionHasIqSnapshot) {
    // Every confirmed detection must carry an IQ snapshot.
    // Snapshot size = min(1024, dwell_samples) × 2 interleaved floats.
    // Test config uses dwell_samples=256, so expected = 256×2 = 512.
    // In production (dwell_samples=131072) the snapshot is always 2048.
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    Detection captured{};
    std::atomic<bool> got{false};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source,
        [&](const Detection& d){ if (!got.exchange(true)) captured = d; });
    scanner.start();
    waitFor([&]{ return got.load(); }, 500);
    scanner.stop();

    ASSERT_TRUE(got.load()) << "No detection received within timeout";
    // Must be non-empty, even, and at most 2048 (1024 complex samples)
    ASSERT_TRUE(captured.iq_snapshot && !captured.iq_snapshot->empty())
        << "IQ snapshot must be present on every confirmed detection";
    EXPECT_EQ(captured.iq_snapshot->size() % 2, 0u)
        << "Snapshot size must be even (interleaved I,Q pairs)";
    EXPECT_LE(captured.iq_snapshot->size(), 2048u)
        << "Snapshot must not exceed 1 024 complex samples";
    EXPECT_GT(captured.snapshot_sample_rate_sps, 0.0)
        << "snapshot_sample_rate_sps must be positive";
}

TEST_F(SpectrumScannerTest, SnapshotSampleRateMatchesConfig) {
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    Detection captured{};
    std::atomic<bool> got{false};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source,
        [&](const Detection& d){ if (!got.exchange(true)) captured = d; });
    scanner.start();
    waitFor([&]{ return got.load(); }, 500);
    scanner.stop();

    ASSERT_TRUE(got.load());
    // makeTestConfig sets sample_rate = 10 MHz
    EXPECT_NEAR(captured.snapshot_sample_rate_sps, 10e6, 1.0)
        << "snapshot_sample_rate_sps must match the configured device sample rate";
}

TEST_F(SpectrumScannerTest, SnapshotContainsNonZeroSamples) {
    // The snapshot must hold actual IQ data, not a zeroed buffer.
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    Detection captured{};
    std::atomic<bool> got{false};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source,
        [&](const Detection& d){ if (!got.exchange(true)) captured = d; });
    scanner.start();
    waitFor([&]{ return got.load(); }, 500);
    scanner.stop();

    ASSERT_TRUE(got.load());
    ASSERT_TRUE(captured.iq_snapshot && !captured.iq_snapshot->empty());
    float rms = 0.f;
    for (float v : *captured.iq_snapshot) rms += v * v;
    rms = std::sqrt(rms / (float)captured.iq_snapshot->size());
    EXPECT_GT(rms, 1e-6f) << "Snapshot should contain non-zero signal samples";
}

// ── Multi-channel ─────────────────────────────────────────────────────────────

TEST_F(SpectrumScannerTest, TwoChannels_StartStop) {
    SoapyIqSource source(makeTestConfig(/*shared_lo=*/false, /*rx_channels=*/2));
    SpectrumScanner scanner(makeTestConfig(false, 2), &source, [](const Detection&){});
    ASSERT_NO_THROW(scanner.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_NO_THROW(scanner.stop());
}

TEST_F(SpectrumScannerTest, TwoChannels_ToneDetectedOnChannel0) {
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    std::atomic<int> count{0};
    SoapyIqSource source(makeTestConfig(false, 2));
    SpectrumScanner scanner(makeTestConfig(false, 2),
        &source, [&count](const Detection&){ count++; });
    scanner.start();
    bool fired = waitFor([&]{ return count.load() > 0; }, 500);
    scanner.stop();

    EXPECT_TRUE(fired) << "Tone should be detected on channel 0 even with 2 channels open";
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST_F(SpectrumScannerTest, NoDetectionsAfterStop) {
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    std::atomic<int> count{0};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(),
        &source, [&count](const Detection&){ count++; });
    scanner.start();

    // Wait until at least one detection fires to confirm the scanner is active.
    waitFor([&]{ return count.load() > 0; }, 500);
    scanner.stop();

    int snapshot = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(count.load(), snapshot)
        << "Callback must not fire after stop() returns";
}

TEST_F(SpectrumScannerTest, ZeroInputNoCallbackFired) {
    FakeAcqSoapy::tone_enabled.store(false);

    std::atomic<bool> fired{false};
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(),
        &source, [&fired](const Detection&){ fired = true; });
    scanner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    scanner.stop();

    EXPECT_FALSE(fired.load()) << "All-zero IQ should never trigger the detection callback";
}

TEST_F(SpectrumScannerTest, DetectionCenterFreqIsInSweepRange) {
    FakeAcqSoapy::tone_enabled.store(true);
    FakeAcqSoapy::tone_freq_frac.store(0.25f);
    FakeAcqSoapy::tone_amplitude.store(1.0f);

    std::vector<Detection> detections;
    std::mutex mu;
    SoapyIqSource source(makeTestConfig());
    SpectrumScanner scanner(makeTestConfig(), &source,
        [&](const Detection& d){
            std::lock_guard lk(mu);
            detections.push_back(d);
        });
    scanner.start();
    waitFor([&]{ std::lock_guard lk(mu); return !detections.empty(); }, 500);
    scanner.stop();

    std::lock_guard lk(mu);
    ASSERT_FALSE(detections.empty());
    for (const auto& d : detections) {
        // Allow half-bin margin (~40 kHz) for quantization at FFT bin edges
        EXPECT_GE(d.center_freq_hz,  99'960'000ULL);
        EXPECT_LE(d.center_freq_hz, 108'000'000ULL);
    }
}
