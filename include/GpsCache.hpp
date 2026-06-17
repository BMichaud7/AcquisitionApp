/*
========================================================================
Project: OpenRFStack
Author:  Brendan Michaud
Year:    2026
Part of OpenRFStack (https://github.com/OpenRFStack)

Licensed under the Personal Use License.
Do not use for commercial, organizational, or military purposes.
========================================================================
*/
#pragma once
/**
 * @file GpsCache.hpp
 * @brief Caches the latest GPS fix from GpsApp's gps.location AMQP topic.
 *
 * GpsApp is the sole source of GPS data — it reads gpsd and publishes
 * position to gps.location.  GpsCache subscribes to that topic and holds
 * the most recent fix so other apps can stamp lat/lon/alt onto records
 * without querying gpsd directly.
 *
 * Returns std::nullopt until GpsApp publishes its first fix.
 */
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <proton/connection.hpp>
#include <proton/container.hpp>
#include <proton/delivery.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/transport.hpp>

namespace acq {

/**
 * @brief GPS fix snapshot from GpsApp.
 *
 * Populated from a @c gps.location AMQP message published by GpsApp.
 * All fields are valid when GpsApp has a 3-D fix (mode ≥ 2).
 */
struct GpsFix {
    double lat{0.0};   ///< Latitude in decimal degrees (WGS-84).
    double lon{0.0};   ///< Longitude in decimal degrees (WGS-84).
    double alt_m{0.0}; ///< Altitude above mean sea level (metres).
};

class GpsCache : public proton::messaging_handler {
public:
    GpsCache(std::string url, std::string username, std::string password,
             std::string topic = "gps.location");
    ~GpsCache();

    GpsCache(const GpsCache&)            = delete;
    GpsCache& operator=(const GpsCache&) = delete;

    void start();
    void stop();

    /**
     * @brief Return the most recently received GPS fix.
     * @return Latest @c GpsFix from GpsApp, or @c std::nullopt if no message
     *         has been received yet (GpsApp not running or no satellite fix).
     */
    std::optional<GpsFix> get() const;

    /**
     * @brief Parse a @c gps.location JSON payload into a @c GpsFix.
     * @param json_str  JSON string: @c {"latitude_deg":…,"longitude_deg":…,"altitude_m":…}
     * @return Parsed fix, or @c std::nullopt if the string is not valid JSON.
     */
    static std::optional<GpsFix> parse_fix(const std::string& json_str) noexcept;

    void on_container_start(proton::container&) override;
    void on_connection_open(proton::connection&) override;
    void on_message(proton::delivery&, proton::message&) override;
    void on_transport_error(proton::transport&) override;
    void on_connection_error(proton::connection&) override;

private:
    std::string           url_, username_, password_, topic_;
    mutable std::mutex    mutex_;
    std::optional<GpsFix> fix_;
    proton::container*    container_{nullptr};
    std::thread           thread_;
    std::atomic<bool>     stopping_{false};
};

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
