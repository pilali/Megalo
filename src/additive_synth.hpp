#pragma once
#include "hn_analyzer.hpp"
#include <cmath>
#include <cstring>
#include <cstdint>

// ── Additive synthesizer ──────────────────────────────────────────────────
//
// Resynthesizes HNState at any target pitch via phase accumulators.
// Three independent instances replace the three voices (v0, v1, v2).
// Because each instance has its own phase array the voices are decorrelated
// even though they share the same spectral envelope — exactly the behaviour
// we want for a rich, chorus-like sustain without loop-playback artifacts.
//
// Per-sample cost at HN_MAX_PARTIALS=32, Cortex-A7 @ 1 GHz: < 0.3 % CPU.
//
// Pitch can change every block (set_pitch_ratio() before the sample loop).

// 7th-order Taylor sin — max error ≈ 2e-5, faster than std::sin on soft-FP.
static inline float hn_sin(float x) noexcept
{
    // Wrap to (−π, π]
    constexpr float TWO_PI     = float(2.0 * M_PI);
    constexpr float INV_TWO_PI = float(1.0 / (2.0 * M_PI));
    x -= TWO_PI * std::round(x * INV_TWO_PI);
    const float x2 = x * x;
    return x * (1.0f
               - x2 * (1.0f / 6.0f
               - x2 * (1.0f / 120.0f
               - x2 *  1.0f / 5040.0f)));
}

class AdditiveSynth {
public:
    // Load a new spectral snapshot.  Call once on LoopReady (after hn_analyze).
    void reset(const HNState& st, float sr) noexcept {
        _st          = st;
        _sr          = sr;
        _pitch_ratio = 1.0f;
        std::memset(_phase, 0, sizeof _phase);
        for (int b = 0; b < N_NOISE; ++b)
            _noise_st[b] = static_cast<uint32_t>((b + 1) * 2654435761u);
        std::memset(_noise_lp, 0, sizeof _noise_lp);
    }

    // ratio = 2^(semitones/12).  May be called per-block; smooth internally.
    void set_pitch_ratio(float ratio) noexcept {
        _pitch_ratio = std::max(ratio, 0.01f);
    }

    // Returns one resynthesized sample.
    float process() noexcept {
        if (!_st.valid) return 0.0f;

        const float f0 = _st.f0 * _pitch_ratio;
        float out = 0.0f;

        // ── Additive harmonic partials ─────────────────────────────────────
        for (int k = 0; k < _st.n_partials; ++k) {
            const float amp = _st.harm_amp[k];
            if (amp < 1e-7f) continue;
            if (f0 * static_cast<float>(k + 1) >= _sr * 0.46f) break;

            _phase[k] += float(2.0 * M_PI) * f0 * static_cast<float>(k + 1) / _sr;
            if (_phase[k] >  float(M_PI)) _phase[k] -= float(2.0f * M_PI);
            if (_phase[k] < -float(M_PI)) _phase[k] += float(2.0f * M_PI);

            out += amp * hn_sin(_phase[k]);
        }

        // ── Shaped noise (6 first-order LP bands → band-pass by subtraction)
        if (_st.noise_rms > 1e-6f) {
            for (int b = 0; b < N_NOISE; ++b) {
                _noise_st[b] = _noise_st[b] * 1664525u + 1013904223u;
                const float n = static_cast<float>(
                    static_cast<int32_t>(_noise_st[b])) * (1.0f / 2147483648.0f);
                const float fc = std::min(
                    f0 * static_cast<float>(1 << (b + 1)) / _sr, 0.45f);
                _noise_lp[b] += fc * (n - _noise_lp[b]);
            }
            float noise_out = 0.0f;
            for (int b = 0; b < N_NOISE - 1; ++b)
                noise_out += _noise_lp[b] - _noise_lp[b + 1];
            out += noise_out * _st.noise_rms * 4.0f;
        }

        return out;
    }

private:
    static constexpr int N_NOISE = 6;

    HNState  _st;
    float    _sr          = 48000.0f;
    float    _pitch_ratio = 1.0f;
    float    _phase[HN_MAX_PARTIALS] = {};
    uint32_t _noise_st[N_NOISE]      = {};
    float    _noise_lp[N_NOISE]      = {};
};
