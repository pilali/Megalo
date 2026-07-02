// Chord-crossfade check for MegaloHN's release bank.
//
// Plucks a 220 Hz tone, then a 330 Hz tone 1.2 s later. After the second
// capture the OLD pad must keep releasing (220 Hz audible and decaying on
// the user's release time) while the new pad rises (330 Hz) — the release
// bank behavior. Before it existed, the outgoing pad was muted in 15 ms at
// every onset, audible as a truncation clack.
//
// Build & run:
//   g++ -O2 -std=c++17 -DMEGALO_HN_SYNTH -pthread -Isrc \
//       tools/release_test.cpp src/megaloHN_dsp.cpp
//
// Exits non-zero when the old pad's tail is missing or fails to decay, or
// when the new pad does not rise.
#include "megalo_dsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static constexpr float SR = 48000.0f;

static float goertzel(const float* x, int n, float f)
{
    const float w = 2.0f * (float)M_PI * f / SR, c = 2.0f * std::cos(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; ++i) { const float s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(std::max(0.0f, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2.0f / n;
}

int main()
{
    MegaloParams p{};
    p.onset_threshold = 0.2f;  p.sample_ms = 150; p.attack_skip_ms = 30;
    p.blend = 1.0f;            p.grain_size_ms = 100; p.grain_xfade_ms = 40;
    p.pitch1_semi = -12;       p.pitch2_semi = 12;    p.chorus_rate = 0.5f;
    p.filter_type = 0;         p.filter_cutoff = 18000; p.filter_q = 0.7f;
    p.env_attack = 100;        p.env_decay = 200;     p.env_sustain = 0.8f;
    p.env_release = 2000;      p.dry_level = 0.0f;    // wet path only

    MegaloDsp* d = megalo_dsp_new(SR);
    const int N = (int)(SR * 4.0);
    std::vector<float> in(N), out(N);
    auto pluck = [&](int i, int s, float f) -> float {
        if (i < s) return 0.0f;
        const float t = (i - s) / SR;
        return (float)((t < 0.002f ? t / 0.002f : 1.0f) *
                       std::exp(-t / 0.5f) * 0.5f *
                       std::sin(2.0 * M_PI * f * (i / SR)));
    };
    for (int i = 0; i < N; ++i)
        in[i] = pluck(i, (int)(0.3f * SR), 220.0f) + pluck(i, (int)(1.5f * SR), 330.0f);
    for (int i = 0; i < N; i += 64) {
        const int n = std::min(64, N - i);
        megalo_dsp_process(d, &p, in.data() + i, out.data() + i, n);
        megalo_dsp_flush_analysis(d);
    }

    auto level = [&](float t, float f) {
        return goertzel(out.data() + (int)(t * SR), (int)(0.1f * SR), f);
    };
    const float old_early = level(1.95f, 220.0f);   // just after capture 2
    const float old_late  = level(3.60f, 220.0f);   // deep into the release
    const float new_pad   = level(2.20f, 330.0f);

    printf("old pad  220 Hz: %.4f @1.95s  ->  %.4f @3.60s\n", old_early, old_late);
    printf("new pad  330 Hz: %.4f @2.20s\n", new_pad);

    bool ok = true;
    if (old_early < 0.05f) { printf("FAIL: old pad truncated at capture\n"); ok = false; }
    if (old_late > 0.6f * old_early) { printf("FAIL: old pad not decaying\n"); ok = false; }
    if (new_pad < 0.15f) { printf("FAIL: new pad missing\n"); ok = false; }
    megalo_dsp_free(d);
    printf(ok ? "PASS\n" : "");
    return ok ? 0 : 1;
}
