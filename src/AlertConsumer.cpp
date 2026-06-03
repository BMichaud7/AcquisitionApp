#include "AlertConsumer.hpp"
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/delivery.hpp>
#include <proton/message.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace acq {
using json = nlohmann::json;

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
        auto j = json::parse(body, nullptr, false);
        if (j.is_discarded()) return;

        // Map JSON fields to RfAlert
        static const std::unordered_map<std::string, AlertType> type_map = {
            {"GPS_JAMMING",    AlertType::GPS_JAMMING},
            {"GPS_SPOOFING",   AlertType::GPS_SPOOFING},
            {"ADSB_SPOOFING",  AlertType::ADSB_SPOOFING},
            {"ADSB_JAMMING",   AlertType::ADSB_JAMMING},
            {"AIS_SPOOFING",   AlertType::AIS_SPOOFING},
            {"EAS_SPOOFING",   AlertType::EAS_SPOOFING},
            {"DSC_SPOOFING",   AlertType::DSC_SPOOFING},
            {"P25_ROGUE_SITE", AlertType::P25_ROGUE_SITE},
        };
        static const std::unordered_map<std::string, AlertSeverity> sev_map = {
            {"LOW", AlertSeverity::LOW}, {"MEDIUM", AlertSeverity::MEDIUM},
            {"HIGH", AlertSeverity::HIGH}, {"CRITICAL", AlertSeverity::CRITICAL},
        };

        RfAlert alert;
        auto t_it = type_map.find(j.value("type", ""));
        if (t_it == type_map.end()) return;
        alert.type     = t_it->second;
        auto s_it = sev_map.find(j.value("severity", "MEDIUM"));
        alert.severity = (s_it != sev_map.end()) ? s_it->second : AlertSeverity::MEDIUM;
        alert.freq_hz  = j.value("freq_hz", 0.0);
        alert.power_db = j.value("power_db", 0.0f);
        alert.details  = j.value("details", "");
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
