// Drives the real megalo_dsp core: trigger one freeze, then measure the wet
// output amplitude envelope for different env_attack / sustain settings to
// confirm the ADSR actually shapes the HN-resynthesized pad.
#include "megalo_dsp.h"
#include <cstdio>
#include <cmath>
#include <vector>

static constexpr float SR = 48000.0f;

static MegaloParams P(float atk, float dcy, float sus, float rel) {
    MegaloParams p{};
    p.onset_threshold=0.2f; p.sample_ms=120; p.attack_skip_ms=0; p.blend=1.0f;
    p.grain_size_ms=100; p.grain_xfade_ms=40; p.base_pitch=0;
    p.pitch1_semi=-12; p.pitch1_level=0; p.pitch2_semi=12; p.pitch2_level=0;
    p.detune_cents=0; p.chorus_rate=0.5f; p.detune_blend=0;
    p.filter_type=0; p.filter_cutoff=18000; p.filter_q=0.7f;
    p.env_attack=atk; p.env_decay=dcy; p.env_sustain=sus; p.env_release=rel;
    p.detune_enable=0; p.pitch1_enable=0; p.pitch2_enable=0; p.pitch_mode=0;
    return p;
}

// A pitched test tone (E3 + harmonics).
static float tone(int n) {
    float t=n/SR, v=0; for(int k=1;k<=6;++k) v+=(1.0f/k)*std::sin(2*M_PI*164.8f*k*t);
    return 0.25f*v;
}

static void run(const char* label, MegaloParams pp) {
    MegaloDsp* d = megalo_dsp_new(SR);
    const int N=(int)(SR*2.5);
    std::vector<float> in(N,0.0f), out(N,0.0f);
    // 0.3s silence, then sustained tone (onset at 0.3s).
    for(int i=0;i<N;++i) in[i] = (i> (int)(0.3f*SR)) ? tone(i) : 0.0f;
    const int B=64;
    for(int i=0;i<N;i+=B){ int n=std::min(B,N-i); megalo_dsp_process(d,&pp,in.data()+i,out.data()+i,n); megalo_dsp_flush_analysis(d); }
    // Wet envelope = |out| since blend=1 (dry is the same tone, but measure post-onset rise).
    // Report wet RMS in 50ms windows after the loop is ready (~0.5s in).
    printf("%-26s wet RMS @ t= ", label);
    for(float t=0.45f;t<=2.4f;t+=0.25f){
        int s=(int)(t*SR); double ss=0; for(int j=0;j<(int)(0.05f*SR);++j) ss+=out[s+j]*out[s+j];
        printf("%.3f ", std::sqrt(ss/(0.05f*SR)));
    }
    printf("\n");
    megalo_dsp_free(d);
}

int main(){
    printf("t (s):                          0.45 0.70 0.95 1.20 1.45 1.70 1.95 2.20\n");
    run("attack=5ms  sustain=1.0",  P(5,    200, 1.0f, 1000));
    run("attack=1500ms sustain=1.0", P(1500, 200, 1.0f, 1000));
    run("attack=5ms  decay=300 sus=0.2", P(5, 300, 0.2f, 1000));
    return 0;
}
