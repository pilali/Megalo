#pragma once

// ── Harmonic+Noise note descriptor ────────────────────────────────────────
//
// Shared spectral snapshot of a single note, produced by an analyzer and
// consumed by AdditiveSynth. Kept in its own header so the resynthesis and the
// polyphonic analyzer do not have to pull in the (optional) monophonic YIN
// front-end just for this struct.

static constexpr int HN_MAX_PARTIALS = 32;

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
    bool  valid      = false; // false → no usable pitched content
};
