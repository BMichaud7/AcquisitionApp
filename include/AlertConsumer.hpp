#pragma once
/**
 * @file AlertConsumer.hpp
 * @brief AMQP subscriber that persists threat alerts from DemodApp to PostgreSQL.
 *
 * DemodApp's protocol validators (ADS-B, AIS, EAS, DSC, P25) publish RF_ALERT
 * JSON messages to the @c rf.alerts AMQP topic.  AlertConsumer subscribes to
 * that topic, parses each message, and calls AlertStore::insert() to write it
 * to the @c rf_alerts PostgreSQL table alongside GPS alerts from GpsMonitor.
 *
 * @par Message format expected on rf.alerts
 * @code{.json}
 * {
 *   "type":     "ADSB_SPOOFING",
 *   "severity": "HIGH",
 *   "freq_hz":  1090000000.0,
 *   "power_db": -72.5,
 *   "details":  "ICAO ABCDEF impossible groundspeed 2200 knots"
 * }
 * @endcode
 *
 * @see AlertStore, AlertPublisher (DemodApp), ThreatConfig
 */
#include "AlertStore.hpp"
#include <proton/messaging_handler.hpp>
#include <proton/container.hpp>
#include <proton/receiver.hpp>
#include <thread>
#include <string>
#include <atomic>

namespace acq {

/**
 * @class AlertConsumer
 * @brief Background AMQP receiver for @c rf.alerts messages.
 *
 * Runs a @c proton::container in a dedicated thread.  Call start() before
 * the main loop and stop() during shutdown.  Thread-safe: the AMQP callback
 * and AlertStore::insert() both execute on the proton thread.
 */
class AlertConsumer : public proton::messaging_handler {
public:
    /**
     * @brief Construct the consumer.
     * @param url   AMQP broker URL (e.g. @c "amqp://localhost:5672").
     * @param user  AMQP username (empty = anonymous auth).
     * @param pass  AMQP password.
     * @param store Reference to the AlertStore that will persist incoming alerts.
     * @param topic AMQP address to subscribe to (default: @c "rf.alerts").
     */
    AlertConsumer(std::string url, std::string user, std::string pass,
                  AlertStore& store, std::string topic = "rf.alerts");

    /// @brief Stop the receiver and join the AMQP thread.
    ~AlertConsumer();

    /// @brief Start the background AMQP receiver thread.
    void start();

    /// @brief Request a clean shutdown and join the thread.
    void stop();

private:
    void on_container_start(proton::container& c) override;
    void on_connection_open(proton::connection& c) override;
    void on_message(proton::delivery& d, proton::message& msg) override;
    void on_error(const proton::error_condition& e) override;

    std::string       url_;      ///< Broker URL.
    std::string       user_;     ///< AMQP username.
    std::string       pass_;     ///< AMQP password.
    std::string       topic_;    ///< Subscribed topic address.
    AlertStore&       store_;    ///< Alert persistence backend.
    proton::container container_;///< Proton messaging container.
    std::thread       thread_;   ///< Thread running container_.run().
    std::atomic<bool> stopping_{false}; ///< Set true by stop() to suppress error logs.
};

} // namespace acq
