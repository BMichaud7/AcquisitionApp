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
 * @file TaskManagerIqSource.hpp
 * @brief Production IqSource that drives SdrResourceManager via AMQP + UDP.
 *
 * TaskManagerIqSource implements the full scan data path:
 *
 * 1. **open()** — builds a SCAN task request from SweepConfig, pre-binds a
 *    UDP socket on the selected port, submits the task via TaskAmqpChannel
 *    (persistent connection; ~16 ms round-trip), and waits for ACCEPTED.
 *
 * 2. **next()** — receives CF32 IQ packets over UDP, accumulates samples into
 *    per-channel buffers, and returns a complete Dwell each time the controller
 *    signals a retune (IQ_FLAG_DWELL_CHANGE) or @p dwell_samples have arrived.
 *
 * 3. **close()** — sends TASK_STOP and closes the socket.
 *
 * The persistent AMQP channel (TaskAmqpChannel) pays the Artemis connection
 * setup cost once at startup rather than on every task submission, reducing
 * per-submission latency from ~30 s (new connection) to ~16 ms.
 *
 * The UDP socket is pre-bound before submitTask() and the port is included in
 * the dest_ports[] field of the task request.  This eliminates the race
 * between task acceptance and the first IQ packet arriving.
 */
#include "IqSource.hpp"
#include "SweepConfig.hpp"
#include <au/units/hertz.hh>
#include <au/units/seconds.hh>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace proton { class container; }
namespace acq {

class TaskAmqpChannel;  ///< Persistent AMQP request/response channel (defined in .cpp).

/**
 * @brief Production IqSource backed by SdrResourceManager.
 *
 * Instantiated once per scanner startup.  The AMQP channel is established
 * in the constructor and reused for the lifetime of the process.
 */
class TaskManagerIqSource : public IqSource {
public:
    /**
     * @brief Construct and connect the persistent AMQP channel.
     * @param cfg              Sweep configuration (AMQP URL, credentials, scan params, etc.).
     * @param preferred_device Device ID to request from the controller (empty = any free device).
     * @throws std::runtime_error if the AMQP channel fails to connect within 60 s.
     */
    explicit TaskManagerIqSource(const SweepConfig& cfg,
                                  std::string preferred_device = "");
    ~TaskManagerIqSource() override;

    /**
     * @brief Query SdrRM for the number of online SDR devices.
     *
     * Sends a HEALTH_QUERY via the persistent AMQP channel and parses
     * controller.num_devices_online from the response. Used by main() to
     * auto-split bands when more devices are available than bands configured.
     *
     * @return Number of online devices, or 1 on timeout / parse failure.
     */
    int queryDeviceCount();

    /**
     * @brief Submit a SCAN task and bind the UDP socket.
     * @throws std::runtime_error if the task is rejected or the socket bind fails.
     */
    void open()  override;

    /// @brief Send TASK_STOP and close the UDP socket.
    void close() override;

    /// @brief Signal the next() loop to return false.  Thread-safe.
    void stop()  override;

    /**
     * @brief Block until a complete Dwell is available.
     * @param d  Output dwell (populated on true return).
     * @return   true while running; false after stop() or on error.
     */
    bool next(Dwell& d) override;

    /// @brief Number of RX channels requested from the controller.
    int  numChannels() const override { return cfg_.device.rx_channels; }

private:
    SweepConfig      cfg_;
    std::string      preferred_device_;  ///< Requested device ID; empty = any.
    int              udp_fd_{-1};        ///< UDP receive socket file descriptor.
    std::string      task_id_;           ///< Controller-assigned task UUID.
    std::atomic<bool> running_{false};

    std::vector<std::vector<std::complex<float>>> ch_accum_; ///< Per-channel sample accumulators.
    au::QuantityD<au::Hertz> current_center_hz_{au::hertz(0.0)};
    /// Start frequency for the next task submission. Advances past the last
    /// completed dwell so re-submissions continue the sweep rather than
    /// restarting from start_hz after each preemption.
    au::QuantityD<au::Hertz> resume_hz_{au::hertz(0.0)};
    std::vector<uint8_t> pkt_buf_;  ///< Pre-allocated UDP receive buffer.

    void         bindUdp(uint16_t port);
    uint16_t     submitTask();
    void         sendTaskStop();
    std::string  buildScanRequest(const std::string& req_id) const;
    void         resetAccum();

    /// Persistent AMQP channel — connects once at startup.
    /// Reduces task submission latency from ~30 s (new connection) to ~16 ms.
    std::unique_ptr<TaskAmqpChannel> amqp_ch_;

    /// UDP port pre-bound before submitTask() so packets arrive without a race.
    uint16_t     prebound_port_{0};
};

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
