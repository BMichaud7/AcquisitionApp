#include "AlertStore.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace acq {

AlertStore::AlertStore(const std::string& conn_str) : conn_(conn_str) {}

void AlertStore::insert(const RfAlert& alert) {
    try {
        pqxx::work tx(conn_);
        // Build nullable fields as SQL literals to avoid std::optional<T>
        // which triggers pqxx 7.x type-registration static initializers that
        // reference pqxx::internal::demangle_type_name (not exported on all distros).
        std::string freq_sql    = alert.freq_hz > 0.0
            ? std::to_string(alert.freq_hz / 1e6) : "NULL";
        std::string power_sql   = alert.power_db != 0.0f
            ? std::to_string(static_cast<double>(alert.power_db)) : "NULL";
        std::string base_sql    = alert.baseline_db != 0.0f
            ? std::to_string(static_cast<double>(alert.baseline_db)) : "NULL";
        std::string scanner_sql = alert.scanner_id.empty()
            ? "NULL" : tx.quote(alert.scanner_id);

        tx.exec0(
            "INSERT INTO rf_alerts "
            "(alert_type, severity, freq_mhz, power_db, baseline_db, scanner_id, details) "
            "VALUES (" +
            tx.quote(std::string(alertTypeName(alert.type))) + "," +
            tx.quote(std::string(severityName(alert.severity))) + "," +
            freq_sql + "," + power_sql + "," + base_sql + "," +
            scanner_sql + "," +
            tx.quote(alert.details) + ")");
        tx.commit();
        spdlog::warn("[ALERT] {} {} — {}", severityName(alert.severity),
                     alertTypeName(alert.type), alert.details);
    } catch (const std::exception& e) {
        spdlog::error("[AlertStore] insert failed: {}", e.what());
    }
}

} // namespace acq
