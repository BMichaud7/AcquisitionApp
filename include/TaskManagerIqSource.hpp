#pragma once
#include "IqSource.hpp"
#include "SweepConfig.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace proton { class container; }
namespace acq {

class TaskAmqpChannel;  // persistent request/response channel — see .cpp

// Production IqSource that drives the task manager as a client.
// open()  — builds a dwell plan from SweepConfig, submits a SCAN task via
//            AMQP, then binds a UDP socket to receive CF32 IQ packets.
// next()  — accumulates incoming packets; returns a complete dwell each time
//            the controller signals a retune (IQ_FLAG_DWELL_CHANGE) or
//            dwell_samples have been collected.
// close() — sends TASK_STOP and shuts down the socket.
class TaskManagerIqSource : public IqSource {
public:
    explicit TaskManagerIqSource(const SweepConfig& cfg);
    ~TaskManagerIqSource() override;

    void open()  override;
    void close() override;
    void stop()  override;
    bool next(Dwell& d) override;
    int  numChannels() const override { return cfg_.device.rx_channels; }

private:
    SweepConfig      cfg_;
    int              udp_fd_{-1};
    std::string      task_id_;
    std::atomic<bool> running_{false};

    // Channel accumulation buffers: [channel][samples]
    std::vector<std::vector<std::complex<float>>> ch_accum_;
    uint64_t current_center_hz_{0};
    std::vector<uint8_t> pkt_buf_;  // pre-allocated UDP recv buffer

    void         bindUdp(uint16_t port);
    uint16_t     submitTask();
    void         sendTaskStop();
    std::string  buildScanRequest(const std::string& req_id) const;
    void         resetAccum();

    // Persistent AMQP channel — connects once so the 30s Artemis settlement
    // cost is paid at startup rather than on every task submission.
    std::unique_ptr<TaskAmqpChannel> amqp_ch_;

    // Port pre-bound before submitTask(); included in dest_ports[] so the
    // controller streams to the already-listening socket (eliminates race).
    uint16_t     prebound_port_{0};
};

} // namespace acq
