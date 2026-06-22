// Run the polyphonic H+N analyzer on real audio (raw float32 mono).
//
// Decode first, e.g.:
//   ffmpeg -i input.m4a -ac 1 -ar 48000 -f f32le audio.f32
// Build & run:
//   g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 \
//       tools/hn_wav.cpp -o /tmp/hn_wav
//   /tmp/hn_wav audio.f32 [win_ms] [t0 t1 t2 ...]
//
// Prints, for each requested time offset (seconds), the detected notes as
// note names + cents deviation. Ground truth is unknown, so this is for eyeball
// validation against what the player knows was performed.

#include "hn_multif0.hpp"
#include "hn_nnls.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>

static constexpr float SR = 48000.0f;

static std::string note_name(float f)
{
    if (f <= 0) return "--";
    const char* N[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const float midi_f = 69.0f + 12.0f * std::log2(f / 440.0f);
    const int   midi   = (int)std::lround(midi_f);
    const float cents  = (midi_f - midi) * 100.0f;
    char buf[32];
    snprintf(buf, sizeof buf, "%s%d%+.0f", N[(midi % 12 + 12) % 12], midi / 12 - 1, cents);
    return buf;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s audio.f32 [win_ms] [t0 ...]\n", argv[0]); return 1; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<float> audio(bytes / sizeof(float));
    if (fread(audio.data(), sizeof(float), audio.size(), f) != audio.size()) { fprintf(stderr,"read\n"); return 1; }
    fclose(f);

    const int   win_ms = (argc >= 3) ? atoi(argv[2]) : 400;
    const int   win    = (int)(SR * win_ms * 0.001f);
    const double total = audio.size() / (double)SR;

    std::vector<double> times;
    if (argc >= 4) for (int i = 3; i < argc; ++i) times.push_back(atof(argv[i]));
    else for (double t = 1.0; t < total - 1.0; t += 2.0) times.push_back(t);  // grid

    printf("paloma — %.1fs, window %d ms, tier %d (FFT %d, max notes %d)\n\n",
           total, win_ms, MEGALO_HN_QUALITY, hnq::FFT_SIZE, hnq::MAX_NOTES);

    for (double t : times) {
        long off = (long)(t * SR);
        if (off < 0 || off + win > (long)audio.size()) continue;

        // Quick RMS so we can flag near-silent windows.
        double ss = 0; for (int i = 0; i < win; ++i) ss += (double)audio[off+i]*audio[off+i];
        const float rms = std::sqrt(ss / win);

        MultiHNState st = hn_multif0_analyze(audio.data() + off, win, SR);

        // Sort detected notes low→high, carrying confidence (relative energy).
        std::vector<std::pair<float,float>> det;   // (f0, confidence)
        for (int i = 0; i < st.n_notes; ++i)
            det.push_back({st.notes[i].f0, st.notes[i].confidence});
        std::sort(det.begin(), det.end());

        printf("t=%6.1fs  rms=%.3f  notes:", t, rms);
        if (det.empty()) printf("  (none)");
        for (auto& d : det)
            printf("  %-7s(%.0f%%)", note_name(d.first).c_str(), 100.0f * d.second);
        printf("\n");
    }
    return 0;
}
