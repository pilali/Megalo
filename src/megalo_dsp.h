#ifndef MEGALO_DSP_H
#define MEGALO_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {           /* indispensable : JUCE est du C++ */
#endif

/* One value per control port, copied from LV2 ports / JUCE parameters.
   Raw values are accepted: the clamp happens inside megalo_dsp_process(),
   exactly as the LV2 run() used to do (same bounds, same order). Field
   names mirror the .ttl symbols (idx 2..25). */
typedef struct {
    float onset_threshold;   /* idx 2  [0 – 1]            */
    float sample_ms;         /* idx 3  [50 – 500]   ms    */
    float attack_skip_ms;    /* idx 4  [0 – 500]    ms    */
    float blend;             /* idx 5  [0 – 1]            */
    float grain_size_ms;     /* idx 6  [5 – 200]    ms    */
    float grain_xfade_ms;    /* idx 7  [5 – 100]    ms    */
    float base_pitch;        /* idx 8  [-12 – 12]   semi  */
    float pitch1_semi;       /* idx 9  [-24 – 24]   semi  */
    float pitch1_level;      /* idx 10 [0 – 1]            */
    float pitch2_semi;       /* idx 11 [-24 – 24]   semi  */
    float pitch2_level;      /* idx 12 [0 – 1]            */
    float detune_cents;      /* idx 13 [0 – 50]     cents */
    float chorus_rate;       /* idx 14 [0.1 – 8]    Hz    */
    float detune_blend;      /* idx 15 [0 – 1]            */
    float filter_type;       /* idx 16 0=LP 1=HP 2=BP     */
    float filter_cutoff;     /* idx 17 [20 – 20000] Hz    */
    float filter_q;          /* idx 18 [0.1 – 10]         */
    float env_attack;        /* idx 19 [0 – 5000]   ms    */
    float env_decay;         /* idx 20 [0 – 5000]   ms    */
    float env_sustain;       /* idx 21 [0 – 1]            */
    float env_release;       /* idx 22 [0 – 10000]  ms    */
    float detune_enable;     /* idx 23 toggled (>=0.5)    */
    float pitch1_enable;     /* idx 24 toggled (>=0.5)    */
    float pitch2_enable;     /* idx 25 toggled (>=0.5)    */
    float pitch_mode;        /* pitch engine: <0.5 = granular, >=0.5 = phase   */
                             /* vocoder. Only honoured when the core is built  */
                             /* with MEGALO_PHASE_VOCODER (else always granular).*/
} MegaloParams;

typedef struct MegaloDsp MegaloDsp;          /* opaque state */

MegaloDsp* megalo_dsp_new(double sample_rate);
void       megalo_dsp_free(MegaloDsp*);
void       megalo_dsp_reset(MegaloDsp*);     /* = activate() : clears filters/state */

/* Process n mono samples. in may equal out (in-place is fine). */
void       megalo_dsp_process(MegaloDsp*, const MegaloParams*,
                              const float* in, float* out, uint32_t n);

/* Process n samples from a mono input into a decorrelated stereo pair.
   The dry signal stays centred (mono-compatible); the granular wet is
   decorrelated between channels (independent grain randomisation,
   anti-phase detune LFO, micro-offset filter cutoff) for stereo width.
   outL and outR must be distinct buffers, both different from in. */
void       megalo_dsp_process_stereo(MegaloDsp*, const MegaloParams*,
                                     const float* in, float* outL, float* outR,
                                     uint32_t n);

/* Onset-trigger pulse for the GUI (LV2 idx 26 trigger_pulse): returns the
   value computed by the most recent megalo_dsp_process() call — 1.0f while
   the post-onset hold window is active, 0.0f otherwise. */
float      megalo_dsp_trigger(const MegaloDsp*);

#ifdef __cplusplus
}
#endif

#endif /* MEGALO_DSP_H */
