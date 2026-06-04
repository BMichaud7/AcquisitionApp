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
 * @file AlertPublisher.hpp
 * @brief AMQP publisher for RF_ALERT messages on rf.alerts topic.
 */
#include "RfAlert.hpp"
#include <proton/container.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/sender.hpp>
#include <proton/work_queue.hpp>
#include <atomic>
#include <thread>
#include <string>

namespace acq {

class AlertPublisher : public proton::messaging_handler {
public:
    AlertPublisher(std::string url, std::string user, std::string pass,
                   std::string topic = "rf.alerts");
    ~AlertPublisher();

    AlertPublisher(const AlertPublisher&)            = delete;
    AlertPublisher& operator=(const AlertPublisher&) = delete;

    void start();
    void stop();
    void publish(const RfAlert& alert);

private:
    void on_container_start(proton::container& c) override;
    void on_connection_open(proton::connection& c) override;
    void on_sender_open(proton::sender& s) override;
    void on_error(const proton::error_condition& e) override;

    std::string             url_, user_, pass_, topic_;
    proton::container       container_;
    proton::sender          sender_;
    proton::work_queue*     wq_{nullptr};
    std::thread             thread_;
    std::atomic<bool>       ready_{false};
};

} // namespace acq
