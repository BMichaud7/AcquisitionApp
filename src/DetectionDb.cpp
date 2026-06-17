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
#include "DetectionDb.hpp"
#include <au/units/hertz.hh>
#include <spdlog/spdlog.h>
#include <chrono>

namespace acq {

using namespace std::chrono;

DetectionDb::DetectionDb(const std::string& conn_str, int batch_size,
                         au::QuantityD<au::Seconds> flush_interval)
    : conn_(conn_str), batch_size_(batch_size), flush_interval_(flush_interval)
{
    pqxx::work txn(conn_);
    // Ensure the shared signals table exists (schema/init.sql should be run first,
    // but this guard prevents a hard crash if it hasn't been).
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS signals (
            id              BIGSERIAL        PRIMARY KEY,
            first_seen      TIMESTAMPTZ      NOT NULL DEFAULT now(),
            last_seen       TIMESTAMPTZ      NOT NULL DEFAULT now(),
            freq_hz         DOUBLE PRECISION NOT NULL,
            freq_mhz        DOUBLE PRECISION NOT NULL,
            bandwidth_hz    DOUBLE PRECISION,
            power_db        REAL,
            snr_db          REAL,
            scanner_id      TEXT             NOT NULL DEFAULT '',
            channel         SMALLINT         NOT NULL DEFAULT 0,
            modulation      TEXT             NOT NULL DEFAULT '',
            mod_class       TEXT             NOT NULL DEFAULT '',
            is_ofdm         BOOLEAN          NOT NULL DEFAULT false,
            is_burst        BOOLEAN          NOT NULL DEFAULT false,
            is_fhss         BOOLEAN          NOT NULL DEFAULT false,
            symbol_rate_sps DOUBLE PRECISION,
            bit_rate_bps    DOUBLE PRECISION,
            hypothesis      TEXT             NOT NULL DEFAULT '',
            hyp_category    TEXT             NOT NULL DEFAULT '',
            hyp_confidence  REAL             DEFAULT 0,
            classified      BOOLEAN          NOT NULL DEFAULT false,
            rule_confidence REAL             DEFAULT 0,
            onnx_used       BOOLEAN          NOT NULL DEFAULT false,
            onnx_confidence REAL             DEFAULT 0,
            fast_path       BOOLEAN          NOT NULL DEFAULT false,
            reject_reason   TEXT             NOT NULL DEFAULT '',
            hits            INTEGER          NOT NULL DEFAULT 1,
            lat             DOUBLE PRECISION,
            lon             DOUBLE PRECISION,
            alt_m           REAL
        );
        CREATE INDEX IF NOT EXISTS idx_signals_freq    ON signals (freq_hz);
        CREATE INDEX IF NOT EXISTS idx_signals_time    ON signals (last_seen DESC);
        CREATE INDEX IF NOT EXISTS idx_signals_unclass ON signals (classified, last_seen DESC)
            WHERE classified = false;
        -- Add GPS columns to existing databases that predate this schema version
        ALTER TABLE signals ADD COLUMN IF NOT EXISTS lat   DOUBLE PRECISION;
        ALTER TABLE signals ADD COLUMN IF NOT EXISTS lon   DOUBLE PRECISION;
        ALTER TABLE signals ADD COLUMN IF NOT EXISTS alt_m REAL;
    )");
    txn.commit();

    thread_ = std::thread([this]{ workerLoop(); });
}

DetectionDb::~DetectionDb() {
    stopped_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DetectionDb::push(const Detection& d) {
    {
        std::lock_guard lock(mutex_);
        queue_.push(d);
    }
    cv_.notify_one();
}

void DetectionDb::workerLoop() {
    std::vector<Detection> batch;
    batch.reserve((size_t)batch_size_);

    while (!stopped_) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::duration<double>(flush_interval_.in(au::seconds))),
                [this]{ return (int)queue_.size() >= batch_size_ || stopped_; });

            while (!queue_.empty() && (int)batch.size() < batch_size_) {
                batch.push_back(queue_.front());
                queue_.pop();
            }
        }

        if (!batch.empty()) {
            try { flush(batch); }
            catch (const std::exception& e) {
                spdlog::error("[DetectionDb] flush failed: {}", e.what());
            }
            batch.clear();
        }
    }

    // Drain remaining items on shutdown
    {
        std::lock_guard lock(mutex_);
        while (!queue_.empty()) {
            batch.push_back(queue_.front());
            queue_.pop();
        }
    }
    if (!batch.empty()) {
        try { flush(batch); }
        catch (const std::exception& e) {
            spdlog::error("[DetectionDb] final flush failed: {}", e.what());
        }
    }
}

void DetectionDb::flush(std::vector<Detection>& batch) {
    pqxx::work txn(conn_);

    for (const auto& d : batch) {
        auto ms = duration_cast<milliseconds>(
            d.timestamp.time_since_epoch()).count();

        const double center_freq_raw = d.center_freq_hz.in(au::hertz);
        const double bandwidth_raw   = d.bandwidth_hz.in(au::hertz);

        std::optional<double> bw_opt;
        if (bandwidth_raw > 0) bw_opt = bandwidth_raw;

        // Find a recent signal at the same frequency (±10 kHz) with similar
        // bandwidth (±50%).  Matches classified OR unclassified rows — if the
        // signal is already classified we still want to update last_seen/power.
        auto existing = txn.exec_params(
            "SELECT id FROM signals "
            "WHERE abs(freq_hz - $1) < 10000 "
            "  AND ($2::double precision IS NULL OR bandwidth_hz IS NULL "
            "       OR abs(bandwidth_hz - $2) / GREATEST(bandwidth_hz, 1000.0) < 0.5) "
            "  AND last_seen > now() - interval '10 minutes' "
            "ORDER BY abs(freq_hz - $1) ASC "
            "LIMIT 1",
            center_freq_raw,
            bw_opt);

        if (!existing.empty()) {
            // Same signal seen again — update last_seen, peak power, hit count.
            // Don't touch modulation/classification — AnalysisApp owns those.
            txn.exec_params(
                "UPDATE signals SET "
                "  last_seen    = to_timestamp($2::double precision / 1000.0), "
                "  power_db     = GREATEST(power_db, $3), "
                "  hits         = hits + 1 "
                "WHERE id = $1",
                existing[0][0].as<long long>(),
                ms,
                d.power_db);
        } else {
            // First time this signal has been seen — insert unclassified row.
            // AnalysisApp will fill in modulation/classification after IQ collection.
            std::optional<double> lat_opt = d.lat;
            std::optional<double> lon_opt = d.lon;
            std::optional<float>  alt_opt = d.alt_m.has_value()
                                            ? std::optional<float>(static_cast<float>(*d.alt_m))
                                            : std::nullopt;
            txn.exec_params(
                "INSERT INTO signals "
                "(first_seen, last_seen, freq_hz, freq_mhz, bandwidth_hz, "
                " power_db, scanner_id, channel, lat, lon, alt_m) "
                "VALUES ("
                "  to_timestamp($1::double precision / 1000.0), "
                "  to_timestamp($1::double precision / 1000.0), "
                "  $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                ms,
                center_freq_raw,
                center_freq_raw / 1e6,
                bw_opt,
                d.power_db,
                d.scanner_id,
                d.channel,
                lat_opt,
                lon_opt,
                alt_opt);
        }
    }

    txn.commit();
    spdlog::debug("[DetectionDb] upserted {} detections", batch.size());
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
