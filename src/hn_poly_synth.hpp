#pragma once
#include "additive_synth.hpp"
#include "hn_multif0.hpp"
#include "hn_quality.hpp"
#include <algorithm>

// ── Polyphonic additive synthesizer ───────────────────────────────────────
//
// A bank of up to hnq::MAX_NOTES AdditiveSynth voices, one per note found by
// the multi-F0 analyzer. Summing the voices resynthesizes the whole frozen
// chord as a sustained harmonic+noise pad — the polyphonic generalisation of
// the original single-note hn_v0/v1/v2 path.
//
// The broadband noise residual is shared, not per-note, so it is emitted by a
// single voice; otherwise N voices would each add it and the noise floor would
// scale with the number of notes.

class PolyAdditiveSynth {
public:
    void reset(const MultiHNState& st, float sr) noexcept {
        _n = std::min(st.n_notes, hnq::MAX_NOTES);
        for (int i = 0; i < _n; ++i) {
            HNState s = st.notes[i];
            if (i > 0) s.noise_rms = 0.0f;   // shared noise via voice 0 only
            _v[i].reset(s, sr);
        }
    }

    // Transpose the whole chord (ratio = 2^(semitones/12)); may change per block.
    void set_pitch_ratio(float ratio) noexcept {
        for (int i = 0; i < _n; ++i) _v[i].set_pitch_ratio(ratio);
    }

    // Timbre controls, applied to every voice (see AdditiveSynth::set_timbre).
    void set_timbre(float brightness, float damping,
                    float even_odd, float noise) noexcept {
        for (int i = 0; i < _n; ++i)
            _v[i].set_timbre(brightness, damping, even_odd, noise);
    }

    float process() noexcept {
        float out = 0.0f;
        for (int i = 0; i < _n; ++i) out += _v[i].process();
        return out;
    }

    // Sum the voices into a stereo pair with the given width [0,1].
    void process_stereo(float& l, float& r, float width) noexcept {
        l = r = 0.0f;
        for (int i = 0; i < _n; ++i) {
            float vl, vr;
            _v[i].process_stereo(vl, vr, width);
            l += vl; r += vr;
        }
    }

    int n_active() const noexcept { return _n; }

private:
    AdditiveSynth _v[hnq::MAX_NOTES];
    int           _n = 0;
};
