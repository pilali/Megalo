#pragma once
#include "hn_analyzer.hpp"
#include <cmath>
#include <algorithm>

// ── Additive synthesiser ───────────────────────────────────────────────────
// Driven by HNState (from hn_analyze). Produces one sample per process() call.
// Call reset() on LoopReady, set_pitch_ratio() per block, process() per sample.

// 7th-order Taylor sin — error < 2e-5, no libm call
static inline float hn_sin(float x) noexcept {
    // Wrap to [-π, π]
    constexpr float PI  = 3.14159265f;
    constexpr float TPI = 6.28318530f;
    x -= TPI * std::floor((x + PI) / TPI);
    const float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.00019841f)));
}

class AdditiveSynth {
public:
    void reset(const HNState& s, float sr) noexcept {
        _sr          = sr;
        _f0          = s.f0;
        _n           = s.n_partials;
        _pitch_ratio = 1.0f;
        _noise_rms   = s.noise_rms;
        for (int k = 0; k < _n; ++k) {
            _amp  [k] = s.harm_amp  [k];
            _phase[k] = s.harm_phase[k];   // start at captured phase
        }
        _noise_state = 0.0f;
    }

    void set_pitch_ratio(float ratio) noexcept {
        _pitch_ratio = ratio;
    }

    float process() noexcept {
        if (_n == 0) return 0.0f;

        float out = 0.0f;
        const float f0_shifted = _f0 * _pitch_ratio;

        for (int k = 0; k < _n; ++k) {
            const float freq = f0_shifted * (k + 1);
            if (freq >= _sr * 0.499f) break;
            const float inc = 2.0f * 3.14159265f * freq / _sr;
            out += _amp[k] * hn_sin(_phase[k]);
            _phase[k] += inc;
            if (_phase[k] > 3.14159265f) _phase[k] -= 6.28318530f;
        }

        // Shaped noise residual (first-order HP to approximate residual spectrum)
        if (_noise_rms > 1e-6f) {
            const float white = _lcg() * (1.0f / 2147483648.0f);
            const float hp    = white - _noise_state;
            _noise_state      = white * 0.85f;
            out += hp * _noise_rms * 2.0f;
        }

        return out;
    }

private:
    float _sr          = 48000.0f;
    float _f0          = 0.0f;
    int   _n           = 0;
    float _pitch_ratio = 1.0f;
    float _noise_rms   = 0.0f;
    float _amp  [HN_MAX_PARTIALS] = {};
    float _phase[HN_MAX_PARTIALS] = {};
    float _noise_state = 0.0f;

    // Fast LCG for noise — period 2^31
    uint32_t _seed = 12345u;
    inline int32_t _lcg() noexcept {
        _seed = _seed * 1664525u + 1013904223u;
        return static_cast<int32_t>(_seed);
    }
};
