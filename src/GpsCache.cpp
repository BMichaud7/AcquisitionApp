/*
========================================================================
Project: OpenRFStack
Author:  Brendan Michaud
Year:    2026
Part of OpenRFStack (https://github.com/OpenRFStack)

Licensed under the Personal Use License.
Do not use for commercial, organizational, or military purposes.
========================================================================
*/
#include "GpsCache.hpp"
#include <nlohmann/json.hpp>
#include <proton/connection_options.hpp>
#include <proton/reconnect_options.hpp>
#include <spdlog/spdlog.h>

namespace acq {

using json = nlohmann::json;

GpsCache::GpsCache(std::string url, std::string username, std::string password,
                   std::string topic)
    : url_(std::move(url)), username_(std::move(username)),
      password_(std::move(password)), topic_(std::move(topic))
{}

GpsCache::~GpsCache() { stop(); }

void GpsCache::start() {
    container_ = new proton::container(*this);
    thread_    = std::thread([this]{ container_->run(); });
}

void GpsCache::stop() {
    stopping_ = true;
    if (container_) {
        container_->stop();
        if (thread_.joinable()) thread_.join();
        delete container_;
        container_ = nullptr;
    }
}

std::optional<GpsFix> GpsCache::get() const {
    std::lock_guard lock(mutex_);
    return fix_;
}

void GpsCache::on_container_start(proton::container& c) {
    proton::connection_options opts;
    if (!username_.empty()) {
        opts.sasl_allowed_mechs("PLAIN");
        opts.sasl_allow_insecure_mechs(true);
        opts.user(username_).password(password_);
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

void GpsCache::on_connection_open(proton::connection& conn) {
    conn.open_receiver(topic_);
    spdlog::debug("[GpsCache] subscribed to {}", topic_);
}

std::optional<GpsFix> GpsCache::parse_fix(const std::string& json_str) noexcept {
    try {
        auto j = json::parse(json_str);
        GpsFix fix;
        fix.lat   = j.value("latitude_deg",  0.0);
        fix.lon   = j.value("longitude_deg", 0.0);
        fix.alt_m = j.value("altitude_m",    0.0);
        return fix;
    } catch (...) {
        return std::nullopt;
    }
}

void GpsCache::on_message(proton::delivery&, proton::message& msg) {
    std::string body;
    try { body = proton::get<std::string>(msg.body()); }
    catch (const std::exception& e) {
        spdlog::warn("[GpsCache] bad message body type: {}", e.what());
        return;
    }
    auto fix = parse_fix(body);
    if (!fix) {
        spdlog::warn("[GpsCache] bad message — not valid JSON");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        fix_ = fix;
    }
    spdlog::trace("[GpsCache] {:.6f},{:.6f} alt={:.1f}m",
                  fix->lat, fix->lon, fix->alt_m);
}

void GpsCache::on_transport_error(proton::transport& t) {
    if (!stopping_)
        spdlog::debug("[GpsCache] transport error: {}", t.error().what());
}

void GpsCache::on_connection_error(proton::connection& c) {
    if (!stopping_)
        spdlog::debug("[GpsCache] connection error: {}", c.error().what());
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
