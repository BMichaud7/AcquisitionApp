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
/**
 * @file P25Db.hpp
 * @brief PostgreSQL writer for p25_channels table.
 *
 * Called by:
 *  - P25Monitor (DemodApp) via AMQP to record control channel on startup
 *  - P25GrantConsumer (AcquisitionApp) on each voice channel grant
 */
#pragma once
#include "P25GrantConsumer.hpp"
#include <string>
#include <memory>

namespace pqxx { class connection; }

namespace acq {

class P25Db {
public:
    explicit P25Db(const std::string& conn_str);
    ~P25Db();

    /// Upsert the P25 control channel (is_control = true).
    void upsert_control(double freq_hz,
                        int wacn, int sys_id, int rfss_id, int site_id,
                        int channel_iden = -1, int channel_num = -1);

    /// Upsert a voice channel grant.
    void upsert_grant(const P25Grant& g,
                      int wacn = 0, int sys_id = 0,
                      int rfss_id = 0, int site_id = 0,
                      int channel_iden = -1, int channel_num = -1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acq
