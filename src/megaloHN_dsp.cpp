// Host-agnostic DSP core for the polyphonic MegaloHN (harmonic+noise).
//
// This file contains ZERO plugin-format dependencies (no lv2.h, no JUCE).
// It is included verbatim by every wrapper: the LV2 wrapper (src/plugin.cpp)
// and the JUCE wrapper (juce/PluginProcessor.cpp). All processing lives here;
// each wrapper only maps host controls onto MegaloParams and calls process().
//
// Megalo and MegaloHN are now fully separate DSP cores: this file is the
// polyphonic harmonic+noise engine (with the granular path kept only as the
// fallback used when no pitched content is found), and the stock granular
// Megalo lives in src/megalo_dsp.cpp. They share only the C API + MegaloParams
// contract declared in megalo_dsp.h. Keeping them apart means the onset
// dry→wet crossfade tuned here for MegaloHN's deferred analysis can never alter
// the granular Megalo sound, and vice-versa.

#include "megalo_dsp.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <new>

#include "freeze_engine.hpp"
#include "granular_looper.hpp"
#include "biquad.hpp"
#include "envelope.hpp"

#ifdef MEGALO_PHASE_VOCODER
#include "phase_vocoder.hpp"
#endif

#ifdef MEGALO_HN_SYNTH
#include "hn_multif0.hpp"
#include "hn_nnls.hpp"
#include "hn_poly_synth.hpp"
#ifdef MEGALO_RAVE
#include "rave_engine.hpp"
#endif
#endif

// Internal constants (not exposed as parameters)
static constexpr int XFADE_MS        = 20;
static constexpr int RETRIGGER_MS    = 200;
static constexpr int CAPTURE_FADE_MS = 10;

// Dry→wet handling. Two parts, shared by Megalo and MegaloHN:
//
//   1. blend — a continuous EQUAL-POWER dry/wet mix. dry = the live input,
//      wet = the frozen pad (after filter + ADSR). At blend 0 → live only,
//      at blend 1 → frozen only, with constant perceived loudness across the
//      sweep (sin/cos law) instead of the old −6 dB dip at the centre.
//
//   2. onset crossfade — at every new freeze the output is held on the live
//      dry signal and then fades to the steady blend over the ENVELOPE ATTACK
//      time (env_attack), so the dry recedes exactly as the frozen pad attacks
//      in. This both masks the capture latency ("no audio hole") and gives a
//      natural dry→wet transition. xfade ∈ [0,1] is 1 right after onset (cover
//      with dry) and decays to 0 (steady blend) once the new pad is ready
//      (LoopReady for granular Megalo; after the deferred analysis for
//      MegaloHN). Per sample:
//        dry_g = (dry_g0 + (1 − dry_g0)·xfade) · dry_level
//        wet_g =  wet_g0 · (1 − xfade)
//        out   = soft_clip(x·dry_g + freeze_sig·wet_g)
//      xfade gates BOTH channels: dry is boosted toward full AND wet is pulled
//      to zero while xfade = 1.  This means envelope.reset() at LoopReady is
//      inaudible (wet was already 0), and the trigger-time crossfade is a clean
//      simultaneous dry-fade-out / wet-fade-in over env_attack milliseconds.

// Stereo width: left/right filter cutoffs are spread by ±this fraction, and
// the right detune LFO runs anti-phase to the left. Subtle enough to stay
// mono-compatible (the dry signal is never decorrelated).
static constexpr float CUTOFF_STEREO_SPREAD = 0.03f;   // ±3 %

// ── DSP instance ─────────────────────────────────────────────────────────────
// = the former LV2 Megalo struct, minus the port pointers (audio_in/out, ctl[],
//   trigger_out), plus trigger_value to hold the GUI pulse for the getter.
struct MegaloDsp {
    double sample_rate;

    FreezeEngine freeze;
    GrainPlayer  gp0;   // base pitch (always granular)
    GrainPlayer  gp_d;  // detuned copy (always granular)
    GrainPlayer  gp1;   // pitch voice 1 — granular engine
    GrainPlayer  gp2;   // pitch voice 2 — granular engine

    // Right-channel mirror, used only by megalo_dsp_process_stereo(). Each
    // grain player is seeded with a different RNG stream so the right grain
    // cloud is decorrelated from the left → wide stereo image. Untouched (and
    // therefore free) on the mono path.
    GrainPlayer  gp0_r;
    GrainPlayer  gp_d_r;
    GrainPlayer  gp1_r;
    GrainPlayer  gp2_r;
#ifdef MEGALO_PHASE_VOCODER
    PhaseVocoder pv1;   // pitch voice 1 — phase-vocoder engine (runtime-selectable)
    PhaseVocoder pv2;   // pitch voice 2 — phase-vocoder engine
    float        pv1_last_semi = 1e9f;
    float        pv2_last_semi = 1e9f;
#endif
    Biquad       filter;
    Biquad       filter_r;   // right channel — micro-offset cutoff (stereo only)
    Envelope     envelope;

    double lfo_phase = 0.0;

    float cached_ftype  = -1.0f;
    float cached_cutoff = -1.0f;
    float cached_q      = -1.0f;
    int   cached_stereo = -1;   // filter setup depends on mono vs stereo cutoff

    // Onset pulse output (held high for a short window after each detected
    // onset so the GUI poll catches the transition reliably).
    int   trigger_hold  = 0;   // samples remaining in the held-high window
    float trigger_value = 0.0f;

    // Onset dry→wet crossfade state (see the comment block up top). Shared by
    // both the granular Megalo and the polyphonic MegaloHN builds.
    float xfade     = 0.0f;    // 1 right after onset (full dry), → 0 at steady blend
    bool  xfade_run = false;   // false while covering the capture gap; true once
                               // the new pad is ready and the dry may recede

#ifdef MEGALO_HN_SYNTH
    // Polyphonic harmonic+noise engine. Analysis runs once per capture (at the
    // block following LoopReady); the three voice banks resynthesize the whole
    // chord at base/pitch1/pitch2. When no pitched content is found the granular
    // path below is used as a fallback.
    MultiHNState      hn_state;
    PolyAdditiveSynth hn_v0;   // base chord (clean)
    PolyAdditiveSynth hn_vd;   // detuned copy of the base chord (chorus voice)
    PolyAdditiveSynth hn_v1;   // pitch voice 1
    PolyAdditiveSynth hn_v2;   // pitch voice 2
    bool hn_needs_analysis = false;
    // MegaloHN anti-click: the additive voices only exist after the deferred
    // analysis runs (the block following LoopReady). We therefore hold the
    // freeze envelope at zero from LoopReady until then and trigger the attack
    // only once the new chord is ready — so the stale previous chord never
    // plays into the new attack and the dry-fill covers a clean dry→wet
    // crossfade instead of a chord-swap discontinuity.
    bool hn_trigger_pending = false;
    // Cached timbre controls so set_timbre() (pow/exp per partial) only re-runs
    // when a knob actually moves. Sentinel forces the first update.
    float hn_t_bright = 1e9f, hn_t_damp = 1e9f, hn_t_eo = 1e9f, hn_t_noise = 1e9f;
#ifdef MEGALO_RAVE
    RaveEngine rave;
#endif
#endif
};

// ── Helpers ──────────────────────────────────────────────────────────────────
static inline double semi_to_ratio(float semi) noexcept {
    return std::pow(2.0, static_cast<double>(semi) / 12.0);
}

// Knee clipper: bit-transparent below the knee, smooth tanh saturation above,
// bounded at ±1. The previous x/(1+|x|) waveshaper compressed the WHOLE
// signal (−3.5 dB and audible odd harmonics already at −6 dBFS), so the dry
// path never passed clean even at blend = 0.
static inline float soft_clip(float x) noexcept {
    constexpr float KNEE = 0.7f;             // ≈ −3 dBFS
    const float a = std::abs(x);
    if (a <= KNEE) return x;
    const float y = KNEE + (1.0f - KNEE) * std::tanh((a - KNEE) / (1.0f - KNEE));
    return (x < 0.0f) ? -y : y;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
// = the former instantiate()
MegaloDsp* megalo_dsp_new(double sample_rate)
{
    MegaloDsp* p = new (std::nothrow) MegaloDsp();
    if (!p) return nullptr;
    p->sample_rate = sample_rate;
    p->freeze.init(sample_rate);
    // Decorrelate the right-channel grain clouds from the left (which keep the
    // default 12345 seed). Distinct constants so each voice is independent too.
    p->gp0_r.seed(0x9E3779B9u);
    p->gp_d_r.seed(0x85EBCA6Bu);
    p->gp1_r.seed(0xC2B2AE35u);
    p->gp2_r.seed(0x27D4EB2Fu);
#ifdef MEGALO_PHASE_VOCODER
    p->pv1.init(sample_rate);
    p->pv2.init(sample_rate);
#endif
    return p;
}

// = the former cleanup()
void megalo_dsp_free(MegaloDsp* p)
{
    delete p;
}

// = the former activate()
void megalo_dsp_reset(MegaloDsp* p)
{
    p->freeze.reset();
    p->gp0.reset();
    p->gp_d.reset();
    p->gp1.reset();
    p->gp2.reset();
    p->gp0_r.reset();
    p->gp_d_r.reset();
    p->gp1_r.reset();
    p->gp2_r.reset();
#ifdef MEGALO_PHASE_VOCODER
    p->pv1.reset();
    p->pv2.reset();
    p->pv1_last_semi = p->pv2_last_semi = 1e9f;
#endif
    p->filter.reset();
    p->filter_r.reset();
    p->envelope.reset();
    p->lfo_phase = 0.0;
    p->cached_ftype = p->cached_cutoff = p->cached_q = -1.0f;
    p->cached_stereo = -1;
    p->trigger_hold = 0;
    p->trigger_value = 0.0f;
    p->xfade     = 0.0f;
    p->xfade_run = false;
#ifdef MEGALO_HN_SYNTH
    p->hn_state          = MultiHNState{};
    p->hn_needs_analysis = false;
    p->hn_trigger_pending = false;
    p->hn_t_bright = p->hn_t_damp = p->hn_t_eo = p->hn_t_noise = 1e9f;
    // Clear the additive voices too (idle = no active notes) so a reset can
    // never leave a stale chord behind. Analysis re-seeds them on the next loop.
    const MultiHNState hn_empty{};
    const float hn_sr = static_cast<float>(p->sample_rate);
    p->hn_v0.reset(hn_empty, hn_sr);
    p->hn_vd.reset(hn_empty, hn_sr);
    p->hn_v1.reset(hn_empty, hn_sr);
    p->hn_v2.reset(hn_empty, hn_sr);
#endif
}

// Shared worker for the mono and stereo entry points. outR == nullptr selects
// the mono path, which stays bit-identical to the original run(): only the
// left chain runs and the right-channel state is never touched. When outR is
// non-null the right chain runs too, decorrelated from the left.
static void process_impl(MegaloDsp* p, const MegaloParams* p_,
                         const float* in, float* outL, float* outR,
                         uint32_t n_samples)
{
    const bool  stereo = (outR != nullptr);
    const float sr = static_cast<float>(p->sample_rate);

    // ── Snapshot controls (block boundary) ────────────────────────────────
    const float threshold     = std::clamp(p_->onset_threshold,     0.0f,   1.0f);
    const int   sample_ms     = std::clamp(static_cast<int>(p_->sample_ms), 50, 500);
    const int   attack_skip   = std::clamp(static_cast<int>(p_->attack_skip_ms), 0, 500);
    const float blend         = std::clamp(p_->blend,               0.0f,   1.0f);
    // Equal-power dry/wet gains for the steady mix (no −6 dB centre dip).
    const float wet_g0        = std::sin(blend * 1.57079633f);
    const float dry_g0        = std::cos(blend * 1.57079633f);
    // Dry level: gain on the live dry signal (1.0 = neutral). Permanent control.
    const float dry_level     = std::clamp(p_->dry_level,           0.0f,   2.0f);
    const int   grain_ms      = std::clamp(static_cast<int>(p_->grain_size_ms),    5, 200);
    const int   grain_samples = std::clamp((int)(sr * grain_ms * 0.001f), 16, FREEZE_MAX_SAMPLES);
    const int   grain_xfade_ms  = std::clamp(static_cast<int>(p_->grain_xfade_ms), 5, 100);
    const int   grain_xfade_smp = std::clamp((int)(sr * grain_xfade_ms * 0.001f), 8, grain_samples / 2);
    const float base_pitch    = std::clamp(p_->base_pitch,         -12.0f, 12.0f);
    const float base_speed    = static_cast<float>(semi_to_ratio(base_pitch));
    const float p1_semi       =            p_->pitch1_semi;
    const float p1_lvl        = std::clamp(p_->pitch1_level,        0.0f,   1.0f);
    const float p2_semi       =            p_->pitch2_semi;
    const float p2_lvl        = std::clamp(p_->pitch2_level,        0.0f,   1.0f);
    const float detune_ct     = std::clamp(p_->detune_cents,        0.0f,  50.0f);
    const float chorus_rate   = std::clamp(p_->chorus_rate,         0.1f,   8.0f);
    const float detune_blend  = std::clamp(p_->detune_blend,        0.0f,   1.0f);
    const float filt_type     =            p_->filter_type;
    const float filt_cutoff   = std::clamp(p_->filter_cutoff,      20.0f, sr * 0.499f);
    const float filt_q        = std::clamp(p_->filter_q,            0.1f,  10.0f);
    const float env_atk       = std::clamp(p_->env_attack,          0.0f,  5000.0f);
    const float env_dcy       = std::clamp(p_->env_decay,           0.0f,  5000.0f);
    const float env_sus       = std::clamp(p_->env_sustain,         0.0f,   1.0f);
    const float env_rel       = std::clamp(p_->env_release,         0.0f, 10000.0f);
    const bool  detune_en     = p_->detune_enable >= 0.5f;
    const bool  pitch1_en     = p_->pitch1_enable >= 0.5f;
    const bool  pitch2_en     = p_->pitch2_enable >= 0.5f;
    // Pitch engine select. The phase vocoder only exists when compiled in;
    // otherwise the granular reader is always used (memory-lean targets).
#ifdef MEGALO_PHASE_VOCODER
    const bool  use_pv        = p_->pitch_mode >= 0.5f;
#else
    [[maybe_unused]] const bool use_pv = false;
#endif

    // ── Filter ────────────────────────────────────────────────────────────
    // Mono: exact cutoff (bit-identical to before). Stereo: spread the L/R
    // cutoffs by ±CUTOFF_STEREO_SPREAD for a subtle tonal decorrelation.
    if (filt_type != p->cached_ftype || filt_cutoff != p->cached_cutoff ||
        filt_q != p->cached_q || (int)stereo != p->cached_stereo) {
        Biquad::Type btype = (filt_type < 0.5f) ? Biquad::LP
                           : (filt_type < 1.5f) ? Biquad::HP
                           :                      Biquad::BP;
        const float nyq = sr * 0.499f;
        const float cut_l = stereo
            ? std::clamp(filt_cutoff * (1.0f - CUTOFF_STEREO_SPREAD), 20.0f, nyq)
            : filt_cutoff;
        p->filter.setup(btype, cut_l, filt_q, sr);
        if (stereo) {
            const float cut_r = std::clamp(filt_cutoff * (1.0f + CUTOFF_STEREO_SPREAD), 20.0f, nyq);
            p->filter_r.setup(btype, cut_r, filt_q, sr);
        }
        p->cached_ftype  = filt_type;
        p->cached_cutoff = filt_cutoff;
        p->cached_q      = filt_q;
        p->cached_stereo = (int)stereo;
    }

    p->envelope.set(env_atk, env_dcy, env_sus, env_rel, sr);

    // The onset dry→wet crossfade tracks the envelope ATTACK: the live dry
    // fades back to the steady blend over the same time the frozen pad takes to
    // attack in — the most natural pairing. (0 ms attack ⇒ instant hand-over.)
    const float xfade_dec = 1.0f /
        std::max(1.0f, env_atk * 0.001f * sr);

    const double lfo_inc  = static_cast<double>(chorus_rate) / p->sample_rate;
    // det_speed range per sample: base_speed * 2^(±detune_ct/1200).
    // Precompute the max ratio; the per-sample LFO modulates within [-1,+1].
    const double det_ratio = std::pow(2.0, static_cast<double>(detune_ct) / 1200.0);

    // Granular pitch playback ratios (always available).
    const float v1_speed = static_cast<float>(semi_to_ratio(p1_semi));
    const float v2_speed = static_cast<float>(semi_to_ratio(p2_semi));
#ifdef MEGALO_PHASE_VOCODER
    if (use_pv) {
        const double lfo_now    = std::sin(2.0 * M_PI * p->lfo_phase);
        const float  pv1_detune = detune_en
                                  ? static_cast<float>(detune_ct * lfo_now / 100.0)
                                  : 0.0f;
        const float  pv1_semi_mod = p1_semi + pv1_detune;
        if (pv1_semi_mod != p->pv1_last_semi) {
            p->pv1.set_pitch(pv1_semi_mod);
            p->pv1_last_semi = pv1_semi_mod;
        }
        if (p2_semi != p->pv2_last_semi) { p->pv2.set_pitch(p2_semi); p->pv2_last_semi = p2_semi; }
    }
#endif

#ifdef MEGALO_HN_SYNTH
    const float hn_bright = std::clamp(p_->hn_brightness, -1.0f, 1.0f);
    const float hn_damp   = std::clamp(p_->hn_damping,     0.0f, 1.0f);
    const float hn_eo     = std::clamp(p_->hn_even_odd,   -1.0f, 1.0f);
    const float hn_noise  = std::clamp(p_->hn_noise,       0.0f, 1.0f);
    const float hn_width  = std::clamp(p_->hn_width,       0.0f, 1.0f);

    // ── H+N analysis (deferred from LoopReady to this block boundary) ──────
    // hn_multif0_analyze() is not RT-safe (FFT + NNLS, a few ms); running it
    // here, once per capture, keeps the per-sample loop clean.
    if (p->hn_needs_analysis) {
        const float* la = p->freeze.loop_data();
        const int    ll = p->freeze.loop_len();
        if (ll > 0) {
            p->hn_state = hn_multif0_analyze(la, ll, sr);
            p->hn_v0.reset(p->hn_state, sr);
            p->hn_vd.reset(p->hn_state, sr);
            p->hn_v1.reset(p->hn_state, sr);
            p->hn_v2.reset(p->hn_state, sr);
            p->hn_t_bright = 1e9f;   // fresh voices → force a timbre re-apply
#ifdef MEGALO_RAVE
            if (p->rave.valid()) p->rave.encode(la, ll, sr);
#endif
        }
        p->hn_needs_analysis = false;
    }
    // The fresh chord (additive voices, or the granular fallback when no pitch
    // was found) now exists → start the freeze attack from zero. Deferring it to
    // here, rather than firing at LoopReady, is what removes the chord-swap click
    // and lets the dry-fill crossfade cleanly into the wet.
    if (p->hn_trigger_pending) {
        p->envelope.trigger();
        p->xfade_run = true;   // new chord ready → fade the dry back to the blend
        p->hn_trigger_pending = false;
    }
    // Re-apply the timbre gains only when a control moved (or after analysis).
    if (p->hn_state.valid &&
        (hn_bright != p->hn_t_bright || hn_damp != p->hn_t_damp ||
         hn_eo != p->hn_t_eo || hn_noise != p->hn_t_noise)) {
        p->hn_v0.set_timbre(hn_bright, hn_damp, hn_eo, hn_noise);
        p->hn_vd.set_timbre(hn_bright, hn_damp, hn_eo, hn_noise);
        p->hn_v1.set_timbre(hn_bright, hn_damp, hn_eo, hn_noise);
        p->hn_v2.set_timbre(hn_bright, hn_damp, hn_eo, hn_noise);
        p->hn_t_bright = hn_bright; p->hn_t_damp = hn_damp;
        p->hn_t_eo = hn_eo;         p->hn_t_noise = hn_noise;
    }
    // Update the chord transpositions every block (real-time knob changes).
    if (p->hn_state.valid) {
        const double lfo_blk = std::sin(2.0 * M_PI * p->lfo_phase);
        // Base voice stays clean; the detuned copy oscillates within ±detune_ct
        // cents around the base (chorus LFO at chorus_rate). detune_blend then
        // mixes that copy into the base in the render loop — mirroring the
        // granular base/gp_d path, so the detune is heard as a blendable
        // detuned layer of the MegaloHN signal rather than a pitch wobble.
        p->hn_v0.set_pitch_ratio(base_speed);
        const float det_speed_blk = detune_en
            ? base_speed * static_cast<float>(1.0 + (det_ratio - 1.0) * lfo_blk)
            : base_speed;
        p->hn_vd.set_pitch_ratio(det_speed_blk);
        p->hn_v1.set_pitch_ratio(static_cast<float>(semi_to_ratio(p1_semi)));
        p->hn_v2.set_pitch_ratio(static_cast<float>(semi_to_ratio(p2_semi)));
    }
#endif

    // ── Sample loop ────────────────────────────────────────────────────────
    bool onset_this_block = false;
    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        const FreezeEvent evt = p->freeze.process(x, threshold, sample_ms, attack_skip,
                                                   XFADE_MS, RETRIGGER_MS, CAPTURE_FADE_MS);

        if (evt == FreezeEvent::Onset) {
            p->envelope.release();
            // Hold the output on the live dry signal; do not start the fade back
            // to the blend until the new pad is ready (LoopReady / HN analysis).
            p->xfade     = 1.0f;
            p->xfade_run = false;
            onset_this_block = true;
        } else if (evt == FreezeEvent::LoopReady) {
            p->gp0.reset();
            p->gp_d.reset();
            p->gp1.reset();
            p->gp2.reset();
            if (stereo) {
                p->gp0_r.reset();
                p->gp_d_r.reset();
                p->gp1_r.reset();
                p->gp2_r.reset();
            }
#ifdef MEGALO_PHASE_VOCODER
            p->pv1.reset();
            p->pv2.reset();
#endif
            // Start the attack from zero so the pad fades in cleanly — no step
            // when the prior release had not fully decayed.
            p->envelope.reset();
            // Keep the output fully on the live dry over the hand-over.
            p->xfade = 1.0f;
#ifdef MEGALO_HN_SYNTH
            // MegaloHN: the additive voices aren't analyzed/reset until the next
            // block boundary. Hold the envelope at zero (stale chord stays
            // silent, dry covers at full) and defer both the attack and the
            // start of the crossfade until the new chord exists — see
            // hn_trigger_pending. The granular Megalo build hands over now.
            p->hn_needs_analysis  = true;   // analyze the fresh loop next block
            p->hn_trigger_pending = true;
#else
            p->envelope.trigger();
            p->xfade_run = true;            // new pad ready → fade dry back to blend
#endif
        }

        // LFO
        const double lfo = std::sin(2.0 * M_PI * p->lfo_phase);
        p->lfo_phase += lfo_inc;
        if (p->lfo_phase >= 1.0) p->lfo_phase -= 1.0;

        const float*   ldata = p->freeze.loop_data();
        const int      llen  = p->freeze.loop_len();

        float freeze_sig;
        // v1/v2 live at loop scope: the right channel's phase-vocoder path
        // shares them. Granular voices for pitch 1/2 (else: HN replaces them).
        float v1 = 0.0f, v2 = 0.0f;
#ifdef MEGALO_HN_SYNTH
        const bool hn_on = p->hn_state.valid;
        float hn_wet_r = 0.0f;   // HN right channel (carried to the stereo block)
        if (hn_on) {
            // Polyphonic additive resynthesis of the frozen chord. The granular
            // players are skipped entirely on this path.
            if (stereo && hn_width > 0.0f) {
                float l0, r0, l1, r1, l2, r2;
                p->hn_v0.process_stereo(l0, r0, hn_width);
                p->hn_v1.process_stereo(l1, r1, hn_width);
                p->hn_v2.process_stereo(l2, r2, hn_width);
                // Base + blended detuned copy (detune_blend = amount injected).
                // The detuned voice bank is only rendered when detune is on —
                // its output is discarded otherwise, so skip the work.
                float base_l = l0, base_r = r0;
                if (detune_en) {
                    float ld, rd;
                    p->hn_vd.process_stereo(ld, rd, hn_width);
                    base_l = l0 * (1.0f - detune_blend) + ld * detune_blend;
                    base_r = r0 * (1.0f - detune_blend) + rd * detune_blend;
                }
                freeze_sig = base_l + (pitch1_en ? l1 * p1_lvl : 0.0f)
                                    + (pitch2_en ? l2 * p2_lvl : 0.0f);
                hn_wet_r   = base_r + (pitch1_en ? r1 * p1_lvl : 0.0f)
                                    + (pitch2_en ? r2 * p2_lvl : 0.0f);
            } else {
                const float h0 = p->hn_v0.process();
                const float h1 = p->hn_v1.process();
                const float h2 = p->hn_v2.process();
                // Base + blended detuned copy; render the detuned bank only when
                // detune is on (its output is discarded otherwise).
                const float base = detune_en
                    ? h0 * (1.0f - detune_blend) + p->hn_vd.process() * detune_blend
                    : h0;
                freeze_sig = base + (pitch1_en ? h1 * p1_lvl : 0.0f)
                                  + (pitch2_en ? h2 * p2_lvl : 0.0f);
                hn_wet_r = freeze_sig;   // centred
            }
        } else
#endif
        {
            // Detuned speed: linear interpolation of 2^(±detune_ct/1200) driven
            // by the LFO. Avoids per-sample std::pow (det_ratio precomputed).
            const float det_speed = base_speed *
                static_cast<float>(1.0 + (det_ratio - 1.0) * lfo);

            const float v0 = p->gp0.process(ldata, llen, grain_samples, grain_xfade_smp, base_speed);
            const float vd = p->gp_d.process(ldata, llen, grain_samples, grain_xfade_smp, det_speed);
#ifdef MEGALO_PHASE_VOCODER
            if (use_pv) {
                v1 = (llen > 0) ? p->pv1.process(ldata, llen) : 0.0f;
                v2 = (llen > 0) ? p->pv2.process(ldata, llen) : 0.0f;
            } else
#endif
            {
                v1 = p->gp1.process(ldata, llen, grain_samples, grain_xfade_smp, v1_speed);
                v2 = p->gp2.process(ldata, llen, grain_samples, grain_xfade_smp, v2_speed);
            }

            // Mix: detune_blend fades between dry base and detuned base
            const float base_sig = detune_en
                ? v0 * (1.0f - detune_blend) + vd * detune_blend
                : v0;
            freeze_sig = base_sig
                + (pitch1_en ? v1 * p1_lvl : 0.0f)
                + (pitch2_en ? v2 * p2_lvl : 0.0f);
        }

        freeze_sig  = p->filter.process(freeze_sig);

        // Advance the envelope exactly once per sample; both channels share it.
        const float env_g = p->envelope.process();
        freeze_sig *= env_g;

        // Advance the onset crossfade once per sample (shared L/R). xfade is held
        // at 1 from onset until the new pad is ready, then decays to 0 over the
        // envelope ATTACK time, fading the boosted dry back to the steady blend.
        if (p->xfade_run)
            p->xfade = std::max(0.0f, p->xfade - xfade_dec);
        const float xf    = p->xfade;
        const float dry_g = (dry_g0 + (1.0f - dry_g0) * xf) * dry_level;
        const float wet_g = wet_g0 * (1.0f - xf);

        // Dry→wet crossfade: xfade gates both sides simultaneously — dry is
        // boosted toward full while wet is pulled to zero (xf=1), then they
        // swap back to the steady blend (xf=0) over env_attack milliseconds.
        outL[i] = soft_clip(x * dry_g + freeze_sig * wet_g);

        if (stereo) {
            float freeze_r;
#ifdef MEGALO_HN_SYNTH
            if (hn_on) {
                // Width-decorrelated right channel (== left when hn_width == 0).
                freeze_r = hn_wet_r;
            } else
#endif
            {
                // Right channel: independent grain randomisation (decorrelated
                // seeds), anti-phase detune LFO, and the micro-offset filter.
                // The dry x stays identical on both sides → mono-compatible.
                const float det_speed_r = base_speed *
                    static_cast<float>(1.0 + (det_ratio - 1.0) * (-lfo));

                const float v0r = p->gp0_r.process(ldata, llen, grain_samples, grain_xfade_smp, base_speed);
                const float vdr = p->gp_d_r.process(ldata, llen, grain_samples, grain_xfade_smp, det_speed_r);
                float v1r, v2r;
#ifdef MEGALO_PHASE_VOCODER
                if (use_pv) {
                    // The phase vocoder is heavy; share its pitch voices across
                    // channels. Width still comes from the granular base + detune.
                    v1r = v1;
                    v2r = v2;
                } else
#endif
                {
                    v1r = p->gp1_r.process(ldata, llen, grain_samples, grain_xfade_smp, v1_speed);
                    v2r = p->gp2_r.process(ldata, llen, grain_samples, grain_xfade_smp, v2_speed);
                }

                const float base_r = detune_en
                    ? v0r * (1.0f - detune_blend) + vdr * detune_blend
                    : v0r;
                freeze_r = base_r
                    + (pitch1_en ? v1r * p1_lvl : 0.0f)
                    + (pitch2_en ? v2r * p2_lvl : 0.0f);
            }

            freeze_r  = p->filter_r.process(freeze_r);
            freeze_r *= env_g;

            // Same crossfade gains as the left channel (xf/dry_g/wet_g shared).
            outR[i] = soft_clip(x * dry_g + freeze_r * wet_g);
        }
    }

    // ── Trigger pulse output ───────────────────────────────────────────────
    // Hold high for ~50 ms after each onset so MOD-UI's port polling (which
    // runs at ~30 Hz) reliably catches the 0→1→0 sequence.
    if (onset_this_block) {
        p->trigger_hold = static_cast<int>(0.050 * p->sample_rate);
    }
    p->trigger_hold -= static_cast<int>(n_samples);
    if (p->trigger_hold < 0) p->trigger_hold = 0;
    p->trigger_value = p->trigger_hold > 0 ? 1.0f : 0.0f;
}

// = the former run(); mono in → mono out (unchanged behaviour).
void megalo_dsp_process(MegaloDsp* p, const MegaloParams* p_,
                        const float* in, float* out, uint32_t n_samples)
{
    process_impl(p, p_, in, out, nullptr, n_samples);
}

// Mono in → decorrelated stereo out.
void megalo_dsp_process_stereo(MegaloDsp* p, const MegaloParams* p_,
                               const float* in, float* outL, float* outR,
                               uint32_t n_samples)
{
    process_impl(p, p_, in, outL, outR, n_samples);
}

float megalo_dsp_trigger(const MegaloDsp* p)
{
    return p->trigger_value;
}
