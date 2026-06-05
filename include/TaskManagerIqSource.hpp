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
 * 1. **queryDeviceCount()** — sends a HEALTH_QUERY to SdrRM and returns
 *    @c controller.num_devices_online. Called once at startup so main() can
 *    auto-split configured bands across all available devices.
 *
 * 2. **open()** — builds a SCAN task request from SweepConfig, pre-binds a
 *    UDP socket on the selected port, submits the task via TaskAmqpChannel
 *    (persistent connection; ~16 ms round-trip), and waits for ACCEPTED.
 *    The dwell entry list starts from @c resume_hz_ so re-submissions after
 *    preemption continue the sweep rather than restarting from start_hz.
 *
 * 3. **next()** — receives CF32 IQ packets over UDP, accumulates samples into
 *    per-channel buffers, and returns a complete Dwell each time the controller
 *    signals a retune (IQ_FLAG_DWELL_CHANGE) or @p dwell_samples have arrived.
 *    Updates @c resume_hz_ on every packet so preemption position is always current.
 *
 * 4. **close()** — sends TASK_STOP and closes the socket.
 *
 * The persistent AMQP channel (TaskAmqpChannel) pays the Artemis connection
 * setup cost once at startup rather than on every task submission, reducing
 * per-submission latency from ~30 s (new connection) to ~16 ms.
 *
 * The UDP socket is pre-bound before submitTask() and the port is included in
 * the dest_ports[] field of the task request, eliminating the race between
 * task acceptance and the first IQ packet arriving.
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
    SweepConfig       cfg_;
    std::string       preferred_device_;        ///< Requested device ID; empty = any free device.
    int               udp_fd_{-1};              ///< UDP receive socket file descriptor.
    std::string       task_id_;                 ///< Controller-assigned task UUID for TASK_STOP.
    std::atomic<bool> running_{false};          ///< Set false by stop() to break the next() loop.

    std::vector<std::vector<std::complex<float>>> ch_accum_; ///< Per-channel IQ sample accumulators.

    /// LO centre frequency of the dwell currently being accumulated.
    au::QuantityD<au::Hertz> current_center_hz_{au::hertz(0.0)};

    /// Start frequency for the next SCAN task submission.
    /// Updated on every received packet to track sweep position. When the
    /// task ends (sweep complete or preemption), buildScanRequest() starts
    /// the next entry list from here, wrapping start_hz → resume_hz after
    /// stop_hz, so every frequency is visited once per task regardless of
    /// where the previous task was interrupted.
    au::QuantityD<au::Hertz> resume_hz_{au::hertz(0.0)};

    std::vector<uint8_t> pkt_buf_;             ///< Pre-allocated 64 KB UDP receive buffer.

    /// @brief Bind a UDP socket to @p port (0 = OS-assigned).
    void        bindUdp(uint16_t port);

    /// @brief Submit TASK_REQUEST_SCAN to SdrRM and return the allocated UDP port.
    /// @throws std::runtime_error on rejection or timeout.
    uint16_t    submitTask();

    /// @brief Send TASK_STOP for the current task_id_ (fire-and-forget).
    void        sendTaskStop();

    /// @brief Build the SCAN task JSON body with dwell entries from resume_hz_.
    std::string buildScanRequest(const std::string& req_id) const;

    /// @brief Reset per-channel accumulators and current_center_hz_ to zero.
    void        resetAccum();

    /// Persistent AMQP channel — connects once at startup, reused for all
    /// task submissions and health queries. Avoids the ~30 s Artemis setup
    /// cost on every open() call.
    std::unique_ptr<TaskAmqpChannel> amqp_ch_;

    /// UDP port pre-bound before submitTask() so IQ packets arrive at a ready
    /// socket from the first frame — eliminates the TASK_ACCEPTED race window.
    uint16_t    prebound_port_{0};
};

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
