#pragma once
// Test-only. SoapySDR is NOT a dependency of the production acquisition binary.
// This header is compiled only into the acq_tests target.
#include "IqSource.hpp"
#include "SweepConfig.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <complex>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <algorithm>

namespace acq {

// Drives a SoapySDR device directly, presenting the same IqSource interface
// that SpectrumScanner expects from the task manager in production.
// driver should be "fake_acq" in unit tests; uri is the SoapySDR device address.
class SoapyIqSource : public IqSource {
public:
    SoapyIqSource(const SweepConfig& cfg,
                  std::string driver = "fake_acq",
                  std::string uri    = "")
        : cfg_(cfg), driver_(std::move(driver)), uri_(std::move(uri)) {}

    ~SoapyIqSource() override { close(); }

    int numChannels() const override { return cfg_.device.rx_channels; }

    void open() override {
        SoapySDR::Kwargs args;
        args["driver"] = driver_;
        if (!uri_.empty()) args["remote"] = uri_;

        device_ = SoapySDR::Device::make(args);
        if (!device_) throw std::runtime_error("SoapyIqSource: Device::make returned null");

        int n_ch = cfg_.device.rx_channels;
        for (int ch = 0; ch < n_ch; ++ch) {
            device_->setSampleRate(SOAPY_SDR_RX, ch, cfg_.device.sample_rate);
            device_->setGain(SOAPY_SDR_RX, ch, cfg_.device.rx_gain_db);
            device_->setFrequency(SOAPY_SDR_RX, ch, (double)cfg_.sweep.start_hz);
        }

        std::vector<size_t> chs;
        for (int i = 0; i < n_ch; ++i) chs.push_back((size_t)i);
        stream_ = device_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, chs);
        device_->activateStream(stream_);

        int ds = cfg_.sweep.dwell_samples;
        bufs_.assign((size_t)n_ch, std::vector<std::complex<float>>((size_t)ds));
        ptrs_.resize((size_t)n_ch);
        for (int ch = 0; ch < n_ch; ++ch) ptrs_[ch] = bufs_[ch].data();

        pos_  = cfg_.sweep.start_hz;
        step_ = static_cast<uint64_t>(cfg_.device.sample_rate * cfg_.sweep.usable_bw_fraction);
        if (step_ == 0) step_ = 1;
        running_ = true;
    }

    void close() override {
        running_ = false;
        if (stream_) {
            device_->deactivateStream(stream_);
            device_->closeStream(stream_);
            stream_ = nullptr;
        }
        if (device_) {
            SoapySDR::Device::unmake(device_);
            device_ = nullptr;
        }
    }

    void stop() override { running_ = false; }

    bool next(Dwell& d) override {
        if (!running_) return false;

        if (pos_ >= cfg_.sweep.stop_hz) pos_ = cfg_.sweep.start_hz;

        uint64_t center = pos_ + step_ / 2;
        int n_ch = cfg_.device.rx_channels;

        for (int ch = 0; ch < n_ch; ++ch)
            device_->setFrequency(SOAPY_SDR_RX, ch, (double)center);

        discardSamples(cfg_.sweep.settle_samples);

        int remaining = cfg_.sweep.dwell_samples;
        int got = 0;
        while (remaining > 0 && running_) {
            std::vector<void*> offsets(n_ch);
            for (int ch = 0; ch < n_ch; ++ch)
                offsets[ch] = reinterpret_cast<void*>(bufs_[ch].data() + got);
            int flags = 0; long long ts = 0;
            int n = device_->readStream(stream_, offsets.data(),
                                        (size_t)remaining, flags, ts, 1'000'000);
            if (n > 0) { got += n; remaining -= n; }
        }
        if (!running_) return false;

        d.center_hz = center;
        d.ch_samples.resize((size_t)n_ch);
        for (int ch = 0; ch < n_ch; ++ch)
            d.ch_samples[ch] = bufs_[ch];

        pos_ += step_;
        return true;
    }

private:
    void discardSamples(int n) {
        if (n <= 0) return;
        while (n > 0 && running_) {
            int chunk = std::min(n, cfg_.sweep.fft_size);
            int flags = 0; long long ts = 0;
            device_->readStream(stream_, ptrs_.data(), (size_t)chunk, flags, ts, 100'000);
            n -= chunk;
        }
    }

    SweepConfig       cfg_;
    std::string       driver_;
    std::string       uri_;
    SoapySDR::Device* device_{nullptr};
    SoapySDR::Stream* stream_{nullptr};
    uint64_t          pos_{0};
    uint64_t          step_{0};

    std::vector<std::vector<std::complex<float>>> bufs_;
    std::vector<void*>                            ptrs_;
    std::atomic<bool>                             running_{false};
};

} // namespace acq
