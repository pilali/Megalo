#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// Granular looper — turns a frozen loop into a smooth, evolving pad sound.
//
// Six Hann-windowed grains run simultaneously with staggered phases and
// independently randomised start positions within the loop buffer.  On each
// grain completion the start position is re-randomised, providing the gentle
// textural variation that prevents the mechanical repetition of a raw loop.
//
// The output gain is normalised so that N evenly-spaced Hann windows sum to
// unity: scale = 2 / N_GRAINS.

class GranularLooper {
public:
    static constexpr int N_GRAINS = 6;

    void reset() noexcept {
        _initialized = false;
        // Keep _rand so consecutive resets don't produce identical textures.
    }

    // Returns one synthesised sample.  grain_samples must be > 0 and ≤ loop_len.
    float process(const float* loop, int loop_len, int grain_samples) noexcept {
        if (!loop || loop_len == 0) return 0.0f;
        grain_samples = std::clamp(grain_samples, 64, loop_len);

        if (!_initialized) {
            const int hop = grain_samples / N_GRAINS;
            for (int k = 0; k < N_GRAINS; ++k) {
                _grains[k].loop_start = _rand01() * (float)loop_len;
                _grains[k].pos        = k * hop;   // stagger phases
                _grains[k].len        = grain_samples;
            }
            _initialized = true;
        }

        float out = 0.0f;
        for (int k = 0; k < N_GRAINS; ++k) {
            Grain& g = _grains[k];
            g.len = grain_samples;   // track knob changes smoothly

            const float t      = (float)g.pos / (float)(g.len - 1);
            const float window = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * t));

            float lp = g.loop_start + (float)g.pos;
            while (lp >= (float)loop_len) lp -= (float)loop_len;
            const int   i0   = (int)lp;
            const int   i1   = (i0 + 1) % loop_len;
            const float frac = lp - (float)i0;

            out += (loop[i0] + frac * (loop[i1] - loop[i0])) * window;

            if (++g.pos >= g.len) {
                g.pos        = 0;
                g.loop_start = _rand01() * (float)loop_len;
            }
        }

        // N evenly-staggered Hann windows sum to N/2 → scale by 2/N for unity.
        return out * (2.0f / (float)N_GRAINS);
    }

private:
    struct Grain { float loop_start = 0.0f; int pos = 0; int len = 1; };

    float _rand01() noexcept {
        _rand = _rand * 1664525u + 1013904223u;
        return (float)(_rand >> 16) / 65535.0f;
    }

    Grain    _grains[N_GRAINS] = {};
    uint32_t _rand       = 12345u;
    bool     _initialized = false;
};
