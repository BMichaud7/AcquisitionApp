#include <gtest/gtest.h>
#include "SweepConfig.hpp"
#include <au/units/hertz.hh>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace acq;

// Write XML to a temp file; caller owns the path and must remove it.
static std::string writeTmpXml(const std::string& xml) {
    char path[] = "/tmp/acq_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    ::write(fd, xml.c_str(), xml.size());
    ::close(fd);
    return path;
}

struct SweepConfigTest : ::testing::Test {
    std::string tmp_path;
    void TearDown() override {
        if (!tmp_path.empty()) std::remove(tmp_path.c_str());
    }
    SweepConfig parse(const std::string& xml) {
        tmp_path = writeTmpXml(xml);
        return SweepConfig::from_file(tmp_path);
    }
};

// ── Minimal valid XML ─────────────────────────────────────────────────────────

TEST_F(SweepConfigTest, ParsesMinimalRequiredElements) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device>
            <driver>rtlsdr</driver>
            <uri>serial=00000001</uri>
          </device>
          <sweep>
            <start_hz>70000000</start_hz>
            <stop_hz>1000000000</stop_hz>
          </sweep>
        </sdr_acquisition>)");

    EXPECT_DOUBLE_EQ(cfg.sweep.start_hz.in(au::hertz), 70'000'000.0);
    EXPECT_DOUBLE_EQ(cfg.sweep.stop_hz.in(au::hertz), 1'000'000'000.0);
}

TEST_F(SweepConfigTest, DefaultsAppliedWhenOptionalElementsMissing) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep>
            <start_hz>100000000</start_hz>
            <stop_hz>200000000</stop_hz>
          </sweep>
        </sdr_acquisition>)");

    // Check key defaults from SweepParams
    EXPECT_EQ(cfg.device.rx_channels,    1);
    EXPECT_DOUBLE_EQ(cfg.device.sample_rate.in(au::hertz), 10e6);
    EXPECT_DOUBLE_EQ(cfg.device.rx_gain_db,  40.0);
    EXPECT_EQ(cfg.sweep.fft_size,         4096);
    EXPECT_DOUBLE_EQ(cfg.sweep.threshold_db, 10.0);
    EXPECT_EQ(cfg.sweep.settle_samples,   512);
    EXPECT_EQ(cfg.scanner_id,             "scanner-0");
    EXPECT_EQ(cfg.amqp.detection_topic,   "rf.detections");
}

// ── Full config ───────────────────────────────────────────────────────────────

TEST_F(SweepConfigTest, ParsesAllOptionalFields) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <scanner_id>scanner-test</scanner_id>
          <amqp>
            <url>amqp://broker:5672</url>
            <username>user</username>
            <password>pass</password>
            <detection_topic>rf.test</detection_topic>
            <reconnect_interval_sec>10</reconnect_interval_sec>
          </amqp>
          <database>
            <host>db-host</host>
            <port>5433</port>
            <name>mydb</name>
            <user>dbuser</user>
            <password>dbpass</password>
          </database>
          <device>
            <driver>remote</driver>
            <uri>192.168.1.100</uri>
            <rx_channels>2</rx_channels>
            <sample_rate_sps>20000000</sample_rate_sps>
            <rx_gain_db>50</rx_gain_db>
            <shared_lo>true</shared_lo>
          </device>
          <sweep>
            <start_hz>88000000</start_hz>
            <stop_hz>108000000</stop_hz>
            <fft_size>512</fft_size>
            <dwell_samples>1024</dwell_samples>
            <usable_bw_fraction>0.75</usable_bw_fraction>
            <threshold_db>8.0</threshold_db>
            <min_signal_bw_hz>5000</min_signal_bw_hz>
            <settle_samples>256</settle_samples>
          </sweep>
        </sdr_acquisition>)");

    EXPECT_EQ(cfg.scanner_id, "scanner-test");

    EXPECT_EQ(cfg.amqp.url,               "amqp://broker:5672");
    EXPECT_EQ(cfg.amqp.username,           "user");
    EXPECT_EQ(cfg.amqp.password,           "pass");
    EXPECT_EQ(cfg.amqp.detection_topic,    "rf.test");
    EXPECT_EQ(cfg.amqp.reconnect_interval_sec, 10);

    EXPECT_EQ(cfg.db.host,     "db-host");
    EXPECT_EQ(cfg.db.port,     5433);
    EXPECT_EQ(cfg.db.name,     "mydb");
    EXPECT_EQ(cfg.db.user,     "dbuser");
    EXPECT_EQ(cfg.db.password, "dbpass");

    EXPECT_EQ(cfg.device.rx_channels, 2);
    EXPECT_DOUBLE_EQ(cfg.device.sample_rate.in(au::hertz), 20e6);
    EXPECT_DOUBLE_EQ(cfg.device.rx_gain_db,  50.0);

    EXPECT_DOUBLE_EQ(cfg.sweep.start_hz.in(au::hertz),  88'000'000.0);
    EXPECT_DOUBLE_EQ(cfg.sweep.stop_hz.in(au::hertz),  108'000'000.0);
    EXPECT_EQ(cfg.sweep.fft_size,            512);
    EXPECT_EQ(cfg.sweep.dwell_samples,       1024);
    EXPECT_DOUBLE_EQ(cfg.sweep.usable_bw_fraction, 0.75);
    EXPECT_DOUBLE_EQ(cfg.sweep.threshold_db,  8.0);
    EXPECT_DOUBLE_EQ(cfg.sweep.min_signal_bw_hz.in(au::hertz), 5000.0);
    EXPECT_EQ(cfg.sweep.settle_samples,      256);
}

// ── Validation errors ─────────────────────────────────────────────────────────

TEST_F(SweepConfigTest, MissingDeviceElementThrows) {
    EXPECT_THROW(parse(R"(
        <sdr_acquisition>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)"),
        std::runtime_error);
}

TEST_F(SweepConfigTest, MissingSweepElementThrows) {
    EXPECT_THROW(parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
        </sdr_acquisition>)"),
        std::runtime_error);
}

TEST_F(SweepConfigTest, StopHzLessThanStartHzThrows) {
    EXPECT_THROW(parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>500000000</start_hz><stop_hz>100000000</stop_hz></sweep>
        </sdr_acquisition>)"),
        std::runtime_error);
}

TEST_F(SweepConfigTest, StopHzEqualToStartHzThrows) {
    EXPECT_THROW(parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>100000000</stop_hz></sweep>
        </sdr_acquisition>)"),
        std::runtime_error);
}

TEST_F(SweepConfigTest, NonexistentFileThrows) {
    EXPECT_THROW(SweepConfig::from_file("/tmp/no_such_file_acq_test.xml"),
        std::runtime_error);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST_F(SweepConfigTest, DwellSamplesClampedToFftSize) {
    // dwell_samples < fft_size → clamped to fft_size
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep>
            <start_hz>100000000</start_hz>
            <stop_hz>200000000</stop_hz>
            <fft_size>256</fft_size>
            <dwell_samples>64</dwell_samples>
          </sweep>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.sweep.dwell_samples, 256);
}

TEST_F(SweepConfigTest, RxChannelsClampedToAtLeastOne) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device>
            <driver>rtlsdr</driver>
            <uri></uri>
            <rx_channels>0</rx_channels>
          </device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_GE(cfg.device.rx_channels, 1);
}

// ── DbConfig::connection_string ───────────────────────────────────────────────

TEST(DbConfig, ConnectionStringFormat) {
    DbConfig db;
    db.host     = "myhost";
    db.port     = 5432;
    db.name     = "mydb";
    db.user     = "alice";
    db.password = "secret";
    std::string cs = db.connection_string();
    EXPECT_NE(cs.find("myhost"),  std::string::npos);
    EXPECT_NE(cs.find("5432"),    std::string::npos);
    EXPECT_NE(cs.find("mydb"),    std::string::npos);
    EXPECT_NE(cs.find("alice"),   std::string::npos);
    EXPECT_NE(cs.find("secret"),  std::string::npos);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST_F(SweepConfigTest, AmqpDetectionTopicDefaultIsRfDetections) {
    // When the <amqp> section is absent, detection_topic must default.
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri>x</uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.amqp.detection_topic, "rf.detections");
}

TEST_F(SweepConfigTest, DeviceBandwidthHzDefaultIsPositive) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><rx_channels>1</rx_channels></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_GT(cfg.device.bandwidth_hz.in(au::hertz), 0.0);
}

TEST_F(SweepConfigTest, UsableBwFractionDefaultInValidRange) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_GT(cfg.sweep.usable_bw_fraction, 0.0);
    EXPECT_LE(cfg.sweep.usable_bw_fraction, 1.0);
}

TEST_F(SweepConfigTest, FftSizeDefaultIsPowerOfTwo) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    int fs = cfg.sweep.fft_size;
    EXPECT_GT(fs, 0);
    EXPECT_EQ(fs & (fs - 1), 0) << "fft_size must be a power of two";
}

TEST_F(SweepConfigTest, ExplicitScannerIdOverridesDefault) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <scanner_id>my-scanner</scanner_id>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.scanner_id, "my-scanner");
}

TEST_F(SweepConfigTest, RankDefaultsToOne) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.rank, 1);
}

TEST_F(SweepConfigTest, ExplicitRankIsLoaded) {
    auto cfg = parse(R"(
        <sdr_acquisition>
          <rank>3</rank>
          <device><driver>rtlsdr</driver><uri></uri></device>
          <sweep><start_hz>100000000</start_hz><stop_hz>200000000</stop_hz></sweep>
        </sdr_acquisition>)");
    EXPECT_EQ(cfg.rank, 3);
}
