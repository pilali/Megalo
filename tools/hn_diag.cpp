// Diagnostic: harmonic vs noise energy balance of the analyzer output.
#include "hn_multif0.hpp"
#include "hn_nnls.hpp"
#include "hn_poly_synth.hpp"
#include <cstdio>
#include <vector>
#include <cmath>

static constexpr float SR = 48000.0f;

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.f32 t0 [win_ms]\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb"); fseek(f,0,SEEK_END); long b=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<float> au(b/4); fread(au.data(),4,au.size(),f); fclose(f);
    const long off = (long)(atof(argv[2]) * SR);
    const int win = (int)(SR * (argc>=4?atoi(argv[3]):400) * 0.001f);

    MultiHNState st = hn_multif0_analyze(au.data()+off, win, SR);
    printf("notes=%d  noise_rms=%.4f\n", st.n_notes, st.noise_rms);

    double harm_pow = 0;
    for (int i=0;i<st.n_notes;++i) {
        double np=0; for (int k=0;k<st.notes[i].n_partials;++k) np += 0.5*st.notes[i].harm_amp[k]*st.notes[i].harm_amp[k];
        harm_pow += np;
        printf("  note %d f0=%.1f partials=%d  harmRMS=%.4f  maxAmp=%.4f\n",
               i, st.notes[i].f0, st.notes[i].n_partials, std::sqrt(np),
               [&]{float m=0;for(int k=0;k<st.notes[i].n_partials;++k)m=std::max(m,st.notes[i].harm_amp[k]);return m;}());
    }
    printf("TOTAL harmonic RMS=%.4f   noise contribution(=noise_rms*4)=%.4f\n",
           std::sqrt(harm_pow), st.noise_rms*4.0f);

    // Measure actual synth output, harmonics-only vs noise-only.
    PolyAdditiveSynth poly; poly.reset(st, SR);
    double full=0; const int N=(int)SR; for(int i=0;i<N;++i){float v=poly.process();full+=v*v;}
    MultiHNState noNoise=st; for(int i=0;i<noNoise.n_notes;++i) noNoise.notes[i].noise_rms=0;
    PolyAdditiveSynth ph; ph.reset(noNoise,SR);
    double harm=0; for(int i=0;i<N;++i){float v=ph.process();harm+=v*v;}
    printf("OUTPUT rms: full=%.4f  harmonics-only=%.4f  -> noise dominates=%s\n",
           std::sqrt(full/N), std::sqrt(harm/N),
           (std::sqrt(harm/N) < 0.3*std::sqrt(full/N)) ? "YES (bug)" : "no");
    return 0;
}
