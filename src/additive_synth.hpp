#pragma once
#include <cmath>
#include <cstdint>
#include "hn_analyzer.hpp"

// ── Additive re-synthesis ──────────────────────────────────────────────────
// Driven by HNState produced by hn_analyze().
// Per-sample: sum of HN_MAX_PARTIALS sinusoids + shaped noise residual.

static inline float hn_sin(float x) noexcept {
    // 7th-order Taylor — good to ~0.1 % for |x| < π
    x -= static_cast<int>(x * (1.0f / 6.2831853f)) * 6.2831853f;
    if (x >  3.1415927f) x -= 6.2831853f;
    if (x < -3.1415927f) x += 6.2831853f;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f - x2 * 0.000198413f)));
}

class AdditiveSynth {
public:
    void reset(const HNState& s, float sr) noexcept {
        _sr  = sr;
        _state = s;
        for (int k = 0; k < HN_MAX_PARTIALS; ++k) _phase[k] = s.harm_phase[k];
        _noise_hp = 0.0f;
        _pitch_ratio = 1.0f;
    }

    void set_pitch_ratio(float r) noexcept { _pitch_ratio = r; }

    float process() noexcept {
        if (!_state.valid) return 0.0f;
        float out = 0.0f;
        for (int k = 0; k < _state.n_partials; ++k) {
            out += _state.harm_amp[k] * hn_sin(_phase[k]);
            _phase[k] += 6.2831853f * _state.f0 * (k + 1) * _pitch_ratio / _sr;
            if (_phase[k] > 3.1415927f) _phase[k] -= 6.2831853f;
        }
        // LCG noise + first-order HP
        _lcg = _lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(static_cast<int32_t>(_lcg)) * (1.0f / 2147483648.0f);
        const float hp = noise - _noise_hp;
        _noise_hp = noise * 0.95f;
        out += hp * _state.noise_rms;
        return out;
    }

private:
    float    _sr          = 48000.0f;
    float    _pitch_ratio = 1.0f;
    float    _phase[HN_MAX_PARTIALS] = {};
    float    _noise_hp    = 0.0f;
    uint32_t _lcg         = 12345u;
    HNState  _state       = {};
};
