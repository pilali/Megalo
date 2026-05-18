#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr int FREEZE_MAX_SR      = 96000;
static constexpr int FREEZE_MAX_CAP_MS  = 500;
static constexpr int FREEZE_MAX_SAMPLES = FREEZE_MAX_SR * FREEZE_MAX_CAP_MS / 1000; // 48 000

// Extra recording margin added on top of sample_ms before the seam search.
// 120 ms ≈ 10 fundamental periods of the lowest guitar string (~82 Hz),
// giving the search enough candidate windows to find a clean loop point.
static constexpr int SEARCH_EXTRA_MS = 120;

enum class FreezeEvent { None = 0, Onset = 1, LoopReady = 2 };

// Forward-capture freeze engine with automatic loop-point search.
//
// State machine:
//   Idle        — waiting for onset
//   PendingSkip — onset detected, counting down attack_skip_ms
//   Recording   — writing live audio into a search buffer
//                 (sample_ms + SEARCH_EXTRA_MS samples)
//   Looping     — best sample_ms window extracted; available for playback
//
// After recording the search buffer, _search_and_finalise() scores every
// candidate start position (stride 4) by the sum-of-squared-differences
// between its first and last CMP samples, then extracts the window with
// the lowest score.  A raised-cosine crossfade is baked at the loop
// boundary and a half-Hann fade-in is applied to the loop head.
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
        _loop_len      = 0;
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

            if (_onset_armed && ratio > thresh_hi) {
                _onset_armed = false;
                _onset_hold  = refractory;

                if (_state == State::Idle || _state == State::Looping) {
                    _loop_target = std::clamp((int)(_sr * sample_len_ms * 0.001),
                                              64, FREEZE_MAX_SAMPLES);
                    // Record extra samples so the seam search has room to work.
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
                _loop[_rec_pos++] = x;   // raw — fade-in applied post-search
                if (_rec_pos >= _rec_target) {
                    _search_and_finalise();
                    evt = FreezeEvent::LoopReady;
                }
            }
            break;

        case State::Recording:
            _loop[_rec_pos++] = x;       // raw — fade-in applied post-search
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

    float read(double speed, double& pos) const noexcept {
        if (_state != State::Looping || _loop_len == 0) return 0.0f;
        while (pos >= _loop_len) pos -= _loop_len;
        while (pos <  0.0)       pos += _loop_len;
        const int   i0   = (int)pos;
        const int   i1   = (i0 + 1) % _loop_len;
        const float frac = (float)(pos - i0);
        pos += speed;
        return _loop[i0] + frac * (_loop[i1] - _loop[i0]);
    }

    bool         is_frozen()  const noexcept { return _state == State::Looping && _loop_len > 0; }
    const float* loop_data()  const noexcept { return (_state == State::Looping) ? _loop : nullptr; }
    int          loop_len()   const noexcept { return (_state == State::Looping) ? _loop_len : 0; }

private:
    enum class State { Idle, PendingSkip, Recording, Looping };

    // Search the recorded buffer for the best loop_target-sample window.
    //
    // Score(p) = Σ (head[k] − tail[k])²  for k in [0, CMP)
    //   head = _loop + p
    //   tail = _loop + p + target_len − CMP
    //
    // A low score means the first CMP samples of the window closely match
    // the last CMP samples → minimal audible discontinuity at the loop point.
    // Stride 4 gives 4× speedup with a resolution of ~83 µs at 48 kHz,
    // which is finer than any pitch period of interest.
    void _search_and_finalise() noexcept {
        const int search_len = _rec_pos;
        const int target_len = std::min(_loop_target, search_len);
        const int cmp        = std::min(128, target_len / 4);

        int   best_start = 0;
        float best_score = 1e38f;

        for (int p = 0; p <= search_len - target_len; p += 4) {
            float score         = 0.0f;
            const float* head   = _loop + p;
            const float* tail   = _loop + p + target_len - cmp;
            for (int k = 0; k < cmp; ++k) {
                const float d = head[k] - tail[k];
                score += d * d;
            }
            if (score < best_score) {
                best_score = score;
                best_start = p;
            }
        }

        if (best_start > 0)
            std::memmove(_loop, _loop + best_start, target_len * sizeof(float));

        _loop_len = target_len;

        // Apply half-Hann fade-in to the loop head.
        const int fi = std::min(_fade_in_len, _loop_len / 4);
        for (int i = 0; i < fi; ++i) {
            const float t = (float)i / (float)fi;
            _loop[i] *= 0.5f * (1.0f - std::cos(3.14159265f * t));
        }

        _apply_xfade();
        _state = State::Looping;
    }

    void _apply_xfade() noexcept {
        const int xf = std::min(_xfade_len, _loop_len / 4);
        for (int i = 0; i < xf; ++i) {
            const float w    = 0.5f * (1.0f - std::cos(3.14159265f * (float)i / (float)(xf - 1)));
            const int   tail = _loop_len - xf + i;
            _loop[tail] = _loop[tail] * (1.0f - w) + _loop[i] * w;
        }
    }

    float  _loop[FREEZE_MAX_SAMPLES] = {};
    int    _loop_len      = 0;
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
