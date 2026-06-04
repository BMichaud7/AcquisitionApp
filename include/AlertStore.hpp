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
#pragma once
/**
 * @file AlertStore.hpp
 * @brief PostgreSQL persistence for RF threat alerts.
 *
 * Uses the pimpl idiom: <pqxx/pqxx> is included only in AlertStore.cpp,
 * never in this header.  This prevents pqxx 7.x's std::optional type-converter
 * static initialisers from firing in translation units that include AlertStore.hpp
 * alongside headers that pull in <optional> (proton, spdlog, etc.).
 *
 * @see RfAlert, AlertConsumer
 */
#include "RfAlert.hpp"
#include <memory>
#include <string>

// Forward-declare pqxx::connection so we can hold a unique_ptr without
// including <pqxx/pqxx> in this header.
namespace pqxx { class connection; }

namespace acq {

/**
 * @class AlertStore
 * @brief Writes RfAlert records to the @c rf_alerts PostgreSQL table.
 *
 * Constructed with a libpqxx connection string.  Each call to insert()
 * opens a transaction, inserts one row, and commits.  On failure the
 * exception is caught, logged via spdlog, and swallowed so the calling
 * thread is never interrupted by a transient database error.
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

    /// @brief Destructor — closes the pqxx::connection (defined in .cpp).
    ~AlertStore();

    /**
     * @brief Insert one alert into @c rf_alerts.
     *
     * A new transaction is opened and committed for each call.
     * On failure the exception is caught, logged, and swallowed.
     * @param alert The alert record to persist.
     */
    void insert(const RfAlert& alert);

private:
    /// Pimpl: pqxx::connection is only a complete type in AlertStore.cpp.
    std::unique_ptr<pqxx::connection> conn_;
};

} // namespace acq
