# Polyphonic H+N engine — testing & watch list

Status of the polyphonic harmonic+noise analysis/resynthesis work on
`claude/hn-polyphonic-analysis` (branched from `experimental/additive-synth`).
This file tracks what still needs **listening tests** (no audio test station was
available when the code was written) and what to **watch** technically.

## What is implemented

| Component | File | Notes |
|---|---|---|
| Self-contained FFT | `src/hn_fft.hpp` | radix-2, one-shot, analysis only |
| Quality tiers | `src/hn_quality.hpp` | `MEGALO_HN_QUALITY=dwarf\|pi5\|desktop` |
| Multi-F0 analyzer | `src/hn_multif0.hpp` | greedy harmonic-comb + sub-bin refine + sub-octave guard |
| NNLS attribution | `src/hn_nnls.hpp` | recovers octave notes (pi5/desktop tiers) |
| Polyphonic resynth | `src/hn_poly_synth.hpp` | bank of up to `MAX_NOTES` `AdditiveSynth` voices |
| Offline harness | `tools/hn_test.cpp` | synthetic notes/chords, hit-rate metrics |
| Real-audio runner | `tools/hn_wav.cpp` | detection over a real recording |
| Offline render | `tools/hn_render.cpp` | analyze → resynthesize a pad to raw f32 |

Tessitura extended down to **A1 = 55 Hz** (a fourth below the guitar low E).

### Build / run the offline tools

```sh
g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 tools/hn_test.cpp   -o /tmp/hn_test   && /tmp/hn_test
g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 tools/hn_wav.cpp    -o /tmp/hn_wav    # needs decoded float32
g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 tools/hn_render.cpp -o /tmp/hn_render
# decode any source first:  ffmpeg -i in.m4a -ac 1 -ar 48000 -f f32le audio.f32
```

## Validation already done

- **Synthetic harness (desktop tier): 17/20 fundamentals, 0 false positives.**
  Single notes incl. A1 spot-on (≤4 cents); pure octave recovered via NNLS.
- **Real acoustic guitar (score-confirmed, standard tuning):** opening chord
  ground truth `C3 + E3/A3/D4` matched by the high-confidence detections within
  a few cents. The real recording exposed and fixed the sub-octave phantom bug.

## TO TEST when an audio station is available (listening)

Render A/B with `tools/hn_render.cpp` (source vs resynthesized pad) and judge:

1. **Timbre fidelity** — does the pad read as a plausible guitar/organ-ish
   sustain, or too synthetic/metallic? If metallic, suspect partial-amplitude
   errors or missing partial phases (we currently start all phases at 0; try
   seeding from `harm_phase`).
2. **Noise balance** — the shared broadband residual (`noise_rms`, emitted by
   voice 0). Too hissy / too dull? The `* 4.0f` scaler in `additive_synth.hpp`
   is a guess; calibrate by ear.
3. **Per-note level balance** — does one note dominate or vanish? Compare with
   the printed confidences; a quiet real note should still be audible.
4. **Polyphonic clarity** — on a full 4–6 note chord, can you hear each note, or
   do shared harmonics muddy it (esp. octave/fifth-related notes)?
5. **Attack/onset** — phase-0 start may click or sound unnaturally uniform;
   listen at the freeze onset.
6. **Confidence gate** — decide the threshold (~25 % looked right on paper) that
   removes parasites without dropping real quiet notes.
7. **In-plugin (after wiring):** latency-masking crossfade interaction (the
   `comp_level` work on the other branch), CPU on the Dwarf, blend behaviour.

## TO WATCH (technical risks / known limits)

- **Fully-buried octaves in dense chords** (e.g. E3 under E2+B2 with three-way
  harmonic overlap) are not recovered — theoretically need a timbre prior.
- **Sub-octave phantoms** are guarded but not impossible; watch very low notes
  (≤ A1) where the guard has least spectral room.
- **Low-range resolution** depends on capture length: a 55 Hz note needs a long
  enough frozen loop (≥ ~2–3 periods ≈ 50 ms minimum, more is better). Short
  captures degrade low-note separation.
- **Level on the polyphonic path**: resynth now SUMS up to 6 notes, so it is
  louder than the old single-note path — verify gain staging into the blend /
  soft-clip downstream.
- **CPU (resynth, real-time)**: cost ≈ `MAX_NOTES × MAX_PARTIALS` oscillators
  per sample, ×3 if the pitch voices (v0/v1/v2) are each made polyphonic.
  Dwarf tier caps at 4 notes × 16 partials; measure before shipping.
- **Analysis cost (one-shot)**: 16384-pt FFT at LoopReady — confirm the click it
  causes is acceptable (it already was for the monophonic path).
- **Arbitrary analysis windows blur arpeggios** in the offline tools; the plugin
  analyzes the onset-locked freeze capture, which is cleaner — don't over-tune
  thresholds to the offline windows.
