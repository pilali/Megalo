#include <lv2/core/lv2.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <new>

#include "freeze_engine.hpp"
#include "granular_looper.hpp"
#include "biquad.hpp"
#include "envelope.hpp"

static constexpr char MEGALO_URI[] = "https://github.com/pilali/megalo";

// ── Port indices ───────────────────────────────────────────────────────────
enum Port : uint32_t {
    P_AUDIO_IN       =  0,
    P_AUDIO_OUT      =  1,
    P_THRESHOLD      =  2,   // onset sensitivity    [0 – 1]
    P_SAMPLE_MS      =  3,   // capture length ms    [50 – 500]
    P_ATTACK_SKIP    =  4,   // skip after onset ms  [0 – 500]
    P_BLEND          =  5,   // dry/wet              [0 – 1]
    P_GRAIN_MS       =  6,   // grain duration ms    [5 – 200]
    P_GRAIN_XFADE_MS =  7,   // grain crossfade ms   [5 – 100]
    P_BASE_PITCH     =  8,   // base voice pitch     [-12 – +12] semitones
    P_PITCH1_SEMI    =  9,   // voice-1 semitones    [-24 – 24]
    P_PITCH1_LVL     = 10,   // voice-1 level        [0 – 1]
    P_PITCH2_SEMI    = 11,   // voice-2 semitones    [-24 – 24]
    P_PITCH2_LVL     = 12,   // voice-2 level        [0 – 1]
    P_DETUNE_CT      = 13,   // LFO detune cents     [0 – 50]
    P_CHORUS_RATE    = 14,   // LFO rate Hz          [0.1 – 8]
    P_DETUNE_BLEND   = 15,   // dry/detuned mix      [0 – 1]
    P_FILT_TYPE      = 16,   // 0=LP 1=HP 2=BP
    P_FILT_CUTOFF    = 17,   // Hz                   [20 – 20000]
    P_FILT_Q         = 18,   // Q                    [0.1 – 10]
    P_ENV_ATK        = 19,   // attack  ms           [0 – 5000]
    P_ENV_DCY        = 20,   // decay   ms           [0 – 5000]
    P_ENV_SUS        = 21,   // sustain level        [0 – 1]
    P_ENV_REL        = 22,   // release ms           [0 – 10000]
    P_COUNT          = 23
};

static constexpr uint32_t N_CTL = P_COUNT - 2;

// Internal constants (not exposed as ports)
static constexpr int XFADE_MS       = 20;
static constexpr int RETRIGGER_MS   = 200;
static constexpr int CAPTURE_FADE_MS = 10;

// ── Plugin instance ────────────────────────────────────────────────────────
struct Megalo {
    double sample_rate;

    const float* audio_in  = nullptr;
    float*       audio_out = nullptr;
    std::array<const float*, N_CTL> ctl = {};

    FreezeEngine freeze;
    GrainPlayer  gp0;   // base pitch
    GrainPlayer  gp_d;  // detuned copy
    GrainPlayer  gp1;   // pitch voice 1
    GrainPlayer  gp2;   // pitch voice 2
    Biquad       filter;
    Envelope     envelope;

    double lfo_phase = 0.0;

    float cached_ftype  = -1.0f;
    float cached_cutoff = -1.0f;
    float cached_q      = -1.0f;
};

// ── Helpers ────────────────────────────────────────────────────────────────
static inline float ctl(const Megalo* p, Port port) noexcept {
    return *p->ctl[port - 2];
}

static inline double semi_to_ratio(float semi) noexcept {
    return std::pow(2.0, static_cast<double>(semi) / 12.0);
}

static inline float soft_clip(float x) noexcept {
    return x / (1.0f + std::abs(x));
}

// ── LV2 callbacks ──────────────────────────────────────────────────────────
static LV2_Handle instantiate(const LV2_Descriptor*,
                               double rate,
                               const char*,
                               const LV2_Feature* const*)
{
    Megalo* p = new (std::nothrow) Megalo();
    if (!p) return nullptr;
    p->sample_rate = rate;
    p->freeze.init(rate);
    return p;
}

static void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    Megalo* p = static_cast<Megalo*>(handle);
    if (port == P_AUDIO_IN)
        p->audio_in = static_cast<const float*>(data);
    else if (port == P_AUDIO_OUT)
        p->audio_out = static_cast<float*>(data);
    else if (port >= 2 && port < P_COUNT)
        p->ctl[port - 2] = static_cast<const float*>(data);
}

static void activate(LV2_Handle handle)
{
    Megalo* p = static_cast<Megalo*>(handle);
    p->freeze.reset();
    p->gp0.reset();
    p->gp_d.reset();
    p->gp1.reset();
    p->gp2.reset();
    p->filter.reset();
    p->envelope.reset();
    p->lfo_phase = 0.0;
    p->cached_ftype = p->cached_cutoff = p->cached_q = -1.0f;
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    Megalo* p = static_cast<Megalo*>(handle);

    const float* in = p->audio_in;
    float*      out = p->audio_out;
    const float  sr = static_cast<float>(p->sample_rate);

    // ── Snapshot controls (block boundary) ────────────────────────────────
    const float threshold     = std::clamp(ctl(p, P_THRESHOLD),     0.0f,   1.0f);
    const int   sample_ms     = std::clamp(static_cast<int>(ctl(p, P_SAMPLE_MS)), 50, 500);
    const int   attack_skip   = std::clamp(static_cast<int>(ctl(p, P_ATTACK_SKIP)), 0, 500);
    const float blend         = std::clamp(ctl(p, P_BLEND),         0.0f,   1.0f);
    const int   grain_ms      = std::clamp(static_cast<int>(ctl(p, P_GRAIN_MS)),       5, 200);
    const int   grain_samples = std::clamp((int)(sr * grain_ms * 0.001f), 16, FREEZE_MAX_SAMPLES);
    const int   grain_xfade_ms  = std::clamp(static_cast<int>(ctl(p, P_GRAIN_XFADE_MS)), 5, 100);
    const int   grain_xfade_smp = std::clamp((int)(sr * grain_xfade_ms * 0.001f), 8, grain_samples / 2);
    const float base_pitch    = std::clamp(ctl(p, P_BASE_PITCH),   -12.0f, 12.0f);
    const float base_speed    = static_cast<float>(semi_to_ratio(base_pitch));
    const float p1_semi       =            ctl(p, P_PITCH1_SEMI);
    const float p1_lvl        = std::clamp(ctl(p, P_PITCH1_LVL),    0.0f,   1.0f);
    const float p2_semi       =            ctl(p, P_PITCH2_SEMI);
    const float p2_lvl        = std::clamp(ctl(p, P_PITCH2_LVL),    0.0f,   1.0f);
    const float detune_ct     = std::clamp(ctl(p, P_DETUNE_CT),     0.0f,  50.0f);
    const float chorus_rate   = std::clamp(ctl(p, P_CHORUS_RATE),   0.1f,   8.0f);
    const float detune_blend  = std::clamp(ctl(p, P_DETUNE_BLEND),  0.0f,   1.0f);
    const float filt_type     =            ctl(p, P_FILT_TYPE);
    const float filt_cutoff   = std::clamp(ctl(p, P_FILT_CUTOFF),  20.0f, sr * 0.499f);
    const float filt_q        = std::clamp(ctl(p, P_FILT_Q),        0.1f,  10.0f);
    const float env_atk       = std::clamp(ctl(p, P_ENV_ATK),       0.0f,  5000.0f);
    const float env_dcy       = std::clamp(ctl(p, P_ENV_DCY),       0.0f,  5000.0f);
    const float env_sus       = std::clamp(ctl(p, P_ENV_SUS),       0.0f,   1.0f);
    const float env_rel       = std::clamp(ctl(p, P_ENV_REL),       0.0f, 10000.0f);

    // ── Filter ────────────────────────────────────────────────────────────
    if (filt_type != p->cached_ftype || filt_cutoff != p->cached_cutoff || filt_q != p->cached_q) {
        Biquad::Type btype = (filt_type < 0.5f) ? Biquad::LP
                           : (filt_type < 1.5f) ? Biquad::HP
                           :                      Biquad::BP;
        p->filter.setup(btype, filt_cutoff, filt_q, sr);
        p->cached_ftype  = filt_type;
        p->cached_cutoff = filt_cutoff;
        p->cached_q      = filt_q;
    }

    p->envelope.set(env_atk, env_dcy, env_sus, env_rel, sr);

    const double lfo_inc      = static_cast<double>(chorus_rate) / p->sample_rate;
    const float  v1_speed     = static_cast<float>(semi_to_ratio(p1_semi));
    const float  v2_speed     = static_cast<float>(semi_to_ratio(p2_semi));

    // ── Sample loop ────────────────────────────────────────────────────────
    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        const FreezeEvent evt = p->freeze.process(x, threshold, sample_ms, attack_skip,
                                                   XFADE_MS, RETRIGGER_MS, CAPTURE_FADE_MS);

        if (evt == FreezeEvent::Onset) {
            p->envelope.release();
        } else if (evt == FreezeEvent::LoopReady) {
            p->gp0.reset();
            p->gp_d.reset();
            p->gp1.reset();
            p->gp2.reset();
            p->envelope.trigger();
        }

        // LFO
        const double lfo = std::sin(2.0 * M_PI * p->lfo_phase);
        p->lfo_phase += lfo_inc;
        if (p->lfo_phase >= 1.0) p->lfo_phase -= 1.0;

        const float*   ldata = p->freeze.loop_data();
        const int      llen  = p->freeze.loop_len();

        // Detuned speed: base ± LFO modulated by detune_cents
        const float det_speed = base_speed *
            static_cast<float>(std::pow(2.0, static_cast<double>(detune_ct) * lfo / 1200.0));

        const float v0 = p->gp0.process(ldata, llen, grain_samples, grain_xfade_smp, base_speed);
        const float vd = p->gp_d.process(ldata, llen, grain_samples, grain_xfade_smp, det_speed);
        const float v1 = p->gp1.process(ldata, llen, grain_samples, grain_xfade_smp, v1_speed);
        const float v2 = p->gp2.process(ldata, llen, grain_samples, grain_xfade_smp, v2_speed);

        // Mix: detune_blend fades between dry base and detuned base
        const float base_sig  = v0 * (1.0f - detune_blend) + vd * detune_blend;
        float freeze_sig = base_sig + v1 * p1_lvl + v2 * p2_lvl;

        freeze_sig  = p->filter.process(freeze_sig);
        freeze_sig *= p->envelope.process();

        out[i] = soft_clip(x * (1.0f - blend) + freeze_sig * blend);
    }
}

static void cleanup(LV2_Handle handle)
{
    delete static_cast<Megalo*>(handle);
}

static const LV2_Descriptor descriptor = {
    MEGALO_URI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
