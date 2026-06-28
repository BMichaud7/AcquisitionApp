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
#include "P25Db.hpp"
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

namespace acq {

struct P25Db::Impl {
    pqxx::connection conn;
    explicit Impl(const std::string& cs) : conn(cs) {}
};

P25Db::P25Db(const std::string& conn_str)
    : impl_(std::make_unique<Impl>(conn_str)) {}

P25Db::~P25Db() = default;

void P25Db::upsert_control(double freq_hz,
                            int wacn, int sys_id, int rfss_id, int site_id,
                            int channel_iden, int channel_num) {
    try {
        pqxx::work tx(impl_->conn);
        tx.exec_params(R"sql(
            INSERT INTO p25_channels
                (freq_hz, wacn, sys_id, rfss_id, site_id,
                 channel_iden, channel_num, is_control, grant_count)
            VALUES ($1,$2,$3,$4,$5,$6,$7,true,1)
            ON CONFLICT (freq_hz) WHERE is_control = true DO UPDATE SET
                last_seen    = now(),
                wacn         = EXCLUDED.wacn,
                sys_id       = EXCLUDED.sys_id,
                rfss_id      = EXCLUDED.rfss_id,
                site_id      = EXCLUDED.site_id,
                grant_count  = p25_channels.grant_count + 1
        )sql",
            freq_hz, wacn, sys_id, rfss_id, site_id,
            channel_iden >= 0 ? std::optional<int>(channel_iden) : std::nullopt,
            channel_num  >= 0 ? std::optional<int>(channel_num)  : std::nullopt);
        tx.commit();
        spdlog::debug("[P25Db] control channel {:.4f} MHz recorded",
                      freq_hz / 1e6);
    } catch (const std::exception& e) {
        spdlog::warn("[P25Db] upsert_control failed: {}", e.what());
    }
}

void P25Db::upsert_grant(const P25Grant& g,
                          int wacn, int sys_id, int rfss_id, int site_id,
                          int channel_iden, int channel_num) {
    try {
        pqxx::work tx(impl_->conn);
        // alg_id / alg_name / key_id may be 0/unknown if HDU not yet decoded
        std::optional<int>         alg_id_opt;
        std::optional<std::string> alg_name_opt;
        std::optional<int>         key_id_opt;
        if (g.encrypted) {
            // Use HDU info if available; otherwise mark as "Encrypted (unknown alg)"
            alg_id_opt   = static_cast<int>(g.alg_id);
            alg_name_opt = g.alg_name;
            if (g.key_id) key_id_opt = static_cast<int>(g.key_id);
        }

        tx.exec_params(R"sql(
            INSERT INTO p25_channels
                (freq_hz, wacn, sys_id, rfss_id, site_id,
                 channel_iden, channel_num, is_control,
                 talk_group, source_id, encrypted, emergency,
                 alg_id, alg_name, key_id, grant_count)
            VALUES ($1,$2,$3,$4,$5,$6,$7,false,$8,$9,$10,$11,$12,$13,$14,1)
            ON CONFLICT (freq_hz, talk_group) DO UPDATE SET
                last_seen   = now(),
                source_id   = EXCLUDED.source_id,
                encrypted   = EXCLUDED.encrypted,
                emergency   = EXCLUDED.emergency,
                alg_id      = COALESCE(EXCLUDED.alg_id,   p25_channels.alg_id),
                alg_name    = COALESCE(EXCLUDED.alg_name, p25_channels.alg_name),
                key_id      = COALESCE(EXCLUDED.key_id,   p25_channels.key_id),
                grant_count = p25_channels.grant_count + 1
        )sql",
            g.freq_hz, wacn, sys_id, rfss_id, site_id,
            channel_iden >= 0 ? std::optional<int>(channel_iden) : std::nullopt,
            channel_num  >= 0 ? std::optional<int>(channel_num)  : std::nullopt,
            (int)g.talk_group, (int)g.source_id, g.encrypted, g.emergency,
            alg_id_opt, alg_name_opt, key_id_opt);
        tx.commit();
        spdlog::debug("[P25Db] grant TG={} {:.4f}MHz recorded",
                      g.talk_group, g.freq_hz / 1e6);
    } catch (const std::exception& e) {
        spdlog::warn("[P25Db] upsert_grant failed: {}", e.what());
    }
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
