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
#include "TaskManagerIqSource.hpp"
#include <au/units/hertz.hh>
#include <sdr/Types.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/sender.hpp>
#include <proton/sender_options.hpp>
#include <proton/receiver.hpp>
#include <proton/receiver_options.hpp>
#include <proton/source_options.hpp>
#include <proton/target_options.hpp>
#include <proton/delivery.hpp>
#include <proton/reconnect_options.hpp>
#include <proton/transport.hpp>
#include <proton/work_queue.hpp>
#include <proton/symbol.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <sstream>

namespace acq {

using json = nlohmann::json;
using namespace std::chrono;

using IqPacketHeader     = sdr::IqPacketHeader;
static constexpr uint32_t IQ_MAGIC          = sdr::IQ_PACKET_MAGIC;
static constexpr uint8_t  FLAG_DWELL_CHANGE = sdr::IQ_FLAG_DWELL_CHANGE;

// ── Persistent AMQP task channel ─────────────────────────────────────────────
// Maintains one long-lived connection to the broker.  The ~30 s Artemis
// subscription-settlement cost is paid ONCE when the channel first connects.
// All subsequent exchange() calls return in < 1 s.
//
// Thread model: proton runs on an internal background thread.  exchange() and
// send() are called from the sweepLoop thread; they post work to the proton
// thread via work_queue and then block on a condition variable.

struct AmqpResponse {
    bool        received{false};
    std::string body;
};

class TaskAmqpChannel : public proton::messaging_handler {
public:
    TaskAmqpChannel(std::string url, std::string user, std::string pass,
                    std::string req_queue, std::string resp_queue)
        : url_(std::move(url)), user_(std::move(user)), pass_(std::move(pass)),
          req_queue_(std::move(req_queue)), resp_queue_(std::move(resp_queue))
    {}

    ~TaskAmqpChannel() { stop(); }

    // Start the background thread and block until sender + receiver are open.
    void start(int connect_timeout_ms = 60000) {
        container_ = std::make_unique<proton::container>(*this);
        thread_ = std::thread([this]{ container_->run(); });
        std::unique_lock<std::mutex> lk(mu_);
        ready_cv_.wait_for(lk, std::chrono::milliseconds(connect_timeout_ms),
                           [this]{ return ready_ || stopped_; });
        if (!ready_)
            spdlog::warn("[TaskAmqpChannel] not ready after {}ms", connect_timeout_ms);
    }

    void stop() {
        if (container_) {
            if (wq_)
                wq_->add([this]{ sender_.connection().close(); });
            else
                // Connection never reached on_receiver_open (e.g. Artemis was
                // still starting), so there's no work queue to post a close
                // through. reconnect_options has max_attempts(0) — infinite
                // retries — so without this, container_->run() never returns
                // and thread_.join() below blocks forever.
                container_->stop();
            if (thread_.joinable()) thread_.join();
            container_.reset();
        }
    }

    // Send msg_body and wait for a response matching corr_id.
    AmqpResponse exchange(const std::string& msg_body, const std::string& corr_id,
                          int timeout_ms = 30000) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!ready_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                    [this]{ return ready_; })) {
                spdlog::warn("[TaskAmqpChannel] not ready for exchange");
                return {};
            }
            pending_corr_  = corr_id;
            pending_result_ = {};
        }
        wq_->add([this, msg_body]() mutable {
            proton::message msg;
            msg.body(msg_body);
            msg.content_type("application/json");
            msg.reply_to(reply_addr_);
            if (sender_) sender_.send(msg);
        });
        std::unique_lock<std::mutex> lk(mu_);
        result_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this]{ return pending_result_.received; });
        return pending_result_;
    }

    // Fire-and-forget send (for TASK_STOP).
    void send(const std::string& msg_body) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!ready_) return;
        }
        wq_->add([this, msg_body]() mutable {
            proton::message msg;
            msg.body(msg_body);
            msg.content_type("application/json");
            sender_.send(msg);
        });
    }

    // proton callbacks ─────────────────────────────────────────────────────────
    void on_container_start(proton::container& c) override {
        proton::connection_options opts;
        if (!user_.empty()) {
            opts.sasl_allowed_mechs("PLAIN");
            opts.sasl_allow_insecure_mechs(true);
            opts.user(user_).password(pass_);
        } else {
            opts.sasl_allowed_mechs("ANONYMOUS");
        }
        proton::reconnect_options ropts;
        ropts.delay(proton::duration(2000));
        ropts.max_delay(proton::duration(30000));
        ropts.max_attempts(0);
        opts.reconnect(ropts);
        c.connect(url_, opts);
    }

    void on_connection_open(proton::connection& conn) override {
        // Open request sender with ANYCAST so Artemis uses a proper queue.
        proton::sender_options sopts;
        sopts.target(proton::target_options().capabilities({proton::symbol("queue")}));
        sender_ = conn.open_sender(req_queue_, sopts);

        // Dynamic receiver: Artemis assigns a unique temporary address.
        // Responses from the controller go to reply_to=<this address>.
        // Because the address is unique per-connection, no competing consumers.
        proton::receiver_options ropts;
        ropts.source(proton::source_options().dynamic(true));
        conn.open_receiver("", ropts);
    }

    void on_receiver_open(proton::receiver& r) override {
        reply_addr_ = r.source().address();
        spdlog::info("[TaskAmqpChannel] connected, reply_to={}", reply_addr_);
        wq_ = &r.work_queue();
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = true;
        ready_cv_.notify_all();
    }

    void on_message(proton::delivery& d, proton::message& msg) override {
        d.accept();
        try {
            std::string b = proton::get<std::string>(msg.body());
            auto j = json::parse(b);
            std::lock_guard<std::mutex> lk(mu_);
            if (!pending_corr_.empty() &&
                (j.value("request_id", "") == pending_corr_ ||
                 j.value("correlation_id", "") == pending_corr_)) {
                pending_result_ = {true, b};
                pending_corr_.clear();
                result_cv_.notify_all();
            }
        } catch (...) {}
    }

    void on_transport_error(proton::transport& t) override {
        spdlog::warn("[TaskAmqpChannel] transport error: {}", t.error().what());
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = false;
        reply_addr_.clear();
    }
    void on_connection_error(proton::connection& c) override {
        spdlog::warn("[TaskAmqpChannel] connection error: {}", c.error().what());
    }

private:
    std::string url_, user_, pass_, req_queue_, resp_queue_;

    std::unique_ptr<proton::container> container_;
    std::thread  thread_;

    proton::sender      sender_;
    proton::work_queue* wq_{nullptr};
    std::string         reply_addr_;

    std::mutex              mu_;
    std::condition_variable ready_cv_;
    std::condition_variable result_cv_;
    bool        ready_{false};
    bool        stopped_{false};
    std::string pending_corr_;
    AmqpResponse pending_result_;
};

// ── Simple request-ID generator ───────────────────────────────────────────────
static std::string makeReqId() {
    auto ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    std::ostringstream ss;
    ss << "acq-" << std::hex << ns;
    return ss.str();
}

// ── TaskManagerIqSource ───────────────────────────────────────────────────────

TaskManagerIqSource::TaskManagerIqSource(const SweepConfig& cfg,
                                          std::string preferred_device)
    : cfg_(cfg), preferred_device_(std::move(preferred_device)),
      resume_hz_(cfg.sweep.start_hz) {
    pkt_buf_.resize(65536);
    resetAccum();

    // Start persistent AMQP channel immediately — Artemis settles the connection
    // in ~30 s.  By the time open() is called, it will already be ready.
    amqp_ch_ = std::make_unique<TaskAmqpChannel>(
        cfg_.amqp.url, cfg_.amqp.username, cfg_.amqp.password,
        cfg_.amqp.task_request_queue, cfg_.amqp.task_response_queue);
    amqp_ch_->start(60000);
}
TaskManagerIqSource::~TaskManagerIqSource() { close(); }

void TaskManagerIqSource::resetAccum() {
    ch_accum_.assign((size_t)cfg_.device.rx_channels, {});
    // Pre-reserve so packet inserts never reallocate mid-dwell.
    for (auto& buf : ch_accum_)
        buf.reserve(static_cast<size_t>(cfg_.sweep.dwell_samples));
    current_center_hz_ = au::hertz(0.0);
}

void TaskManagerIqSource::bindUdp(uint16_t port) {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0)
        throw std::runtime_error(
            std::string("UDP socket() failed: ") + strerror(errno));

    // Set receive timeout so next() can check the stop flag
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100'000};  // 100 ms
    setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Enlarge kernel receive buffer to absorb the full sweep while waiting for
    // TASK_ACCEPTED.  At 20 MSPS (160 MB/s) a 920 MHz sweep takes ~373ms = ~59 MB.
    // Requesting 50 MB → kernel allocates min(2×50 MB, rmem_max) ≈ 100 MB effective,
    // which is enough to buffer the complete sweep before we start draining.
    int rcvbuf = 50 * 1024 * 1024;
    if (setsockopt(udp_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
        spdlog::warn("[TaskMgrSrc] SO_RCVBUF failed: {}", strerror(errno));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    inet_aton(cfg_.receiver.local_ip.c_str(), &addr.sin_addr);

    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(udp_fd_); udp_fd_ = -1;
        throw std::runtime_error(
            std::string("UDP bind() failed on port ") + std::to_string(port)
            + ": " + strerror(errno));
    }

    spdlog::info("[TaskMgrSrc] UDP socket bound on {}:{}", cfg_.receiver.local_ip, port);
}

std::string TaskManagerIqSource::buildScanRequest(const std::string& req_id) const {
    const double sr_hz    = cfg_.device.sample_rate.in(au::hertz);
    const double bw_hz    = cfg_.device.bandwidth_hz.in(au::hertz);
    const double start_hz = cfg_.sweep.start_hz.in(au::hertz);
    const double stop_hz  = cfg_.sweep.stop_hz.in(au::hertz);
    const double resume   = resume_hz_.in(au::hertz);

    const double step_hz = sr_hz * cfg_.sweep.usable_bw_fraction;
    int dwell_ms = std::max(1, static_cast<int>(cfg_.sweep.dwell_samples * 1000.0 / sr_hz));

    // Build entries starting from resume_hz so a re-submission after preemption
    // continues the sweep rather than restarting at start_hz. The second loop
    // wraps back through start_hz → resume_hz, ensuring every frequency is
    // covered exactly once per task regardless of where we were interrupted.
    auto makeEntry = [&](int& step_i, double pos) {
        return json{
            {"step",            step_i++},
            {"center_freq_hz",  pos + step_hz / 2.0},
            {"bandwidth_hz",    bw_hz},
            {"sample_rate_sps", sr_hz},
            {"dwell_ms",        dwell_ms}
        };
    };

    json entries = json::array();
    int step_i = 0;
    for (double pos = resume; pos + step_hz / 2.0 <= stop_hz;  pos += step_hz) entries.push_back(makeEntry(step_i, pos));
    for (double pos = start_hz; pos + step_hz / 2.0 <= resume; pos += step_hz) entries.push_back(makeEntry(step_i, pos));

    std::vector<double> gains(cfg_.device.rx_channels, cfg_.device.rx_gain_db);

    auto now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    // Build the streaming object; include the pre-bound port so the controller
    // can stream to it immediately without waiting for a handshake.
    std::string dest_ip = (cfg_.receiver.local_ip == "0.0.0.0")
                          ? "127.0.0.1" : cfg_.receiver.local_ip;
    json streaming_obj = {{"dest_ip", dest_ip}};
    if (prebound_port_ > 0)
        streaming_obj["dest_ports"] = json::array({static_cast<int>(prebound_port_)});

    json req = {
        {"msg_type",       "TASK_REQUEST_SCAN"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   now_ms},
        {"request_id",     req_id},
        {"task_type",      "SCAN"},
        {"rank",           cfg_.rank},
        {"schedule", {{"mode", "CONTINUOUS"}}},
        {"rf", {
            {"center_freq_hz",    (start_hz + stop_hz) / 2.0},
            {"bandwidth_hz",      bw_hz},
            {"sample_rate_sps",   sr_hz},
            {"rx_count",          cfg_.device.rx_channels},
            {"rx_gain_db",        gains},
            {"preferred_device",  preferred_device_}
        }},
        {"streaming", streaming_obj},
        {"scan_params", {
            {"repeat",  false},
            {"entries", entries}
        }}
    };
    return req.dump();
}

// Returns the UDP port the controller allocated for our stream.
uint16_t TaskManagerIqSource::submitTask() {
    std::string req_id = makeReqId();
    std::string body   = buildScanRequest(req_id);

    spdlog::info("[TaskMgrSrc] submitting SCAN task (req_id={})", req_id);

    auto resp = amqp_ch_->exchange(body, req_id, 30000);
    if (!resp.received)
        throw std::runtime_error("Task request timed out — is the controller running?");

    auto j = json::parse(resp.body);
    // Controller sends {"status": "ACCEPTED" | "REJECTED"} not {"accepted": bool}
    bool accepted = (j.value("status", "REJECTED") == "ACCEPTED") ||
                    j.value("accepted", false);
    if (!accepted) {
        throw std::runtime_error(
            "Task rejected: " + j.value("reject_reason", "unknown"));
    }
    task_id_ = j.value("task_id", "");

    // Controller allocates ports from its own pool and returns them in streams[].udp_port
    uint16_t port = 0;
    if (j.contains("streams") && !j["streams"].empty())
        port = j["streams"][0].value("udp_port", 0);
    if (port == 0)
        throw std::runtime_error("ACCEPTED response missing streams[0].udp_port");

    spdlog::info("[TaskMgrSrc] task accepted (task_id={} udp_port={})", task_id_, port);
    return port;
}

void TaskManagerIqSource::sendTaskStop() {
    if (task_id_.empty()) return;
    std::string req_id = makeReqId();
    auto now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    json msg = {
        {"msg_type",       "TASK_STOP"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   now_ms},
        {"request_id",     req_id},
        {"task_id",        task_id_},
        {"reason",         "scanner stopping"}
    };
    amqp_ch_->send(msg.dump());
    spdlog::info("[TaskMgrSrc] TASK_STOP sent for task_id={}", task_id_);
    task_id_.clear();
}

int TaskManagerIqSource::queryDeviceCount() {
    auto req_id = makeReqId();
    auto now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    json req = {
        {"msg_type",       "HEALTH_QUERY"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   now_ms},
        {"request_id",     req_id}
    };
    // exchange()'s readiness wait consumes its entire timeout_ms budget
    // before ever attempting to send+await a response (it returns
    // immediately if the channel isn't connected yet) -- a single 120s
    // shot isn't always enough: a cold Artemis pod has been observed
    // taking up to ~4 min to start accepting AMQP connections on a
    // Raspberry Pi. Retry in a loop with periodic warnings instead of
    // giving up after one shot, mirroring the patient-retry pattern used
    // for AMQP session bring-up elsewhere (see sdr_recon.py). Each retry
    // re-sends the same idempotent query under the same request_id, which
    // is harmless if an earlier attempt's response arrives late.
    constexpr int kStepMs = 60000;
    constexpr int kMaxWaitMs = 300000;
    AmqpResponse resp;
    int waited = 0;
    while (waited < kMaxWaitMs) {
        resp = amqp_ch_->exchange(req.dump(), req_id, kStepMs);
        if (resp.received) break;
        waited += kStepMs;
        spdlog::warn("[TaskMgrSrc] HEALTH_QUERY not answered after {}s — "
                     "controller/broker still starting?", waited / 1000);
    }
    if (!resp.received) {
        spdlog::warn("[TaskMgrSrc] HEALTH_QUERY timed out after {}s — assuming 1 device",
                     kMaxWaitMs / 1000);
        return 1;
    }
    try {
        auto j = json::parse(resp.body);
        // Controller emits "online_devices" (see MessageCodec::encodeControllerHealth
        // in SdrTaskApi) -- not "num_devices_online". A mismatched key here silently
        // falls back to the default via .value() with no warning or exception, which
        // is why band-splitting always saw "1 device" even with N genuinely online.
        int n = j["controller"].value("online_devices", 1);
        spdlog::info("[TaskMgrSrc] SdrRM reports {} online device(s)", n);
        return std::max(1, n);
    } catch (...) {
        spdlog::warn("[TaskMgrSrc] HEALTH_QUERY parse failed — assuming 1 device");
        return 1;
    }
}

void TaskManagerIqSource::open() {
    // Pre-bind before submitting the task so the controller streams to a
    // ready socket from the first packet — eliminates the race where
    // AMQP delivery of TASK_ACCEPTED arrives after streaming has finished.
    //
    // With port=0, the OS assigns an ephemeral port (e.g. 54321); we include
    // it in the task request's dest_ports[] so the controller honours it.
    // With a fixed port in config, we bind that port and include it too.
    uint16_t local_port = static_cast<uint16_t>(cfg_.receiver.port);
    bindUdp(local_port);

    // Learn the actual bound port (important when local_port==0).
    if (local_port == 0) {
        struct sockaddr_in sa{};
        socklen_t sl = sizeof(sa);
        getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&sa), &sl);
        local_port = ntohs(sa.sin_port);
    }
    prebound_port_ = local_port;   // buildScanRequest() reads this

    uint16_t ctrl_port = submitTask();
    prebound_port_ = 0;

    if (ctrl_port != 0 && ctrl_port != local_port) {
        // Controller overrode our port (didn't support dest_ports[]) —
        // re-bind to its assignment.  Data may be lost for this sweep.
        spdlog::warn("[TaskMgrSrc] controller overrode port {} → {}; re-binding "
                     "(upgrade controller to honour dest_ports[])",
                     local_port, ctrl_port);
        ::close(udp_fd_); udp_fd_ = -1;
        bindUdp(ctrl_port);
    }
    resetAccum();
    first_task_packet_ = true;
    running_ = true;
}

void TaskManagerIqSource::stop() {
    running_ = false;
}

void TaskManagerIqSource::close() {
    running_ = false;
    sendTaskStop();
    if (udp_fd_ >= 0) { ::close(udp_fd_); udp_fd_ = -1; }
}

bool TaskManagerIqSource::next(Dwell& d) {
    // Two-phase no-data timeout:
    //  • Before the first packet of the TASK arrives the SCAN task may sit
    //    in PENDING while AnalysisApp finishes a WIDEBAND task — allow up
    //    to 30 s. (first_task_packet_ is a member set once per open(), not
    //    once per call -- see its declaration for why a local here was a
    //    bug: it silently re-granted this 30s budget after every single
    //    completed dwell, including the final call after the controller's
    //    sweep had already finished, so every successful task still ended
    //    in a guaranteed ~30s of dead waiting before being recognized as
    //    over.)
    //  • After data starts flowing the inter-dwell gap (PLL calibration) is
    //    3-4 s; allow 5 s before declaring the stream ended.
    static constexpr int INITIAL_TIMEOUT_MS = 30000;
    static constexpr int STREAM_TIMEOUT_MS  =  5000;
    int no_data_ms     = 0;

    while (running_) {
        ssize_t n = ::recvfrom(udp_fd_, pkt_buf_.data(), pkt_buf_.size(), 0, nullptr, nullptr);
        if (n < 0) {
            // EINTR is not a real failure -- it just means a signal hit this
            // thread mid-syscall (observed live: proton's AMQP container
            // thread/timers deliver these periodically). Previously this fell
            // through to the generic error branch below and returned false,
            // aborting an otherwise perfectly healthy stream and forcing a
            // full task resubmit. Confirmed via strace: a session that was
            // successfully receiving ~9000 pkts/sec for 57s got killed by a
            // single EINTR and had to restart from scratch -- with the SCAN
            // task's 30s-to-first-packet budget, repeated mid-stream EINTRs
            // are enough to make a device that's genuinely streaming data
            // look like it never produces any.
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                no_data_ms += 100;
                int limit = first_task_packet_ ? INITIAL_TIMEOUT_MS : STREAM_TIMEOUT_MS;
                if (no_data_ms >= limit) {
                    spdlog::info("[TaskMgrSrc] no IQ data for {}ms — task complete, re-submitting",
                                 no_data_ms);
                    return false;
                }
                continue;
            }
            spdlog::error("[TaskMgrSrc] recvfrom error: {}", strerror(errno));
            return false;
        }
        first_task_packet_ = false;
        no_data_ms   = 0;
        // Advance resume_hz_ on every packet so it always reflects the last
        // frequency we actually received data at. When next() returns false
        // (preemption or sweep complete) the next open() will resume from here.
        if (static_cast<size_t>(n) >= sizeof(IqPacketHeader)) {
            const auto& peek = *reinterpret_cast<const IqPacketHeader*>(pkt_buf_.data());
            if (peek.magic == IQ_MAGIC) {
                const double step_hz = cfg_.device.sample_rate.in(au::hertz)
                                       * cfg_.sweep.usable_bw_fraction;
                auto next_hz = au::hertz(static_cast<double>(peek.center_freq_hz) + step_hz);
                resume_hz_ = (next_hz >= cfg_.sweep.stop_hz)
                             ? cfg_.sweep.start_hz : next_hz;
            }
        }

        if (static_cast<size_t>(n) < sizeof(IqPacketHeader)) continue;

        const auto& hdr = *reinterpret_cast<const IqPacketHeader*>(pkt_buf_.data());
        if (hdr.magic != IQ_MAGIC) continue;

        int n_samp = hdr.num_samples;
        int ch     = hdr.channel_index;

        // Validate declared sample count against actual received bytes to guard
        // against truncated or malformed packets corrupting the accumulator.
        size_t expected = sizeof(IqPacketHeader) + (size_t)n_samp * sizeof(std::complex<float>);
        if (static_cast<size_t>(n) < expected) {
            spdlog::warn("[TaskMgrSrc] truncated packet: {} bytes, expected {} for {} samples",
                         n, expected, n_samp);
            continue;
        }

        const auto* samples = reinterpret_cast<const std::complex<float>*>(
            pkt_buf_.data() + sizeof(IqPacketHeader));

        // IQ_FLAG_DWELL_CHANGE: first packet of a new dwell position.
        // Return the accumulated dwell (if any), then reset.
        if ((hdr.flags & FLAG_DWELL_CHANGE) && current_center_hz_ != au::hertz(0.0)) {
            bool has_data = false;
            for (auto& buf : ch_accum_)
                if (!buf.empty()) { has_data = true; break; }

            if (has_data) {
                d.center_hz  = current_center_hz_;
                d.ch_samples = std::move(ch_accum_);
                resetAccum();
                current_center_hz_ = au::hertz(static_cast<double>(hdr.center_freq_hz));
                // Buffer the new-dwell samples for next call
                if (ch < (int)ch_accum_.size())
                    ch_accum_[ch].insert(ch_accum_[ch].end(), samples, samples + n_samp);
                return true;
            }
        }

        current_center_hz_ = au::hertz(static_cast<double>(hdr.center_freq_hz));
        if (ch < (int)ch_accum_.size())
            ch_accum_[ch].insert(ch_accum_[ch].end(), samples, samples + n_samp);

        // Return a complete dwell once we have enough samples on channel 0
        if ((int)ch_accum_[0].size() >= cfg_.sweep.dwell_samples) {
            d.center_hz  = current_center_hz_;
            d.ch_samples = std::move(ch_accum_);
            resetAccum();
            return true;
        }
    }
    return false;
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
