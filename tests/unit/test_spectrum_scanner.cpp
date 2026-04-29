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
    cfg.sweep.stop_hz             = 110'000'000;   // 110 MHz — one dwell step
    cfg.sweep.dwell_samples       = 256;
    cfg.sweep.fft_size            = 256;
    cfg.sweep.usable_bw_fraction  = 0.80;
    cfg.sweep.threshold_db        = 5.0;
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
    EXPECT_LT(fc, 110e6);
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
