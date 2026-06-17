// Host-agnostic DSP core for Megalo.
//
// This file contains ZERO plugin-format dependencies (no lv2.h, no JUCE).
// It is included verbatim by every wrapper: the LV2 wrapper (src/plugin.cpp)
// and the JUCE wrapper (juce/PluginProcessor.cpp). All processing lives here;
// each wrapper only maps host controls onto MegaloParams and calls process().
//
// The body below was lifted directly from the former LV2 run()/activate()/
// instantiate()/cleanup() so the audio output stays bit-identical.

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

// Internal constants (not exposed as parameters)
static constexpr int XFADE_MS        = 20;
static constexpr int RETRIGGER_MS    = 200;
static constexpr int CAPTURE_FADE_MS = 10;

// Latency-compensation dry-fill ("no audio hole").
// When an onset triggers a new freeze, the captured loop is not ready for the
// whole PendingSkip+Recording window (hundreds of ms). During that gap the wet
// path is silent (first freeze) or the stale loop fading out, so at full blend
// the output would drop out. We fill the gap with the live dry signal and then
// hand back to the processed pad once the new loop is ready.
//
// Two phases (comp_level = dry-fill amount in [0,1]):
//   • Gap (comp_target = 1): crossfade output toward the live dry, masking the
//     stale/silent wet — out = x·comp + mix·(1−comp).
//   • Recovery (comp_target = 0, loop ready): the pad rises on its OWN envelope
//     (env_g) and the dry-fill is added on top, receding as env_g climbs —
//     out = mix + x·blend·comp. The pad is therefore gained by env_g ALONE
//     (single rise); the earlier code multiplied it by (1−comp) as well, so a
//     fresh pad attacking from zero was attenuated twice and came out far too
//     quiet next to the slowly-receding dry. Driving the recede off env_g keeps
//     the two perfectly complementary.
static constexpr int COMP_FADE_IN_MS  = 15;   // onset → dry (fast, anti-click)
static constexpr int COMP_FADE_OUT_MS = 60;   // anti-click floor: fastest the
                                              // dry-fill may recede (the env
                                              // attack drives it when slower)

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

    // Latency-compensation crossfade state (see COMP_FADE_*_MS above).
    float comp_level  = 0.0f;  // current dry-fill amount [0,1]
    float comp_target = 0.0f;  // 1 on onset (cover the gap), 0 once loop is ready
};

// ── Helpers ──────────────────────────────────────────────────────────────────
static inline double semi_to_ratio(float semi) noexcept {
    return std::pow(2.0, static_cast<double>(semi) / 12.0);
}

static inline float soft_clip(float x) noexcept {
    return x / (1.0f + std::abs(x));
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
    p->comp_level = 0.0f;
    p->comp_target = 0.0f;
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

    // Latency-compensation dry-fill increments (per sample). On onset the dry
    // fades in fast (anti-click). On recovery it recedes following the pad's own
    // attack envelope (env_g) — comp_dn_inc only caps how fast it may drop, so a
    // very short attack still gets an anti-click fade and a slow pad swell is
    // tracked sample-for-sample.
    const float comp_up_inc = 1.0f /
        std::max(1.0f, COMP_FADE_IN_MS * 0.001f * sr);
    const float comp_dn_inc = 1.0f /
        std::max(1.0f, COMP_FADE_OUT_MS * 0.001f * sr);

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

    // ── Sample loop ────────────────────────────────────────────────────────
    bool onset_this_block = false;
    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        const FreezeEvent evt = p->freeze.process(x, threshold, sample_ms, attack_skip,
                                                   XFADE_MS, RETRIGGER_MS, CAPTURE_FADE_MS);

        if (evt == FreezeEvent::Onset) {
            p->envelope.release();
            // Cover the capture latency with the live dry signal.
            p->comp_target = 1.0f;
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
            // Start the attack from zero so env_g (which drives the dry-fill
            // recede below) ramps 0→1 cleanly — no step when the prior release
            // had not fully decayed.
            p->envelope.reset();
            p->envelope.trigger();
            // New pad is ready → hand the dry-fill back to the pad's envelope.
            p->comp_target = 0.0f;
        }

        // LFO
        const double lfo = std::sin(2.0 * M_PI * p->lfo_phase);
        p->lfo_phase += lfo_inc;
        if (p->lfo_phase >= 1.0) p->lfo_phase -= 1.0;

        const float*   ldata = p->freeze.loop_data();
        const int      llen  = p->freeze.loop_len();

        // Detuned speed: linear interpolation of 2^(±detune_ct/1200) driven by LFO.
        // Avoids per-sample std::pow; det_ratio = 2^(detune_ct/1200) precomputed above.
        const float det_speed = base_speed *
            static_cast<float>(1.0 + (det_ratio - 1.0) * lfo);

        const float v0 = p->gp0.process(ldata, llen, grain_samples, grain_xfade_smp, base_speed);
        const float vd = p->gp_d.process(ldata, llen, grain_samples, grain_xfade_smp, det_speed);
        float v1, v2;
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
        float freeze_sig = base_sig
            + (pitch1_en ? v1 * p1_lvl : 0.0f)
            + (pitch2_en ? v2 * p2_lvl : 0.0f);

        freeze_sig  = p->filter.process(freeze_sig);

        // Advance the envelope exactly once per sample; both channels share it.
        const float env_g = p->envelope.process();
        freeze_sig *= env_g;

        // Update the latency-compensation dry-fill (shared L/R, once per sample).
        if (p->comp_target > 0.0f) {
            // Gap: ramp the dry-fill up fast (anti-click).
            p->comp_level = std::min(1.0f, p->comp_level + comp_up_inc);
        } else {
            // Recovery: recede following the pad's own attack (1 − env_g) but no
            // faster than the anti-click floor. min() latches the fade so a later
            // decay/release of env_g can't bring the dry-fill back up.
            p->comp_level = std::min(p->comp_level,
                std::max(1.0f - env_g, p->comp_level - comp_dn_inc));
        }

        // Mix the wet (env-shaped pad) with the live dry, then fold in the
        // dry-fill. Gap: crossfade to dry, masking the stale/silent wet.
        // Recovery: add the dry-fill on top so the pad is gained by env_g alone
        // (single rise) while the dry recedes complementarily.
        const float mixL = x * (1.0f - blend) + freeze_sig * blend;
        outL[i] = soft_clip(p->comp_target > 0.0f
            ? x * p->comp_level + mixL * (1.0f - p->comp_level)
            : mixL + x * blend * p->comp_level);

        if (stereo) {
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
            float freeze_r = base_r
                + (pitch1_en ? v1r * p1_lvl : 0.0f)
                + (pitch2_en ? v2r * p2_lvl : 0.0f);

            freeze_r  = p->filter_r.process(freeze_r);
            freeze_r *= env_g;

            const float mixR = x * (1.0f - blend) + freeze_r * blend;
            outR[i] = soft_clip(p->comp_target > 0.0f
                ? x * p->comp_level + mixR * (1.0f - p->comp_level)
                : mixR + x * blend * p->comp_level);
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
