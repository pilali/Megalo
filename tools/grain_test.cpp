// Amplitude-ripple (pumping) meter for the granular pad.
//
// Freezes a pure 220 Hz tone and measures the min/max RMS of the wet output
// over the steady zone, across several grain-size / crossfade settings.
// Historical bugs caught by this harness: the out-of-phase seam crossfade
// (a baked ~20 ms cancellation notch → 24–30 dB pumping) and the
// non-constant grain-envelope sum.
//
// Build & run:
//   g++ -O2 -std=c++17 -Isrc tools/grain_test.cpp src/megalo_dsp.cpp
//
// Exits non-zero when any configuration pumps by more than PASS_DB.
#include "megalo_dsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static constexpr float SR      = 48000.0f;
static constexpr float PASS_DB = 8.0f;

static float run(int grain_ms, int xfade_ms)
{
    MegaloParams p{};
    p.onset_threshold = 0.2f;  p.sample_ms = 150; p.attack_skip_ms = 0;
    p.blend = 1.0f;            p.grain_size_ms = (float)grain_ms;
    p.grain_xfade_ms = (float)xfade_ms;
    p.pitch1_semi = -12;       p.pitch2_semi = 12;    p.chorus_rate = 0.5f;
    p.filter_type = 0;         p.filter_cutoff = 18000; p.filter_q = 0.7f;
    p.env_attack = 5;          p.env_decay = 0;       p.env_sustain = 1.0f;
    p.env_release = 500;

    MegaloDsp* d = megalo_dsp_new(SR);
    const int N = (int)(SR * 3.0);
    std::vector<float> in(N, 0.0f), out(N, 0.0f);
    for (int i = (int)(0.3f * SR); i < N; ++i)
        in[i] = 0.4f * std::sin(2.0 * M_PI * 220.0 * (i / SR));
    for (int i = 0; i < N; i += 64) {
        const int n = std::min(64, N - i);
        megalo_dsp_process(d, &p, in.data() + i, out.data() + i, n);
        megalo_dsp_flush_analysis(d);
    }

    // RMS over 5 ms windows in the steady zone.
    float lo = 1e9f, hi = 0.0f;
    for (float t = 1.5f; t < 2.9f; t += 0.005f) {
        const int s = (int)(t * SR), w = (int)(0.005f * SR);
        double ss = 0.0;
        for (int j = 0; j < w; ++j) ss += (double)out[s + j] * out[s + j];
        const float r = std::sqrt(ss / w);
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    const float db = 20.0f * std::log10(hi / std::max(lo, 1e-9f));
    printf("grain=%3d ms xfade=%3d ms   rms[%.3f..%.3f]  ripple=%.2f dB\n",
           grain_ms, xfade_ms, lo, hi, db);
    megalo_dsp_free(d);
    return db;
}

int main()
{
    float worst = 0.0f;
    worst = std::max(worst, run(100, 10));
    worst = std::max(worst, run(100, 40));
    worst = std::max(worst, run(50, 5));
    worst = std::max(worst, run(200, 100));
    if (worst > PASS_DB) {
        printf("FAIL: ripple %.2f dB > %.1f dB\n", worst, PASS_DB);
        return 1;
    }
    printf("PASS (worst ripple %.2f dB <= %.1f dB)\n", worst, PASS_DB);
    return 0;
}
