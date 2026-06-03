#pragma once
/**
 * @file AlertStore.hpp
 * @brief PostgreSQL persistence for RF threat alerts.
 *
 * AlertStore owns a single persistent @c pqxx::connection and serialises
 * RfAlert records into the @c rf_alerts table.  It is not thread-safe; the
 * caller (main loop or AlertConsumer callback) must ensure single-threaded
 * access, or wrap with a mutex.
 *
 * @par Schema
 * The @c rf_alerts table is created by @c schema/init.sql:
 * @code{.sql}
 * CREATE TABLE rf_alerts (
 *   id          BIGSERIAL PRIMARY KEY,
 *   detected_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 *   alert_type  TEXT NOT NULL,
 *   severity    TEXT NOT NULL,
 *   freq_mhz    DOUBLE PRECISION,
 *   power_db    REAL,
 *   baseline_db REAL,
 *   scanner_id  TEXT,
 *   details     TEXT NOT NULL
 * );
 * @endcode
 *
 * @see RfAlert, AlertConsumer
 */
#include "RfAlert.hpp"
#include <pqxx/pqxx>
#include <string>

namespace acq {

/**
 * @class AlertStore
 * @brief Writes RfAlert records to the @c rf_alerts PostgreSQL table.
 *
 * Constructed with a libpqxx connection string (same format as DetectionDb).
 * Each call to insert() opens a transaction, inserts one row, and commits.
 * If the database is unavailable the error is logged and the alert is dropped
 * silently — the system continues operating without threat persistence.
 */
class AlertStore {
public:
    /**
     * @brief Construct an AlertStore and open the database connection.
     * @param conn_str libpqxx connection string, e.g.
     *   @c "host=localhost port=5432 dbname=sdr_scanner user=sdr password=…"
     * @throws pqxx::broken_connection if the database cannot be reached.
     */
    explicit AlertStore(const std::string& conn_str);

    /**
     * @brief Insert one alert into @c rf_alerts.
     *
     * A new transaction is opened and committed for each call.
     * On failure the exception is caught, logged via spdlog, and swallowed.
     * @param alert The alert to persist.
     */
    void insert(const RfAlert& alert);

private:
    pqxx::connection conn_; ///< Persistent database connection.
};

} // namespace acq
