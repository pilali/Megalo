#pragma once
#include <cmath>
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
    float process(const float* loop, int loop_len,
                  int grain_samples, int xfade_samples,
                  float speed) noexcept {
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

        float out = 0.0f;
        for (int k = 0; k < N_VOICES; ++k) {
            Voice& v      = _v[k];
            v.grain_len   = grain_samples;
            v.xfade_len   = xfade_samples;

            // Trapezoidal cosine amplitude envelope
            out += _read(loop, loop_len, v, speed) * _amp(v);

            if (++v.cursor >= v.grain_len) {
                v.cursor = 0;
                v.pos    = _rand01() * (float)loop_len;  // truly random respawn
            }
        }

        return out * (1.0f / (float)N_VOICES);
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

    // Linear-interpolated read at pos + cursor*speed, wrapped into loop.
    static float _read(const float* loop, int loop_len,
                       const Voice& v, float speed) noexcept {
        float lp = v.pos + (float)v.cursor * speed;
        lp -= (float)loop_len * std::floor(lp / (float)loop_len);
        const int   i0   = (int)lp;
        const int   i1   = (i0 + 1) % loop_len;
        const float frac = lp - (float)i0;
        return loop[i0] + frac * (loop[i1] - loop[i0]);
    }

    float _rand01() noexcept {
        _rand = _rand * 1664525u + 1013904223u;
        return (float)(_rand >> 16) / 65535.0f;
    }

    Voice    _v[N_VOICES] = {};
    uint32_t _rand        = 12345u;
    bool     _init        = false;
};
