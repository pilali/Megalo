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
        _period        = 0.0f;
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
    // Dominant period of the current loop in samples (0 = not periodic enough:
    // noise/percussive content). Estimated once per capture; used by the grain
    // players to phase-align grain respawns on tonal material.
    float        period()    const noexcept { return _period; }

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

        // Estimate the dominant period on the raw selected window, then trim
        // the loop to an integer number of periods. This is what makes the
        // seam truly clean on tonal input: the crossfade below mixes content
        // whose phase difference is (loop length mod period), so a fractional
        // cycle count baked a fixed cancellation notch into the seam zone —
        // audible as slow pumping reproduced by every grain, and corrupting
        // the H+N analysis spectrum. No seam-position search can fix that
        // (the phase mismatch is position-independent); only the length can.
        _estimate_period(_loop + best_start, search_len - best_start);
        int final_len = target_len;
        if (_period > 3.0f) {
            const float m = std::max(1.0f, std::round((float)target_len / _period));
            int trimmed = (int)std::lround(m * _period);
            trimmed = std::min(trimmed, search_len - best_start);
            if (trimmed >= 64) final_len = trimmed;
        }

        // Copy best window into the playback buffer (never interrupts playback).
        std::memcpy(_play, _loop + best_start, final_len * sizeof(float));
        _play_len = final_len;

        // NOTE: the loop head is deliberately NOT faded in. The old half-Hann
        // head fade dates from sequential playback; the grain players read the
        // loop at random positions, so a faded head is just a baked-in quiet
        // zone that every grain crossing it reproduces — audible as periodic
        // pumping, worst on short loops where the fades were a large fraction
        // of the buffer. Wrap continuity is provided by the seam crossfade
        // below, and capture-boundary clicks by the grain envelopes.

        // Raised-cosine crossfade baked at the loop boundary. The tail is
        // blended toward the recording content ADJACENT to the seam — the
        // samples that precede the head (or, when the window starts at the
        // very beginning of the recording, the head is blended from the
        // samples that follow the tail). After the fade, _play[len-1] flows
        // into _play[0] exactly as the source did.
        //
        // The old version blended the tail toward the HEAD ITSELF
        // (_play[0..xf]), which is offset by xf samples from the content the
        // wrap actually needs: on periodic material that baked a constant
        // phase error of xf samples into the seam — typically a deep ~20 ms
        // cancellation notch reproduced by every grain on every revolution,
        // audible as slow pumping of the pad.
        const int xf = std::min(_xfade_len, _play_len / 4);
        if (best_start >= xf) {
            // Tail → pre-head content (recording just before the window).
            for (int i = 0; i < xf; ++i) {
                const float w    = 0.5f * (1.0f - std::cos(3.14159265f * (float)i / (float)(xf - 1)));
                const int   tail = _play_len - xf + i;
                const float pre  = _loop[best_start - xf + i];
                _play[tail] = _play[tail] * (1.0f - w) + pre * w;
            }
        } else if (best_start + final_len + xf <= search_len) {
            // Head ← post-tail content (recording just after the window).
            for (int i = 0; i < xf; ++i) {
                const float w    = 0.5f * (1.0f - std::cos(3.14159265f * (float)i / (float)(xf - 1)));
                const float post = _loop[best_start + final_len + i];
                _play[i] = post * (1.0f - w) + _play[i] * w;
            }
        }
        // (Both source regions exist thanks to the SEARCH_EXTRA_MS margin;
        //  if neither fits — degenerate short recording — no fade is baked
        //  and the grain envelopes still mask the seam.)

        _state = State::Looping;

        // Anti-rebond post-LoopReady: prevent an immediate re-onset before the
        // analysis block has run and the new attack has started from zero.
        const int post_hold = static_cast<int>(_sr * FREEZE_POST_LOOP_HOLD_MS * 0.001f);
        if (_onset_hold < post_hold)
            _onset_hold = post_hold;
    }

    // Normalised autocorrelation over a short window of x[]: the dominant lag
    // in [1 kHz .. 55 Hz] becomes the loop period when its correlation is high
    // enough. Runs on the RAW selected recording window (before any fade is
    // baked). One-shot per capture, sized to stay affordable on the Dwarf
    // (~200 lags × 384 taps). Non-periodic content (noise, percussive) yields
    // 0: the length-trim is skipped and the grain players fall back to free
    // random respawn.
    void _estimate_period(const float* x, int avail) noexcept {
        _period = 0.0f;
        const int W = 384;
        const int min_lag = std::max(8, (int)(_sr / 1000.0));            // ≤ 1 kHz
        const int max_lag = std::min(avail - W - 1, (int)(_sr / 55.0));  // ≥ 55 Hz
        if (max_lag <= min_lag) return;

        float e0 = 1e-12f;
        for (int i = 0; i < W; ++i) e0 += x[i] * x[i];
        if (e0 < 1e-8f) return;                                          // silence

        auto ncorr = [&](int L) noexcept -> float {
            float r = 0.0f, eL = 1e-12f;
            const float* y = x + L;
            for (int i = 0; i < W; ++i) { r += x[i] * y[i]; eL += y[i] * y[i]; }
            return r / std::sqrt(e0 * eL);
        };

        float best_r = 0.0f;
        for (int L = min_lag; L <= max_lag; L += 4) {
            const float rn = ncorr(L);
            if (rn > best_r) best_r = rn;
        }
        if (best_r < 0.5f) return;

        // Prefer the SMALLEST lag close to the best: every multiple of the
        // true period correlates equally well, and a small fundamental lag
        // keeps the integer-periods length-trim rounding error low.
        int cand = 0;
        for (int L = min_lag; L <= max_lag && cand == 0; L += 4)
            if (ncorr(L) >= 0.90f * best_r) cand = L;

        // Hill-climb to the local maximum: the 0.9·best threshold triggers on
        // the RISING slope of the peak, which can be 10+ samples below the
        // summit — a fixed ±3 window around the coarse pick missed it, and a
        // few-percent period error, multiplied by the number of cycles in the
        // loop, ruins the integer-periods trim.
        int fl = cand; float fr = ncorr(cand);
        for (int step = 0; step < 32; ++step) {
            const float up = (fl + 1 <= max_lag) ? ncorr(fl + 1) : -2.0f;
            const float dn = (fl - 1 >= min_lag) ? ncorr(fl - 1) : -2.0f;
            if (up > fr && up >= dn)  { ++fl; fr = up; }
            else if (dn > fr)         { --fl; fr = dn; }
            else break;
        }

        // Parabolic sub-sample refinement — the trim multiplies the period by
        // the number of cycles in the loop, so fractional accuracy matters.
        float per = (float)fl;
        if (fl > min_lag && fl < max_lag) {
            const float a = ncorr(fl - 1), b = fr, c = ncorr(fl + 1);
            const float den = a - 2.0f * b + c;
            if (std::abs(den) > 1e-9f) {
                float p = 0.5f * (a - c) / den;
                p = std::clamp(p, -0.5f, 0.5f);
                per += p;
            }
        }
        _period = per;
    }

    float  _loop[FREEZE_MAX_SAMPLES] = {};  // recording + search scratch
    float  _play[FREEZE_MAX_SAMPLES] = {};  // active playback loop
    int    _play_len      = 0;
    float  _period        = 0.0f;           // dominant period, 0 = unpitched
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
