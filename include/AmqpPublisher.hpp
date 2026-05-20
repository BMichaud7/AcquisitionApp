#pragma once
/**
 * @file AmqpPublisher.hpp
 * @brief Thread-safe AMQP publisher for RF_DETECTION messages.
 *
 * Runs a proton::container in a background thread and exposes a publish()
 * method that can be called safely from any other thread (including the
 * SpectrumScanner sweep thread).
 *
 * Each Detection is serialised to a JSON RF_DETECTION message (schema 1.2)
 * and sent to the configured AMQP topic.  The IQ snapshot is base64-encoded
 * into the `iq_snapshot_b64` field.
 */
#include "Types.hpp"
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/sender.hpp>
#include <proton/transport.hpp>
#include <proton/work_queue.hpp>
#include <atomic>
#include <thread>
#include <string>

namespace acq {

/**
 * @brief Publishes Detection objects as AMQP messages to an rf.detections topic.
 *
 * Non-copyable.  Call start() before publish(), stop() before destruction.
 * publish() is safe to call from any thread after start() returns.
 */
class AmqpPublisher : public proton::messaging_handler {
public:
    /**
     * @brief Construct the publisher.
     * @param url        AMQP broker URL.
     * @param username   AMQP username.
     * @param password   AMQP password.
     * @param topic      Destination topic address (e.g. "rf.detections").
     * @param scanner_id Scanner identifier embedded in every message.
     */
    AmqpPublisher(std::string url, std::string username, std::string password,
                  std::string topic, std::string scanner_id);
    ~AmqpPublisher();

    AmqpPublisher(const AmqpPublisher&)            = delete;
    AmqpPublisher& operator=(const AmqpPublisher&) = delete;

    /// @brief Connect to the broker and start the container thread.
    void start();
    /// @brief Drain in-flight messages, disconnect, and join the thread.
    void stop();

    /**
     * @brief Serialise and publish a Detection.
     *
     * Encodes the Detection as a JSON RF_DETECTION message (schema 1.2),
     * including base64 IQ snapshot and SNR.  Thread-safe; may be called
     * from any thread after start() returns.
     *
     * @param d Detection to publish.
     */
    void publish(const Detection& d);

    /// @cond INTERNAL
    void on_container_start(proton::container&) override;
    void on_connection_open(proton::connection&) override;
    void on_sender_open(proton::sender&) override;
    void on_transport_error(proton::transport&) override;
    void on_connection_error(proton::connection&) override;
    /// @endcond

private:
    std::string         url_;
    std::string         username_;
    std::string         password_;
    std::string         topic_;
    std::string         scanner_id_;
    proton::sender      sender_;
    proton::work_queue* work_queue_{nullptr};
    proton::container*  container_{nullptr};
    std::thread         thread_;
    std::atomic<bool>   stopping_{false};

    proton::message makeMessage(const Detection& d) const;
};

} // namespace acq
