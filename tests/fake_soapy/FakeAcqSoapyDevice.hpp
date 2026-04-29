#pragma once
#include "FakeAcqSoapyControl.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// In-process SoapySDR device for acq unit tests.
// Registered under driver="fake_acq" via the SoapySDR plugin registry.
// Generates a configurable complex tone via FakeAcqSoapy:: knobs.
class FakeAcqSoapyDevice : public SoapySDR::Device {
public:
    explicit FakeAcqSoapyDevice(const SoapySDR::Kwargs& = {}) {}

    std::string getDriverKey()   const override { return "fake_acq"; }
    std::string getHardwareKey() const override { return "FakeAcqSoapyDevice"; }
    size_t      getNumChannels(const int) const override { return 2; }

    // ── Frequency ─────────────────────────────────────────────────────────
    void   setFrequency(const int, const size_t ch, const double f,
                        const SoapySDR::Kwargs&) override {
        if (ch < 4) freq_[ch] = f;
    }
    double getFrequency(const int, const size_t ch,
                        const std::string&) const override {
        return ch < 4 ? freq_[ch] : 0.0;
    }
    std::vector<std::string> listFrequencies(const int, const size_t) const override {
        return {"RF"};
    }

    // ── Sample rate ────────────────────────────────────────────────────────
    void   setSampleRate(const int, const size_t ch, const double r) override {
        if (ch < 4) rate_[ch] = r;
    }
    double getSampleRate(const int, const size_t ch) const override {
        return ch < 4 ? rate_[ch] : 0.0;
    }

    // ── Gain ───────────────────────────────────────────────────────────────
    void   setGain(const int, const size_t, const double g) override { gain_ = g; }
    double getGain(const int, const size_t) const override { return gain_; }
    void   setGainMode(const int, const size_t, const bool) override {}
    bool   getGainMode(const int, const size_t) const override { return false; }

    // ── Antenna ────────────────────────────────────────────────────────────
    void setAntenna(const int, const size_t, const std::string&) override {}

    // ── Stream ────────────────────────────────────────────────────────────
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                  const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(&stream_tok_);
    }
    int  activateStream(SoapySDR::Stream*, const int, const long long,
                        const size_t) override { return 0; }
    int  deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    void closeStream(SoapySDR::Stream*) override {}
    size_t getStreamMTU(SoapySDR::Stream*) const override { return 4096; }

    // readStream fills buffs[0] with a tone or silence.
    // Only channel 0 is written; other channels retain their zero-initialised state.
    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int& flags, long long& timeNs, const long) override {
        bool  tone = FakeAcqSoapy::tone_enabled.load();
        float frac = FakeAcqSoapy::tone_freq_frac.load();
        float amp  = FakeAcqSoapy::tone_amplitude.load();

        auto* buf = static_cast<float*>(buffs[0]);
        for (size_t i = 0; i < numElems; ++i) {
            if (tone) {
                float phi = 2.0f * 3.14159265f * frac * static_cast<float>(phase_ + i);
                buf[i * 2 + 0] = amp * std::cos(phi);
                buf[i * 2 + 1] = amp * std::sin(phi);
            } else {
                buf[i * 2 + 0] = 0.0f;
                buf[i * 2 + 1] = 0.0f;
            }
        }
        phase_ += numElems;
        flags = 0; timeNs = 0;
        return static_cast<int>(numElems);
    }

private:
    double   freq_[4]     = {};
    double   rate_[4]     = {};
    double   gain_        = 40.0;
    int      stream_tok_  = 0;
    uint64_t phase_       = 0;
};
