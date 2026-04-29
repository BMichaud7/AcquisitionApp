#pragma once
#include <atomic>

// Global knobs for the in-process fake SoapySDR device used by acq unit tests.
// All fields are atomic so the sweep thread can read them while the test thread writes.
namespace FakeAcqSoapy {

extern std::atomic<bool>  fail_open;       // Device::make() throws when true
extern std::atomic<bool>  tone_enabled;    // Inject a complex tone into readStream
extern std::atomic<float> tone_freq_frac;  // Fraction of sample rate in [0, 1)
extern std::atomic<float> tone_amplitude;  // Tone amplitude (0 = silence)

void reset();  // Restore all fields to defaults

} // namespace FakeAcqSoapy
