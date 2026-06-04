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
#include "AlertConsumer.hpp"
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/delivery.hpp>
#include <proton/message.hpp>
#include <spdlog/spdlog.h>
// NOTE: deliberately no <nlohmann/json.hpp> here — combining <pqxx/pqxx>
// (pulled in via AlertStore.hpp) with <optional> (pulled in by nlohmann)
// in the same TU triggers pqxx 7.x's std::optional type-converter static
// initialiser, which references pqxx::internal::demangle_type_name — not
// exported by the CI distro's libpqxx binary.  Parse the simple alert JSON
// with plain string operations instead.

namespace acq {

// ── Minimal JSON field extractor (no deps on nlohmann / optional) ─────────────
static std::string jsonStr(const std::string& body, const char* key) {
    std::string search = std::string("\"") + key + "\":\"";
    auto pos = body.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    auto end = body.find('"', pos);
    return end == std::string::npos ? std::string{} : body.substr(pos, end - pos);
}

static double jsonDouble(const std::string& body, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return 0.0;
    pos += search.size();
    try { return std::stod(body.substr(pos)); } catch (...) { return 0.0; }
}

AlertConsumer::AlertConsumer(std::string url, std::string user, std::string pass,
                             AlertStore& store, std::string topic)
    : url_(std::move(url)), user_(std::move(user)), pass_(std::move(pass)),
      store_(store), topic_(std::move(topic)), container_(*this) {}

AlertConsumer::~AlertConsumer() { stop(); }

void AlertConsumer::start() {
    thread_ = std::thread([this]{ container_.run(); });
}

void AlertConsumer::stop() {
    stopping_ = true;
    container_.stop();
    if (thread_.joinable()) thread_.join();
}

void AlertConsumer::on_container_start(proton::container& c) {
    proton::connection_options opts;
    if (!user_.empty()) {
        opts.sasl_allowed_mechs("PLAIN");
        opts.sasl_allow_insecure_mechs(true);
        opts.user(user_).password(pass_);
    } else {
        opts.sasl_allowed_mechs("ANONYMOUS");
    }
    c.connect(url_, opts);
}

void AlertConsumer::on_connection_open(proton::connection& c) {
    c.open_receiver(topic_);
    spdlog::info("[AlertConsumer] subscribed → {}", topic_);
}

void AlertConsumer::on_message(proton::delivery& d, proton::message& msg) {
    d.accept();
    try {
        auto body = proton::get<std::string>(msg.body());
        if (body.empty()) return;

        std::string type_str = jsonStr(body, "type");
        std::string sev_str  = jsonStr(body, "severity");
        std::string details  = jsonStr(body, "details");
        double freq_hz       = jsonDouble(body, "freq_hz");

        // Map type string to AlertType
        static const std::pair<const char*, AlertType> type_map[] = {
            {"GPS_JAMMING",    AlertType::GPS_JAMMING},
            {"GPS_SPOOFING",   AlertType::GPS_SPOOFING},
            {"ADSB_SPOOFING",  AlertType::ADSB_SPOOFING},
            {"ADSB_JAMMING",   AlertType::ADSB_JAMMING},
            {"AIS_SPOOFING",   AlertType::AIS_SPOOFING},
            {"EAS_SPOOFING",   AlertType::EAS_SPOOFING},
            {"DSC_SPOOFING",   AlertType::DSC_SPOOFING},
            {"P25_ROGUE_SITE", AlertType::P25_ROGUE_SITE},
        };
        AlertType type = AlertType::GPS_JAMMING;
        bool found = false;
        for (const auto& [k, v] : type_map) {
            if (type_str == k) { type = v; found = true; break; }
        }
        if (!found) return;

        AlertSeverity sev = AlertSeverity::MEDIUM;
        if      (sev_str == "LOW")      sev = AlertSeverity::LOW;
        else if (sev_str == "HIGH")     sev = AlertSeverity::HIGH;
        else if (sev_str == "CRITICAL") sev = AlertSeverity::CRITICAL;

        RfAlert alert;
        alert.type     = type;
        alert.severity = sev;
        alert.freq_hz  = freq_hz;
        alert.details  = details;
        store_.insert(alert);
    } catch (const std::exception& e) {
        spdlog::warn("[AlertConsumer] parse error: {}", e.what());
    }
}

void AlertConsumer::on_error(const proton::error_condition& e) {
    if (!stopping_)
        spdlog::warn("[AlertConsumer] error: {}", e.what());
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
