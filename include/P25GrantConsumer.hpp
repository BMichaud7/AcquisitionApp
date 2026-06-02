/**
 * @file P25GrantConsumer.hpp
 * @brief P25 voice channel follower — retunes acquisition on channel grants.
 *
 * Subscribes to rf.p25.grants published by DemodApp's P25Monitor.
 * On each grant, submits a priority NARROWBAND scan task to the SDR
 * controller tuned to the voice channel frequency for the grant duration.
 * The captured IQ passes through the normal detection pipeline and is
 * published to rf.detections tagged with the talk group ID.
 *
 * Audio output (IMBE decoding → SpeechApp) is NOT wired here — this
 * handles only the SDR tuning and IQ capture side.
 */
#pragma once
#include "SweepConfig.hpp"
#include "TaskManagerIqSource.hpp"

#include <proton/container.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/delivery.hpp>
#include <proton/message.hpp>
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/receiver_options.hpp>
#include <proton/source_options.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>

namespace acq {

/// Grant event from rf.p25.grants
struct P25Grant {
    uint32_t    talk_group;
    uint32_t    source_id;
    double      freq_hz;
    bool        encrypted;
    bool        emergency;
    int64_t     ts_ms;
};

/// Called when a grant is received — the consumer owns tuning to freq_hz.
using GrantHandler = std::function<void(const P25Grant&)>;

/**
 * @class P25GrantConsumer
 * @brief AMQP subscriber for P25 channel grants with IQ capture.
 *
 * Lifecycle:
 *   start() — begins AMQP subscription on a background thread
 *   stop()  — clean shutdown
 *
 * When a grant arrives, it is queued and a worker thread calls the
 * GrantHandler which performs the actual SDR retune + capture.
 */
class P25GrantConsumer {
public:
    /**
     * @param amqp_url      Broker URL (e.g. amqp://localhost:5672)
     * @param username       Optional broker credentials.
     * @param password
     * @param grant_topic   Topic to subscribe to (default rf.p25.grants).
     * @param on_grant      Called in a worker thread for each received grant.
     */
    P25GrantConsumer(std::string amqp_url,
                     std::string username,
                     std::string password,
                     std::string grant_topic,
                     GrantHandler on_grant);
    ~P25GrantConsumer();

    void start();
    void stop();

private:
    class Handler;
    void enqueue(P25Grant g);
    void worker_loop();

    std::string    url_;
    std::string    user_;
    std::string    pass_;
    std::string    topic_;
    GrantHandler   on_grant_;

    std::unique_ptr<proton::container> container_;
    std::thread    amqp_thread_;
    std::thread    worker_thread_;

    std::queue<P25Grant>     queue_;
    std::mutex               queue_mu_;
    std::condition_variable  queue_cv_;
    std::atomic<bool>        running_{false};
};

} // namespace acq
