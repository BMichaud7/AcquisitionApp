#include "AlertStore.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace acq {

AlertStore::AlertStore(const std::string& conn_str) : conn_(conn_str) {}

void AlertStore::insert(const RfAlert& alert) {
    try {
        pqxx::work tx(conn_);
        tx.exec_params(
            "INSERT INTO rf_alerts "
            "(alert_type, severity, freq_mhz, power_db, baseline_db, scanner_id, details) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7)",
            alertTypeName(alert.type),
            severityName(alert.severity),
            alert.freq_hz > 0.0 ? std::make_optional(alert.freq_hz / 1e6) : std::nullopt,
            alert.power_db != 0.0f ? std::make_optional(static_cast<double>(alert.power_db)) : std::nullopt,
            alert.baseline_db != 0.0f ? std::make_optional(static_cast<double>(alert.baseline_db)) : std::nullopt,
            alert.scanner_id.empty() ? std::nullopt : std::make_optional(alert.scanner_id),
            alert.details);
        tx.commit();
        spdlog::warn("[ALERT] {} {} — {}", severityName(alert.severity),
                     alertTypeName(alert.type), alert.details);
    } catch (const std::exception& e) {
        spdlog::error("[AlertStore] insert failed: {}", e.what());
    }
}

} // namespace acq
