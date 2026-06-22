// Analyze a window of real audio, then resynthesize the detected chord as a
// sustained polyphonic H+N pad — so the whole analysis→resynthesis chain can
// be heard. Writes raw float32 mono (encode to taste with ffmpeg).
//
//   g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 \
//       tools/hn_render.cpp -o /tmp/hn_render
//   /tmp/hn_render audio.f32 t0 out.f32 [dur_s] [win_ms]

#include "hn_multif0.hpp"
#include "hn_nnls.hpp"
#include "hn_poly_synth.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <string>

static constexpr float SR = 48000.0f;

static std::string note_name(float f)
{
    const char* N[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const float mf = 69.0f + 12.0f * std::log2(f / 440.0f);
    const int   m  = (int)std::lround(mf);
    char b[16]; snprintf(b, sizeof b, "%s%d", N[(m % 12 + 12) % 12], m / 12 - 1);
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr,"usage: %s in.f32 t0 out.f32 [dur_s] [win_ms]\n",argv[0]); return 1; }
    const double t0     = atof(argv[2]);
    const float  dur    = (argc >= 5) ? atof(argv[4]) : 4.0f;
    const int    win_ms = (argc >= 6) ? atoi(argv[5]) : 400;
    const int    win    = (int)(SR * win_ms * 0.001f);

    FILE* f = fopen(argv[1],"rb");
    if (!f) { perror("open"); return 1; }
    fseek(f,0,SEEK_END); long bytes=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<float> au(bytes/sizeof(float));
    if (fread(au.data(),sizeof(float),au.size(),f)!=au.size()){fprintf(stderr,"read\n");return 1;}
    fclose(f);

    const long off = (long)(t0 * SR);
    if (off < 0 || off + win > (long)au.size()) { fprintf(stderr,"t0 out of range\n"); return 1; }

    MultiHNState st = hn_multif0_analyze(au.data()+off, win, SR);
    printf("t=%.1fs  %d notes:", t0, st.n_notes);
    for (int i=0;i<st.n_notes;++i)
        printf("  %s(%.1f, %.0f%%)", note_name(st.notes[i].f0).c_str(),
               st.notes[i].f0, 100.0f*st.notes[i].confidence);
    printf("\n");

    PolyAdditiveSynth poly;
    poly.reset(st, SR);

    const int n = (int)(SR * dur);
    std::vector<float> out(n);
    const int fade = (int)(SR * 0.02f);            // 20 ms anti-click ramps
    float peak = 1e-9f;
    for (int i=0;i<n;++i) { float v=poly.process(); out[i]=v; peak=std::max(peak,std::fabs(v)); }
    const float g = 0.9f / peak;                   // normalize for listening
    for (int i=0;i<n;++i) {
        float env = 1.0f;
        if (i < fade)        env = (float)i / fade;
        else if (i > n-fade) env = (float)(n-i) / fade;
        out[i] *= g * env;
    }

    FILE* o = fopen(argv[3],"wb");
    fwrite(out.data(), sizeof(float), out.size(), o);
    fclose(o);
    printf("wrote %s (%.1fs)\n", argv[3], dur);
    return 0;
}
