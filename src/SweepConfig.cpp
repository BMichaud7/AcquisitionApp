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
#include "SweepConfig.hpp"
#include <tinyxml2.h>
#include <fmt/format.h>
#include <sstream>

namespace acq {

using namespace tinyxml2;

namespace {

const char* textOrDefault(XMLElement* el, const char* def = "") {
    return el && el->GetText() ? el->GetText() : def;
}
XMLElement* need(XMLElement* parent, const char* name) {
    auto* el = parent ? parent->FirstChildElement(name) : nullptr;
    if (!el) throw std::runtime_error(fmt::format("Missing XML element <{}>", name));
    return el;
}
XMLElement* opt(XMLElement* parent, const char* name) {
    return parent ? parent->FirstChildElement(name) : nullptr;
}

} // namespace

std::string DbConfig::connection_string() const {
    return fmt::format("host={} port={} dbname={} user={} password={}",
        host, port, name, user, password);
}

SweepConfig SweepConfig::from_file(const std::string& path) {
    XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != XML_SUCCESS)
        throw std::runtime_error(fmt::format("Cannot open config: {} — {}", path, doc.ErrorStr()));

    auto* root = doc.FirstChildElement("sdr_acquisition");
    if (!root) throw std::runtime_error("Root element <sdr_acquisition> not found");

    SweepConfig cfg;

    if (auto* el = opt(root, "scanner_id"))
        cfg.scanner_id = el->GetText() ? el->GetText() : cfg.scanner_id;
    if (auto* el = opt(root, "rank"))
        el->QueryIntText(&cfg.rank);
    if (auto* el = opt(root, "analysis_pause_ms")) {
        int pause_ms_raw = 0;
        el->QueryIntText(&pause_ms_raw);
        cfg.analysis_pause_ms = au::milli(au::seconds)(pause_ms_raw);
    }
    if (auto* el = opt(root, "scan_device_ids")) {
        std::istringstream ss(el->GetText() ? el->GetText() : "");
        std::string id;
        while (ss >> id) cfg.scan_device_ids.push_back(id);
    }

    // ── Multi-band parallel scan (collected here, applied after <sweep>) ───────
    // Bands are stored temporarily and pushed into cfg.bands after the sweep block
    // so each BandConfig inherits the fully-parsed SweepParams defaults.
    std::vector<BandConfig> pending_bands;
    if (auto* bands_el = opt(root, "bands")) {
        for (auto* b = bands_el->FirstChildElement("band"); b; b = b->NextSiblingElement("band")) {
            BandConfig bc;
            bc.device_id = textOrDefault(opt(b, "device"));
            // <required_device> overrides <device> and marks the device as mandatory
            if (auto* el = opt(b, "required_device")) {
                bc.device_id       = textOrDefault(el);
                bc.device_required = true;
            }
            uint64_t raw = 0;
            if (auto* e = opt(b, "start_hz")) { e->QueryUnsigned64Text(&raw); bc.start_hz = au::hertz(static_cast<double>(raw)); raw = 0; }
            if (auto* e = opt(b, "stop_hz"))  { e->QueryUnsigned64Text(&raw); bc.stop_hz  = au::hertz(static_cast<double>(raw)); }
            if (bc.stop_hz <= bc.start_hz)
                throw std::runtime_error(fmt::format(
                    "<band> stop_hz must be > start_hz (got {:.3f} MHz – {:.3f} MHz)",
                    bc.start_hz.in(au::hertz) / 1e6, bc.stop_hz.in(au::hertz) / 1e6));
            pending_bands.push_back(std::move(bc));
        }
    }

    // ── Threat detection ─────────────────────────────────────────────────────
    if (auto* th = opt(root, "threat")) {
        auto boolopt = [&](XMLElement* parent, const char* tag, bool def) -> bool {
            if (auto* e = opt(parent, tag)) {
                std::string v = e->GetText() ? e->GetText() : "";
                return (v == "true" || v == "1" || v == "yes");
            }
            return def;
        };
        cfg.threat.enabled          = boolopt(th, "enabled", false);
        cfg.threat.gps_enabled      = boolopt(th, "gps_enabled",   false);
        cfg.threat.adsb_enabled     = boolopt(th, "adsb_enabled",   true);
        cfg.threat.ais_enabled      = boolopt(th, "ais_enabled",    true);
        cfg.threat.eas_enabled      = boolopt(th, "eas_enabled",    true);
        cfg.threat.dsc_enabled      = boolopt(th, "dsc_enabled",    true);
        cfg.threat.p25_rogue_enabled= boolopt(th, "p25_rogue_enabled", true);
        if (auto* e = opt(th, "gps_jammer_threshold_db"))
            e->QueryDoubleText(&cfg.threat.gps_jammer_threshold_db);
        if (auto* e = opt(th, "gps_spoof_detectable_dbm"))
            e->QueryDoubleText(&cfg.threat.gps_spoof_detectable_dbm);
        if (auto* e = opt(th, "gps_check_interval_s"))
            e->QueryDoubleText(&cfg.threat.gps_check_interval_s);
        if (auto* e = opt(th, "alert_topic"))
            cfg.threat.alert_topic = textOrDefault(e, cfg.threat.alert_topic.c_str());
    }

    // ── P25 grant follower ───────────────────────────────────────────────────
    if (auto* p25 = opt(root, "p25")) {
        auto boolopt = [&](const char* tag, bool def) -> bool {
            if (auto* e = opt(p25, tag)) {
                std::string v = e->GetText() ? e->GetText() : "";
                return (v == "true" || v == "1" || v == "yes");
            }
            return def;
        };
        cfg.p25.enabled     = boolopt("enabled", false);
        cfg.p25.grant_topic = textOrDefault(opt(p25, "grant_topic"),
                                             cfg.p25.grant_topic.c_str());
        if (auto* e = opt(p25, "capture_s"))
            cfg.p25.capture_s = std::stod(e->GetText() ? e->GetText() : "3.0");
        if (auto* e = opt(p25, "rank"))
            cfg.p25.rank = std::stoi(e->GetText() ? e->GetText() : "3");
        if (auto* e = opt(p25, "tg_whitelist")) {
            std::istringstream ss(e->GetText() ? e->GetText() : "");
            uint32_t tg;
            while (ss >> tg) cfg.p25.tg_whitelist.push_back(tg);
        }
    }

    // ── AMQP ────────────────────────────────────────────────────────────────
    if (auto* amqp = opt(root, "amqp")) {
        cfg.amqp.url                 = textOrDefault(opt(amqp, "url"), cfg.amqp.url.c_str());
        cfg.amqp.username            = textOrDefault(opt(amqp, "username"));
        cfg.amqp.password            = textOrDefault(opt(amqp, "password"));
        cfg.amqp.detection_topic     = textOrDefault(opt(amqp, "detection_topic"),
                                                      cfg.amqp.detection_topic.c_str());
        cfg.amqp.task_request_queue  = textOrDefault(opt(amqp, "task_request_queue"),
                                                      cfg.amqp.task_request_queue.c_str());
        cfg.amqp.task_response_queue = textOrDefault(opt(amqp, "task_response_queue"),
                                                      cfg.amqp.task_response_queue.c_str());
        if (auto* el = opt(amqp, "reconnect_interval_sec"))
            el->QueryIntText(&cfg.amqp.reconnect_interval_sec);
    }

    // ── Database ─────────────────────────────────────────────────────────────
    if (auto* db = opt(root, "database")) {
        cfg.db.host     = textOrDefault(opt(db, "host"), cfg.db.host.c_str());
        if (auto* el = opt(db, "port")) el->QueryIntText(&cfg.db.port);
        cfg.db.name     = textOrDefault(opt(db, "name"), cfg.db.name.c_str());
        cfg.db.user     = textOrDefault(opt(db, "user"));
        cfg.db.password = textOrDefault(opt(db, "password"));
    }

    // ── Device ───────────────────────────────────────────────────────────────
    auto* dev = need(root, "device");
    if (auto* el = opt(dev, "rx_channels"))    el->QueryIntText(&cfg.device.rx_channels);
    if (auto* el = opt(dev, "sample_rate_sps")) {
        double sr_raw = 0.0;
        el->QueryDoubleText(&sr_raw);
        cfg.device.sample_rate = au::hertz(sr_raw);
    }
    if (auto* el = opt(dev, "rx_gain_db"))     el->QueryDoubleText(&cfg.device.rx_gain_db);
    if (auto* el = opt(dev, "required_device_id"))
        cfg.device.required_device_id = textOrDefault(el);
    if (auto* el = opt(dev, "bandwidth_hz")) {
        double bw_raw = 0.0;
        el->QueryDoubleText(&bw_raw);
        cfg.device.bandwidth_hz = au::hertz(bw_raw);
    }
    cfg.device.rx_channels = std::max(1, cfg.device.rx_channels);

    // ── Sweep ────────────────────────────────────────────────────────────────
    auto* sweep = need(root, "sweep");
    if (auto* el = opt(sweep, "start_hz")) {
        uint64_t raw = 0; el->QueryUnsigned64Text(&raw);
        cfg.sweep.start_hz = au::hertz(static_cast<double>(raw));
    }
    if (auto* el = opt(sweep, "stop_hz")) {
        uint64_t raw = 0; el->QueryUnsigned64Text(&raw);
        cfg.sweep.stop_hz = au::hertz(static_cast<double>(raw));
    }
    if (auto* el = opt(sweep, "dwell_samples"))     el->QueryIntText(&cfg.sweep.dwell_samples);
    if (auto* el = opt(sweep, "fft_size"))          el->QueryIntText(&cfg.sweep.fft_size);
    if (auto* el = opt(sweep, "usable_bw_fraction"))el->QueryDoubleText(&cfg.sweep.usable_bw_fraction);
    if (auto* el = opt(sweep, "threshold_db"))      el->QueryDoubleText(&cfg.sweep.threshold_db);
    if (auto* el = opt(sweep, "min_signal_bw_hz")) {
        uint64_t raw = 0; el->QueryUnsigned64Text(&raw);
        cfg.sweep.min_signal_bw_hz = au::hertz(static_cast<double>(raw));
    }
    if (auto* el = opt(sweep, "settle_samples"))    el->QueryIntText(&cfg.sweep.settle_samples);
    if (auto* el = opt(sweep, "dc_guard_hz")) {
        uint64_t raw = 0; el->QueryUnsigned64Text(&raw);
        cfg.sweep.dc_guard_hz = au::hertz(static_cast<double>(raw));
    }
    if (auto* el = opt(sweep, "cfar_guard_bins"))   el->QueryIntText(&cfg.sweep.cfar_guard_bins);
    if (auto* el = opt(sweep, "cfar_ref_bins"))     el->QueryIntText(&cfg.sweep.cfar_ref_bins);
    if (auto* el = opt(sweep, "min_papr_db"))       el->QueryFloatText(&cfg.sweep.min_papr_db);
    if (auto* el = opt(sweep, "noise_floor_alpha")) el->QueryFloatText(&cfg.sweep.noise_floor_alpha);

    if (cfg.sweep.stop_hz <= cfg.sweep.start_hz)
        throw std::runtime_error("sweep stop_hz must be > start_hz");
    if (cfg.sweep.fft_size < 64 || (cfg.sweep.fft_size & (cfg.sweep.fft_size - 1)) != 0)
        throw std::runtime_error("fft_size must be a power of 2 >= 64");
    if (cfg.sweep.dwell_samples < cfg.sweep.fft_size)
        cfg.sweep.dwell_samples = cfg.sweep.fft_size;
    if (cfg.sweep.usable_bw_fraction <= 0.0 || cfg.sweep.usable_bw_fraction > 1.0)
        throw std::runtime_error("usable_bw_fraction must be in (0, 1]");
    if (cfg.sweep.cfar_guard_bins < 0)
        throw std::runtime_error("cfar_guard_bins must be >= 0");
    if (cfg.sweep.cfar_ref_bins < 1)
        throw std::runtime_error("cfar_ref_bins must be >= 1");
    if (cfg.sweep.noise_floor_alpha <= 0.0f || cfg.sweep.noise_floor_alpha > 1.0f)
        throw std::runtime_error("noise_floor_alpha must be in (0, 1]");

    // ── Apply pending bands now that sweep defaults are set ──────────────────
    cfg.bands = std::move(pending_bands);

    // ── Receiver ─────────────────────────────────────────────────────────────
    if (auto* rx = opt(root, "receiver")) {
        cfg.receiver.local_ip = textOrDefault(opt(rx, "local_ip"), cfg.receiver.local_ip.c_str());
        if (auto* el = opt(rx, "port")) el->QueryIntText(&cfg.receiver.port);
    }

    return cfg;
}


std::vector<BandConfig> SweepConfig::splitBands(const std::vector<BandConfig>& bands,
                                                   int n_devices) {
    if (n_devices <= static_cast<int>(bands.size())) return bands;

    std::vector<BandConfig> out;
    out.reserve(static_cast<size_t>(n_devices));
    const int nb        = static_cast<int>(bands.size());
    const int base      = n_devices / nb;
    const int remainder = n_devices % nb;

    for (int i = 0; i < nb; ++i) {
        const int    pieces = base + (i < remainder ? 1 : 0);
        const double lo     = bands[i].start_hz.in(au::hertz);
        const double hi     = bands[i].stop_hz.in(au::hertz);
        const double step   = (hi - lo) / pieces;
        for (int j = 0; j < pieces; ++j) {
            BandConfig bc;
            bc.device_id       = bands[i].device_id;
            bc.device_required = bands[i].device_required;
            bc.start_hz  = au::hertz(lo + j       * step);
            bc.stop_hz   = au::hertz(lo + (j + 1) * step);
            out.push_back(bc);
        }
    }
    return out;
}

} // namespace acq

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
