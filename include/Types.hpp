#pragma once
/**
 * @file Types.hpp
 * @brief Detection result type for AcquisitionApp.
 *
 * A Detection is produced by SpectrumScanner for each confirmed signal and
 * forwarded to AmqpPublisher (AMQP) and DetectionDb (PostgreSQL).
 */
#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

/// @namespace acq
/// @brief AcquisitionApp internals.
namespace acq {

/// @brief RF_DETECTION schema version emitted by this build.
inline constexpr const char* SCHEMA_VERSION = "1.2";  // base64 iq_snapshot, snr_db

/**
 * @brief A confirmed RF signal detection from one dwell of a frequency sweep.
 *
 * Detections are produced by SpectrumScanner::processDwell() after the
 * persistence filter confirms the signal in at least two consecutive sweeps
 * (unless PAPR ≥ PERSIST_BYPASS_PAPR_DB, in which case it emits immediately).
 *
 * The @p iq_snapshot is shared across all detections from the same dwell
 * via shared_ptr — zero copies regardless of how many signals are found.
 */
struct Detection {
    std::chrono::system_clock::time_point timestamp; ///< Wall-clock time of detection.
    uint64_t    center_freq_hz{0};   ///< Sub-bin interpolated centre frequency (Hz).
    uint32_t    bandwidth_hz{0};     ///< Estimated occupied bandwidth (Hz).
    float       power_db{0.f};       ///< Peak power in the detection run (dBFS).
    std::string scanner_id;          ///< Scanner identifier from SweepConfig.
    int         channel{0};          ///< RX channel index (0-based).
    float       snr_db{0.f};         ///< PAPR of the detected signal group (peak − mean, dB).

    /// 1 024-sample IQ snapshot from the detecting dwell (interleaved float32 I,Q,…).
    /// Shared across all detections from the same dwell — zero-copy shared_ptr.
    /// Null when no snapshot was collected (unit tests or snapshot disabled).
    std::shared_ptr<const std::vector<float>> iq_snapshot;
    double snapshot_sample_rate_sps{0.0}; ///< Sample rate of the IQ snapshot (samples/s).
};

} // namespace acq
