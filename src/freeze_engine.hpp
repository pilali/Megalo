#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>

// Maximum supported sample rate × max capture length (500 ms).
static constexpr int FREEZE_MAX_SR      = 96000;
static constexpr int FREEZE_MAX_CAP_MS  = 500;
static constexpr int FREEZE_MAX_SAMPLES = FREEZE_MAX_SR * FREEZE_MAX_CAP_MS / 1000; // 48 000
static constexpr int FREEZE_XFADE      = 512;  // crossfade length (≈5 ms @ 96 kHz)

// Records a continuous ring buffer, detects onsets via a fast/slow RMS
// ratio, captures a snapshot on each onset, and loops it seamlessly.
// Multiple independent readers (variable speed) provide pitch shifting.
class FreezeEngine {
public:
    void init(double sample_rate) noexcept {
        _sr = sample_rate;
        // One-pole LP coefficients for squared-signal followers
        _fast_c = 1.0f - std::exp(-1.0f / (0.002f  * (float)sample_rate)); // ~2 ms
        _slow_c = 1.0f - std::exp(-1.0f / (0.200f  * (float)sample_rate)); // ~200 ms
        reset();
    }

    void reset() noexcept {
        std::memset(_ring, 0, sizeof(_ring));
        std::memset(_loop, 0, sizeof(_loop));
        _ring_pos    = 0;
        _loop_len    = 0;
        _frozen      = false;
        _onset_hold  = 0;
        _onset_armed = true;
        _rms_fast    = 0.0f;
        _rms_slow    = 1e-10f;
    }

    // Feed one input sample. Returns true on the exact sample an onset fires.
    // sample_len_ms is snapshotted at call time from the control port.
    bool process(float x, float threshold, int sample_len_ms) noexcept {
        // --- ring buffer --------------------------------------------------
        _ring[_ring_pos] = x;
        _ring_pos = (_ring_pos + 1) % FREEZE_MAX_SAMPLES;

        // --- onset detection (fast/slow RMS ratio) ------------------------
        float x2 = x * x;
        _rms_fast += _fast_c * (x2 - _rms_fast);
        _rms_slow += _slow_c * (x2 - _rms_slow);

        if (_onset_hold > 0) { --_onset_hold; return false; }

        float ratio       = _rms_fast / (_rms_slow + 1e-10f);
        float thresh_low  = 1.5f + threshold * 13.5f;
        float thresh_high = thresh_low * 1.3f;  // hysteresis

        if (_onset_armed && ratio > thresh_high) {
            _onset_armed = false;
            _onset_hold  = (int)(_sr * 0.15);  // 150 ms refractory period
            _capture(sample_len_ms);
            return true;
        }
        if (!_onset_armed && ratio < thresh_low)
            _onset_armed = true;

        return false;
    }

    // Read one sample from the loop at variable speed. `pos` is maintained
    // by the caller (one per voice) so voices are fully independent.
    float read(double speed, double& pos) const noexcept {
        if (_loop_len == 0) return 0.0f;

        // Wrap
        while (pos >= _loop_len) pos -= _loop_len;
        while (pos <  0.0)       pos += _loop_len;

        // Linear interpolation
        int   i0   = (int)pos;
        int   i1   = (i0 + 1) % _loop_len;
        float frac = (float)(pos - i0);
        float out  = _loop[i0] + frac * (_loop[i1] - _loop[i0]);

        pos += speed;
        return out;
    }

    bool is_frozen() const noexcept { return _frozen && _loop_len > 0; }

    // Direct loop buffer access for the phase vocoder
    const float* loop_data() const noexcept { return _loop; }
    int          loop_len()  const noexcept { return _loop_len; }

private:
    void _capture(int sample_len_ms) noexcept {
        int samples = (int)(_sr * sample_len_ms * 0.001);
        samples = std::clamp(samples, FREEZE_XFADE * 4, FREEZE_MAX_SAMPLES);

        _loop_len = samples;

        // Copy the most recent `samples` samples from the ring buffer
        for (int i = 0; i < samples; ++i) {
            int idx = (_ring_pos - samples + i + FREEZE_MAX_SAMPLES * 2) % FREEZE_MAX_SAMPLES;
            _loop[i] = _ring[idx];
        }

        // Bake a crossfade at the loop boundary so the wrap is click-free:
        // the last XFADE samples fade into the first XFADE samples.
        for (int i = 0; i < FREEZE_XFADE; ++i) {
            float t   = (float)i / (FREEZE_XFADE - 1);  // 0 → 1
            int   tail = samples - FREEZE_XFADE + i;
            _loop[tail] = _loop[tail] * (1.0f - t) + _loop[i] * t;
        }

        _frozen = true;
    }

    float  _ring[FREEZE_MAX_SAMPLES] = {};
    float  _loop[FREEZE_MAX_SAMPLES] = {};
    int    _ring_pos    = 0;
    int    _loop_len    = 0;
    bool   _frozen      = false;
    bool   _onset_armed = true;
    int    _onset_hold  = 0;
    float  _rms_fast    = 0.0f;
    float  _rms_slow    = 1e-10f;
    float  _fast_c      = 0.0f;
    float  _slow_c      = 0.0f;
    double _sr          = 48000.0;
};
