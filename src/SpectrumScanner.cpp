#include "SpectrumScanner.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace acq {

using namespace std::chrono;

SpectrumScanner::SpectrumScanner(SweepConfig cfg, IqSource* source, DetectionCallback cb)
    : cfg_(std::move(cfg)), source_(source), cb_(std::move(cb))
{
    for (int i = 0; i < cfg_.device.rx_channels; ++i)
        processors_.push_back(std::make_unique<FftProcessor>(cfg_.sweep.fft_size));
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
            cfg_.sweep.start_hz / 1e6,
            cfg_.sweep.stop_hz  / 1e6);

        while (running_) {
            Dwell dwell;
            if (!source_->next(dwell) || !running_) break;

            int n_ch = std::min(static_cast<int>(dwell.ch_samples.size()),
                                static_cast<int>(processors_.size()));
            for (int ch = 0; ch < n_ch; ++ch) {
                if (dwell.ch_samples[ch].size() < (size_t)cfg_.sweep.fft_size)
                    continue;  // too few samples for FFT
                auto dets = processDwell(ch, dwell.center_hz, dwell.ch_samples[ch]);
                for (const auto& d : dets) {
                    spdlog::debug("[Scanner] detection {:.3f} MHz  BW {:.1f} kHz  {:.1f} dB",
                        d.center_freq_hz / 1e6, d.bandwidth_hz / 1e3, d.power_db);
                    cb_(d);
                }
            }
        }

        source_->close();
    }
}

std::vector<Detection> SpectrumScanner::processDwell(
    int ch, uint64_t center_hz,
    const std::vector<std::complex<float>>& samples)
{
    auto& proc = *processors_[ch];
    auto  sigs = proc.detect(
        samples.data(),
        static_cast<float>(cfg_.sweep.threshold_db),
        static_cast<float>(cfg_.sweep.usable_bw_fraction),
        cfg_.device.sample_rate,
        cfg_.sweep.min_signal_bw_hz);

    auto now = system_clock::now();
    std::vector<Detection> out;
    out.reserve(sigs.size());
    for (const auto& s : sigs) {
        uint64_t f_lo = processors_[ch]->binToHz(s.start_bin, cfg_.device.sample_rate, center_hz);
        uint64_t f_hi = processors_[ch]->binToHz(s.end_bin,   cfg_.device.sample_rate, center_hz);
        uint64_t fc   = processors_[ch]->binToHz((s.start_bin + s.end_bin) / 2,
                                      cfg_.device.sample_rate, center_hz);
        Detection d;
        d.timestamp      = now;
        d.center_freq_hz = fc;
        d.bandwidth_hz   = static_cast<uint32_t>(f_hi > f_lo ? f_hi - f_lo : 1);
        d.power_db       = s.peak_db;
        d.scanner_id     = cfg_.scanner_id;
        d.channel        = ch;
        out.push_back(d);
    }
    return out;
}

} // namespace acq
