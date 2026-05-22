#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Harmonic + Noise analysis ──────────────────────────────────────────────
// Runs ONCE per captured loop (not per sample) on LoopReady.
// Output feeds AdditiveSynth for real-time re-synthesis.

static constexpr int   HN_MAX_PARTIALS = 32;
static constexpr float HN_F0_MIN       = 40.0f;
static constexpr float HN_F0_MAX       = 2000.0f;
static constexpr float HN_CONF_THRESH  = 0.55f;

struct HNState {
    float f0          = 0.0f;
    float confidence  = 0.0f;
    int   n_partials  = 0;
    float harm_amp  [HN_MAX_PARTIALS] = {};
    float harm_phase[HN_MAX_PARTIALS] = {};
    float noise_rms   = 0.0f;
    bool  valid       = false;
};

// YIN F0 estimator on up to 1024 samples of the loop.
static inline float hn_yin_f0(const float* buf, int len, float sr,
                               float* conf_out) noexcept
{
    const int W = std::min(len, 1024);
    const int T_MAX = static_cast<int>(sr / HN_F0_MIN) + 1;
    const int T_MIN = static_cast<int>(sr / HN_F0_MAX);

    // CMNDF (Cumulative Mean Normalised Difference Function)
    static float d[1024];
    d[0] = 1.0f;
    float running_sum = 0.0f;
    for (int tau = 1; tau < T_MAX && tau < W / 2; ++tau) {
        float diff = 0.0f;
        for (int j = 0; j < W - tau; ++j) {
            float v = buf[j] - buf[j + tau];
            diff += v * v;
        }
        running_sum += diff;
        d[tau] = (running_sum > 0.0f) ? diff * tau / running_sum : 1.0f;
    }

    // Find first minimum below threshold
    int best_tau = -1;
    for (int tau = T_MIN + 1; tau < T_MAX && tau < W / 2 - 1; ++tau) {
        if (d[tau] < 0.1f && d[tau] < d[tau - 1] && d[tau] <= d[tau + 1]) {
            best_tau = tau;
            break;
        }
    }
    if (best_tau < 0) {
        // Absolute minimum above threshold
        float min_val = 1.0f;
        for (int tau = T_MIN; tau < T_MAX && tau < W / 2; ++tau) {
            if (d[tau] < min_val) { min_val = d[tau]; best_tau = tau; }
        }
    }
    if (best_tau <= 0) { *conf_out = 0.0f; return 0.0f; }

    // Parabolic interpolation
    float frac = 0.0f;
    if (best_tau > 0 && best_tau < W / 2 - 1) {
        float a = d[best_tau - 1], b = d[best_tau], c = d[best_tau + 1];
        float denom = 2.0f * (2.0f * b - a - c);
        if (std::abs(denom) > 1e-9f) frac = (a - c) / denom;
    }
    float tau_f = static_cast<float>(best_tau) + frac;
    *conf_out = 1.0f - d[best_tau];
    return sr / tau_f;
}

// Full H+N analysis: YIN f0 → Goertzel partials → noise RMS
static inline HNState hn_analyze(const float* loop, int loop_len,
                                  float sr) noexcept
{
    HNState s;
    if (!loop || loop_len < 64) return s;

    s.f0 = hn_yin_f0(loop, loop_len, sr, &s.confidence);
    if (s.confidence < HN_CONF_THRESH || s.f0 < HN_F0_MIN || s.f0 > HN_F0_MAX)
        return s;

    // Goertzel per harmonic on up to 4096 samples
    const int N = std::min(loop_len, 4096);
    float harmonic_power = 0.0f;
    float total_power    = 0.0f;

    for (int k = 1; k <= HN_MAX_PARTIALS; ++k) {
        const float freq = s.f0 * k;
        if (freq >= sr * 0.499f) { s.n_partials = k - 1; break; }

        const float omega = 2.0f * static_cast<float>(M_PI) * freq / sr;
        const float coeff = 2.0f * std::cos(omega);
        float q1 = 0.0f, q2 = 0.0f;
        for (int n = 0; n < N; ++n) {
            float q0 = loop[n] + coeff * q1 - q2;
            q2 = q1; q1 = q0;
        }
        const float re = q1 - q2 * std::cos(omega);
        const float im =      q2 * std::sin(omega);
        s.harm_amp  [k - 1] = 2.0f * std::sqrt(re * re + im * im) / N;
        s.harm_phase[k - 1] = std::atan2(im, re);
        harmonic_power += s.harm_amp[k - 1] * s.harm_amp[k - 1];
        s.n_partials = k;
    }

    for (int n = 0; n < N; ++n) total_power += loop[n] * loop[n];
    total_power /= N;
    const float residual = total_power - harmonic_power * 0.5f;
    s.noise_rms = (residual > 0.0f) ? std::sqrt(residual) : 0.0f;
    s.valid     = (s.n_partials > 0);
    return s;
}
