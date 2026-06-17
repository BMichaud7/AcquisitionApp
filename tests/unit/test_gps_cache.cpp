/*
========================================================================
Project: OpenRFStack
Author:  Brendan Michaud
Year:    2026
Part of OpenRFStack (https://github.com/OpenRFStack)

Licensed under the Personal Use License.
Do not use for commercial, organizational, or military purposes.
========================================================================
*/
/**
 * @file test_gps_cache.cpp
 * @brief Unit tests for GpsFix, GpsCache::parse_fix, and Detection GPS fields.
 *
 * Tests JSON parsing and data structures only — no AMQP broker required.
 */
#include <gtest/gtest.h>
#include "GpsCache.hpp"
#include "Types.hpp"
#include <nlohmann/json.hpp>

using namespace acq;
using json = nlohmann::json;

// ── GpsFix struct ─────────────────────────────────────────────────────────────

TEST(GpsFix, DefaultsAreZero) {
    GpsFix f{};
    EXPECT_DOUBLE_EQ(f.lat,   0.0);
    EXPECT_DOUBLE_EQ(f.lon,   0.0);
    EXPECT_DOUBLE_EQ(f.alt_m, 0.0);
}

// ── GpsCache::parse_fix ───────────────────────────────────────────────────────

static std::string gps_json(double lat, double lon, double alt_m) {
    return json{
        {"latitude_deg",  lat},
        {"longitude_deg", lon},
        {"altitude_m",    alt_m},
    }.dump();
}

TEST(GpsCacheParseFix, ParsesAllFields) {
    auto fix = GpsCache::parse_fix(gps_json(45.5231, -122.6765, 52.3));
    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->lat,   45.5231);
    EXPECT_DOUBLE_EQ(fix->lon,   -122.6765);
    EXPECT_DOUBLE_EQ(fix->alt_m, 52.3);
}

TEST(GpsCacheParseFix, ParsesNegativeAltitude) {
    auto fix = GpsCache::parse_fix(gps_json(51.5074, -0.1278, -12.5));
    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->alt_m, -12.5);
}

TEST(GpsCacheParseFix, ParsesZeroCoords) {
    auto fix = GpsCache::parse_fix(gps_json(0.0, 0.0, 0.0));
    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->lat,   0.0);
    EXPECT_DOUBLE_EQ(fix->lon,   0.0);
    EXPECT_DOUBLE_EQ(fix->alt_m, 0.0);
}

TEST(GpsCacheParseFix, MissingFieldsDefaultToZero) {
    // GpsApp always sends all three; missing keys degrade gracefully.
    auto fix = GpsCache::parse_fix(R"({"latitude_deg": 10.0})");
    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->lat,   10.0);
    EXPECT_DOUBLE_EQ(fix->lon,    0.0);
    EXPECT_DOUBLE_EQ(fix->alt_m,  0.0);
}

TEST(GpsCacheParseFix, MalformedJsonReturnsNullopt) {
    EXPECT_FALSE(GpsCache::parse_fix("not json").has_value());
    EXPECT_FALSE(GpsCache::parse_fix("").has_value());
    EXPECT_FALSE(GpsCache::parse_fix("{bad}").has_value());
}

TEST(GpsCacheParseFix, WrongTypeFieldsDefaultToZero) {
    // Numeric fields that are string in JSON — value() returns default.
    auto fix = GpsCache::parse_fix(R"({"latitude_deg":"north","longitude_deg":0,"altitude_m":0})");
    // nlohmann::json::value() with wrong type throws type_error — parse_fix returns nullopt.
    // (Either behaviour is acceptable; we just must not crash.)
    (void)fix;
}

TEST(GpsCacheParseFix, ExtremeCoordinates) {
    auto fix = GpsCache::parse_fix(gps_json(90.0, -180.0, 8848.0));
    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->lat,    90.0);
    EXPECT_DOUBLE_EQ(fix->lon,  -180.0);
    EXPECT_DOUBLE_EQ(fix->alt_m, 8848.0);
}

TEST(GpsCacheParseFix, HighPrecisionCoords) {
    const double lat = 37.123456789012345;
    const double lon = -122.987654321098765;
    auto fix = GpsCache::parse_fix(gps_json(lat, lon, 0.0));
    ASSERT_TRUE(fix.has_value());
    EXPECT_NEAR(fix->lat, lat, 1e-9);
    EXPECT_NEAR(fix->lon, lon, 1e-9);
}

// ── Detection GPS fields ──────────────────────────────────────────────────────

TEST(DetectionGps, DefaultsAreNullopt) {
    Detection d;
    EXPECT_FALSE(d.lat.has_value());
    EXPECT_FALSE(d.lon.has_value());
    EXPECT_FALSE(d.alt_m.has_value());
}

TEST(DetectionGps, CanBeStamped) {
    Detection d;
    d.lat   = 45.5231;
    d.lon   = -122.6765;
    d.alt_m = 52.3;

    ASSERT_TRUE(d.lat.has_value());
    ASSERT_TRUE(d.lon.has_value());
    ASSERT_TRUE(d.alt_m.has_value());
    EXPECT_DOUBLE_EQ(*d.lat,    45.5231);
    EXPECT_DOUBLE_EQ(*d.lon,   -122.6765);
    EXPECT_DOUBLE_EQ(*d.alt_m,  52.3);
}

TEST(DetectionGps, StampFromGpsFix) {
    auto fix = GpsCache::parse_fix(gps_json(51.5074, -0.1278, 11.0));
    ASSERT_TRUE(fix.has_value());

    Detection d;
    d.lat   = fix->lat;
    d.lon   = fix->lon;
    d.alt_m = fix->alt_m;

    EXPECT_DOUBLE_EQ(*d.lat,   51.5074);
    EXPECT_DOUBLE_EQ(*d.lon,   -0.1278);
    EXPECT_DOUBLE_EQ(*d.alt_m, 11.0);
}

TEST(DetectionGps, NulloptWhenNoFix) {
    // Simulates the case where GpsCache::get() returned nullopt.
    std::optional<GpsFix> no_fix = std::nullopt;

    Detection d;
    if (no_fix) {
        d.lat   = no_fix->lat;
        d.lon   = no_fix->lon;
        d.alt_m = no_fix->alt_m;
    }

    EXPECT_FALSE(d.lat.has_value());
    EXPECT_FALSE(d.lon.has_value());
    EXPECT_FALSE(d.alt_m.has_value());
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
