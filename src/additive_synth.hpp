#pragma once
#include "hn_state.hpp"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ── Additive synthesizer ──────────────────────────────────────────────────
//
// Resynthesizes HNState at any target pitch via phase accumulators. Three
// independent instances drive the three voices (v0, v1, v2). Each instance has
// its own phase array, so the voices are decorrelated even though they share
// the same spectral envelope.
//
// Timbre shaping (set_timbre, applied per block, recomputed only on change):
//   brightness  spectral tilt   k^(0.6·b)        b ∈ [-1,1]  dark ↔ bright
//   damping     high roll-off   e^(-0.15·d·k)    d ∈ [ 0,1]  open ↔ muted
//   even_odd    even-harmonic   even ×(1+e)      e ∈ [-1,1]  hollow ↔ full
//   noise       breath/air      ×noise           n ∈ [ 0,1]  amount of residual
//
// Stereo width (process_stereo): the right channel reads each partial with a
// per-partial phase offset scaled by `width`, decorrelating L/R for a wide pad
// while staying mono-compatible at width = 0.

// 7th-order Taylor sin — max error ≈ 2e-5, faster than std::sin on soft-FP.
static inline float hn_sin(float x) noexcept
{
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
    // Load a new spectral snapshot. Call once on LoopReady (after analysis).
    void reset(const HNState& st, float sr) noexcept {
        _st          = st;
        _sr          = sr;
        _pitch_ratio = 1.0f;
        std::memcpy(_eff_amp, st.harm_amp, sizeof _eff_amp);   // neutral timbre
        std::memset(_phase, 0, sizeof _phase);
        for (int b = 0; b < N_NOISE; ++b)
            _noise_st[b] = static_cast<uint32_t>((b + 1) * 2654435761u);
        std::memset(_noise_lp, 0, sizeof _noise_lp);
        // Fixed per-partial phase offsets (golden-ratio sequence → well spread)
        // used to decorrelate the right channel for stereo width.
        for (int k = 0; k < HN_MAX_PARTIALS; ++k) {
            float frac = (k + 1) * 0.6180339887f;
            frac -= std::floor(frac);
            _woff[k] = (frac * 2.0f - 1.0f) * float(M_PI);
        }
    }

    // ratio = 2^(semitones/12). May be called per-block.
    void set_pitch_ratio(float ratio) noexcept {
        _pitch_ratio = std::max(ratio, 0.01f);
    }

    // Recompute per-partial gains from the timbre controls (per block, only on
    // change — cheap but not free with the pow/exp).
    void set_timbre(float brightness, float damping,
                    float even_odd, float noise) noexcept {
        _noise_scale = noise;
        for (int k = 0; k < _st.n_partials; ++k) {
            const float k1 = static_cast<float>(k + 1);
            float g = std::pow(k1, brightness * 0.6f);          // tilt
            g *= std::exp(-damping * static_cast<float>(k) * 0.15f);  // roll-off
            if (((k + 1) & 1) == 0)                             // even harmonic
                g *= std::max(0.0f, 1.0f + even_odd);
            _eff_amp[k] = _st.harm_amp[k] * g;
        }
    }

    // Mono sample.
    float process() noexcept {
        float l, r;
        render(l, r, 0.0f, false);
        return l;
    }

    // Stereo pair. width ∈ [0,1]: 0 = centred (l == r), 1 = fully decorrelated.
    void process_stereo(float& l, float& r, float width) noexcept {
        render(l, r, width, true);
    }

private:
    static constexpr int N_NOISE = 6;

    void render(float& l, float& r, float width, bool stereo) noexcept {
        l = r = 0.0f;
        if (!_st.valid) return;

        const float f0 = _st.f0 * _pitch_ratio;

        for (int k = 0; k < _st.n_partials; ++k) {
            const float amp = _eff_amp[k];
            // Measured partial frequency when available (keeps the string's
            // natural inharmonic stretch); ideal k·f0 otherwise (legacy
            // analyzers leave harm_freq at 0).
            const float fk = ((_st.harm_freq[k] > 0.0f)
                              ? _st.harm_freq[k]
                              : _st.f0 * static_cast<float>(k + 1)) * _pitch_ratio;
            if (fk >= _sr * 0.46f) break;
            if (amp < 1e-7f) continue;

            _phase[k] += float(2.0 * M_PI) * fk / _sr;
            if (_phase[k] >  float(M_PI)) _phase[k] -= float(2.0f * M_PI);
            if (_phase[k] < -float(M_PI)) _phase[k] += float(2.0f * M_PI);

            l += amp * hn_sin(_phase[k]);
            if (stereo) r += amp * hn_sin(_phase[k] + width * _woff[k]);
        }

        // ── Shaped noise (6 first-order LP bands → band-pass by subtraction)
        if (_st.noise_rms * _noise_scale > 1e-7f) {
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
            noise_out *= _st.noise_rms * _noise_scale * 4.0f;
            l += noise_out;
            if (stereo) r += noise_out;   // noise stays centred; width is harmonic
        }
    }

    HNState  _st;
    float    _sr          = 48000.0f;
    float    _pitch_ratio = 1.0f;
    float    _noise_scale = 0.4f;
    float    _eff_amp[HN_MAX_PARTIALS] = {};
    float    _phase  [HN_MAX_PARTIALS] = {};
    float    _woff   [HN_MAX_PARTIALS] = {};
    uint32_t _noise_st[N_NOISE]        = {};
    float    _noise_lp[N_NOISE]        = {};
};
