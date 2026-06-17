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
// ════════════════════════════════════════════════════════════════════════
//  sdr_acquisition — Continuous Spectrum Scanner
//
//  Submits a SCAN task to the sdr_controller via AMQP, receives CF32 IQ
//  data over UDP, detects RF energy above a per-dwell noise floor estimate,
//  records detections to PostgreSQL, and publishes them over AMQP.
//
//  Usage:
//    sdr_acquisition [config.xml]
//    SDR_ACQ_CONFIG=/etc/sdr-acquisition/scanner.xml  sdr_acquisition
//    SDR_LOG_LEVEL=debug sdr_acquisition config/scanner.xml
// ════════════════════════════════════════════════════════════════════════
#include "SweepConfig.hpp"
#include "SpectrumScanner.hpp"
#include "TaskManagerIqSource.hpp"
#include "DetectionDb.hpp"
#include "AmqpPublisher.hpp"
#include "P25GrantConsumer.hpp"
#include "GpsCache.hpp"
#include "P25Db.hpp"
#include <au/units/hertz.hh>
#include <au/units/seconds.hh>
#include <au/prefix.hh>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>
#include <chrono>

static std::atomic<bool> g_running{true};
static void sigHandler(int) { g_running = false; }

static void setupLogging() {
    const char* lvl = std::getenv("SDR_LOG_LEVEL");
    if (!lvl) lvl = "info";
    std::string s(lvl);
    spdlog::level::level_enum level = spdlog::level::info;
    if      (s == "trace") level = spdlog::level::trace;
    else if (s == "debug") level = spdlog::level::debug;
    else if (s == "warn")  level = spdlog::level::warn;
    else if (s == "error") level = spdlog::level::err;
    spdlog::set_level(level);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

int main(int argc, char* argv[]) {
    setupLogging();

    std::string config_path = "/etc/sdr-acquisition/scanner.xml";
    if (argc > 1)
        config_path = argv[1];
    else if (const char* env = std::getenv("SDR_ACQ_CONFIG"))
        config_path = env;

    acq::SweepConfig cfg;
    try {
        cfg = acq::SweepConfig::from_file(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Config error: {}", e.what());
        return 1;
    }

    spdlog::info("SDR Acquisition — scanner_id={}", cfg.scanner_id);
    spdlog::info("Sweep: {:.3f}–{:.3f} MHz  rx_channels={}  controller={}",
        cfg.sweep.start_hz.in(au::hertz) / 1e6,
        cfg.sweep.stop_hz.in(au::hertz)  / 1e6,
        cfg.device.rx_channels, cfg.amqp.url);

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    // ── AMQP detection publisher ───────────────────────────────────────────
    std::unique_ptr<acq::AmqpPublisher> amqp;
    bool amqp_ok = false;
    try {
        amqp = std::make_unique<acq::AmqpPublisher>(
            cfg.amqp.url, cfg.amqp.username, cfg.amqp.password,
            cfg.amqp.detection_topic, cfg.scanner_id);
        amqp->start();
        amqp_ok = true;
    } catch (const std::exception& e) {
        spdlog::warn("AMQP publisher failed to start: {} — continuing without AMQP", e.what());
    }

    // ── PostgreSQL detection writer ───────────────────────────────────────
    std::unique_ptr<acq::DetectionDb> db;
    bool db_ok = false;
    try {
        db = std::make_unique<acq::DetectionDb>(cfg.db.connection_string());
        db_ok = true;
    } catch (const std::exception& e) {
        spdlog::warn("PostgreSQL connection failed: {} — continuing without DB", e.what());
    }

    if (!amqp_ok && !db_ok) {
        spdlog::error("Neither AMQP nor DB available — check config");
        return 1;
    }

    // ── GPS position cache — subscribes to gps.location ──────────────────
    std::unique_ptr<acq::GpsCache> gps_cache;
    if (amqp_ok) {
        try {
            gps_cache = std::make_unique<acq::GpsCache>(
                cfg.amqp.url, cfg.amqp.username, cfg.amqp.password);
            gps_cache->start();
            spdlog::info("GPS cache started — listening on gps.location");
        } catch (const std::exception& e) {
            spdlog::warn("GPS cache failed to start: {} — detections will have no position", e.what());
        }
    }

    auto on_detection = [&](const acq::Detection& d) {
        acq::Detection stamped = d;
        if (gps_cache) {
            if (auto fix = gps_cache->get()) {
                stamped.lat   = fix->lat;
                stamped.lon   = fix->lon;
                stamped.alt_m = fix->alt_m;
            }
        }
        if (db_ok)   db->push(stamped);
        if (amqp_ok) amqp->publish(stamped);
    };

    // ── IQ sources + scanners ────────────────────────────────────────────────
    // Three modes, in priority order:
    //
    //   1. bands non-empty — one scanner per band, each requests "next available
    //      device" from SdrRM (or a pinned device if band.device_id is set).
    //      This is the preferred multi-SDR mode: N bands spread across N devices
    //      with no duplicate frequency coverage and no inter-process coordination.
    //
    //   2. scan_device_ids set — one scanner per named device, all share the same
    //      sweep range. Legacy mode; prefer bands instead.
    //
    //   3. both empty — one scanner, SdrRM assigns any free device.
    //
    // Rank 1 (lowest) ensures AnalysisApp (2) and DfApp (3) can preempt.
    std::vector<std::unique_ptr<acq::TaskManagerIqSource>> sources;
    std::vector<std::unique_ptr<acq::SpectrumScanner>>     scanners;

    // ── Build effective band list ──────────────────────────────────────────────
    // Priority: explicit bands > scan_device_ids (legacy) > single sweep.
    // For band and single-sweep modes, query SdrRM for online device count and
    // auto-split so every available device covers a unique frequency slice.
    if (!cfg.scan_device_ids.empty()) {
        // Legacy mode: named devices, all scan the same configured sweep range.
        spdlog::info("Multi-device mode: {} named device(s)", cfg.scan_device_ids.size());
        for (const auto& dev_id : cfg.scan_device_ids) {
            spdlog::info("  → {}", dev_id);
            sources.push_back(std::make_unique<acq::TaskManagerIqSource>(cfg, dev_id));
            scanners.push_back(std::make_unique<acq::SpectrumScanner>(cfg, sources.back().get(), on_detection));
        }
    } else {
        // Band mode (or single sweep treated as one band).
        // Step 1: normalise to a band list.
        std::vector<acq::BandConfig> effective_bands = cfg.bands;
        if (effective_bands.empty()) {
            acq::BandConfig b;
            b.start_hz = cfg.sweep.start_hz;
            b.stop_hz  = cfg.sweep.stop_hz;
            effective_bands.push_back(b);
        }

        // Step 2: query SdrRM for online device count, then split bands to match.
        // One temporary IqSource is created just for the HEALTH_QUERY — it uses
        // the persistent AMQP channel so the cost is a single ~16 ms round trip.
        {
            auto probe      = std::make_unique<acq::TaskManagerIqSource>(cfg);
            const int n_dev = probe->queryDeviceCount();
            probe.reset();

            if (n_dev > static_cast<int>(effective_bands.size())) {
                // More devices than bands — subdivide each band into equal slices.
                // Distribute remainder to the first bands so all devices are used.
                //   1 band  + 4 devices → [slice0, slice1, slice2, slice3]
                //   2 bands + 4 devices → each band → [slice0, slice1]
                //   3 bands + 4 devices → band0 → [s0, s1],  band1 → [s0],  band2 → [s0]
                std::vector<acq::BandConfig> split;
                const int nb        = static_cast<int>(effective_bands.size());
                const int base      = n_dev / nb;
                const int remainder = n_dev % nb;
                for (int i = 0; i < nb; ++i) {
                    const int    pieces = base + (i < remainder ? 1 : 0);
                    const double lo     = effective_bands[i].start_hz.in(au::hertz);
                    const double hi     = effective_bands[i].stop_hz.in(au::hertz);
                    const double step   = (hi - lo) / pieces;
                    for (int j = 0; j < pieces; ++j) {
                        acq::BandConfig bc;
                        bc.device_id = effective_bands[i].device_id;
                        bc.start_hz  = au::hertz(lo + j       * step);
                        bc.stop_hz   = au::hertz(lo + (j + 1) * step);
                        split.push_back(bc);
                    }
                }
                spdlog::info("Auto-split: {} band(s) × {} device(s) → {} slice(s)",
                    nb, n_dev, split.size());
                effective_bands = std::move(split);
            }
        }

        // Step 3: spawn one (IqSource, SpectrumScanner) per effective band.
        spdlog::info("Launching {} scanner(s):", effective_bands.size());
        for (const auto& band : effective_bands) {
            acq::SweepConfig bcfg = cfg;
            bcfg.sweep.start_hz = band.start_hz;
            bcfg.sweep.stop_hz  = band.stop_hz;
            bcfg.bands.clear();
            spdlog::info("  {:.3f}–{:.3f} MHz  device={}",
                band.start_hz.in(au::hertz) / 1e6,
                band.stop_hz.in(au::hertz)  / 1e6,
                band.device_id.empty() ? "any" : band.device_id);
            sources.push_back(std::make_unique<acq::TaskManagerIqSource>(bcfg, band.device_id));
            scanners.push_back(std::make_unique<acq::SpectrumScanner>(bcfg, sources.back().get(), on_detection));
        }
    }

    for (auto& s : scanners) s->start();

    // ── P25 grant consumer — retune to voice channel on grant ─────────────
    // Reads rf.p25.grants; for each whitelisted TG, submits a priority
    // NARROWBAND capture and publishes the resulting detections.
    // Audio decoding (IMBE→PCM→SpeechApp) is not wired yet.
    std::unique_ptr<acq::P25GrantConsumer> p25;
    if (cfg.p25.enabled) {
        spdlog::info("[P25] grant consumer enabled — topic={} capture={:.1f}s",
                     cfg.p25.grant_topic, cfg.p25.capture_s);

        // P25 channel DB writer
        std::shared_ptr<acq::P25Db> p25db;
        if (db_ok) {
            try {
                p25db = std::make_shared<acq::P25Db>(cfg.db.connection_string());
                spdlog::info("[P25] p25_channels table connected");
            } catch (const std::exception& e) {
                spdlog::warn("[P25] DB unavailable: {}", e.what());
            }
        }

        // Build a one-shot IQ source for voice channel captures
        // (separate from the sweep scanner sources so grants don't interrupt sweeps)
        auto p25_src = std::make_unique<acq::TaskManagerIqSource>(cfg, "");

        p25 = std::make_unique<acq::P25GrantConsumer>(
            cfg.amqp.url, cfg.amqp.username, cfg.amqp.password,
            cfg.p25.grant_topic,
            [&on_detection, &cfg, p25_src = p25_src.get(), p25db](const acq::P25Grant& g) {

                spdlog::info("[P25] tuning to TG={} @ {:.4f}MHz for {:.1f}s",
                             g.talk_group, g.freq_hz / 1e6, cfg.p25.capture_s);

                // Record voice channel grant in p25_channels
                if (p25db) p25db->upsert_grant(g);

                // Build a temporary sweep config for the voice channel
                // (narrow: ±6.25 kHz around centre, 12.5 kHz BW, single step)
                acq::SweepConfig vcfg = cfg;
                vcfg.sweep.start_hz  = au::hertz(g.freq_hz - 6250.0);
                vcfg.sweep.stop_hz   = au::hertz(g.freq_hz + 6250.0);
                // Override scanner_id to tag detections with talk group
                vcfg.scanner_id = cfg.scanner_id + "-tg" + std::to_string(g.talk_group);

                // Submit NARROWBAND capture via the dedicated P25 IQ source
                // The source's sweepLoop will fire on_detection for each signal found.
                // We pass through on_detection unchanged — detections appear on
                // rf.detections tagged with the modified scanner_id (tg<N>).
                acq::SpectrumScanner voice_scanner(vcfg, p25_src, on_detection);
                voice_scanner.start();

                // Capture for grant duration then stop
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(cfg.p25.capture_s));
                voice_scanner.stop();

                spdlog::info("[P25] TG={} capture complete", g.talk_group);
            });

        // Transfer ownership of p25_src into the closure via shared_ptr
        // (p25_src unique_ptr must outlive p25 consumer)
        p25->start();
    }

    spdlog::info("Running — Ctrl+C to stop");
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    spdlog::info("Stopping...");
    if (p25) p25->stop();
    for (auto& s : scanners) s->stop();
    gps_cache.reset();
    amqp.reset();
    db.reset();
    spdlog::info("Shutdown complete");
    return 0;
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
