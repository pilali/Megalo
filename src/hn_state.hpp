#pragma once

// ── Harmonic+Noise note descriptor ────────────────────────────────────────
//
// Shared spectral snapshot of a single note, produced by an analyzer and
// consumed by AdditiveSynth. Kept in its own header so the resynthesis and the
// polyphonic analyzer do not have to pull in the (optional) monophonic YIN
// front-end just for this struct.

static constexpr int HN_MAX_PARTIALS = 32;
// Octave-spaced residual bands (relative to f0), matching the synth's noise
// filter-bank. Index b weights the band-pass region [f0·2^(b+1), f0·2^(b+2)].
static constexpr int HN_NOISE_BANDS  = 6;

struct HNState {
    float f0         = 0.0f;  // detected fundamental, Hz (0 = no pitch found)
    float confidence = 0.0f;  // analyzer confidence [0, 1]
    int   n_partials = 0;
    float harm_amp  [HN_MAX_PARTIALS] = {};
    float harm_phase[HN_MAX_PARTIALS] = {};
    // Measured partial frequencies in Hz (0 = not measured → synth falls back
    // to the ideal k·f0). Real strings are inharmonic (partials stretched
    // above k·f0); resynthesising at the measured frequencies is what keeps
    // the pad from sounding organ-like.
    float harm_freq [HN_MAX_PARTIALS] = {};
    float noise_rms  = 0.0f;  // broadband residual level
    // Measured spectral SHAPE of the residual, one weight per octave band
    // (mean ≈ 1). Default flat → identical to the old fixed filter-bank; the
    // multi-F0 analyzer fills it from the post-subtraction spectrum so the
    // noise takes the instrument's actual air/breath colour instead of a
    // generic f0-harmonic comb.
    float noise_band[HN_NOISE_BANDS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool  valid      = false; // false → no usable pitched content
};
