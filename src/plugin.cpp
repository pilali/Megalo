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

#ifdef MEGALO_PHASE_VOCODER
#include "phase_vocoder.hpp"
#endif

static constexpr char MEGALO_URI[] = "https://github.com/pilali/megalo";

// ── Port indices ───────────────────────────────────────────────────────────
enum Port : uint32_t {
    P_AUDIO_IN       =  0,
    P_AUDIO_OUT      =  1,
    P_THRESHOLD      =  2,   // onset sensitivity  [0 – 1]
    P_SAMPLE_MS      =  3,   // capture length ms  [50 – 500]
    P_BLEND          =  4,   // dry/wet            [0 – 1]
    P_DETUNE_CT      =  5,   // LFO detune cents   [0 – 50]
    P_CHORUS_RATE    =  6,   // LFO rate Hz        [0.1 – 8]
    P_PITCH1_SEMI    =  7,   // voice-1 semitones  [-24 – 24]
    P_PITCH1_LVL     =  8,   // voice-1 level      [0 – 1]
    P_PITCH2_SEMI    =  9,   // voice-2 semitones  [-24 – 24]
    P_PITCH2_LVL     = 10,   // voice-2 level      [0 – 1]
    P_FILT_TYPE      = 11,   // 0=LP 1=HP 2=BP
    P_FILT_CUTOFF    = 12,   // Hz                 [20 – 20000]
    P_FILT_Q         = 13,   // Q                  [0.1 – 10]
    P_ENV_ATK        = 14,   // attack  ms         [0 – 5000]
    P_ENV_DCY        = 15,   // decay   ms         [0 – 5000]
    P_ENV_SUS        = 16,   // sustain level      [0 – 1]
    P_ENV_REL        = 17,   // release ms         [0 – 10000]
    P_ATTACK_SKIP_MS = 18,   // skip after onset   [0 – 100]  ms
    P_GRAIN_MS       = 19,   // granular grain     [20 – 200] ms
    P_XFADE_MS       = 20,   // loop boundary xfade [5 – 100] ms
    P_GRAIN_SCATTER  = 21,   // grain jitter       [0 – 1]
    P_RETRIGGER_MS   = 22,   // onset refractory   [50 – 1000] ms
    P_COUNT          = 23
};

static constexpr uint32_t N_CTL = P_COUNT - 2;

// ── Plugin instance ────────────────────────────────────────────────────────
struct Megalo {
    double sample_rate;

    const float* audio_in  = nullptr;
    float*       audio_out = nullptr;
    std::array<const float*, N_CTL> ctl = {};

    FreezeEngine   freeze;
    GranularLooper granular;
    Biquad         filter;
    Envelope       envelope;

    // Loop positions for the variable-speed readers (voices 1 & 2 non-PV path)
    double pos[2] = {0.0, 0.0};

    // Chorus LFO
    double lfo_phase = 0.0;

    // Filter param cache
    float cached_ftype  = -1.0f;
    float cached_cutoff = -1.0f;
    float cached_q      = -1.0f;

#ifdef MEGALO_PHASE_VOCODER
    PhaseVocoder pv1;
    PhaseVocoder pv2;
    float pv1_last_semi   = 1e9f;
    float pv2_last_semi   = 1e9f;
#endif
};

// ── Helpers ────────────────────────────────────────────────────────────────
static inline float ctl(const Megalo* p, Port port) noexcept {
    return *p->ctl[port - 2];
}

static inline double semi_to_ratio(float semi) noexcept {
    return std::pow(2.0, static_cast<double>(semi) / 12.0);
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
#ifdef MEGALO_PHASE_VOCODER
    p->pv1.init(rate);
    p->pv2.init(rate);
#endif
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
    p->granular.reset();
    p->filter.reset();
    p->envelope.reset();
    p->pos[0] = p->pos[1] = 0.0;
    p->lfo_phase = 0.0;
    p->cached_ftype = p->cached_cutoff = p->cached_q = -1.0f;
#ifdef MEGALO_PHASE_VOCODER
    p->pv1.reset();
    p->pv2.reset();
    p->pv1_last_semi = p->pv2_last_semi = 1e9f;
#endif
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    Megalo* p = static_cast<Megalo*>(handle);

    const float* in = p->audio_in;
    float*      out = p->audio_out;
    const float  sr = static_cast<float>(p->sample_rate);

    // ── Snapshot controls (block boundary) ────────────────────────────────
    const float threshold      = std::clamp(ctl(p, P_THRESHOLD),      0.0f,   1.0f);
    const int   sample_ms      = std::clamp(static_cast<int>(ctl(p, P_SAMPLE_MS)), 50, 500);
    const float blend          = std::clamp(ctl(p, P_BLEND),          0.0f,   1.0f);
    const float detune_ct      = std::clamp(ctl(p, P_DETUNE_CT),      0.0f,  50.0f);
    const float chorus_rate    = std::clamp(ctl(p, P_CHORUS_RATE),    0.1f,   8.0f);
    const float p1_semi        =            ctl(p, P_PITCH1_SEMI);
    const float p1_lvl         = std::clamp(ctl(p, P_PITCH1_LVL),     0.0f,  1.0f);
    const float p2_semi        =            ctl(p, P_PITCH2_SEMI);
    const float p2_lvl         = std::clamp(ctl(p, P_PITCH2_LVL),     0.0f,  1.0f);
    const float filt_type      =            ctl(p, P_FILT_TYPE);
    const float filt_cutoff    = std::clamp(ctl(p, P_FILT_CUTOFF),   20.0f, sr * 0.499f);
    const float filt_q         = std::clamp(ctl(p, P_FILT_Q),         0.1f,  10.0f);
    const float env_atk        = std::clamp(ctl(p, P_ENV_ATK),        0.0f,  5000.0f);
    const float env_dcy        = std::clamp(ctl(p, P_ENV_DCY),        0.0f,  5000.0f);
    const float env_sus        = std::clamp(ctl(p, P_ENV_SUS),        0.0f,   1.0f);
    const float env_rel        = std::clamp(ctl(p, P_ENV_REL),        0.0f, 10000.0f);
    const int   attack_skip_ms  = std::clamp(static_cast<int>(ctl(p, P_ATTACK_SKIP_MS)), 0, 100);
    const int   grain_ms        = std::clamp(static_cast<int>(ctl(p, P_GRAIN_MS)),      20, 200);
    const int   grain_samples   = std::clamp((int)(sr * grain_ms * 0.001f), 64, FREEZE_MAX_SAMPLES);
    const int   xfade_ms        = std::clamp(static_cast<int>(ctl(p, P_XFADE_MS)),       5, 100);
    const float grain_scatter   = std::clamp(ctl(p, P_GRAIN_SCATTER),                 0.0f, 1.0f);
    const int   retrigger_ms    = std::clamp(static_cast<int>(ctl(p, P_RETRIGGER_MS)), 50, 1000);

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

#ifdef MEGALO_PHASE_VOCODER
    const double lfo_now   = std::sin(2.0 * M_PI * p->lfo_phase);
    const float  pv1_semi  = p1_semi + static_cast<float>(detune_ct * lfo_now / 100.0);
    if (pv1_semi != p->pv1_last_semi) {
        p->pv1.set_pitch(pv1_semi);
        p->pv1_last_semi = pv1_semi;
    }
    if (p2_semi != p->pv2_last_semi) {
        p->pv2.set_pitch(p2_semi);
        p->pv2_last_semi = p2_semi;
    }
#endif

    const double lfo_inc   = static_cast<double>(chorus_rate) / p->sample_rate;
    const double speed_v2  = semi_to_ratio(p2_semi);

    // ── Sample loop ────────────────────────────────────────────────────────
    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        const FreezeEvent evt = p->freeze.process(x, threshold, sample_ms, attack_skip_ms,
                                                   xfade_ms, retrigger_ms);

        if (evt == FreezeEvent::Onset) {
            // Begin the skip/record phase — fade out the current loop.
            p->envelope.release();
#ifdef MEGALO_PHASE_VOCODER
            p->pv1.reset();
            p->pv2.reset();
#endif
        } else if (evt == FreezeEvent::LoopReady) {
            // New loop is captured — reset playback state and attack.
            p->granular.reset();
            p->pos[0] = p->pos[1] = 0.0;
            p->envelope.trigger();
        }

        // LFO
        const double lfo = std::sin(2.0 * M_PI * p->lfo_phase);
        p->lfo_phase += lfo_inc;
        if (p->lfo_phase >= 1.0) p->lfo_phase -= 1.0;

        const float*   ldata = p->freeze.loop_data();
        const int      llen  = p->freeze.loop_len();

        float v0, v1, v2;

#ifdef MEGALO_PHASE_VOCODER
        v0 = p->granular.process(ldata, llen, grain_samples, grain_scatter);
        v1 = (llen > 0) ? p->pv1.process(ldata, llen) : 0.0f;
        v2 = (llen > 0) ? p->pv2.process(ldata, llen) : 0.0f;
#else
        const double detune_ratio = std::pow(2.0, static_cast<double>(detune_ct) * lfo / 1200.0);
        const double speed_v1     = semi_to_ratio(p1_semi) * detune_ratio;
        v0 = p->granular.process(ldata, llen, grain_samples, grain_scatter);
        v1 = p->freeze.read(speed_v1, p->pos[0]);
        v2 = p->freeze.read(speed_v2, p->pos[1]);
#endif

        float freeze_sig = v0 + v1 * p1_lvl + v2 * p2_lvl;
        freeze_sig  = p->filter.process(freeze_sig);
        freeze_sig *= p->envelope.process();

        out[i] = x * (1.0f - blend) + freeze_sig * blend;
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
