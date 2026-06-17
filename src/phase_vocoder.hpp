#pragma once
#include <complex>
#include <cmath>
#include <cstring>
#include <algorithm>

// Phase vocoder pitch shifter — no external dependencies.
//
// Algorithm: frequency-domain bin remapping (Laroche & Dolson).
// The frozen loop is read at normal speed; pitch shift is achieved by
// mapping analysis bin k → synthesis bin round(k × ratio).
//
// FFT size is controlled at compile time:
//   -DMEGALO_PV_N=2048   (default, recommended for Pi 5)
//   -DMEGALO_PV_N=1024   (lighter, use for MOD Dwarf)
//
// Latency: N/2 samples (~21 ms @ 48 kHz with N=2048).
// All buffers are member variables — zero stack allocation in process().

class PhaseVocoder {
public:
#ifdef MEGALO_PV_N
    static constexpr int N = MEGALO_PV_N;
#else
    static constexpr int N = 2048;
#endif
    static constexpr int HOP    = N / 4;        // 75 % overlap
    static constexpr int BINS   = N / 2 + 1;
    static constexpr int OUTBUF = N * 4;        // ring buffer ≥ 2 × max unread

    void init(double sr) noexcept {
        _sr        = static_cast<float>(sr);
        _freq_pbin = _sr / N;
        _osamp     = static_cast<float>(N) / HOP;   // = 4
        for (int i = 0; i < N; i++)
            _win[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (N - 1)));
        reset();
    }

    void reset() noexcept {
        std::memset(_ana_phase, 0, sizeof _ana_phase);
        std::memset(_syn_phase, 0, sizeof _syn_phase);
        std::memset(_ana_mag,   0, sizeof _ana_mag);
        std::memset(_ana_freq,  0, sizeof _ana_freq);
        std::memset(_syn_mag,   0, sizeof _syn_mag);
        std::memset(_syn_freq,  0, sizeof _syn_freq);
        std::memset(_out_buf,   0, sizeof _out_buf);
        for (auto& c : _cx) c = {};
        _read_pos  = 0.0;
        _hop_cnt   = 0;
        _out_write = 0;
        _out_read  = 0;
        _out_fill  = 0;
    }

    void set_pitch(float semitones) noexcept {
        _ratio = std::pow(2.0f, semitones / 12.0f);
    }

    // Returns one pitch-shifted sample from the frozen loop.
    // Must be called once per output sample from plugin::run().
    float process(const float* loop, int loop_len) noexcept {
        if (loop_len == 0) return 0.0f;

        if (++_hop_cnt >= HOP) {
            _hop_cnt   = 0;
            _read_pos += HOP;
            if (_read_pos >= loop_len) _read_pos -= loop_len;
            _process_frame(loop, loop_len);
        }

        if (_out_fill <= 0) return 0.0f;
        float out = _out_buf[_out_read];
        _out_buf[_out_read] = 0.0f;
        _out_read = (_out_read + 1) % OUTBUF;
        --_out_fill;
        return out;
    }

private:
    // ── FFT frame ─────────────────────────────────────────────────────────
    void _process_frame(const float* loop, int loop_len) noexcept {
        // Collect N samples centered at _read_pos (look back N-HOP, forward HOP)
        double start = _read_pos - (N - HOP);
        for (int i = 0; i < N; i++) {
            double p = start + i;
            // Wrap into [0, loop_len)
            p -= loop_len * std::floor(p / loop_len);
            int   i0   = static_cast<int>(p);
            float frac = static_cast<float>(p - i0);
            int   i1   = (i0 + 1) % loop_len;
            _cx[i] = { (loop[i0] + frac * (loop[i1] - loop[i0])) * _win[i], 0.0f };
        }

        _fft(false);

        // ── Analysis: true instantaneous frequency per bin ─────────────────
        const float expct = 2.0f * float(M_PI) * HOP / N;
        for (int k = 0; k < BINS; k++) {
            float mag   = std::abs(_cx[k]);
            float phase = std::arg(_cx[k]);

            float dp = phase - _ana_phase[k];
            _ana_phase[k] = phase;

            // Remove expected phase advance, wrap deviation to [-π, π]
            dp -= k * expct;
            dp -= 2.0f * float(M_PI) * std::round(dp * float(M_1_PI) * 0.5f);

            _ana_mag[k]  = mag;
            _ana_freq[k] = k * _freq_pbin + dp * _osamp * _freq_pbin;
        }

        // ── Pitch shift: remap bin k → bin ⌊k × ratio + 0.5⌋ ────────────
        std::memset(_syn_mag,  0, sizeof _syn_mag);
        std::memset(_syn_freq, 0, sizeof _syn_freq);
        for (int k = 0; k < BINS; k++) {
            int dst = static_cast<int>(k * _ratio + 0.5f);
            if (dst < BINS) {
                _syn_mag[dst]  += _ana_mag[k];
                _syn_freq[dst]  = _ana_freq[k] * _ratio;
            }
        }

        // ── Synthesis: accumulate phase, build complex spectrum ────────────
        const float synth_expct = 2.0f * float(M_PI) * HOP / _sr;
        for (int k = 0; k < BINS; k++) {
            _syn_phase[k] += _syn_freq[k] * synth_expct;
            _cx[k] = std::polar(_syn_mag[k], _syn_phase[k]);
        }
        // Hermitian symmetry for real output
        for (int k = BINS; k < N; k++)
            _cx[k] = std::conj(_cx[N - k]);

        _fft(true);

        // ── Overlap-add ───────────────────────────────────────────────────
        // Hann analysis+synthesis normalization: the sum of w² over the
        // overlapping frames is 0.375 × osamp (= 1.5 at 4× / 75 % overlap),
        // so dividing by it yields unity passthrough. (The previous factor was
        // wrong by ~N/2, making the pitch voices ~1340× too quiet / inaudible.)
        const float scale = 1.0f / (0.375f * _osamp);
        for (int i = 0; i < N; i++) {
            int idx = (_out_write + i) % OUTBUF;
            _out_buf[idx] += _cx[i].real() * _win[i] * scale;
        }
        _out_write = (_out_write + HOP) % OUTBUF;
        _out_fill  = std::min(_out_fill + HOP, OUTBUF);
    }

    // ── Radix-2 DIT Cooley-Tukey FFT (in-place, operates on _cx) ─────────
    void _fft(bool inverse) noexcept {
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < N; i++) {
            int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(_cx[i], _cx[j]);
        }
        // Butterfly stages
        for (int len = 2; len <= N; len <<= 1) {
            float ang = float(M_PI) * (inverse ? 1.0f : -1.0f) * 2.0f / len;
            std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (int i = 0; i < N; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; j++) {
                    auto u = _cx[i + j];
                    auto v = _cx[i + j + len / 2] * w;
                    _cx[i + j]           = u + v;
                    _cx[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
        if (inverse)
            for (int i = 0; i < N; i++) _cx[i] /= float(N);
    }

    // ── State ──────────────────────────────────────────────────────────────
    float _sr        = 48000.0f;
    float _ratio     = 1.0f;
    float _freq_pbin = 48000.0f / N;
    float _osamp     = 4.0f;

    float _win[N]               = {};
    float _ana_phase[BINS]      = {};
    float _syn_phase[BINS]      = {};
    float _ana_mag[BINS]        = {};
    float _ana_freq[BINS]       = {};
    float _syn_mag[BINS]        = {};
    float _syn_freq[BINS]       = {};
    float _out_buf[OUTBUF]      = {};
    std::complex<float> _cx[N]  = {};

    double _read_pos  = 0.0;
    int    _hop_cnt   = 0;
    int    _out_write = 0;
    int    _out_read  = 0;
    int    _out_fill  = 0;
};
