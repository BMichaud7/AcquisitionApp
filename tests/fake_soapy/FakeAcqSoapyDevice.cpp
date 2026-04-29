#include "FakeAcqSoapyControl.hpp"
#include "FakeAcqSoapyDevice.hpp"
#include <SoapySDR/Registry.hpp>
#include <stdexcept>

// ── Global control variable definitions ──────────────────────────────────────
namespace FakeAcqSoapy {
    std::atomic<bool>  fail_open       {false};
    std::atomic<bool>  tone_enabled    {false};
    std::atomic<float> tone_freq_frac  {0.25f};
    std::atomic<float> tone_amplitude  {1.0f};

    void reset() {
        fail_open      .store(false);
        tone_enabled   .store(false);
        tone_freq_frac .store(0.25f);
        tone_amplitude .store(1.0f);
    }
}

// ── SoapySDR plugin registration ──────────────────────────────────────────────
static SoapySDR::KwargsList findFakeAcq(const SoapySDR::Kwargs&) {
    if (FakeAcqSoapy::fail_open.load()) return {};
    SoapySDR::Kwargs k;
    k["driver"] = "fake_acq";
    k["label"]  = "Fake Acq SDR Device (unit-test)";
    return {k};
}

static SoapySDR::Device* makeFakeAcq(const SoapySDR::Kwargs& args) {
    if (FakeAcqSoapy::fail_open.load())
        throw std::runtime_error("FakeAcqSoapy: forced open failure");
    return new FakeAcqSoapyDevice(args);
}

// Static constructor registers "fake_acq" driver before any test runs.
static SoapySDR::Registry gFakeAcqRegistry("fake_acq", &findFakeAcq, &makeFakeAcq,
                                            SOAPY_SDR_ABI_VERSION);
