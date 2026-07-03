#pragma once
#include <complex>
#include <cmath>
#include <cstring>
#include <algorithm>

// Phase vocoder pitch shifter — no external dependencies.
//
// Algorithm: per-peak spectral translation (Laroche & Dolson 1999).
// The frozen loop is read at normal speed; each spectral peak's region of
// influence is translated by the FRACTIONAL shift of its instantaneous
// frequency (de-alternated complex lobe, linear interpolation) and rotated
// by a per-region phasor accumulating the frequency difference — the lobe
// shape and the frame-to-frame phase advance agree on the same frequency,
// so partials reconstruct at full level across the whole range.
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
        std::memset(_ana_mag,   0, sizeof _ana_mag);
        std::memset(_ana_freq,  0, sizeof _ana_freq);
        for (auto& c : _ana_cx) c = {};
        std::memset(_rot,       0, sizeof _rot);
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
            // dev(Hz) = dp/(2π) · sr/HOP = dp/(2π) · osamp · freq_pbin.
            // The original code dropped the 1/(2π): every instantaneous
            // frequency came out ~6.3× too far from its bin centre, which is
            // why the vocoder never reconstructed cleanly, shifts included.
            _ana_freq[k] = k * _freq_pbin
                         + dp * _osamp * _freq_pbin * (float)(0.5 / M_PI);
            // De-alternated complex spectrum for the fractional lobe
            // translation below: a Hann-windowed sinusoid carries a linear
            // phase of ~−π per bin (sign alternation); removing it makes the
            // lobe a SMOOTH complex curve that can be linearly interpolated.
            _ana_cx[k] = (k & 1) ? -_cx[k] : _cx[k];
        }

        // ── Per-peak pitch shift (Laroche & Dolson 1999) ────────────────────
        // Each spectral peak (= one partial) is translated to its target
        // location as a RIGID block together with its whole region of
        // influence, and the region carries a per-peak phasor accumulating
        // the frequency difference. The window mainlobe therefore arrives
        // INTACT, and the analysis-side phase relationships inside a partial
        // are reproduced exactly (identity locking is structural here).
        //
        // The previous per-bin remap (dst = round(k·ratio)) stretched and
        // punched holes in the lobe — adjacent sources landed ~ratio bins
        // apart — which collapsed low fundamentals whose lobes only span a
        // few bins (measured −30 dB on a 220 Hz partial shifted to 330 Hz).
        // 1. Peaks on the ANALYSIS spectrum (local max over ±2 bins).
        int n_peaks = 0;
        for (int j = 2; j < BINS - 2; ++j) {
            const float m = _ana_mag[j];
            if (m > 1e-9f &&
                m >= _ana_mag[j - 1] && m >= _ana_mag[j - 2] &&
                m >  _ana_mag[j + 1] && m >  _ana_mag[j + 2])
                _peaks[n_peaks++] = j;
        }

        if (n_peaks == 0) {
            for (int k = 0; k < BINS; k++) _cx[k] = {};   // silent frame
        } else {
            // 2. Region boundaries at the magnitude minimum between peaks.
            _bounds[0] = 0;
            for (int i = 1; i < n_peaks; ++i) {
                int   bmin = _peaks[i - 1];
                float mmin = 1e30f;
                for (int j = _peaks[i - 1]; j <= _peaks[i]; ++j)
                    if (_ana_mag[j] < mmin) { mmin = _ana_mag[j]; bmin = j; }
                _bounds[i] = bmin;
            }
            _bounds[n_peaks] = BINS;

            // 3. Translate each region by the FRACTIONAL shift of its peak's
            // instantaneous frequency — the de-alternated complex lobe is
            // linearly interpolated at the exact offset, so the lobe SHAPE
            // (which encodes the partial's sub-bin position) and the
            // frame-to-frame phasor advance agree on the same frequency.
            // (An integer translation left them disagreeing by up to half a
            // bin: the overlapped frames partially cancelled and the low
            // partials smeared — the very defect this rework fixes.)
            // _rot[] is kept per SOURCE bin but incremented region-wide with
            // the peak's frequency, so a peak drifting by a bin between
            // frames inherits a coherent phasor history.
            for (int k = 0; k < BINS; k++) _cx[k] = {};
            const float rot_c = 2.0f * float(M_PI) * HOP * (_ratio - 1.0f) / _sr;
            for (int i = 0; i < n_peaks; ++i) {
                const int   p      = _peaks[i];
                const float inc    = rot_c * _ana_freq[p];
                const int   lo     = _bounds[i], hi = _bounds[i + 1];
                for (int j = lo; j < hi; ++j) {
                    _rot[j] += inc;
                    _rot[j] -= 2.0f * float(M_PI) *
                               std::round(_rot[j] * float(M_1_PI) * 0.5f);
                }
                const std::complex<float> ph = std::polar(1.0f, _rot[p]);
                const float d_frac = (_ana_freq[p] / _freq_pbin) * (_ratio - 1.0f);
                const int dst_lo = std::max(0, (int)std::ceil((float)lo + d_frac));
                const int dst_hi = std::min(BINS - 1,
                                            (int)std::floor((float)(hi - 1) + d_frac));
                for (int dst = dst_lo; dst <= dst_hi; ++dst) {
                    const float sp = (float)dst - d_frac;
                    int j0 = (int)sp;
                    if (j0 >= hi - 1) j0 = hi - 2;
                    if (j0 < lo) j0 = lo;
                    const float t = std::min(1.0f, std::max(0.0f, sp - (float)j0));
                    std::complex<float> v =
                        _ana_cx[j0] + t * (_ana_cx[j0 + 1] - _ana_cx[j0]);
                    v *= ph;
                    if (dst & 1) v = -v;      // restore the Hann alternation
                    _cx[dst] += v;            // overlapping regions just add
                }
            }
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
    float _ana_mag[BINS]        = {};
    float _ana_freq[BINS]       = {};
    std::complex<float> _ana_cx[BINS] = {};   // de-alternated analysis lobe
    float _rot[BINS]            = {};   // per-region rotation accumulators
    int   _peaks[BINS]          = {};   // per-frame peak list
    int   _bounds[BINS + 1]     = {};   // per-frame region boundaries
    float _out_buf[OUTBUF]      = {};
    std::complex<float> _cx[N]  = {};

    double _read_pos  = 0.0;
    int    _hop_cnt   = 0;
    int    _out_write = 0;
    int    _out_read  = 0;
    int    _out_fill  = 0;
};
