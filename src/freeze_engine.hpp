#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr int FREEZE_MAX_SR      = 96000;
static constexpr int FREEZE_MAX_CAP_MS  = 500;
static constexpr int FREEZE_MAX_SAMPLES = FREEZE_MAX_SR * FREEZE_MAX_CAP_MS / 1000; // 48 000

// Extra recording margin for the seam search (ms beyond sample_ms).
// Covers ~10 fundamental periods of the lowest guitar string (~82 Hz).
static constexpr int SEARCH_EXTRA_MS = 120;

// After LoopReady the original onset refractory (retrigger_ms ≈ 200 ms) has
// already expired (capture takes longer than that).  Without a fresh hold the
// detector can immediately re-arm and fire a second onset — which later causes
// envelope.reset() to interrupt an active pad, producing a click.
// This short post-loop hold blocks re-detection long enough for the analysis
// block to complete and the new attack to start from zero cleanly.
// Tune this value if re-triggers are needed faster (lower) or the click persists
// on very short attacks (raise slightly).
static constexpr int FREEZE_POST_LOOP_HOLD_MS = 20;

// Absolute onset gate: the fast/slow RMS ratio is level-independent, so in
// near-silence the tiniest click can trip it and freeze a buffer of noise.
// Require the fast RMS (power) to exceed ~-55 dBFS before an onset may fire.
static constexpr float ONSET_ABS_GATE = 3.2e-6f;

// Loop-seam comparison window (ms). Must cover at least one period of the
// lowest expected fundamental to be meaningful; ~10 ms covers ~100 Hz and
// keeps the one-shot search affordable on the MOD Dwarf.
static constexpr int SEAM_CMP_MS = 10;

enum class FreezeEvent { None = 0, Onset = 1, LoopReady = 2 };

// Forward-capture freeze engine with automatic loop-point search.
//
// Two separate buffers keep the old loop audible during capture:
//   _loop[] — recording + search scratch space (overwritten each capture)
//   _play[] — last validated loop, never interrupted during a new capture
//
// State machine:
//   Idle        — waiting for onset
//   PendingSkip — onset detected, counting down attack_skip_ms
//   Recording   — writing into _loop[] (sample_ms + SEARCH_EXTRA_MS)
//   Looping     — best window copied to _play[], ready for playback
//
// loop_data() / loop_len() always return _play[], so GrainPlayers
// continue reading the previous loop while a new one is being recorded.
class FreezeEngine {
public:
    void init(double sample_rate) noexcept {
        _sr     = sample_rate;
        _fast_c = 1.0f - std::exp(-1.0f / (0.002f * (float)sample_rate));
        _slow_c = 1.0f - std::exp(-1.0f / (0.200f * (float)sample_rate));
        reset();
    }

    void reset() noexcept {
        std::memset(_loop, 0, sizeof(_loop));
        std::memset(_play, 0, sizeof(_play));
        _play_len      = 0;
        _loop_target   = 0;
        _state         = State::Idle;
        _skip_remain   = 0;
        _rec_pos       = 0;
        _rec_target    = 0;
        _xfade_len     = 512;
        _fade_in_len   = 0;
        _onset_armed   = true;
        _onset_hold    = 0;
        _rms_fast      = 0.0f;
        _rms_slow      = 1e-10f;
    }

    // All timing constants are passed per-block so the host can update them.
    // xfade_ms / retrigger_ms / capture_fade_ms are kept internal (not exposed
    // as user ports) — callers pass fixed values from plugin.cpp.
    FreezeEvent process(float x, float threshold,
                        int sample_len_ms, int attack_skip_ms,
                        int xfade_ms, int retrigger_ms,
                        int capture_fade_ms) noexcept {
        _xfade_len = std::clamp((int)(_sr * xfade_ms * 0.001), 16, FREEZE_MAX_SAMPLES / 8);
        const int refractory = std::clamp((int)(_sr * retrigger_ms * 0.001), 1, (int)_sr);

        const float x2 = x * x;
        _rms_fast += _fast_c * (x2 - _rms_fast);
        _rms_slow += _slow_c * (x2 - _rms_slow);

        FreezeEvent evt = FreezeEvent::None;

        if (_onset_hold > 0) {
            --_onset_hold;
        } else {
            const float ratio      = _rms_fast / (_rms_slow + 1e-10f);
            const float thresh_low = 1.5f + threshold * 13.5f;
            const float thresh_hi  = thresh_low * 1.3f;

            if (_onset_armed && ratio > thresh_hi && _rms_fast > ONSET_ABS_GATE) {
                _onset_armed = false;
                _onset_hold  = refractory;

                if (_state == State::Idle || _state == State::Looping) {
                    _loop_target = std::clamp((int)(_sr * sample_len_ms * 0.001),
                                              64, FREEZE_MAX_SAMPLES);
                    const int extra = (int)(_sr * SEARCH_EXTRA_MS * 0.001);
                    _rec_target  = std::min(_loop_target + extra, FREEZE_MAX_SAMPLES);
                    _skip_remain = std::max(0, (int)(_sr * attack_skip_ms * 0.001));
                    _fade_in_len = std::clamp((int)(_sr * capture_fade_ms * 0.001),
                                              0, _loop_target / 4);
                    _state       = State::PendingSkip;
                    evt          = FreezeEvent::Onset;
                }
            }
            if (!_onset_armed && ratio < thresh_low)
                _onset_armed = true;
        }

        switch (_state) {
        case State::PendingSkip:
            if (_skip_remain > 0) {
                --_skip_remain;
            } else {
                _state   = State::Recording;
                _rec_pos = 0;
                _loop[_rec_pos++] = x;
                if (_rec_pos >= _rec_target) {
                    _search_and_finalise();
                    evt = FreezeEvent::LoopReady;
                }
            }
            break;

        case State::Recording:
            _loop[_rec_pos++] = x;
            if (_rec_pos >= _rec_target) {
                _search_and_finalise();
                evt = FreezeEvent::LoopReady;
            }
            break;

        default:
            break;
        }

        return evt;
    }

    // _play[] is always served, even during Recording — keeps old loop audible.
    const float* loop_data() const noexcept { return (_play_len > 0) ? _play : nullptr; }
    int          loop_len()  const noexcept { return _play_len; }
    bool         is_frozen() const noexcept { return _play_len > 0; }

private:
    enum class State { Idle, PendingSkip, Recording, Looping };

    void _search_and_finalise() noexcept {
        const int search_len = _rec_pos;
        const int target_len = std::min(_loop_target, search_len);
        // Compare over ≥ one period of a low note (SEAM_CMP_MS), not a fixed
        // 128 samples (~2.7 ms) — below ~350 Hz that short window made the
        // seam choice essentially random, causing per-revolution wobble.
        const int cmp = std::clamp((int)(_sr * SEAM_CMP_MS * 0.001),
                                   128, std::max(128, target_len / 4));
        // Keep the one-shot search cost bounded: coarser stride for the
        // longer window (same total MACs order as the previous 128 × step-4).
        const int step = (cmp > 256) ? 8 : 4;

        int   best_start = 0;
        float best_score = 1e38f;

        for (int p = 0; p <= search_len - target_len; p += step) {
            float diff          = 0.0f;
            float energy        = 0.0f;
            const float* head   = _loop + p;
            const float* tail   = _loop + p + target_len - cmp;
            for (int k = 0; k < cmp; ++k) {
                const float d = head[k] - tail[k];
                diff   += d * d;
                energy += head[k] * head[k] + tail[k] * tail[k];
            }
            // Energy-normalised mismatch: the raw Σd² is minimised by any
            // quiet window regardless of how bad the seam is *relatively*,
            // which biased the pick toward silence on decaying notes.
            const float score = diff / (energy + 1e-9f);
            if (score < best_score) { best_score = score; best_start = p; }
        }

        // Copy best window into the playback buffer (never interrupts playback).
        std::memcpy(_play, _loop + best_start, target_len * sizeof(float));
        _play_len = target_len;

        // Half-Hann fade-in at loop head.
        const int fi = std::min(_fade_in_len, _play_len / 4);
        for (int i = 0; i < fi; ++i) {
            const float t = (float)i / (float)fi;
            _play[i] *= 0.5f * (1.0f - std::cos(3.14159265f * t));
        }

        // Raised-cosine crossfade baked at loop boundary.
        const int xf = std::min(_xfade_len, _play_len / 4);
        for (int i = 0; i < xf; ++i) {
            const float w    = 0.5f * (1.0f - std::cos(3.14159265f * (float)i / (float)(xf - 1)));
            const int   tail = _play_len - xf + i;
            _play[tail] = _play[tail] * (1.0f - w) + _play[i] * w;
        }

        _state = State::Looping;

        // Anti-rebond post-LoopReady: prevent an immediate re-onset before the
        // analysis block has run and the new attack has started from zero.
        const int post_hold = static_cast<int>(_sr * FREEZE_POST_LOOP_HOLD_MS * 0.001f);
        if (_onset_hold < post_hold)
            _onset_hold = post_hold;
    }

    float  _loop[FREEZE_MAX_SAMPLES] = {};  // recording + search scratch
    float  _play[FREEZE_MAX_SAMPLES] = {};  // active playback loop
    int    _play_len      = 0;
    int    _loop_target   = 0;
    int    _xfade_len     = 512;
    int    _fade_in_len   = 0;
    State  _state         = State::Idle;
    bool   _onset_armed   = true;
    int    _onset_hold    = 0;
    int    _skip_remain   = 0;
    int    _rec_pos       = 0;
    int    _rec_target    = 0;
    float  _rms_fast      = 0.0f;
    float  _rms_slow      = 1e-10f;
    float  _fast_c        = 0.0f;
    float  _slow_c        = 0.0f;
    double _sr            = 48000.0;
};
