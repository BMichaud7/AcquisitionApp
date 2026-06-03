#pragma once
/**
 * @file AlertStore.hpp
 * @brief Persists RfAlert records to the rf_alerts PostgreSQL table.
 */
#include "RfAlert.hpp"
#include <pqxx/pqxx>
#include <string>

namespace acq {

class AlertStore {
public:
    explicit AlertStore(const std::string& conn_str);

    /// Insert an alert. No-op if the database connection is unavailable.
    void insert(const RfAlert& alert);

private:
    pqxx::connection conn_;
};

} // namespace acq
