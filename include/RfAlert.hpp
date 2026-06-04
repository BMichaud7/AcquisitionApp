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
 * @file RfAlert.hpp
 * @brief RF threat alert type shared between AcquisitionApp and DemodApp.
 *
 * RfAlert is the canonical record written to the @c rf_alerts PostgreSQL table
 * whenever a threat is detected.  Two components produce alerts:
 *
 *  - **GpsMonitor** (AcquisitionApp) — scans GPS L1/L2/L5 frequencies and
 *    fires GPS_JAMMING or GPS_SPOOFING based on received power and spectral shape.
 *  - **DemodApp validators** — each protocol demodulator (ADS-B, AIS, EAS, DSC,
 *    P25) checks content-layer anomalies and populates @c DemodResult::alert_json.
 *    DemodRouter forwards those alerts to AcquisitionApp's AlertConsumer via the
 *    @c rf.alerts AMQP topic, which then calls AlertStore::insert().
 *
 * @see AlertStore, AlertConsumer, GpsMonitor, ThreatConfig
 */
#include <string>
#include <cstdint>

namespace acq {

/**
 * @brief Severity level of a detected RF threat.
 *
 * Used both in the @c rf_alerts table's @c severity column and in log output.
 */
enum class AlertSeverity {
    LOW,      ///< Informational — low confidence or minor anomaly.
    MEDIUM,   ///< Moderate confidence — should be reviewed.
    HIGH,     ///< High confidence threat — operator action recommended.
    CRITICAL, ///< Confirmed or severe threat (e.g. GPS spoofing, fake Mayday).
};

/**
 * @brief Discriminates the source and nature of a detected RF threat.
 *
 * Each enumerator maps to a distinct detection algorithm and a named row in
 * the @c rf_alerts PostgreSQL table.
 */
enum class AlertType {
    GPS_JAMMING,    ///< Broadband noise or CW tone at L1/L2/L5 exceeds baseline by ≥ threshold.
    GPS_SPOOFING,   ///< BPSK-shaped DSSS signal (~2 MHz BW) at GPS frequency at detectable power.
                    ///<   Real GPS is always ~20 dB below the noise floor; any detectable signal is suspicious.
    ADSB_SPOOFING,  ///< ADS-B DF17/18 frame contains impossible aircraft parameters
                    ///<   (altitude > 60 000 ft, groundspeed > 700 kt).
    ADSB_JAMMING,   ///< CRC-24 failure rate > 80 % of Mode S candidates on 1090 MHz.
    AIS_SPOOFING,   ///< AIS type-1/2/3 position report with SOG > 50 kt or invalid MMSI.
    EAS_SPOOFING,   ///< EAS/SAME ZCZC header with invalid FCC originator code or rare national event.
    DSC_SPOOFING,   ///< DSC distress call (category 112) with anomalous or invalid MMSI structure.
    P25_ROGUE_SITE, ///< P25 RFSS_STATUS_BCAST WACN or SYS_ID changed mid-session on the same frequency.
};

/**
 * @brief Return the string representation of an AlertType (used in DB and JSON).
 * @param t Alert type enumerator.
 * @return Null-terminated C string (e.g. "GPS_JAMMING").
 */
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

/**
 * @brief Return the string representation of an AlertSeverity (used in DB and JSON).
 * @param s Severity enumerator.
 * @return Null-terminated C string (e.g. "CRITICAL").
 */
inline const char* severityName(AlertSeverity s) {
    switch (s) {
        case AlertSeverity::LOW:      return "LOW";
        case AlertSeverity::MEDIUM:   return "MEDIUM";
        case AlertSeverity::HIGH:     return "HIGH";
        case AlertSeverity::CRITICAL: return "CRITICAL";
    }
    return "LOW";
}

/**
 * @brief A single RF threat detection record.
 *
 * Persisted to the @c rf_alerts PostgreSQL table by AlertStore::insert().
 * Query recent alerts with:
 * @code{.sql}
 * SELECT * FROM recent_alerts;   -- last hour
 * @endcode
 */
struct RfAlert {
    AlertType     type;                   ///< Nature of the detected threat.
    AlertSeverity severity;               ///< Confidence / urgency level.
    double        freq_hz{0.0};          ///< Centre frequency of the threat in Hz (0 if not applicable).
    float         power_db{0.0f};        ///< Measured signal power at @c freq_hz (dBFS or dBm).
    float         baseline_db{0.0f};     ///< EMA noise baseline at @c freq_hz used for delta calculation.
    std::string   details;               ///< Human-readable description including numeric evidence.
    int64_t       timestamp_ms{0};       ///< UTC detection time in Unix milliseconds (0 = now()).
    std::string   scanner_id;            ///< Scanner identifier from SweepConfig::scanner_id.
};

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
