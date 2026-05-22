#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Harmonic + Noise analyzer ─────────────────────────────────────────────
//
// Runs ONCE on LoopReady (NOT inside the per-sample loop).
// Produces HNState consumed by AdditiveSynth for real-time resynthesis.
//
// Typical cost on Cortex-A7 @ 1 GHz:
//   YIN   on 1024 samples                   ≈ 1.5 ms
//   Goertzel for 32 partials × 4096 samples ≈ 1.5 ms
//   Total ≈ 3 ms — acceptable at block boundary (one-shot per capture)

static constexpr int   HN_MAX_PARTIALS = 32;
static constexpr float HN_F0_MIN       = 40.0f;   // ~E1, below bass guitar low E
static constexpr float HN_F0_MAX       = 2000.0f; // well above guitar fretboard
static constexpr float HN_CONF_THRESH  = 0.55f;   // minimum YIN confidence

struct HNState {
    float f0         = 0.0f;  // detected fundamental, Hz (0 = no pitch found)
    float confidence = 0.0f;  // YIN CMNDF confidence [0, 1]
    int   n_partials = 0;
    float harm_amp  [HN_MAX_PARTIALS] = {};
    float harm_phase[HN_MAX_PARTIALS] = {};
    float noise_rms  = 0.0f;  // broadband residual level
    bool  valid      = false; // false → caller should fall back to granular
};

// ── YIN F0 detector ───────────────────────────────────────────────────────
// Cumulative Mean Normalised Difference Function on at most WIN=1024 samples.
static float hn_yin_f0(const float* buf, int len, float sr,
                        float* conf_out) noexcept
{
    static constexpr int WIN     = 1024;
    static constexpr int TAU_MAX = WIN / 2;   // 512 → f0_min ≈ sr/512

    const int W = std::min(len, WIN);

    float d[TAU_MAX];
    d[0] = 1.0f;
    double cum = 0.0;
    for (int tau = 1; tau < TAU_MAX; ++tau) {
        double sum = 0.0;
        for (int j = 0; j < W - tau; ++j) {
            double diff = static_cast<double>(buf[j]) - buf[j + tau];
            sum += diff * diff;
        }
        cum += sum;
        d[tau] = (cum > 0.0) ? static_cast<float>(sum * tau / cum) : 0.0f;
    }

    // First minimum below threshold; fall back to global minimum
    const float thresh = 0.15f;
    int   best_tau = -1;
    float best_val = 1.0f;
    for (int tau = 2; tau < TAU_MAX - 1; ++tau) {
        if (d[tau] < thresh && d[tau] <= d[tau - 1] && d[tau] <= d[tau + 1]) {
            best_tau = tau;
            best_val = d[tau];
            break;
        }
        if (d[tau] < best_val) { best_val = d[tau]; best_tau = tau; }
    }
    if (conf_out) *conf_out = 1.0f - best_val;
    if (best_tau <= 0) return 0.0f;

    // Parabolic sub-sample refinement
    float frac = 0.0f;
    if (best_tau > 0 && best_tau < TAU_MAX - 1) {
        float a = d[best_tau - 1], b = d[best_tau], c = d[best_tau + 1];
        float denom = 2.0f * (a - 2.0f * b + c);
        if (std::abs(denom) > 1e-9f) frac = (a - c) / denom;
    }
    return sr / (static_cast<float>(best_tau) + frac);
}

// ── Full H+N analysis ─────────────────────────────────────────────────────
static HNState hn_analyze(const float* loop, int loop_len, float sr) noexcept
{
    HNState st;
    if (loop_len < 256) return st;

    // 1. F0 via YIN
    float conf = 0.0f;
    const float f0 = hn_yin_f0(loop, loop_len, sr, &conf);
    if (f0 < HN_F0_MIN || f0 > HN_F0_MAX || conf < HN_CONF_THRESH)
        return st;   // not pitched — granular fallback will be used

    st.f0         = f0;
    st.confidence = conf;

    // 2. Harmonic partial extraction via Goertzel (bounded to 4096 samples)
    const int ANA_LEN = std::min(loop_len, 4096);
    float signal_ss = 0.0f;
    for (int i = 0; i < ANA_LEN; ++i) signal_ss += loop[i] * loop[i];
    signal_ss /= static_cast<float>(ANA_LEN);

    float harm_ss = 0.0f;
    st.n_partials = 0;

    for (int k = 1; k <= HN_MAX_PARTIALS; ++k) {
        const float freq_k = f0 * static_cast<float>(k);
        if (freq_k >= sr * 0.48f) break;   // Nyquist guard

        const float omega = 2.0f * float(M_PI) * freq_k / sr;
        const float coeff = 2.0f * std::cos(omega);
        float s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < ANA_LEN; ++i) {
            float s0 = loop[i] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        const float re  = s1 - s2 * std::cos(omega);
        const float im  = s2 * std::sin(omega);
        const float amp = 2.0f * std::sqrt(re * re + im * im)
                          / static_cast<float>(ANA_LEN);

        st.harm_amp  [k - 1] = amp;
        st.harm_phase[k - 1] = std::atan2(im, re);
        harm_ss += 0.5f * amp * amp;
        ++st.n_partials;
    }

    // 3. Noise RMS ≈ residual after harmonic subtraction (energy estimate)
    const float residual_ss = std::max(0.0f, signal_ss - harm_ss);
    st.noise_rms = std::sqrt(residual_ss);

    st.valid = true;
    return st;
}
