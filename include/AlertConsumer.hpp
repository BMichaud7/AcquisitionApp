#pragma once
/**
 * @file AlertConsumer.hpp
 * @brief AMQP subscriber for rf.alerts topic — persists alerts to PostgreSQL.
 *
 * Receives RF_ALERT JSON messages published by DemodApp's validators
 * (ADS-B, AIS, EAS, DSC, P25) and writes them to the rf_alerts table
 * via AlertStore.
 */
#include "AlertStore.hpp"
#include <proton/messaging_handler.hpp>
#include <proton/container.hpp>
#include <proton/receiver.hpp>
#include <thread>
#include <string>
#include <atomic>

namespace acq {

class AlertConsumer : public proton::messaging_handler {
public:
    AlertConsumer(std::string url, std::string user, std::string pass,
                  AlertStore& store, std::string topic = "rf.alerts");
    ~AlertConsumer();

    void start();
    void stop();

private:
    void on_container_start(proton::container& c) override;
    void on_connection_open(proton::connection& c) override;
    void on_message(proton::delivery& d, proton::message& msg) override;
    void on_error(const proton::error_condition& e) override;

    std::string    url_, user_, pass_, topic_;
    AlertStore&    store_;
    proton::container container_;
    std::thread    thread_;
    std::atomic<bool> stopping_{false};
};

} // namespace acq
