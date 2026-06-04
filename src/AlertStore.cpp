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
// <pqxx/pqxx> is intentionally included ONLY here, not in AlertStore.hpp.
// This prevents pqxx 7.x's std::optional type-converter static initialisers
// from firing in TUs that include AlertStore.hpp alongside headers that pull
// in <optional> (proton message.hpp, spdlog, etc.).
#include "AlertStore.hpp"
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

namespace acq {

AlertStore::AlertStore(const std::string& conn_str)
    : conn_(std::make_unique<pqxx::connection>(conn_str)) {}

// Destructor must be in .cpp so pqxx::connection is a complete type when deleted.
AlertStore::~AlertStore() = default;

void AlertStore::insert(const RfAlert& alert) {
    try {
        pqxx::work tx(*conn_);
        // Build nullable fields as SQL literals to avoid std::optional<T>
        // which also triggers the demangle_type_name static initialisers.
        std::string freq_sql  = alert.freq_hz > 0.0
            ? std::to_string(alert.freq_hz / 1e6) : "NULL";
        std::string power_sql = alert.power_db != 0.0f
            ? std::to_string(static_cast<double>(alert.power_db)) : "NULL";
        std::string base_sql  = alert.baseline_db != 0.0f
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
