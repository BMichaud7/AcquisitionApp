#include "TaskManagerIqSource.hpp"
#include <sdr/Types.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection.hpp>
#include <proton/sender.hpp>
#include <proton/receiver.hpp>
#include <proton/delivery.hpp>
#include <proton/work_queue.hpp>
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
        if (!username_.empty()) opts.user(username_);
        if (!password_.empty()) opts.password(password_);
        c.connect(url_, opts);
    }
    void on_connection_open(proton::connection& conn) override {
        conn.open_sender(send_addr_);
        if (!recv_addr_.empty())
            conn.open_receiver(recv_addr_);
    }
    void on_sender_open(proton::sender& s) override {
        proton::message msg;
        msg.body(body_);
        msg.content_type("application/json");
        s.send(msg);
        if (recv_addr_.empty()) {
            result_.received = true;
            s.connection().close();
        }
    }
    void on_message(proton::delivery& d, proton::message& msg) override {
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
    resetAccum();
}
TaskManagerIqSource::~TaskManagerIqSource() { close(); }

void TaskManagerIqSource::resetAccum() {
    ch_accum_.assign((size_t)cfg_.device.rx_channels, {});
    current_center_hz_ = 0;
}

void TaskManagerIqSource::bindUdp() {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0)
        throw std::runtime_error(
            std::string("UDP socket() failed: ") + strerror(errno));

    // Set receive timeout so next() can check the stop flag
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100'000};  // 100 ms
    setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg_.receiver.port));
    inet_aton(cfg_.receiver.local_ip.c_str(), &addr.sin_addr);

    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(udp_fd_); udp_fd_ = -1;
        throw std::runtime_error(
            std::string("UDP bind() failed: ") + strerror(errno));
    }

    socklen_t len = sizeof(addr);
    ::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);
    spdlog::info("[TaskMgrSrc] UDP socket bound on {}:{}", cfg_.receiver.local_ip, bound_port_);
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
        {"streaming", {
            {"dest_ip",    cfg_.receiver.local_ip == "0.0.0.0"
                           ? "127.0.0.1" : cfg_.receiver.local_ip},
            {"dest_ports", json::array({bound_port_})}
        }},
        {"scan_params", {
            {"repeat",  true},
            {"entries", entries}
        }}
    };
    return req.dump();
}

void TaskManagerIqSource::submitTask() {
    std::string req_id = makeReqId();
    std::string body   = buildScanRequest(req_id);

    spdlog::info("[TaskMgrSrc] submitting SCAN task (req_id={})", req_id);

    SyncAmqpExchange exchange(
        cfg_.amqp.url, cfg_.amqp.username, cfg_.amqp.password,
        cfg_.amqp.task_request_queue,
        cfg_.amqp.task_response_queue,
        body, req_id, 10);

    auto resp = exchange.run();
    if (!resp.received)
        throw std::runtime_error("Task request timed out — is the controller running?");

    auto j = json::parse(resp.body);
    if (!j.value("accepted", false)) {
        throw std::runtime_error(
            "Task rejected: " + j.value("reject_reason", "unknown"));
    }
    task_id_ = j.value("task_id", "");
    spdlog::info("[TaskMgrSrc] task accepted (task_id={})", task_id_);
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
    bindUdp();
    submitTask();
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
    constexpr size_t MAX_PKT = 65536;
    std::vector<uint8_t> pkt(MAX_PKT);

    while (running_) {
        ssize_t n = ::recvfrom(udp_fd_, pkt.data(), pkt.size(), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            spdlog::error("[TaskMgrSrc] recvfrom error: {}", strerror(errno));
            return false;
        }
        if (static_cast<size_t>(n) < sizeof(IqPacketHeader)) continue;

        const auto& hdr = *reinterpret_cast<const IqPacketHeader*>(pkt.data());
        if (hdr.magic != IQ_MAGIC) continue;

        const auto* samples = reinterpret_cast<const std::complex<float>*>(
            pkt.data() + sizeof(IqPacketHeader));
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
