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
struct SweepConfig {
    std::string    scanner_id{"scanner-0"}; ///< Identifies this scanner in AMQP messages.
    int            rank{1};  ///< Preemption tier: 1=Acq (lowest), 2=Ana, 3=DF (highest).
    /// Duration to sleep after each sweep pass before re-submitting the SCAN task.
    /// Gives AnalysisApp (lower rank) a guaranteed window to grab the SDR.
    /// 0 = disabled (continuous sweep, no analysis window).
    au::QuantityD<au::Seconds> analysis_pause_ms{au::seconds(0.0)};
    /// Device IDs to scan simultaneously. Empty = scheduler picks any free device.
    /// Set in XML: <scan_device_ids>pluto-0 pluto-1</scan_device_ids>
    /// AcquisitionApp submits one SCAN task per device and scans them in parallel.
    std::vector<std::string> scan_device_ids;
    AmqpConfig     amqp;
    DbConfig       db;
    DeviceConfig   device;
    SweepParams    sweep;
    ReceiverConfig receiver;

    /**
     * @brief Load SweepConfig from an XML file.
     * @param path Path to the scanner.xml configuration file.
     * @return Populated SweepConfig.
     * @throws std::runtime_error if the file cannot be opened or parsed.
     */
    static SweepConfig from_file(const std::string& path);
};

} // namespace acq
