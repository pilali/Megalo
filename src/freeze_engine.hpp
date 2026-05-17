#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr int FREEZE_MAX_SR      = 96000;
static constexpr int FREEZE_MAX_CAP_MS  = 500;
static constexpr int FREEZE_MAX_SAMPLES = FREEZE_MAX_SR * FREEZE_MAX_CAP_MS / 1000; // 48 000

enum class FreezeEvent { None = 0, Onset = 1, LoopReady = 2 };

// Forward-capture freeze engine.
//
// State machine:
//   Idle        — waiting for onset
//   PendingSkip — onset detected, counting down attack_skip_ms
//   Recording   — writing live audio directly into the loop buffer
//   Looping     — loop buffer ready, available for playback
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

    // Feed one input sample.  Returns Onset when a new capture starts, and
    // LoopReady on the exact sample the loop becomes available for playback.
    // xfade_ms        : crossfade baked at the loop boundary    [5–100 ms]
    // retrigger_ms    : refractory period after an onset        [50–1000 ms]
    // capture_fade_ms : half-Hann fade-in at capture start      [0–50 ms]
    FreezeEvent process(float x, float threshold,
                        int sample_len_ms, int attack_skip_ms,
                        int xfade_ms, int retrigger_ms,
                        int capture_fade_ms) noexcept {
        // Store tunable timing values for use in _finalise() / onset detection.
        _xfade_len = std::clamp((int)(_sr * xfade_ms * 0.001), 16, FREEZE_MAX_SAMPLES / 8);
        const int refractory = std::clamp((int)(_sr * retrigger_ms * 0.001), 1, (int)_sr);

        // Onset detection (always running)
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
                    int samples  = (int)(_sr * sample_len_ms * 0.001);
                    // Minimum is 64 samples (~1.3 ms at 48 kHz), independent of
                    // xfade length — _finalise() already caps xfade to loop/4.
                    _rec_target  = std::clamp(samples, 64, FREEZE_MAX_SAMPLES);
                    _skip_remain = std::max(0, (int)(_sr * attack_skip_ms * 0.001));
                    // Cap fade-in so it can never fill more than a quarter of the loop.
                    _fade_in_len = std::clamp((int)(_sr * capture_fade_ms * 0.001),
                                              0, _rec_target / 4);
                    _state       = State::PendingSkip;
                    evt          = FreezeEvent::Onset;
                }
            }
            if (!_onset_armed && ratio < thresh_low)
                _onset_armed = true;
        }

        // State transitions
        switch (_state) {
        case State::PendingSkip:
            if (_skip_remain > 0) {
                --_skip_remain;
            } else {
                _state   = State::Recording;
                _rec_pos = 0;
                _loop[_rec_pos++] = x * _fade_gain(0);
                if (_rec_pos >= _rec_target) {
                    _finalise();
                    evt = FreezeEvent::LoopReady;
                }
            }
            break;

        case State::Recording:
            _loop[_rec_pos] = x * _fade_gain(_rec_pos);
            ++_rec_pos;
            if (_rec_pos >= _rec_target) {
                _finalise();
                evt = FreezeEvent::LoopReady;
            }
            break;

        default:
            break;
        }

        return evt;
    }

    // Variable-speed loop reader.  Returns 0 if loop is not ready.
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

    // Half-Hann fade-in: 0 at sample 0, 1 at sample _fade_in_len.
    float _fade_gain(int pos) const noexcept {
        if (_fade_in_len == 0 || pos >= _fade_in_len) return 1.0f;
        const float t = (float)pos / (float)_fade_in_len;
        return 0.5f * (1.0f - std::cos(3.14159265f * t));
    }

    void _finalise() noexcept {
        _loop_len = _rec_pos;
        // Bake a raised-cosine crossfade at the loop boundary (end → start)
        // so the wrap point is click-free regardless of loop content.
        const int xf = std::min(_xfade_len, _loop_len / 4);
        for (int i = 0; i < xf; ++i) {
            const float t    = (float)i / (float)(xf - 1);
            // Raised cosine: 0 at tail end, 1 at tail start → smooth fade
            const float w    = 0.5f * (1.0f - std::cos(3.14159265f * t));
            const int   tail = _loop_len - xf + i;
            _loop[tail] = _loop[tail] * (1.0f - w) + _loop[i] * w;
        }
        _state = State::Looping;
    }

    float  _loop[FREEZE_MAX_SAMPLES] = {};
    int    _loop_len      = 0;
    int    _xfade_len     = 512;
    int    _fade_in_len   = 0;
    State  _state         = State::Idle;
    bool   _onset_armed = true;
    int    _onset_hold  = 0;
    int    _skip_remain = 0;
    int    _rec_pos     = 0;
    int    _rec_target  = 0;
    float  _rms_fast    = 0.0f;
    float  _rms_slow    = 1e-10f;
    float  _fast_c      = 0.0f;
    float  _slow_c      = 0.0f;
    double _sr          = 48000.0;
};
