#include <lv2/core/lv2.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <new>

#include "freeze_engine.hpp"
#include "biquad.hpp"
#include "envelope.hpp"

static constexpr char MEGALO_URI[] = "https://github.com/pilali/megalo";

// ── Port indices ───────────────────────────────────────────────────────────
enum Port : uint32_t {
    P_AUDIO_IN    =  0,
    P_AUDIO_OUT   =  1,
    // Freeze
    P_THRESHOLD   =  2,   // onset sensitivity  [0 – 1]
    P_SAMPLE_MS   =  3,   // capture length ms  [50 – 500]
    P_BLEND       =  4,   // dry/wet            [0 – 1]
    // Pitch / Detune
    P_DETUNE_CT   =  5,   // LFO detune cents   [0 – 50]
    P_CHORUS_RATE =  6,   // LFO rate Hz        [0.1 – 8]
    P_PITCH1_SEMI =  7,   // voice-1 semitones  [-24 – 24]
    P_PITCH1_LVL  =  8,   // voice-1 level      [0 – 1]
    P_PITCH2_SEMI =  9,   // voice-2 semitones  [-24 – 24]
    P_PITCH2_LVL  = 10,   // voice-2 level      [0 – 1]
    // Filter
    P_FILT_TYPE   = 11,   // 0=LP 1=HP 2=BP
    P_FILT_CUTOFF = 12,   // Hz                 [20 – 20000]
    P_FILT_Q      = 13,   // Q                  [0.1 – 10]
    // Envelope
    P_ENV_ATK     = 14,   // attack  ms         [0 – 5000]
    P_ENV_DCY     = 15,   // decay   ms         [0 – 5000]
    P_ENV_SUS     = 16,   // sustain level      [0 – 1]
    P_ENV_REL     = 17,   // release ms         [0 – 10000]
    P_COUNT       = 18
};

static constexpr uint32_t N_CTL = P_COUNT - 2;  // 16 control ports

// ── Plugin instance ────────────────────────────────────────────────────────
struct Megalo {
    double sample_rate;

    const float* audio_in  = nullptr;
    float*       audio_out = nullptr;
    std::array<const float*, N_CTL> ctl = {};   // ctl[i] ↔ port (i+2)

    FreezeEngine freeze;
    Biquad       filter;
    Envelope     envelope;

    // Independent playback positions for the three loop voices
    double pos[3] = {0.0, 0.0, 0.0};

    // Chorus LFO state
    double lfo_phase = 0.0;

    // Filter parameter cache — avoid recomputing coefficients every block
    float cached_ftype   = -1.0f;
    float cached_cutoff  = -1.0f;
    float cached_q       = -1.0f;
};

// ── Helpers ────────────────────────────────────────────────────────────────
static inline double semi_to_ratio(float semi) noexcept {
    return std::pow(2.0, (double)semi / 12.0);
}

static inline float ctl(const Megalo* p, Port port) noexcept {
    return *p->ctl[port - 2];
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
    p->filter.reset();
    p->envelope.reset();
    p->pos[0] = p->pos[1] = p->pos[2] = 0.0;
    p->lfo_phase = 0.0;
    p->cached_ftype = p->cached_cutoff = p->cached_q = -1.0f;
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    Megalo* p = static_cast<Megalo*>(handle);

    const float* in = p->audio_in;
    float*      out = p->audio_out;
    const float  sr = (float)p->sample_rate;

    // Snapshot controls at block boundary (LV2 control-rate convention)
    const float threshold   = std::clamp(ctl(p, P_THRESHOLD),   0.0f,   1.0f);
    const int   sample_ms   = std::clamp((int)ctl(p, P_SAMPLE_MS), 50, 500);
    const float blend       = std::clamp(ctl(p, P_BLEND),        0.0f,   1.0f);
    const float detune_ct   = std::clamp(ctl(p, P_DETUNE_CT),    0.0f,  50.0f);
    const float chorus_rate = std::clamp(ctl(p, P_CHORUS_RATE),  0.1f,   8.0f);
    const float p1_semi     =            ctl(p, P_PITCH1_SEMI);
    const float p1_lvl      = std::clamp(ctl(p, P_PITCH1_LVL),  0.0f,   1.0f);
    const float p2_semi     =            ctl(p, P_PITCH2_SEMI);
    const float p2_lvl      = std::clamp(ctl(p, P_PITCH2_LVL),  0.0f,   1.0f);
    const float filt_type   = ctl(p, P_FILT_TYPE);
    const float filt_cutoff = std::clamp(ctl(p, P_FILT_CUTOFF), 20.0f, sr * 0.499f);
    const float filt_q      = std::clamp(ctl(p, P_FILT_Q),      0.1f,  10.0f);
    const float env_atk     = std::clamp(ctl(p, P_ENV_ATK),     0.0f, 5000.0f);
    const float env_dcy     = std::clamp(ctl(p, P_ENV_DCY),     0.0f, 5000.0f);
    const float env_sus     = std::clamp(ctl(p, P_ENV_SUS),     0.0f,   1.0f);
    const float env_rel     = std::clamp(ctl(p, P_ENV_REL),     0.0f, 10000.0f);

    // Recompute filter only when parameters change
    if (filt_type   != p->cached_ftype  ||
        filt_cutoff != p->cached_cutoff ||
        filt_q      != p->cached_q)
    {
        Biquad::Type btype = (filt_type < 0.5f) ? Biquad::LP
                           : (filt_type < 1.5f) ? Biquad::HP
                           :                      Biquad::BP;
        p->filter.setup(btype, filt_cutoff, filt_q, sr);
        p->cached_ftype  = filt_type;
        p->cached_cutoff = filt_cutoff;
        p->cached_q      = filt_q;
    }

    p->envelope.set(env_atk, env_dcy, env_sus, env_rel, sr);

    const double lfo_inc  = (double)chorus_rate / p->sample_rate;
    const double speed_v2 = semi_to_ratio(p2_semi);

    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        // Onset detection — returns true on the triggering sample
        bool onset = p->freeze.process(x, threshold, sample_ms);
        if (onset) {
            // New note: release old freeze, reset readers, re-trigger envelope
            p->envelope.release();
            p->pos[0] = p->pos[1] = p->pos[2] = 0.0;
            p->envelope.trigger();
        }

        // LFO (sine, one phase accumulator)
        double lfo = std::sin(2.0 * M_PI * p->lfo_phase);
        p->lfo_phase += lfo_inc;
        if (p->lfo_phase >= 1.0) p->lfo_phase -= 1.0;

        // Voice speeds
        // v0: base freeze at unity pitch (always present)
        // v1: pitch_1_semi + LFO-modulated detune (chorus / vibrato layer)
        // v2: pitch_2_semi (static second harmony)
        double detune_ratio = std::pow(2.0, (double)detune_ct * lfo / 1200.0);
        double speed_v1 = semi_to_ratio(p1_semi) * detune_ratio;

        float v0 = p->freeze.read(1.0,      p->pos[0]);
        float v1 = p->freeze.read(speed_v1, p->pos[1]);
        float v2 = p->freeze.read(speed_v2, p->pos[2]);

        // Sum: base voice + additive harmonic layers
        float freeze_sig = v0 + v1 * p1_lvl + v2 * p2_lvl;

        // Filter → envelope
        freeze_sig  = p->filter.process(freeze_sig);
        freeze_sig *= p->envelope.process();

        // Dry/wet
        out[i] = x * (1.0f - blend) + freeze_sig * blend;
    }
}

static void cleanup(LV2_Handle handle)
{
    delete static_cast<Megalo*>(handle);
}

static const LV2_Descriptor descriptor = {
    MEGALO_URI,
    instantiate,
    connect_port,
    activate,
    run,
    nullptr,   // deactivate
    cleanup,
    nullptr    // extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
