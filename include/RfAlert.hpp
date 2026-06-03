#pragma once
/**
 * @file RfAlert.hpp
 * @brief RF_ALERT message type published to rf.alerts AMQP topic.
 *
 * Published by:
 *   - GpsMonitor (jamming / spoofing at L1/L2/L5)
 *   - DemodApp validators (ADS-B / AIS / EAS / DSC / P25 anomalies)
 */
#include <string>
#include <cstdint>

namespace acq {

enum class AlertSeverity { LOW, MEDIUM, HIGH, CRITICAL };
enum class AlertType {
    GPS_JAMMING,      ///< Broadband noise at GPS frequency above baseline
    GPS_SPOOFING,     ///< DSSS-shaped signal at L1/L2/L5 at detectable power
    ADSB_SPOOFING,    ///< ADS-B frame with impossible aircraft physics
    ADSB_JAMMING,     ///< High CRC failure rate on 1090 MHz
    AIS_SPOOFING,     ///< AIS frame with impossible vessel physics / MMSI conflict
    EAS_SPOOFING,     ///< EAS/SAME alert with invalid FIPS or suspicious origin
    DSC_SPOOFING,     ///< DSC distress call with anomalous MMSI / position
    P25_ROGUE_SITE,   ///< P25 control channel with unknown WACN or mismatched site info
};

inline const char* alertTypeName(AlertType t) {
    switch (t) {
        case AlertType::GPS_JAMMING:    return "GPS_JAMMING";
        case AlertType::GPS_SPOOFING:   return "GPS_SPOOFING";
        case AlertType::ADSB_SPOOFING:  return "ADSB_SPOOFING";
        case AlertType::ADSB_JAMMING:   return "ADSB_JAMMING";
        case AlertType::AIS_SPOOFING:   return "AIS_SPOOFING";
        case AlertType::EAS_SPOOFING:   return "EAS_SPOOFING";
        case AlertType::DSC_SPOOFING:   return "DSC_SPOOFING";
        case AlertType::P25_ROGUE_SITE: return "P25_ROGUE_SITE";
    }
    return "UNKNOWN";
}

inline const char* severityName(AlertSeverity s) {
    switch (s) {
        case AlertSeverity::LOW:      return "LOW";
        case AlertSeverity::MEDIUM:   return "MEDIUM";
        case AlertSeverity::HIGH:     return "HIGH";
        case AlertSeverity::CRITICAL: return "CRITICAL";
    }
    return "LOW";
}

struct RfAlert {
    AlertType     type;
    AlertSeverity severity;
    double        freq_hz{0.0};     ///< Centre frequency of the threat (0 if N/A)
    float         power_db{0.0f};   ///< Measured power at threat frequency
    float         baseline_db{0.0f};///< Expected/baseline power (for jamming delta)
    std::string   details;          ///< Human-readable detail string
    int64_t       timestamp_ms{0};  ///< Unix epoch ms
    std::string   scanner_id;
};

} // namespace acq
