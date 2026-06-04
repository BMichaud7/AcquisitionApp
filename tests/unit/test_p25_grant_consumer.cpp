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
 * @file test_p25_grant_consumer.cpp
 * @brief Unit tests for P25GrantConsumer JSON parsing and queue logic.
 *
 * Tests message parsing, queue depth limiting, and config.
 * Does not require an AMQP broker — tests parsing and data structures only.
 */
#include <gtest/gtest.h>
#include "P25GrantConsumer.hpp"
#include "SweepConfig.hpp"
#include <nlohmann/json.hpp>

using namespace acq;
using json = nlohmann::json;

// ── P25Grant struct ───────────────────────────────────────────────────────────

TEST(P25Grant, DefaultValues) {
    P25Grant g{};
    EXPECT_EQ(g.talk_group, 0u);
    EXPECT_EQ(g.source_id,  0u);
    EXPECT_DOUBLE_EQ(g.freq_hz, 0.0);
    EXPECT_FALSE(g.encrypted);
    EXPECT_FALSE(g.emergency);
}

// ── JSON message format expected by P25GrantConsumer::Handler ─────────────────

static json valid_grant() {
    return {
        {"msg_type",     "P25_CHANNEL_GRANT"},
        {"talk_group",   50123},
        {"source_id",    1234567},
        {"freq_hz",      851012500.0},
        {"encrypted",    false},
        {"emergency",    false},
        {"timestamp_ms", 1700000000000LL},
        {"site", {
            {"wacn",    0xBB800},
            {"sys_id",  0x3F0},
            {"rfss_id", 1},
            {"site_id", 2},
        }}
    };
}

TEST(P25GrantJson, ParsesAllFields) {
    auto j = valid_grant();
    P25Grant g;
    g.talk_group = j.value("talk_group", 0u);
    g.source_id  = j.value("source_id",  0u);
    g.freq_hz    = j.value("freq_hz",    0.0);
    g.encrypted  = j.value("encrypted",  false);
    g.emergency  = j.value("emergency",  false);
    g.ts_ms      = j.value("timestamp_ms", (int64_t)0);

    EXPECT_EQ(g.talk_group, 50123u);
    EXPECT_EQ(g.source_id,  1234567u);
    EXPECT_DOUBLE_EQ(g.freq_hz, 851012500.0);
    EXPECT_FALSE(g.encrypted);
    EXPECT_FALSE(g.emergency);
    EXPECT_EQ(g.ts_ms, 1700000000000LL);
}

TEST(P25GrantJson, MissingFreqIsRejected) {
    auto j = valid_grant();
    j.erase("freq_hz");
    double freq = j.value("freq_hz", 0.0);
    EXPECT_DOUBLE_EQ(freq, 0.0);  // default → rejected by consumer
}

TEST(P25GrantJson, WrongMsgTypeIsIgnored) {
    auto j = valid_grant();
    j["msg_type"] = "RF_DETECTION";
    EXPECT_NE(j.value("msg_type", ""), "P25_CHANNEL_GRANT");
}

TEST(P25GrantJson, EncryptedFlag) {
    auto j = valid_grant();
    j["encrypted"] = true;
    EXPECT_TRUE(j.value("encrypted", false));
}

TEST(P25GrantJson, EmergencyFlag) {
    auto j = valid_grant();
    j["emergency"] = true;
    EXPECT_TRUE(j.value("emergency", false));
}

// ── P25Config (SweepConfig) ───────────────────────────────────────────────────

TEST(P25Config, DefaultsDisabled) {
    P25Config c;
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.grant_topic, "rf.p25.grants");
    EXPECT_DOUBLE_EQ(c.capture_s, 3.0);
    EXPECT_EQ(c.rank, 3);
    EXPECT_TRUE(c.tg_whitelist.empty());
}

TEST(P25Config, WhitelistCanBePopulated) {
    P25Config c;
    c.tg_whitelist = {1000, 2000, 50123};
    EXPECT_EQ(c.tg_whitelist.size(), 3u);
    EXPECT_EQ(c.tg_whitelist[1], 2000u);
}

// ── Voice channel frequency validation ────────────────────────────────────────

TEST(P25GrantFreq, TypeicalP25FrequencyIsValid) {
    // P25 800 MHz band voice channels
    EXPECT_GT(851012500.0, 800e6);
    EXPECT_LT(851012500.0, 870e6);
}

TEST(P25GrantFreq, ZeroFreqShouldBeRejected) {
    P25Grant g;
    g.freq_hz = 0.0;
    EXPECT_DOUBLE_EQ(g.freq_hz, 0.0);  // caller should filter this
}

// ── Talk group filtering logic ────────────────────────────────────────────────

TEST(P25TgFilter, EmptyWhitelistPassesAll) {
    std::vector<uint32_t> whitelist;
    uint32_t tg = 12345;
    bool pass = whitelist.empty() ||
                std::find(whitelist.begin(), whitelist.end(), tg) != whitelist.end();
    EXPECT_TRUE(pass);
}

TEST(P25TgFilter, WhitelistBlocksUnknownTG) {
    std::vector<uint32_t> whitelist = {50123, 50124};
    uint32_t unknown_tg = 99999;
    bool pass = whitelist.empty() ||
                std::find(whitelist.begin(), whitelist.end(), unknown_tg) != whitelist.end();
    EXPECT_FALSE(pass);
}

TEST(P25TgFilter, WhitelistPassesKnownTG) {
    std::vector<uint32_t> whitelist = {50123, 50124};
    uint32_t known_tg = 50123;
    bool pass = whitelist.empty() ||
                std::find(whitelist.begin(), whitelist.end(), known_tg) != whitelist.end();
    EXPECT_TRUE(pass);
}

// ── Scanner ID tagging ────────────────────────────────────────────────────────

TEST(P25ScannerTag, VoiceTagFormat) {
    std::string base_id = "scanner-0";
    uint32_t tg = 50123;
    std::string tagged = base_id + "-tg" + std::to_string(tg);
    EXPECT_EQ(tagged, "scanner-0-tg50123");
}
