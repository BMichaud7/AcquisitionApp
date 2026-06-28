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
#include "AmqpPublisher.hpp"
#include <au/units/hertz.hh>
#include <sdr/Base64.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <proton/reconnect_options.hpp>
#include <chrono>

namespace acq {

using json = nlohmann::json;
using namespace std::chrono;

AmqpPublisher::AmqpPublisher(std::string url, std::string username, std::string password,
                             std::string topic, std::string scanner_id)
    : url_(std::move(url)), username_(std::move(username)), password_(std::move(password)),
      topic_(std::move(topic)), scanner_id_(std::move(scanner_id))
{}

AmqpPublisher::~AmqpPublisher() { stop(); }

void AmqpPublisher::start() {
    container_ = new proton::container(*this);
    thread_ = std::thread([this]{ container_->run(); });
}

void AmqpPublisher::stop() {
    stopping_ = true;
    if (container_) {
        if (work_queue_)
            work_queue_->add([this]{ sender_.connection().close(); });
        else
            // Connection never reached on_sender_open, so there's no work
            // queue to post a close through. reconnect_options above sets
            // max_attempts(0) — infinite retries — so without this,
            // container_->run() never returns and thread_.join() blocks
            // forever.
            container_->stop();
        if (thread_.joinable()) thread_.join();
        delete container_;
        container_ = nullptr;
    }
}

void AmqpPublisher::publish(const Detection& d) {
    if (!work_queue_ || stopping_) return;
    proton::message msg = makeMessage(d);
    work_queue_->add([this, msg]() mutable {
        if (sender_) sender_.send(msg);
    });
}

void AmqpPublisher::on_container_start(proton::container& c) {
    proton::connection_options opts;
    if (!username_.empty()) {
        opts.sasl_allowed_mechs("PLAIN");
        opts.sasl_allow_insecure_mechs(true);
        opts.user(username_).password(password_);
    } else {
        opts.sasl_allowed_mechs("ANONYMOUS");
    }
    // Without this, a failed initial connection (e.g. broker not up yet) is
    // permanent — proton tears down the container and the publisher never
    // recovers. Retry indefinitely with backoff, matching TaskAmqpChannel.
    proton::reconnect_options ropts;
    ropts.delay(proton::duration(2000));
    ropts.max_delay(proton::duration(30000));
    ropts.max_attempts(0);
    opts.reconnect(ropts);
    c.connect(url_, opts);
}

void AmqpPublisher::on_connection_open(proton::connection& conn) {
    conn.open_sender(topic_);
}

void AmqpPublisher::on_sender_open(proton::sender& s) {
    sender_     = s;
    work_queue_ = &s.work_queue();
    spdlog::info("[AmqpPublisher] connected → {}", topic_);
}

void AmqpPublisher::on_transport_error(proton::transport& t) {
    spdlog::warn("[AmqpPublisher] transport error: {}", t.error().what());
}

void AmqpPublisher::on_connection_error(proton::connection& c) {
    spdlog::warn("[AmqpPublisher] connection error: {}", c.error().what());
}

proton::message AmqpPublisher::makeMessage(const Detection& d) const {
    auto ms = duration_cast<milliseconds>(
        d.timestamp.time_since_epoch()).count();

    json body = {
        {"msg_type",        "RF_DETECTION"},
        {"schema_version",  SCHEMA_VERSION},
        {"timestamp_ms",    ms},
        {"scanner_id",      d.scanner_id},
        {"center_freq_hz",  d.center_freq_hz.in(au::hertz)},
        {"bandwidth_hz",    d.bandwidth_hz.in(au::hertz)},
        {"power_db",        d.power_db},
        {"snr_db",          d.snr_db},
        {"channel",         d.channel}
    };

    // IQ snapshot: base64-encoded raw float32 bytes (~11 KB) instead of a
    // JSON float array (~28 KB). Receivers decode with sdr::base64::decodeFloats().
    // Backward-compatible decoders fall back to "iq_snapshot" (JSON array).
    if (d.iq_snapshot && !d.iq_snapshot->empty()) {
        body["iq_snapshot_b64"] = sdr::base64::encode(
            d.iq_snapshot->data(),
            d.iq_snapshot->size() * sizeof(float));
        body["snapshot_sample_rate_sps"] = d.snapshot_sample_rate_sps.in(au::hertz);
    }

    proton::message msg;
    msg.body(body.dump());
    msg.content_type("application/json");
    return msg;
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
