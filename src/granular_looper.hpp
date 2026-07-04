#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

// Grain player — pad synthesis from a frozen loop via randomised granular
// synthesis with configurable crossfade.
//
// Four independent voices play simultaneously, each reading from a
// randomly chosen position in the loop and applying a trapezoidal
// amplitude envelope (cosine fade-in + flat top + cosine fade-out).
// On completion every voice draws a new random start position, so
// consecutive grains are unrelated in phase → no audible repetition.
//
// Voices are initialised with staggered cursors so the output is at
// steady state from the very first sample.  Normalisation: 1/N_VOICES.

class GrainPlayer {
public:
    static constexpr int N_VOICES = 4;

    void reset() noexcept { _init = false; }

    // Re-seed the RNG stream. Used to decorrelate the right-channel grain
    // cloud from the left so the stereo image is wide. Call once at setup;
    // the stream then evolves independently across resets.
    void seed(uint32_t s) noexcept { _rand = s; }

    // loop         : frozen loop buffer (nullptr / len=0 → silent)
    // grain_samples: grain duration  [16, loop_len]
    // xfade_samples: cosine fade length at each end  [8, grain_samples/2]
    // speed        : loop read speed per cursor step (1.0 = normal pitch)
    // period       : dominant loop period in samples (FreezeEngine::period());
    //                > 0 enables phase-aligned respawn: two free-running voices
    //                that happen to read a periodic loop half a period apart
    //                cancel each other for a whole grain, which was audible as
    //                deep slow pumping on tonal material. 0 = free respawn.
    float process(const float* loop, int loop_len,
                  int grain_samples, int xfade_samples,
                  float speed, float period = 0.0f) noexcept {
        if (!loop || loop_len == 0) return 0.0f;

        xfade_samples = std::clamp(xfade_samples, 8, loop_len / 4);
        grain_samples = std::clamp(grain_samples, xfade_samples * 2 + 8, loop_len);
        speed         = std::clamp(speed, 0.25f, 4.0f);

        if (!_init) {
            const int hop = grain_samples / N_VOICES;
            for (int k = 0; k < N_VOICES; ++k) {
                _v[k].pos       = _rand01() * (float)loop_len;
                _v[k].cursor    = k * hop;          // stagger for steady state
                _v[k].grain_len = grain_samples;
                _v[k].xfade_len = xfade_samples;
            }
            _init = true;
        }

        float out  = 0.0f;
        float wsum = 0.0f;
        for (int k = 0; k < N_VOICES; ++k) {
            Voice& v      = _v[k];
            v.grain_len   = grain_samples;
            v.xfade_len   = xfade_samples;

            // Trapezoidal cosine amplitude envelope
            const float a = _amp(v);
            out  += _read(loop, loop_len, v, speed) * a;
            wsum += a;

            if (++v.cursor >= v.grain_len) {
                v.cursor  = 0;
                float raw = _rand01() * (float)loop_len;  // truly random respawn
                if (period > 0.0f) {
                    // Phase-align the new grain with what the previous voice is
                    // playing right now (see _aligned_pos).
                    const Voice& ref = _v[(k + N_VOICES - 1) % N_VOICES];
                    raw = _aligned_pos(loop, loop_len, raw,
                                       ref.pos + (float)ref.cursor * speed,
                                       speed, period);
                }
                v.pos = raw;
            }
        }

        // Normalise by the actual envelope sum (overlap-add style) instead of
        // the fixed 1/N: unless xfade == grain/4 the trapezoid sum ripples at
        // sr/grain_len Hz, which was audible as periodic pumping of the pad.
        // ×0.75 matches the average duty of the old 1/N scaling so existing
        // presets keep their level.
        return (out / std::max(wsum, 0.5f)) * 0.75f;
    }

private:
    struct Voice {
        float pos       = 0.0f;
        int   cursor    = 0;
        int   grain_len = 1;
        int   xfade_len = 8;
    };

    // Cosine fade-in → flat top → cosine fade-out
    static float _amp(const Voice& v) noexcept {
        const int c = v.cursor;
        const int g = v.grain_len;
        const int x = v.xfade_len;
        if (c < x)
            return 0.5f * (1.0f - std::cos(3.14159265f * (float)c / (float)x));
        if (c >= g - x)
            return 0.5f * (1.0f - std::cos(3.14159265f * (float)(g - c) / (float)x));
        return 1.0f;
    }

    // 4-point Catmull-Rom read at pos + cursor*speed, wrapped into loop.
    // Cubic interpolation keeps the top octave clean on transposed voices
    // (linear reads audibly dulled ±12 st and let images fold back).
    static float _read(const float* loop, int loop_len,
                       const Voice& v, float speed) noexcept {
        float lp = v.pos + (float)v.cursor * speed;
        lp -= (float)loop_len * std::floor(lp / (float)loop_len);
        const int   i1   = (int)lp;
        const float frac = lp - (float)i1;
        const int   i0   = (i1 > 0) ? i1 - 1 : loop_len - 1;
        const int   i2   = (i1 + 1 < loop_len) ? i1 + 1 : 0;
        const int   i3   = (i2 + 1 < loop_len) ? i2 + 1 : 0;
        const float y0 = loop[i0], y1 = loop[i1], y2 = loop[i2], y3 = loop[i3];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + y1;
    }

    float _rand01() noexcept {
        _rand = _rand * 1664525u + 1013904223u;
        return (float)(_rand >> 16) / 65535.0f;
    }

    // Refine a random respawn position so the new grain starts IN PHASE with
    // what a reference voice is currently playing: scan one period's worth of
    // offsets from `raw` and keep the best normalized correlation between the
    // candidate segment and the reference voice's current segment (both read
    // at the same speed). Correlating the actual signal (rather than snapping
    // to a k·period grid) means period-estimate error cannot accumulate over
    // large position jumps. Cost ≈ period/2 × K MACs per respawn — respawns
    // happen ~10×/s per voice, so this is noise next to the per-sample work.
    static float _aligned_pos(const float* loop, int loop_len, float raw,
                              float ref_pos, float speed, float period) noexcept {
        constexpr int K = 24;
        const int range = (int)period;
        if (range < 4 || loop_len < 2 * range + 8) return raw;

        float ref[K];
        {
            float p = ref_pos - (float)loop_len * std::floor(ref_pos / (float)loop_len);
            for (int i = 0; i < K; ++i) {
                ref[i] = loop[(int)p];
                p += speed;
                if (p >= (float)loop_len) p -= (float)loop_len;
            }
        }

        int   best_d = 0;
        float best_r = -1e30f;
        for (int d = 0; d < range; d += 2) {
            float p = raw + (float)d;
            p -= (float)loop_len * std::floor(p / (float)loop_len);
            float r = 0.0f, e = 1e-9f;
            for (int i = 0; i < K; ++i) {
                const float v = loop[(int)p];
                r += ref[i] * v;
                e += v * v;
                p += speed;
                if (p >= (float)loop_len) p -= (float)loop_len;
            }
            const float rn = r / std::sqrt(e);
            if (rn > best_r) { best_r = rn; best_d = d; }
        }

        float pos = raw + (float)best_d;
        pos -= (float)loop_len * std::floor(pos / (float)loop_len);
        return pos;
    }

    Voice    _v[N_VOICES] = {};
    uint32_t _rand        = 12345u;
    bool     _init        = false;
};
