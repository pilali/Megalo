// Phase-vocoder quality gate: freeze-loop passthrough and shifts must
// reconstruct the target frequency at (near-)full level with a stable
// envelope. Catches the historical 2π instantaneous-frequency bug (output
// was spectral mush at every setting) and lobe-placement regressions.
//
// Build & run:
//   g++ -O2 -std=c++17 -DMEGALO_PV_N=2048 -Isrc tools/pv_test.cpp
#include "phase_vocoder.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
static constexpr float SR=48000.0f;
static float goertzel(const float* x,int n,float f){
    const float w=2*M_PI*f/SR,c=2*std::cos(w);float s1=0,s2=0;
    for(int i=0;i<n;++i){float s0=x[i]+c*s1-s2;s2=s1;s1=s0;}
    return std::sqrt(std::max(0.0f,s1*s1+s2*s2-c*s1*s2))*2.0f/n;
}
int main(){
    bool fail = false;
    const int LEN=7200;                       // 33 périodes exactes de 220 Hz
    std::vector<float> loop(LEN);
    for(int i=0;i<LEN;++i) loop[i]=0.4f*std::sin(2*M_PI*220.0/48000.0*i);
    for(float semi : {0.0f, 7.0f, 12.0f, -12.0f}){
        PhaseVocoder pv; pv.init(SR); pv.set_pitch(semi);
        const int M=96000; std::vector<float> out(M);
        for(int i=0;i<M;++i) out[i]=pv.process(loop.data(),LEN);
        const float ft=220.0f*std::pow(2.0f,semi/12.0f);
        // niveau à la cible + ripple RMS 10ms sur la fin
        float lo=1e9f,hi=0;
        for(int s=48000;s<M-480;s+=480){
            double ss=0;for(int j=0;j<480;++j)ss+=(double)out[s+j]*out[s+j];
            float r=std::sqrt(ss/480); lo=std::min(lo,r); hi=std::max(hi,r);
        }
        const float lvl = goertzel(out.data()+48000,24000,ft);
        const float rip = 20*std::log10(hi/std::max(lo,1e-9f));
        const bool bad = (lvl < 0.25f) || (rip > 3.0f);
        printf("semi %+5.0f: |target %.1fHz|=%.3f (expected 0.40)  ripple=%.1f dB  %s\n",
               semi, ft, lvl, rip, bad ? "FAIL" : "ok");
        if (bad) fail = true;
    }
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail ? 1 : 0;
}
