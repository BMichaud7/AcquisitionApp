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
#include <vector>
/**
 * @file SweepConfig.hpp
 * @brief Configuration structs for AcquisitionApp, loaded from XML.
 *
 * SweepConfig::from_file() parses the scanner.xml config file.
 * All sub-structs (AmqpConfig, DeviceConfig, SweepParams, etc.) are
 * populated from the corresponding XML elements.
 */
#include <au/units/hertz.hh>
#include <au/units/seconds.hh>
#include <au/prefix.hh>
#include <string>
#include <cstdint>
#include <stdexcept>

namespace acq {

/// @brief AMQP broker connection and topic configuration.
struct AmqpConfig {
    std::string url{"amqp://localhost:5672"};              ///< Broker URL.
    std::string username;                                   ///< AMQP username.
    std::string password;                                   ///< AMQP password.
    std::string detection_topic{"rf.detections"};           ///< Topic for RF_DETECTION messages.
    std::string task_request_queue{"sdr.task.request"};    ///< Queue for outbound task requests.
    std::string task_response_queue{"sdr.task.response"};  ///< Queue for inbound task responses.
    int reconnect_interval_sec{5}; ///< Seconds between reconnect attempts on AMQP error.
};

/// @brief PostgreSQL detection database connection parameters.
struct DbConfig {
    std::string host{"localhost"};      ///< Database host.
    int         port{5432};             ///< Database port.
    std::string name{"sdr_scanner"};    ///< Database name.
    std::string user;                   ///< Database user.
    std::string password;               ///< Database password.
    /// @brief Build a libpqxx connection string from these parameters.
    std::string connection_string() const;
};

/// @brief SDR hardware configuration.
struct DeviceConfig {
    int    rx_channels{1};       ///< Number of RX channels to request.
    au::QuantityD<au::Hertz> sample_rate{au::hertz(10e6)};    ///< Sample rate (samples/s).
    double rx_gain_db{40.0};     ///< RX gain (dB).
    au::QuantityD<au::Hertz> bandwidth_hz{au::hertz(10e6)};   ///< Requested RF bandwidth per channel.
};

/**
 * @brief Frequency sweep and signal detection parameters.
 *
 * These parameters control the core detection pipeline.
 * See the README "Sweep parameters" table for the effect of each value.
 */
struct SweepParams {
    au::QuantityD<au::Hertz> start_hz{au::hertz(70'000'000.0)};    ///< Sweep start frequency.
    au::QuantityD<au::Hertz> stop_hz{au::hertz(1'000'000'000.0)};  ///< Sweep stop frequency.
    int      dwell_samples{4096};        ///< IQ samples collected per dwell (≥ fft_size).
    int      fft_size{4096};             ///< FFT size (power of 2, ≥ 64).
    double   usable_bw_fraction{0.80};   ///< Fraction of FFT bins examined (discards roll-off edges).
    double   threshold_db{10.0};         ///< Detection threshold above local CA-CFAR noise (dB).
    au::QuantityD<au::Hertz> min_signal_bw_hz{au::hertz(1'000.0)}; ///< Minimum run width to report.
    int      settle_samples{512};        ///< Samples discarded after each retune.
    /// Bins within this range of DC are blanked to suppress AD9361 LO leakage.
    /// Set 0 to disable.
    au::QuantityD<au::Hertz> dc_guard_hz{au::hertz(50'000.0)};
    int      cfar_guard_bins{8};         ///< CA-CFAR guard cells each side of test bin.
    int      cfar_ref_bins{32};          ///< CA-CFAR reference cells each side for noise average.
    /// Minimum peak-to-mean power ratio (dB) for multi-bin runs.
    /// Flat noise bumps below this threshold are rejected.
    float    min_papr_db{3.0f};
    /// EMA coefficient for the per-frequency noise floor tracker.
    /// Smaller = slower adaptation.  Range (0, 1).
    float    noise_floor_alpha{0.08f};
};

/// @brief UDP receive parameters for IQ data from SdrResourceManager.
struct ReceiverConfig {
    std::string local_ip{"0.0.0.0"}; ///< Local IP to bind (must be routable from the controller).
    int         port{0};              ///< UDP port to pre-bind.  0 = OS-assigned.
};

/**
 * @brief Top-level configuration for AcquisitionApp.
 *
 * Loaded from scanner.xml via SweepConfig::from_file().
 */
/**
 * @brief Threat detection configuration (GPS jamming/spoofing, content-layer validators).
 *
 * Controls which threat monitors are active. All disabled by default so the
 * system behaves identically to a version without threat detection unless
 * explicitly enabled in scanner.xml.
 */
struct ThreatConfig {
    bool enabled{false};              ///< Master switch — disables all threat detection when false.

    // ── GPS frequency monitoring ─────────────────────────────────────────────
    bool gps_enabled{false};          ///< Scan L1/L2/L5 for jammers and spoofers.
    double gps_jammer_threshold_db{20.0}; ///< dB above EMA baseline to declare jamming.
    double gps_spoof_detectable_dbm{-110.0}; ///< Any GPS-band signal above this = spoofing suspect.
    double gps_check_interval_s{30.0};   ///< Seconds between L1/L2/L5 checks.

    // ── Content-layer validators (run inside DemodApp demodulators) ──────────
    bool adsb_enabled{true};          ///< ADS-B physics validation (impossible altitude/speed/CRC rate).
    bool ais_enabled{true};           ///< AIS impossible vessel speed / invalid MMSI.
    bool eas_enabled{true};           ///< EAS/SAME invalid originator or rare event code.
    bool dsc_enabled{true};           ///< DSC distress call MMSI anomaly detection.
    bool p25_rogue_enabled{true};     ///< P25 rogue site (unexpected WACN change).

    // ── Alert persistence ────────────────────────────────────────────────────
    std::string alert_topic{"rf.alerts"}; ///< AMQP topic alerts are published to by DemodApp.
};

/// P25 trunked-system voice channel follower config.
struct P25Config {
    bool        enabled     = false;
    std::string grant_topic = "rf.p25.grants"; ///< Published by DemodApp P25Monitor
    double      capture_s   = 3.0;             ///< IQ capture per voice channel grant
    int         rank        = 3;               ///< Priority rank for voice captures
    std::vector<uint32_t> tg_whitelist;        ///< empty = all talk groups
};

/**
 * @brief One frequency band for multi-band parallel scanning.
 *
 * Each BandConfig spawns its own IqSource + SpectrumScanner, requesting
 * whichever SDR device is next available from SdrResourceManager (or a
 * specific device if device_id is set). All sweep parameters (FFT size,
 * CFAR, threshold, etc.) are inherited from the top-level SweepParams and
 * only start_hz / stop_hz are overridden per band.
 *
 * XML syntax inside <bands>:
 * @code{.xml}
 *   <band>
 *     <start_hz>88000000</start_hz>
 *     <stop_hz>174000000</stop_hz>
 *     <!-- <device>pluto-0</device>  optional: pin to a specific device -->
 *   </band>
 * @endcode
 */
struct BandConfig {
    std::string device_id;                              ///< Empty = SdrRM assigns next available.
    au::QuantityD<au::Hertz> start_hz{au::hertz(0.0)}; ///< Band start frequency.
    au::QuantityD<au::Hertz> stop_hz{au::hertz(0.0)};  ///< Band stop frequency.
};

struct SweepConfig {
    std::string    scanner_id{"scanner-0"}; ///< Identifies this scanner in AMQP messages.
    int            rank{1};  ///< Preemption tier: 1=Acq (lowest), 2=Ana, 3=DF (highest).
    /// Duration to sleep after each sweep pass before re-submitting the SCAN task.
    /// Gives AnalysisApp (lower rank) a guaranteed window to grab the SDR.
    /// 0 = disabled (continuous sweep, no analysis window).
    au::QuantityD<au::Seconds> analysis_pause_ms{au::seconds(0.0)};

    /**
     * @brief Multi-band parallel scan configuration.
     *
     * When non-empty, one SpectrumScanner is spawned per band and scan_device_ids
     * is ignored. Each scanner independently requests "next available device" from
     * SdrResourceManager, so N bands naturally spread across N available SDRs with
     * no duplicate frequency coverage and no inter-process coordination required.
     *
     * When empty, falls back to the legacy scan_device_ids / single-sweep behaviour.
     */
    std::vector<BandConfig>  bands;

    /// Legacy: device IDs to scan simultaneously with the same sweep range.
    /// Ignored when bands is non-empty.
    /// Empty = scheduler picks any free device.
    std::vector<std::string> scan_device_ids;
    AmqpConfig     amqp;
    DbConfig       db;
    DeviceConfig   device;
    SweepParams    sweep;
    ReceiverConfig receiver;
    P25Config      p25;       ///< P25 grant follower (disabled by default)
    ThreatConfig   threat;    ///< Threat detection monitors (all disabled by default)

    /**
     * @brief Load SweepConfig from an XML file.
     * @param path Path to the scanner.xml configuration file.
     * @return Populated SweepConfig.
     * @throws std::runtime_error if the file cannot be opened or parsed.
     */
    static SweepConfig from_file(const std::string& path);
};

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
