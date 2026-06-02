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

    auto on_detection = [&](const acq::Detection& d) {
        if (db_ok)   db->push(d);
        if (amqp_ok) amqp->publish(d);
    };

    // ── IQ sources + scanners — one per device ────────────────────────────
    // scan_device_ids empty → one scanner, scheduler picks any free device.
    // scan_device_ids set  → one scanner per device, all run in parallel.
    // Rank 1 (lowest) ensures AnalysisApp (2) and DfApp (3) can preempt.
    std::vector<std::unique_ptr<acq::TaskManagerIqSource>> sources;
    std::vector<std::unique_ptr<acq::SpectrumScanner>>     scanners;

    auto& dev_ids = cfg.scan_device_ids;
    if (dev_ids.empty()) {
        spdlog::info("Devices: any (scheduler assigns)");
        sources.push_back(std::make_unique<acq::TaskManagerIqSource>(cfg));
        scanners.push_back(std::make_unique<acq::SpectrumScanner>(cfg, sources.back().get(), on_detection));
    } else {
        spdlog::info("Devices: {} (scanning all simultaneously)", dev_ids.size());
        for (const auto& dev_id : dev_ids) {
            spdlog::info("  → {}", dev_id);
            sources.push_back(std::make_unique<acq::TaskManagerIqSource>(cfg, dev_id));
            scanners.push_back(std::make_unique<acq::SpectrumScanner>(cfg, sources.back().get(), on_detection));
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

        // Build a one-shot IQ source for voice channel captures
        // (separate from the sweep scanner sources so grants don't interrupt sweeps)
        auto p25_src = std::make_unique<acq::TaskManagerIqSource>(cfg, "", cfg.p25.rank);

        p25 = std::make_unique<acq::P25GrantConsumer>(
            cfg.amqp.url, cfg.amqp.username, cfg.amqp.password,
            cfg.p25.grant_topic,
            [&on_detection, &cfg, p25_src = p25_src.get()](const acq::P25Grant& g) {

                spdlog::info("[P25] tuning to TG={} @ {:.4f}MHz for {:.1f}s",
                             g.talk_group, g.freq_hz / 1e6, cfg.p25.capture_s);

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
    amqp.reset();
    db.reset();
    spdlog::info("Shutdown complete");
    return 0;
}
