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
/**
 * @file test_multi_device.cpp
 * @brief Tests for SweepConfig::splitBands() and <bands> XML parsing.
 *
 * Covers the auto-split logic that distributes N SDR devices across M
 * configured frequency bands so every device scans a unique non-overlapping
 * frequency range. Also verifies the XML parser correctly reads <bands>.
 */
#include <gtest/gtest.h>
#include "SweepConfig.hpp"
#include <au/units/hertz.hh>
#include <cstdio>
#include <unistd.h>
#include <numeric>

using namespace acq;

// ── Helpers ───────────────────────────────────────────────────────────────────

static BandConfig makeBand(double lo_hz, double hi_hz, std::string dev = "") {
    BandConfig b;
    b.start_hz = au::hertz(lo_hz);
    b.stop_hz  = au::hertz(hi_hz);
    b.device_id = std::move(dev);
    return b;
}

/// Verify the split result covers the same total span as the input bands
/// with no gaps or overlaps (each slice stop == next slice start).
static void assertContiguous(const std::vector<BandConfig>& slices) {
    for (size_t i = 1; i < slices.size(); ++i) {
        EXPECT_DOUBLE_EQ(slices[i].start_hz.in(au::hertz),
                         slices[i-1].stop_hz.in(au::hertz))
            << "Gap or overlap between slice " << i-1 << " and " << i;
    }
}

static void assertAllPositiveWidth(const std::vector<BandConfig>& slices) {
    for (size_t i = 0; i < slices.size(); ++i) {
        EXPECT_GT(slices[i].stop_hz.in(au::hertz),
                  slices[i].start_hz.in(au::hertz))
            << "Slice " << i << " has zero or negative width";
    }
}

static std::string writeTmpXml(const std::string& xml) {
    char path[] = "/tmp/acq_mdev_XXXXXX";
    int fd = mkstemp(path);
    ::write(fd, xml.c_str(), xml.size());
    ::close(fd);
    return path;
}

// ── No-split cases ────────────────────────────────────────────────────────────

TEST(SplitBands, NoSplitWhenDevicesEqualBands) {
    auto bands = { makeBand(70e6, 500e6), makeBand(500e6, 1100e6),
                   makeBand(1100e6, 3e9), makeBand(3e9, 6e9) };
    auto result = SweepConfig::splitBands(
        std::vector<BandConfig>(bands.begin(), bands.end()), 4);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[3].stop_hz.in(au::hertz),  6e9);
}

TEST(SplitBands, NoSplitWhenDevicesLessThanBands) {
    auto bands = { makeBand(70e6, 500e6), makeBand(500e6, 1100e6),
                   makeBand(1100e6, 3e9), makeBand(3e9, 6e9) };
    auto result = SweepConfig::splitBands(
        std::vector<BandConfig>(bands.begin(), bands.end()), 2);
    ASSERT_EQ(result.size(), 4u);  // unchanged
}

TEST(SplitBands, NoSplitWithOneDeviceOneBand) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 1);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[0].stop_hz.in(au::hertz),  6e9);
}

// ── 1 band → N slices ─────────────────────────────────────────────────────────

TEST(SplitBands, OneBandTwoDevices) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 2);
    ASSERT_EQ(result.size(), 2u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[1].stop_hz.in(au::hertz),  6e9);
    // Each slice is half the total span
    double half = (6e9 - 70e6) / 2.0;
    EXPECT_DOUBLE_EQ(result[0].stop_hz.in(au::hertz) - result[0].start_hz.in(au::hertz), half);
    EXPECT_DOUBLE_EQ(result[1].stop_hz.in(au::hertz) - result[1].start_hz.in(au::hertz), half);
}

TEST(SplitBands, OneBandFourDevices) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 4);
    ASSERT_EQ(result.size(), 4u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[3].stop_hz.in(au::hertz),  6e9);
    double quarter = (6e9 - 70e6) / 4.0;
    for (const auto& s : result)
        EXPECT_NEAR(s.stop_hz.in(au::hertz) - s.start_hz.in(au::hertz),
                    quarter, 1.0);  // allow 1 Hz float rounding
}

TEST(SplitBands, OneBandTenDevices) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 10);
    ASSERT_EQ(result.size(), 10u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[9].stop_hz.in(au::hertz),  6e9);
}

TEST(SplitBands, OneBandHundredDevices) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 100);
    ASSERT_EQ(result.size(), 100u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz),  70e6);
    EXPECT_DOUBLE_EQ(result[99].stop_hz.in(au::hertz),   6e9);
}

// ── 2 bands → N slices ────────────────────────────────────────────────────────

TEST(SplitBands, TwoBandsFourDevices) {
    // 4 devices / 2 bands = 2 slices each
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 1100e6), makeBand(1100e6, 6e9) }, 4);
    ASSERT_EQ(result.size(), 4u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[3].stop_hz.in(au::hertz),  6e9);
}

TEST(SplitBands, TwoBandsTenDevices) {
    // 10 devices / 2 bands = 5 slices each = 10 total
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 1100e6), makeBand(1100e6, 6e9) }, 10);
    ASSERT_EQ(result.size(), 10u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
}

// ── Remainder distribution ────────────────────────────────────────────────────

TEST(SplitBands, ThreeBandsFourDevicesRemainder) {
    // 4 / 3 = base 1, remainder 1 → band0 gets 2 slices, band1 and band2 get 1
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 500e6), makeBand(500e6, 1100e6), makeBand(1100e6, 6e9) }, 4);
    ASSERT_EQ(result.size(), 4u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    // band0 [70–500 MHz] split into 2: midpoint at 285 MHz
    EXPECT_DOUBLE_EQ(result[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result[0].stop_hz.in(au::hertz),  (70e6 + 500e6) / 2.0);
    EXPECT_DOUBLE_EQ(result[1].start_hz.in(au::hertz), (70e6 + 500e6) / 2.0);
    EXPECT_DOUBLE_EQ(result[1].stop_hz.in(au::hertz),  500e6);
    // band1 and band2 unchanged
    EXPECT_DOUBLE_EQ(result[2].start_hz.in(au::hertz), 500e6);
    EXPECT_DOUBLE_EQ(result[2].stop_hz.in(au::hertz),  1100e6);
    EXPECT_DOUBLE_EQ(result[3].start_hz.in(au::hertz), 1100e6);
    EXPECT_DOUBLE_EQ(result[3].stop_hz.in(au::hertz),  6e9);
}

TEST(SplitBands, FiveBandsTenDevicesEvenSplit) {
    // 10 / 5 = 2 slices each, no remainder
    std::vector<BandConfig> bands;
    for (int i = 0; i < 5; ++i)
        bands.push_back(makeBand(i * 1e9, (i + 1) * 1e9));
    auto result = SweepConfig::splitBands(bands, 10);
    ASSERT_EQ(result.size(), 10u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
}

TEST(SplitBands, TwoBandsThreeDevicesRemainder) {
    // 3 / 2 = base 1, remainder 1 → band0 gets 2, band1 gets 1
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 1100e6), makeBand(1100e6, 6e9) }, 3);
    ASSERT_EQ(result.size(), 3u);
    assertContiguous(result);
    assertAllPositiveWidth(result);
    EXPECT_DOUBLE_EQ(result[2].stop_hz.in(au::hertz), 6e9);
}

// ── device_id inheritance ─────────────────────────────────────────────────────

TEST(SplitBands, DeviceIdInheritedToAllSlices) {
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 6e9, "pluto-0") }, 4);
    ASSERT_EQ(result.size(), 4u);
    for (const auto& s : result)
        EXPECT_EQ(s.device_id, "pluto-0");
}

TEST(SplitBands, EmptyDeviceIdRemainsEmpty) {
    auto result = SweepConfig::splitBands({ makeBand(70e6, 6e9) }, 4);
    for (const auto& s : result)
        EXPECT_TRUE(s.device_id.empty());
}

TEST(SplitBands, MixedDeviceIdsPreservedPerBand) {
    auto result = SweepConfig::splitBands(
        { makeBand(70e6, 1100e6, "pluto-0"), makeBand(1100e6, 6e9, "pluto-1") }, 4);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0].device_id, "pluto-0");
    EXPECT_EQ(result[1].device_id, "pluto-0");
    EXPECT_EQ(result[2].device_id, "pluto-1");
    EXPECT_EQ(result[3].device_id, "pluto-1");
}

// ── Invariants at scale ───────────────────────────────────────────────────────

TEST(SplitBands, TotalCoveragePreservedAt100Devices) {
    // Regardless of how many slices, first.start and last.stop must match input.
    std::vector<BandConfig> bands = {
        makeBand(70e6, 500e6), makeBand(500e6, 1100e6)
    };
    auto result = SweepConfig::splitBands(bands, 100);
    ASSERT_EQ(result.size(), 100u);
    EXPECT_DOUBLE_EQ(result.front().start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(result.back().stop_hz.in(au::hertz),   1100e6);
    assertContiguous(result);
    assertAllPositiveWidth(result);
}

TEST(SplitBands, AllSlicesHaveUniqueNonOverlappingRanges) {
    auto result = SweepConfig::splitBands({ makeBand(0, 1e9) }, 50);
    ASSERT_EQ(result.size(), 50u);
    for (size_t i = 0; i < result.size(); ++i)
        for (size_t j = i + 1; j < result.size(); ++j) {
            double lo_i = result[i].start_hz.in(au::hertz);
            double hi_i = result[i].stop_hz.in(au::hertz);
            double lo_j = result[j].start_hz.in(au::hertz);
            double hi_j = result[j].stop_hz.in(au::hertz);
            bool overlap = lo_i < hi_j && lo_j < hi_i;
            EXPECT_FALSE(overlap) << "Slices " << i << " and " << j << " overlap";
        }
}

// ── XML parsing — <bands> block ───────────────────────────────────────────────

struct BandParseTest : ::testing::Test {
    std::string tmp;
    void TearDown() override { if (!tmp.empty()) std::remove(tmp.c_str()); }
    SweepConfig parse(const std::string& xml) {
        tmp = writeTmpXml(xml);
        return SweepConfig::from_file(tmp);
    }
};

TEST_F(BandParseTest, ParsesOneBand) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
          <bands>
            <band>
              <start_hz>70000000</start_hz>
              <stop_hz>1100000000</stop_hz>
            </band>
          </bands>
        </sdr_acquisition>)");
    ASSERT_EQ(cfg.bands.size(), 1u);
    EXPECT_DOUBLE_EQ(cfg.bands[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(cfg.bands[0].stop_hz.in(au::hertz),  1100e6);
    EXPECT_TRUE(cfg.bands[0].device_id.empty());
}

TEST_F(BandParseTest, ParsesFourBands) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
          <bands>
            <band><start_hz>70000000</start_hz><stop_hz>500000000</stop_hz></band>
            <band><start_hz>500000000</start_hz><stop_hz>1100000000</stop_hz></band>
            <band><start_hz>1100000000</start_hz><stop_hz>3000000000</stop_hz></band>
            <band><start_hz>3000000000</start_hz><stop_hz>6000000000</stop_hz></band>
          </bands>
        </sdr_acquisition>)");
    ASSERT_EQ(cfg.bands.size(), 4u);
    EXPECT_DOUBLE_EQ(cfg.bands[0].start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(cfg.bands[1].start_hz.in(au::hertz), 500e6);
    EXPECT_DOUBLE_EQ(cfg.bands[2].start_hz.in(au::hertz), 1100e6);
    EXPECT_DOUBLE_EQ(cfg.bands[3].stop_hz.in(au::hertz),  6e9);
}

TEST_F(BandParseTest, ParsesBandWithPinnedDevice) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
          <bands>
            <band>
              <start_hz>1080000000</start_hz>
              <stop_hz>1100000000</stop_hz>
              <device>pluto-0</device>
            </band>
          </bands>
        </sdr_acquisition>)");
    ASSERT_EQ(cfg.bands.size(), 1u);
    EXPECT_EQ(cfg.bands[0].device_id, "pluto-0");
}

TEST_F(BandParseTest, BandStopLessThanStartThrows) {
    EXPECT_THROW(parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
          <bands>
            <band><start_hz>1100000000</start_hz><stop_hz>70000000</stop_hz></band>
          </bands>
        </sdr_acquisition>)"),
        std::runtime_error);
}

TEST_F(BandParseTest, NoBandsElementLeavesVectorEmpty) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_TRUE(cfg.bands.empty());
}

TEST_F(BandParseTest, BandInheritsDefaultSweepParamsAfterParse) {
    // The <sweep> block sets fft_size=512; bands should be available alongside.
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep>
            <start_hz>70000000</start_hz>
            <stop_hz>6000000000</stop_hz>
            <fft_size>512</fft_size>
          </sweep>
          <bands>
            <band><start_hz>70000000</start_hz><stop_hz>500000000</stop_hz></band>
          </bands>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.sweep.fft_size, 512);
    ASSERT_EQ(cfg.bands.size(), 1u);
    EXPECT_DOUBLE_EQ(cfg.bands[0].start_hz.in(au::hertz), 70e6);
}

TEST_F(BandParseTest, TenBandsParsedCorrectly) {
    std::string xml = R"(<sdr_acquisition>
      <device><rx_channels>1</rx_channels></device>
      <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
      <bands>)";
    for (int i = 0; i < 10; ++i)
        xml += "<band><start_hz>" + std::to_string(i * 600000000LL) +
               "</start_hz><stop_hz>" + std::to_string((i+1) * 600000000LL) +
               "</stop_hz></band>\n";
    xml += "</bands></sdr_acquisition>";
    auto cfg = parse(xml);
    ASSERT_EQ(cfg.bands.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_DOUBLE_EQ(cfg.bands[i].start_hz.in(au::hertz), i * 600e6);
        EXPECT_DOUBLE_EQ(cfg.bands[i].stop_hz.in(au::hertz),  (i+1) * 600e6);
    }
}

// ── splitBands + round-trip: parse then split ─────────────────────────────────

TEST_F(BandParseTest, ParseThenSplitTwoBandsFourDevices) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>70000000</start_hz><stop_hz>6000000000</stop_hz></sweep>
          <bands>
            <band><start_hz>70000000</start_hz><stop_hz>1100000000</stop_hz></band>
            <band><start_hz>1100000000</start_hz><stop_hz>6000000000</stop_hz></band>
          </bands>
        </sdr_acquisition>)");
    ASSERT_EQ(cfg.bands.size(), 2u);
    auto slices = SweepConfig::splitBands(cfg.bands, 4);
    ASSERT_EQ(slices.size(), 4u);
    assertContiguous(slices);
    assertAllPositiveWidth(slices);
    EXPECT_DOUBLE_EQ(slices.front().start_hz.in(au::hertz), 70e6);
    EXPECT_DOUBLE_EQ(slices.back().stop_hz.in(au::hertz),   6e9);
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
