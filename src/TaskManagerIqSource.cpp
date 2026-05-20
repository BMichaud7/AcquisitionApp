#include "TaskManagerIqSource.hpp"
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

// ── Minimal synchronous AMQP helper ──────────────────────────────────────────
// Sends one message and optionally waits for a correlated reply.

struct AmqpResponse {
    bool        received{false};
    std::string body;
};

class SyncAmqpExchange : public proton::messaging_handler {
public:
    SyncAmqpExchange(std::string url, std::string username, std::string password,
                     std::string send_addr, std::string recv_addr,
                     std::string msg_body, std::string correlation_id, int timeout_sec)
        : url_(std::move(url)), username_(std::move(username)), password_(std::move(password)),
          send_addr_(std::move(send_addr)),
          recv_addr_(std::move(recv_addr)), body_(std::move(msg_body)),
          corr_id_(std::move(correlation_id)), timeout_sec_(timeout_sec) {}

    AmqpResponse run() {
        proton::container c(*this);
        // Schedule a timeout to prevent indefinite blocking
        std::thread t([&c, this]{
            std::this_thread::sleep_for(std::chrono::seconds(timeout_sec_));
            c.stop();
        });
        c.run();
        t.join();
        return result_;
    }

    void on_container_start(proton::container& c) override {
        proton::connection_options opts;
        if (!username_.empty()) {
            opts.sasl_allowed_mechs("PLAIN");
            opts.sasl_allow_insecure_mechs(true);
            opts.user(username_).password(password_);
        } else {
            opts.sasl_allowed_mechs("ANONYMOUS");
        }
        c.connect(url_, opts);
    }
    void on_connection_open(proton::connection& conn) override {
        // ANYCAST capabilities: Artemis routes to a proper queue address instead
        // of defaulting to MULTICAST, which stalls credit propagation for ~30 s.
        proton::sender_options sopts;
        sopts.target(proton::target_options().capabilities(
            {proton::symbol("queue")}));
        conn.open_sender(send_addr_, sopts);

        if (!recv_addr_.empty()) {
            proton::receiver_options ropts;
            ropts.source(proton::source_options().capabilities(
                {proton::symbol("queue")}));
            conn.open_receiver(recv_addr_, ropts);
        }
    }
    void on_sender_open(proton::sender& s) override {
        proton::message msg;
        msg.body(body_);
        msg.content_type("application/json");
        // Tell the controller exactly which queue to reply to.
        // This prevents competing subscribers from consuming each other's
        // TASK_ACCEPTED messages off the shared response queue.
        if (!recv_addr_.empty())
            msg.reply_to(recv_addr_);
        s.send(msg);
        if (recv_addr_.empty()) {
            result_.received = true;
            s.connection().close();
        }
    }
    void on_message(proton::delivery& d, proton::message& msg) override {
        // Always accept (ACK) messages to avoid stale redelivery across sessions.
        d.accept();
        try {
            std::string b = proton::get<std::string>(msg.body());
            auto j = json::parse(b);
            // Match by request_id (controller echoes request_id in response)
            if (j.value("request_id", "") == corr_id_ ||
                j.value("correlation_id", "") == corr_id_) {
                result_.received = true;
                result_.body     = b;
                d.connection().close();
            }
            // Non-matching messages are acknowledged and discarded (stale responses).
        } catch (...) {}
    }
    void on_transport_error(proton::transport&) override {}
    void on_connection_error(proton::connection&) override {}

private:
    std::string    url_, username_, password_, send_addr_, recv_addr_, body_, corr_id_;
    int            timeout_sec_;
    AmqpResponse   result_;
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

TaskManagerIqSource::TaskManagerIqSource(const SweepConfig& cfg) : cfg_(cfg) {
    pkt_buf_.resize(65536);
    resetAccum();
}
TaskManagerIqSource::~TaskManagerIqSource() { close(); }

void TaskManagerIqSource::resetAccum() {
    ch_accum_.assign((size_t)cfg_.device.rx_channels, {});
    // Pre-reserve so packet inserts never reallocate mid-dwell.
    for (auto& buf : ch_accum_)
        buf.reserve(static_cast<size_t>(cfg_.sweep.dwell_samples));
    current_center_hz_ = 0;
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
    double step_hz = cfg_.device.sample_rate * cfg_.sweep.usable_bw_fraction;
    int    dwell_ms = static_cast<int>(
        cfg_.sweep.dwell_samples * 1000.0 / cfg_.device.sample_rate);
    dwell_ms = std::max(dwell_ms, 1);

    json entries = json::array();
    int step_i = 0;
    for (uint64_t pos = cfg_.sweep.start_hz; pos < cfg_.sweep.stop_hz; pos += (uint64_t)step_hz) {
        double center = pos + step_hz / 2.0;
        entries.push_back({
            {"step",            step_i++},
            {"center_freq_hz",  center},
            {"bandwidth_hz",    cfg_.device.bandwidth_hz},
            {"sample_rate_sps", cfg_.device.sample_rate},
            {"dwell_ms",        dwell_ms}
        });
    }

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
            {"center_freq_hz",  (cfg_.sweep.start_hz + cfg_.sweep.stop_hz) / 2.0},
            {"bandwidth_hz",    cfg_.device.bandwidth_hz},
            {"sample_rate_sps", cfg_.device.sample_rate},
            {"rx_count",        cfg_.device.rx_channels},
            {"rx_gain_db",      gains}
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

    SyncAmqpExchange exchange(
        cfg_.amqp.url, cfg_.amqp.username, cfg_.amqp.password,
        cfg_.amqp.task_request_queue,
        cfg_.amqp.task_response_queue,
        body, req_id, 30);

    auto resp = exchange.run();
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
    SyncAmqpExchange exchange(
        cfg_.amqp.url, cfg_.amqp.username, cfg_.amqp.password,
        cfg_.amqp.task_request_queue,
        "",  // no reply expected
        msg.dump(), req_id, 5);
    exchange.run();
    spdlog::info("[TaskMgrSrc] TASK_STOP sent for task_id={}", task_id_);
    task_id_.clear();
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
    //  • Before the first packet arrives the SCAN task may sit in PENDING
    //    while AnalysisApp finishes a WIDEBAND task — allow up to 60 s.
    //  • After data starts flowing the inter-dwell gap (PLL calibration) is
    //    3-4 s; allow 20 s before declaring the stream ended.
    static constexpr int INITIAL_TIMEOUT_MS = 30000;
    static constexpr int STREAM_TIMEOUT_MS  =  5000;
    int no_data_ms     = 0;
    bool first_packet  = true;

    while (running_) {
        ssize_t n = ::recvfrom(udp_fd_, pkt_buf_.data(), pkt_buf_.size(), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                no_data_ms += 100;
                int limit = first_packet ? INITIAL_TIMEOUT_MS : STREAM_TIMEOUT_MS;
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
        first_packet = false;
        no_data_ms   = 0;
        if (static_cast<size_t>(n) < sizeof(IqPacketHeader)) continue;

        const auto& hdr = *reinterpret_cast<const IqPacketHeader*>(pkt_buf_.data());
        if (hdr.magic != IQ_MAGIC) continue;

        const auto* samples = reinterpret_cast<const std::complex<float>*>(
            pkt_buf_.data() + sizeof(IqPacketHeader));
        int n_samp = hdr.num_samples;
        int ch     = hdr.channel_index;

        // IQ_FLAG_DWELL_CHANGE: first packet of a new dwell position.
        // Return the accumulated dwell (if any), then reset.
        if ((hdr.flags & FLAG_DWELL_CHANGE) && current_center_hz_ != 0) {
            bool has_data = false;
            for (auto& buf : ch_accum_)
                if (!buf.empty()) { has_data = true; break; }

            if (has_data) {
                d.center_hz  = current_center_hz_;
                d.ch_samples = std::move(ch_accum_);
                resetAccum();
                current_center_hz_ = hdr.center_freq_hz;
                // Buffer the new-dwell samples for next call
                if (ch < (int)ch_accum_.size())
                    ch_accum_[ch].insert(ch_accum_[ch].end(), samples, samples + n_samp);
                return true;
            }
        }

        current_center_hz_ = hdr.center_freq_hz;
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
