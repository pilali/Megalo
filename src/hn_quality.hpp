#pragma once

// ── H+N analysis/resynthesis quality tiers ────────────────────────────────
//
// Selected at compile time with -DMEGALO_HN_QUALITY=0|1|2
//   0 = dwarf   (MOD Dwarf, Cortex-A35)   — conservative, best-effort low range
//   1 = pi5     (Raspberry Pi 5, A76)     — full 6-note polyphony
//   2 = desktop (x86/Apple-silicon hosts) — full polyphony, largest analysis
//
// The Makefile maps MEGALO_HN_QUALITY=dwarf|pi5|desktop onto these integers and
// picks a per-target default. The analyzer reads these constants; only FFT_SIZE,
// MAX_NOTES and MAX_PARTIALS change the CPU/quality trade-off.
//
// Why the analysis is allowed to be large: hn_multif0_analyze() runs ONCE per
// capture (at LoopReady), never inside the per-sample loop. A 16384-point FFT is
// well under a millisecond even on the A76; on the A35 we drop to 8192 and fewer
// notes/partials. The real-time budget lives entirely in the resynthesis
// (oscillators per sample), which is why MAX_NOTES × MAX_PARTIALS is capped per
// tier rather than the FFT size.

#ifndef MEGALO_HN_QUALITY
#  define MEGALO_HN_QUALITY 2   // default to the most capable tier
#endif

namespace hnq {

#if   MEGALO_HN_QUALITY == 0     // ── MOD Dwarf (Cortex-A35) ──
    static constexpr int  FFT_SIZE     = 8192;
    static constexpr int  MAX_NOTES    = 4;
    static constexpr int  MAX_PARTIALS = 16;
    static constexpr bool USE_NNLS     = false;
#elif MEGALO_HN_QUALITY == 1     // ── Raspberry Pi 5 (Cortex-A76) ──
    static constexpr int  FFT_SIZE     = 16384;
    static constexpr int  MAX_NOTES    = 6;
    static constexpr int  MAX_PARTIALS = 32;
    static constexpr bool USE_NNLS     = true;
#else                            // ── Desktop ──
    static constexpr int  FFT_SIZE     = 16384;
    static constexpr int  MAX_NOTES    = 6;
    static constexpr int  MAX_PARTIALS = 32;
    static constexpr bool USE_NNLS     = true;
#endif

    // Pitch range, shared by every tier. F0_MIN extends a perfect fourth below
    // the guitar's low E (E2 ≈ 82.4 Hz) down to A1 = 55 Hz, as requested.
    static constexpr float F0_MIN = 55.0f;    // A1
    static constexpr float F0_MAX = 1320.0f;  // ~E6, above the fretboard

    // Noise level scaler applied to the residual RMS. The residual still holds
    // un-modelled tonal content (inharmonicity, partials beyond MAX_PARTIALS,
    // inter-note beating, reverb tail); played as broadband noise it sounds far
    // too prominent on a sustained resynthesis. A plucked string is nearly pure
    // harmonic in sustain, so we keep only a subtle breath/pick component.
    static constexpr float NOISE_GAIN = 0.4f;

} // namespace hnq
