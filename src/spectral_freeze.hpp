#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Spectral freeze ────────────────────────────────────────────────────────
// Captures the magnitude spectrum of the loop on LoopReady, then re-synthesises
// continuously by advancing each bin's phase at its natural frequency.
// Result: the spectral envelope (timbre) stays frozen, but the waveform never
// repeats — no audible loop artefact, works on any polyphonic content.
//
// Algorithm: analysis Hann window → FFT → store |X[k]|, arg(X[k])
//            per hop: rebuild X from stored magnitudes + advancing phases
//            → IFFT → synthesis Hann window → 50 % overlap-add

class SpectralFreeze {
public:
    static constexpr int N   = 1024;   // FFT size (≈21 ms @ 48 kHz)
    static constexpr int HOP = N / 2;  // 50 % overlap

    bool valid = false;

    void reset() noexcept {
        valid = false;
        std::memset(_out_buf, 0, sizeof(_out_buf));
        _out_pos = HOP;   // triggers synthesis on first process() call
    }

    void init(const float* loop, int loop_len) noexcept {
        if (!loop || loop_len < 32) { valid = false; return; }

        // Hann-windowed snapshot of the loop
        for (int i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * cosf(k2PI * i / N);
            _re[i] = (i < loop_len ? loop[i] : 0.0f) * w;
            _im[i] = 0.0f;
        }
        fft_radix2(_re, _im, N, false);

        // Store magnitude, initial phase, and natural phase-advance per hop
        for (int k = 0; k <= N / 2; ++k) {
            _mag[k]       = sqrtf(_re[k]*_re[k] + _im[k]*_im[k]);
            _phase[k]     = atan2f(_im[k], _re[k]);
            _phase_adv[k] = k2PI * static_cast<float>(k) * HOP / N;
        }

        std::memset(_out_buf, 0, sizeof(_out_buf));
        _out_pos = HOP;
        valid = true;
    }

    // Returns one output sample.  Call once per sample in the RT loop.
    float process() noexcept {
        if (!valid) return 0.0f;
        if (_out_pos >= HOP) {
            // Slide the OLA buffer left by HOP, clear the tail, fill it.
            for (int i = 0; i < N - HOP; ++i) _out_buf[i] = _out_buf[i + HOP];
            for (int i = N - HOP; i < N;  ++i) _out_buf[i] = 0.0f;
            synthesize_hop();
            _out_pos = 0;
        }
        return _out_buf[_out_pos++] * NORM;
    }

private:
    static constexpr float k2PI = 6.28318530f;
    // NORM = 2: compensates for Hann analysis × synthesis with 50 % OLA
    // (each output point accumulates from 2 Hann-weighted IFFT frames whose
    //  OLA sum is 1.0; the Hann analysis window halves the effective amplitude).
    static constexpr float NORM = 2.0f;

    float _mag      [N/2 + 1] = {};
    float _phase    [N/2 + 1] = {};
    float _phase_adv[N/2 + 1] = {};
    float _re[N] = {};  // FFT work buffer (reused each hop)
    float _im[N] = {};
    float _out_buf[N] = {};
    int   _out_pos = HOP;

    void synthesize_hop() noexcept {
        // Reconstruct complex spectrum from frozen magnitudes + current phases
        _re[0] = _mag[0];  _im[0] = 0.0f;           // DC: real only
        for (int k = 1; k < N/2; ++k) {
            _re[k] = _mag[k] * cosf(_phase[k]);
            _im[k] = _mag[k] * sinf(_phase[k]);
            _phase[k] += _phase_adv[k];
            if (_phase[k] >  k2PI * 0.5f) _phase[k] -= k2PI;
            if (_phase[k] < -k2PI * 0.5f) _phase[k] += k2PI;
        }
        _re[N/2] = _mag[N/2];  _im[N/2] = 0.0f;     // Nyquist: real only

        // Conjugate symmetry for real-valued IFFT output
        for (int k = N/2 + 1; k < N; ++k) {
            _re[k] =  _re[N - k];
            _im[k] = -_im[N - k];
        }

        fft_radix2(_re, _im, N, true);   // in-place IFFT (divides by N)

        // Synthesis Hann window + overlap-add into _out_buf
        for (int i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * cosf(k2PI * i / N);
            _out_buf[i] += _re[i] * w;
        }
    }

    // In-place Cooley-Tukey radix-2 DIT FFT / IFFT.
    // inverse=false → FFT (no 1/N scaling).
    // inverse=true  → IFFT (applies 1/N scaling).
    static void fft_radix2(float* re, float* im, int n, bool inverse) noexcept {
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        // Butterfly stages
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
                    re[i+j]          = uR + vR;  im[i+j]          = uI + vI;
                    re[i+j+len/2]    = uR - vR;  im[i+j+len/2]    = uI - vI;
                    const float ncr  = cr*wRe - ci*wIm;
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
