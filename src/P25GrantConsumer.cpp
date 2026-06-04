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
/**
 * @file P25GrantConsumer.cpp
 * @brief P25 channel grant AMQP consumer with SDR retune on grant.
 */
#include "P25GrantConsumer.hpp"
#include <proton/sender.hpp>

using json = nlohmann::json;

namespace acq {

// ── AMQP handler ──────────────────────────────────────────────────────────────

class P25GrantConsumer::Handler : public proton::messaging_handler {
public:
    explicit Handler(P25GrantConsumer& owner) : owner_(owner) {}

    void on_container_start(proton::container& c) override {
        proton::connection_options copts;
        if (!owner_.user_.empty()) copts.user(owner_.user_);
        if (!owner_.pass_.empty()) copts.password(owner_.pass_);
        c.connect(owner_.url_, copts);
    }

    void on_connection_open(proton::connection& conn) override {
        conn.open_receiver(owner_.topic_,
            proton::receiver_options().source(
                proton::source_options().address(owner_.topic_)));
        spdlog::info("[P25GrantConsumer] subscribed to {} on {}",
                     owner_.topic_, owner_.url_);
    }

    void on_message(proton::delivery& d, proton::message& m) override {
        try {
            auto j = json::parse(m.body().get<std::string>());
            if (j.value("msg_type", "") != "P25_CHANNEL_GRANT") { d.accept(); return; }

            P25Grant g;
            g.talk_group = j.value("talk_group", 0u);
            g.source_id  = j.value("source_id",  0u);
            g.freq_hz    = j.value("freq_hz",     0.0);
            g.encrypted  = j.value("encrypted",   false);
            g.emergency  = j.value("emergency",   false);
            g.ts_ms      = j.value("timestamp_ms", (int64_t)0);
            g.alg_id     = static_cast<uint8_t>(j.value("alg_id",  0xFF));
            g.key_id     = static_cast<uint16_t>(j.value("key_id", 0));
            g.alg_name   = j.value("alg_name", std::string{});

            if (g.freq_hz <= 0) { d.accept(); return; }

            spdlog::info("[P25GrantConsumer] grant TG={} freq={:.4f}MHz enc={} alg={}",
                         g.talk_group, g.freq_hz / 1e6, g.encrypted,
                         g.alg_name.empty() ? "unknown" : g.alg_name);
            owner_.enqueue(g);
        } catch (const std::exception& e) {
            spdlog::warn("[P25GrantConsumer] bad message: {}", e.what());
        }
        d.accept();
    }

    P25GrantConsumer& owner_;
};

// ── Construction ──────────────────────────────────────────────────────────────

P25GrantConsumer::P25GrantConsumer(std::string amqp_url,
                                   std::string username,
                                   std::string password,
                                   std::string grant_topic,
                                   GrantHandler on_grant)
    : url_(std::move(amqp_url))
    , user_(std::move(username))
    , pass_(std::move(password))
    , topic_(std::move(grant_topic))
    , on_grant_(std::move(on_grant))
{}

P25GrantConsumer::~P25GrantConsumer() { stop(); }

void P25GrantConsumer::start() {
    running_ = true;

    // Worker thread: dispatches grant handling outside the AMQP event loop
    worker_thread_ = std::thread([this] { worker_loop(); });

    // AMQP thread
    auto* h = new Handler(*this);
    container_ = std::make_unique<proton::container>(*h);
    amqp_thread_ = std::thread([this] { container_->run(); });
}

void P25GrantConsumer::stop() {
    running_ = false;
    queue_cv_.notify_all();
    if (container_) { container_->stop(); }
    if (amqp_thread_.joinable()) amqp_thread_.join();
    if (worker_thread_.joinable()) worker_thread_.join();
    container_.reset();
}

void P25GrantConsumer::enqueue(P25Grant g) {
    {
        std::lock_guard lk(queue_mu_);
        // Limit queue depth — drop older grants if backed up
        if (queue_.size() > 4) {
            spdlog::warn("[P25GrantConsumer] grant queue full, dropping oldest");
            queue_.pop();
        }
        queue_.push(std::move(g));
    }
    queue_cv_.notify_one();
}

void P25GrantConsumer::worker_loop() {
    while (running_) {
        std::unique_lock lk(queue_mu_);
        queue_cv_.wait(lk, [this] { return !queue_.empty() || !running_; });
        if (!running_ && queue_.empty()) break;

        P25Grant g = queue_.front();
        queue_.pop();
        lk.unlock();

        try {
            on_grant_(g);
        } catch (const std::exception& e) {
            spdlog::error("[P25GrantConsumer] grant handler error: {}", e.what());
        }
    }
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
