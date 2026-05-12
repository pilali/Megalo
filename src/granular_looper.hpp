#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// Granular looper — converts a frozen loop into a smooth, evolving pad.
//
// Six Hann-windowed grains run simultaneously with staggered phases.
// A global playhead advances through the loop at 1 sample/sample.
// When a grain finishes it respawns near the playhead (±JITTER × grain size),
// which preserves pitch coherence while adding subtle textural variation.
//
// Contrast with fully-random positions (the previous approach): grains
// scattered across the whole loop produce random phase relationships that
// partially cancel on harmonic content, causing amplitude instability.
//
// Normalisation: N evenly-staggered Hann windows sum to N/2, so we
// multiply by 2/N to restore unity gain.

class GranularLooper {
public:
    static constexpr int N_GRAINS = 6;

    void reset() noexcept {
        _initialized = false;
        _playhead    = 0.0f;
        // Keep _rand so successive resets don't produce identical textures.
    }

    // Returns one synthesised output sample.
    // grain_samples : clamped to [64, loop_len] internally
    // scatter       : jitter radius as a fraction of grain size [0.0 – 1.0]
    // speed         : loop read speed (1.0 = normal, 2.0 = +1 oct, 0.5 = −1 oct)
    float process(const float* loop, int loop_len,
                  int grain_samples, float scatter, float speed) noexcept {
        if (!loop || loop_len == 0) return 0.0f;
        grain_samples = std::clamp(grain_samples, 64, loop_len);
        scatter       = std::clamp(scatter, 0.0f, 1.0f);
        speed         = std::clamp(speed, 0.25f, 4.0f);

        if (!_initialized) {
            const int hop = grain_samples / N_GRAINS;
            for (int k = 0; k < N_GRAINS; ++k) {
                // Distribute starting positions evenly around the loop so
                // grains are immediately at steady-state coverage.
                _grains[k].start  = (float)k * (float)loop_len / (float)N_GRAINS;
                _grains[k].cursor = k * hop;   // stagger Hann phases
                _grains[k].len    = grain_samples;
            }
            _playhead    = 0.0f;
            _initialized = true;
        }

        float out = 0.0f;
        for (int k = 0; k < N_GRAINS; ++k) {
            Grain& g = _grains[k];
            g.len = grain_samples;   // track knob changes

            // Hann window at current cursor position
            const float t      = (float)g.cursor / (float)(g.len - 1);
            const float window = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * t));

            // Loop read position advances at `speed` per cursor step — this
            // is what shifts the pitch without touching the Hann window duration.
            float lp = g.start + (float)g.cursor * speed;
            lp -= (float)loop_len * std::floor(lp / (float)loop_len);
            const int   i0   = (int)lp;
            const int   i1   = (i0 + 1) % loop_len;
            const float frac = lp - (float)i0;
            out += (loop[i0] + frac * (loop[i1] - loop[i0])) * window;

            if (++g.cursor >= g.len) {
                g.cursor = 0;
                // Respawn near the global playhead; scatter controls blur vs. coherence.
                const float jitter = _rand_bipolar() * (float)grain_samples * scatter;
                g.start = _playhead + jitter;
                g.start -= (float)loop_len * std::floor(g.start / (float)loop_len);
            }
        }

        _playhead += speed;
        if (_playhead >= (float)loop_len) _playhead -= (float)loop_len;

        return out * (2.0f / (float)N_GRAINS);
    }

private:
    struct Grain { float start = 0.0f; int cursor = 0; int len = 1; };

    float _rand_bipolar() noexcept {
        _rand = _rand * 1664525u + 1013904223u;
        return (float)(int32_t(_rand >> 16) - 32768) * (1.0f / 32768.0f);
    }

    Grain    _grains[N_GRAINS] = {};
    float    _playhead         = 0.0f;
    uint32_t _rand             = 12345u;
    bool     _initialized      = false;
};
