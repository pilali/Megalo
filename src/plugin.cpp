// Thin LV2 wrapper around the host-agnostic DSP core (megalo_dsp.{h,cpp}).
//
// All processing lives in megalo_dsp.cpp. This file only:
//   - declares the LV2 port indices,
//   - holds port pointers + a MegaloDsp* instance,
//   - maps the connected control ports into a MegaloParams on each run().
#include <lv2/core/lv2.h>
#include <array>
#include <cstdint>
#include <new>

#include "megalo_dsp.h"

// Distinct URI per build so the two plugins cohabit: the granular Megalo and
// the polyphonic MegaloHN.
#ifdef MEGALO_HN_SYNTH
static constexpr char MEGALO_URI[] = "https://github.com/pilali/megalo/hn";
#else
static constexpr char MEGALO_URI[] = "https://github.com/pilali/megalo";
#endif

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
    P_DETUNE_EN      = 23,   // detune on/off        [0, 1]
    P_PITCH1_EN      = 24,   // voice-1 on/off       [0, 1]
    P_PITCH2_EN      = 25,   // voice-2 on/off       [0, 1]
    P_TRIGGER_OUT    = 26,   // momentary onset pulse [0, 1] — output for GUI flash
    P_AUDIO_OUT_R    = 27,   // optional right output — connected ⇒ stereo path
    P_DRY_LEVEL      = 28,   // dry-fill gain [0 – 2] — dry→wet crossfade level
#ifdef MEGALO_HN_SYNTH
    P_HN_BRIGHTNESS  = 29,   // H+N timbre: spectral tilt   [-1 – 1]
    P_HN_DAMPING     = 30,   // H+N timbre: high roll-off   [0 – 1]
    P_HN_EVEN_ODD    = 31,   // H+N timbre: even-harmonic   [-1 – 1]
    P_HN_NOISE       = 32,   // H+N timbre: noise / air     [0 – 1]
    P_HN_WIDTH       = 33,   // H+N stereo width            [0 – 1]
    P_COUNT          = 34
#else
    P_COUNT          = 29
#endif
};

// ctl[] slots cover every port up to P_COUNT (index port-2). The two output
// ports (trigger, audio_out_r) leave unused holes and are stored separately.
static constexpr uint32_t N_CTL = P_COUNT - 2;

// ── Plugin instance ────────────────────────────────────────────────────────
struct MegaloLV2 {
    MegaloDsp* dsp = nullptr;

    const float* audio_in     = nullptr;
    float*       audio_out    = nullptr;   // left / mono output
    float*       audio_out_r  = nullptr;   // right output (NULL when host runs us mono)
    std::array<const float*, N_CTL> ctl = {};
    float*       trigger_out  = nullptr;
};

static inline float ctl(const MegaloLV2* p, Port port) noexcept {
    const float* ptr = p->ctl[port - 2];
    return ptr ? *ptr : 0.0f;
}

// ── LV2 callbacks ──────────────────────────────────────────────────────────
static LV2_Handle instantiate(const LV2_Descriptor*,
                              double rate,
                              const char*,
                              const LV2_Feature* const*)
{
    MegaloLV2* p = new (std::nothrow) MegaloLV2();
    if (!p) return nullptr;
    p->dsp = megalo_dsp_new(rate);
    if (!p->dsp) { delete p; return nullptr; }
    return p;
}

static void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    MegaloLV2* p = static_cast<MegaloLV2*>(handle);
    if (port == P_AUDIO_IN)
        p->audio_in = static_cast<const float*>(data);
    else if (port == P_AUDIO_OUT)
        p->audio_out = static_cast<float*>(data);
    else if (port == P_TRIGGER_OUT)
        p->trigger_out = static_cast<float*>(data);
    else if (port == P_AUDIO_OUT_R)
        p->audio_out_r = static_cast<float*>(data);
    else if (port >= 2 && port < P_TRIGGER_OUT)        // control inputs 2..25
        p->ctl[port - 2] = static_cast<const float*>(data);
    else if (port > P_AUDIO_OUT_R && port < P_COUNT)   // dry_level (28) + timbre (29..)
        p->ctl[port - 2] = static_cast<const float*>(data);
}

static void activate(LV2_Handle handle)
{
    MegaloLV2* p = static_cast<MegaloLV2*>(handle);
    megalo_dsp_reset(p->dsp);
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    MegaloLV2* p = static_cast<MegaloLV2*>(handle);

    const MegaloParams params {
        ctl(p, P_THRESHOLD),
        ctl(p, P_SAMPLE_MS),
        ctl(p, P_ATTACK_SKIP),
        ctl(p, P_BLEND),
        ctl(p, P_GRAIN_MS),
        ctl(p, P_GRAIN_XFADE_MS),
        ctl(p, P_BASE_PITCH),
        ctl(p, P_PITCH1_SEMI),
        ctl(p, P_PITCH1_LVL),
        ctl(p, P_PITCH2_SEMI),
        ctl(p, P_PITCH2_LVL),
        ctl(p, P_DETUNE_CT),
        ctl(p, P_CHORUS_RATE),
        ctl(p, P_DETUNE_BLEND),
        ctl(p, P_FILT_TYPE),
        ctl(p, P_FILT_CUTOFF),
        ctl(p, P_FILT_Q),
        ctl(p, P_ENV_ATK),
        ctl(p, P_ENV_DCY),
        ctl(p, P_ENV_SUS),
        ctl(p, P_ENV_REL),
        ctl(p, P_DETUNE_EN),
        ctl(p, P_PITCH1_EN),
        ctl(p, P_PITCH2_EN),
        // pitch_mode: fixed per build (no LV2 port). Matches the legacy
        // behaviour — phase vocoder only on builds that compiled it in.
#ifdef MEGALO_PHASE_VOCODER
        1.0f,
#else
        0.0f,
#endif
        ctl(p, P_DRY_LEVEL),
#ifdef MEGALO_HN_SYNTH
        ctl(p, P_HN_BRIGHTNESS),
        ctl(p, P_HN_DAMPING),
        ctl(p, P_HN_EVEN_ODD),
        ctl(p, P_HN_NOISE),
        ctl(p, P_HN_WIDTH),
#else
        // Stock Megalo: the H+N timbre fields exist in the shared struct but
        // are unused by the granular core. Zero them to keep -Wextra quiet.
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
#endif
    };

    // The right output is an optional port: when the host connects it we run
    // the decorrelated stereo path, otherwise the original mono path.
    if (p->audio_out_r)
        megalo_dsp_process_stereo(p->dsp, &params, p->audio_in,
                                  p->audio_out, p->audio_out_r, n_samples);
    else
        megalo_dsp_process(p->dsp, &params, p->audio_in, p->audio_out, n_samples);

    if (p->trigger_out) *p->trigger_out = megalo_dsp_trigger(p->dsp);
}

static void cleanup(LV2_Handle handle)
{
    MegaloLV2* p = static_cast<MegaloLV2*>(handle);
    megalo_dsp_free(p->dsp);
    delete p;
}

static const LV2_Descriptor descriptor = {
    MEGALO_URI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
