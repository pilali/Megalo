#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

// ── Spectral freeze (Paulstretch-style) ─────────────────────────────────────────────
// On LoopReady, capture the magnitude spectrum of the loop (averaged across a
// few analysis frames to smooth out transients).  At synthesis time, rebuild
// the complex spectrum with the frozen magnitudes and FRESH RANDOM PHASES
// every hop, IFFT → synthesis Hann → 75 % overlap-add.
//
// 75 % overlap (HOP = N/4) gives exact Hann² COLA reconstruction — no
// amplitude ripple.  Random phases produce a smooth, non-repeating texture
// whose long-term spectrum matches the input.  Loses inter-bin coherence,
// trading sharpness for a pad/ambient character — exactly what we want for
// natural-sustain freeze on polyphonic material.

class SpectralFreeze {
public:
    static constexpr int N   = 1024;
    static constexpr int HOP = N / 4;   // 75 % overlap → Hann² COLA-perfect

    bool valid = false;

    void reset() noexcept {
        valid = false;
        std::memset(_out_buf, 0, sizeof(_out_buf));
        _out_pos     = HOP;
        _pitch_ratio = 1.0f;
    }

    void seed(uint32_t s) noexcept { _rng = s ? s : 0x9E3779B9u; }

    void init(const float* loop, int loop_len) noexcept {
        if (!loop || loop_len < 32) { valid = false; return; }

        // Average magnitudes across N_FRAMES windows spaced through the loop.
        // Smooths over transient content and gives a more representative
        // steady-state spectrum than a single snapshot.
        std::memset(_mag, 0, sizeof(_mag));
        constexpr int N_FRAMES = 3;
        const int max_off = std::max(0, loop_len - N);
        const int step    = (N_FRAMES > 1) ? max_off / (N_FRAMES - 1) : 0;

        for (int f = 0; f < N_FRAMES; ++f) {
            const int off = step * f;
            for (int i = 0; i < N; ++i) {
                const float w  = 0.5f - 0.5f * cosf(k2PI * i / N);
                const int   si = std::min(off + i, loop_len - 1);
                _re[i] = loop[si] * w;
                _im[i] = 0.0f;
            }
            fft_radix2(_re, _im, N, false);
            const float inv = 1.0f / static_cast<float>(N_FRAMES);
            for (int k = 0; k <= N / 2; ++k) {
                _mag[k] += sqrtf(_re[k]*_re[k] + _im[k]*_im[k]) * inv;
            }
        }

        std::memset(_out_buf, 0, sizeof(_out_buf));
        _out_pos = HOP;
        valid    = true;
    }

    // ratio = 1.0 → natural; small deviations transpose the spectrum by
    // resampling bin magnitudes.  Driven per-sample by the LFO for detune.
    void set_pitch_ratio(float ratio) noexcept { _pitch_ratio = ratio; }

    float process() noexcept {
        if (!valid) return 0.0f;
        if (_out_pos >= HOP) {
            for (int i = 0; i < N - HOP; ++i) _out_buf[i] = _out_buf[i + HOP];
            for (int i = N - HOP; i < N;     ++i) _out_buf[i] = 0.0f;
            synthesize_hop();
            _out_pos = 0;
        }
        return _out_buf[_out_pos++] * NORM;
    }

private:
    static constexpr float k2PI = 6.28318530f;
    // Hann analysis × Hann synthesis × 4-frame OLA at HOP=N/4 sums to 1.5
    // (constant).  Random phases add incoherently → empirically tuned.
    static constexpr float NORM = 1.0f;

    float    _mag[N/2 + 1] = {};
    float    _re[N] = {};
    float    _im[N] = {};
    float    _out_buf[N] = {};
    int      _out_pos     = HOP;
    float    _pitch_ratio = 1.0f;
    uint32_t _rng         = 0x9E3779B9u;

    inline float next_phase() noexcept {
        _rng = _rng * 1664525u + 1013904223u;
        const float u = static_cast<float>(_rng) * (1.0f / 4294967296.0f);
        return u * k2PI - 3.1415927f;   // uniform on [-π, π)
    }

    void synthesize_hop() noexcept {
        // DC and Nyquist: real-only, magnitude preserved
        _re[0]   = _mag[0];   _im[0]   = 0.0f;
        _re[N/2] = _mag[N/2]; _im[N/2] = 0.0f;

        // For each positive-frequency bin: take magnitude (optionally
        // resampled for pitch shift) and assign a fresh random phase.
        const float inv_ratio = 1.0f / _pitch_ratio;
        for (int k = 1; k < N/2; ++k) {
            float mag;
            if (_pitch_ratio == 1.0f) {
                mag = _mag[k];
            } else {
                // Source bin for output bin k: src = k / ratio (linear interp)
                const float src    = k * inv_ratio;
                const int   src_lo = static_cast<int>(src);
                const float frac   = src - src_lo;
                if (src_lo < 0 || src_lo >= N/2) {
                    mag = 0.0f;
                } else {
                    mag = _mag[src_lo] * (1.0f - frac);
                    if (src_lo + 1 <= N/2) mag += _mag[src_lo + 1] * frac;
                }
            }
            const float phi = next_phase();
            _re[k] = mag * cosf(phi);
            _im[k] = mag * sinf(phi);
        }

        // Hermitian symmetry for real-valued IFFT
        for (int k = N/2 + 1; k < N; ++k) {
            _re[k] =  _re[N - k];
            _im[k] = -_im[N - k];
        }

        fft_radix2(_re, _im, N, true);   // in-place IFFT (divides by N)

        // Synthesis Hann window + overlap-add
        for (int i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * cosf(k2PI * i / N);
            _out_buf[i] += _re[i] * w;
        }
    }

    // In-place Cooley-Tukey radix-2 DIT FFT / IFFT.
    static void fft_radix2(float* re, float* im, int n, bool inverse) noexcept {
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        const float sign = inverse ? 1.0f : -1.0f;
        for (int len = 2; len <= n; len <<= 1) {
            const float ang = sign * k2PI / len;
            const float wRe = cosf(ang), wIm = sinf(ang);
            for (int i = 0; i < n; i += len) {
                float cr = 1.0f, ci = 0.0f;
                for (int j = 0; j < len / 2; ++j) {
                    const float uR = re[i+j],              uI = im[i+j];
                    const float vR = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                    const float vI = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                    re[i+j]       = uR + vR;  im[i+j]       = uI + vI;
                    re[i+j+len/2] = uR - vR;  im[i+j+len/2] = uI - vI;
                    const float ncr = cr*wRe - ci*wIm;
                    ci = cr*wIm + ci*wRe;
                    cr = ncr;
                }
            }
        }
        if (inverse) {
            const float inv = 1.0f / n;
            for (int i = 0; i < n; ++i) { re[i] *= inv;  im[i] *= inv; }
        }
    }
};
