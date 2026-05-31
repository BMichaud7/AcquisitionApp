#include "SpectrumScanner.hpp"
#include <au/units/seconds.hh>

#include <au/prefix.hh>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <future>
#include <unordered_set>

namespace acq {

using namespace std::chrono;

SpectrumScanner::SpectrumScanner(SweepConfig cfg, IqSource* source, DetectionCallback cb)
    : cfg_(std::move(cfg)), source_(source), cb_(std::move(cb))
{
    int n_ch = cfg_.device.rx_channels;
    for (int i = 0; i < n_ch; ++i)
        processors_.push_back(std::make_unique<FftProcessor>(
            cfg_.sweep.fft_size,
            cfg_.sweep.cfar_guard_bins,
            cfg_.sweep.cfar_ref_bins));
    ch_noise_floor_.resize((size_t)n_ch);
    ch_persist_.resize((size_t)n_ch);
}

SpectrumScanner::~SpectrumScanner() { stop(); }

void SpectrumScanner::start() {
    running_ = true;
    thread_  = std::thread([this]{ sweepLoop(); });
}

void SpectrumScanner::stop() {
    running_ = false;
    if (source_) source_->stop();
    if (thread_.joinable()) thread_.join();
}

void SpectrumScanner::sweepLoop() {
    while (running_) {
        try {
            source_->open();
        } catch (const std::exception& e) {
            spdlog::error("[Scanner] source open failed: {} — retrying in 5s", e.what());
            std::this_thread::sleep_for(seconds(5));
            continue;
        }

        spdlog::info("[Scanner] source open: {} channel(s)  {:.3f}–{:.3f} MHz",
            cfg_.device.rx_channels,
            cfg_.sweep.start_hz.in(au::hertz) / 1e6,
            cfg_.sweep.stop_hz.in(au::hertz)  / 1e6);

        while (running_) {
            Dwell dwell;
            if (!source_->next(dwell) || !running_) break;

            int n_ch = std::min(static_cast<int>(dwell.ch_samples.size()),
                                static_cast<int>(processors_.size()));

            if (n_ch == 1) {
                if (dwell.ch_samples[0].size() >= (size_t)cfg_.sweep.fft_size) {
                    for (auto& d : processDwell(0, dwell.center_hz, dwell.ch_samples[0]))
                        cb_(d);
                }
            } else {
                // Multi-channel: process concurrently, fire callbacks on sweep thread.
                std::vector<std::future<std::vector<Detection>>> futs;
                futs.reserve((size_t)n_ch);
                for (int ch = 0; ch < n_ch; ++ch) {
                    if (dwell.ch_samples[ch].size() < (size_t)cfg_.sweep.fft_size) continue;
                    futs.push_back(std::async(std::launch::async,
                        [this, ch, &dwell]{
                            return processDwell(ch, dwell.center_hz, dwell.ch_samples[ch]);
                        }));
                }
                for (auto& f : futs)
                    for (auto& d : f.get())
                        cb_(d);
            }
        }

        source_->close();

        if (running_ && cfg_.analysis_pause_ms > au::seconds(0.0)) {
            auto pause_ms = static_cast<long long>(
                cfg_.analysis_pause_ms.in(au::milli(au::seconds)));
            spdlog::info("[Scanner] analysis window — pausing {}ms before next sweep",
                         pause_ms);
            auto pause_end = steady_clock::now() + milliseconds(pause_ms);
            while (running_ && steady_clock::now() < pause_end)
                std::this_thread::sleep_for(milliseconds(100));
        }
    }
}

std::vector<Detection> SpectrumScanner::processDwell(
    int ch, au::QuantityD<au::Hertz> center_hz,
    const std::vector<std::complex<float>>& samples)
{
    auto& proc = *processors_[ch];

    // Use raw Hz value as map key for the persistence and noise-floor maps.
    const uint64_t center_hz_raw = static_cast<uint64_t>(
        std::llround(center_hz.in(au::hertz)));

    // ── Step 1: Welch spectrum (includes IQ correction estimation) ────────────
    proc.computeSpectrum(samples.data(), static_cast<int>(samples.size()));

    // ── Step 2: CA-CFAR detection ─────────────────────────────────────────────
    // hist_floor is NOT passed to detectFromSpectrum here. The EMA floor is
    // seeded from pdb (which includes signal power), so feeding it back into the
    // detector would raise the threshold above persistent signals and suppress them.
    // The floor is maintained separately for monitoring; it is not used in detection.
    auto sigs = proc.detectFromSpectrum(
        static_cast<float>(cfg_.sweep.threshold_db),
        static_cast<float>(cfg_.sweep.usable_bw_fraction),
        cfg_.device.sample_rate,
        cfg_.sweep.min_signal_bw_hz,
        cfg_.sweep.dc_guard_hz,
        cfg_.sweep.min_papr_db,
        nullptr);

    // ── Step 3: Asymmetric-EMA per-frequency noise floor (monitoring only) ───
    // Updated AFTER detection so signal-bin vs noise-bin distinction can be made.
    // Rises quickly when new interference appears; falls slowly when it clears.
    auto& floor_map = ch_noise_floor_[ch];
    auto  it        = floor_map.find(center_hz_raw);
    const auto& pdb = proc.powerDb();
    if (it == floor_map.end()) {
        floor_map[center_hz_raw] = pdb;
        it = floor_map.find(center_hz_raw);
    } else {
        const float alpha_fall = cfg_.sweep.noise_floor_alpha;
        const float alpha_rise = std::min(4.0f * alpha_fall, 0.4f);
        auto& floor = it->second;
        for (int k = 0; k < (int)floor.size(); ++k) {
            float a = (pdb[k] > floor[k]) ? alpha_rise : alpha_fall;
            floor[k] = a * pdb[k] + (1.0f - a) * floor[k];
        }
    }

    // ── Step 4: Build IQ snapshot for ONNX fast-path / MUSIC ─────────────────
    // Built once per dwell and shared across all Detection objects via shared_ptr
    // (zero-copy: N detections → one allocation instead of N × 8 KB copies).
    static constexpr int SNAP_SAMPLES = 1024;
    auto dwell_snapshot = std::make_shared<std::vector<float>>();
    {
        int snap_n = std::min(SNAP_SAMPLES, (int)samples.size());
        dwell_snapshot->reserve(snap_n * 2);
        for (int i = 0; i < snap_n; ++i) {
            dwell_snapshot->push_back(samples[i].real());
            dwell_snapshot->push_back(samples[i].imag());
        }
    }

    // ── Step 5: Persistence filter ────────────────────────────────────────────
    // Each candidate must be detected in ≥ PERSIST_MIN_HITS consecutive sweeps
    // before being emitted. Strong signals (PAPR ≥ PERSIST_BYPASS_PAPR_DB) emit
    // immediately without waiting for confirmation — they're clearly real.
    auto& pmap = ch_persist_[ch][center_hz_raw];
    std::unordered_set<uint64_t> seen_this_dwell;
    seen_this_dwell.reserve(sigs.size() * 2);

    auto now = system_clock::now();
    std::vector<Detection> out;
    out.reserve(sigs.size());

    for (const auto& s : sigs) {
        auto f_lo_q = proc.binToHz(s.start_bin,  cfg_.device.sample_rate, center_hz);
        auto f_hi_q = proc.binToHz(s.end_bin,    cfg_.device.sample_rate, center_hz);
        auto fc_q   = proc.binToHz(s.center_bin, cfg_.device.sample_rate, center_hz);
        uint64_t f_lo = static_cast<uint64_t>(std::llround(f_lo_q.in(au::hertz)));
        uint64_t f_hi = static_cast<uint64_t>(std::llround(f_hi_q.in(au::hertz)));
        uint64_t fc   = static_cast<uint64_t>(std::llround(fc_q.in(au::hertz)));
        uint64_t qfc  = (fc / PERSIST_FREQ_QUANT_HZ) * PERSIST_FREQ_QUANT_HZ;

        seen_this_dwell.insert(qfc);

        auto& pe = pmap[qfc];
        ++pe.hits;
        pe.misses  = 0;
        pe.peak_db = std::max(pe.peak_db, s.peak_db);

        bool strong  = (s.peak_db - s.mean_db) >= PERSIST_BYPASS_PAPR_DB;
        bool confirm = pe.hits >= PERSIST_MIN_HITS;
        if (!strong && !confirm) continue;  // first-hit noise candidate: skip

        spdlog::debug("[Scanner] ch{} {:.3f} MHz  BW {:.1f} kHz  {:.1f} dB"
                      "  PAPR {:.1f} dB  hits={}",
            ch, fc / 1e6,
            (f_hi > f_lo ? f_hi - f_lo : 0) / 1e3,
            s.peak_db, s.peak_db - s.mean_db, pe.hits);

        Detection d;
        d.timestamp                  = now;
        d.center_freq_hz             = fc_q;
        d.bandwidth_hz               = au::hertz(f_hi > f_lo
                                           ? static_cast<double>(f_hi - f_lo)
                                           : 1.0);
        d.power_db                   = s.peak_db;
        d.scanner_id                 = cfg_.scanner_id;
        d.channel                    = ch;
        d.snr_db                     = s.peak_db - s.mean_db;  // PAPR proxy
        d.iq_snapshot                = dwell_snapshot;          // shared_ptr, zero-copy
        d.snapshot_sample_rate_sps   = cfg_.device.sample_rate;
        out.push_back(d);
    }

    // Decay signals not seen this dwell; expire stale entries entirely.
    for (auto it2 = pmap.begin(); it2 != pmap.end(); ) {
        if (!seen_this_dwell.count(it2->first)) {
            if (++it2->second.misses > PERSIST_MAX_MISSES)
                it2 = pmap.erase(it2);
            else
                ++it2;
        } else {
            ++it2;
        }
    }

    return out;
}

#include <spdlog/spdlog.h>
