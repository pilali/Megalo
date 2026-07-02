// Click detector for the dry→wet hand-over (both cores).
//
// Drives the real DSP core with a tone whose amplitude swells smoothly (2 ms
// rise — NO discontinuity in the input) to trigger two captures. Any output
// sample-to-sample step far above the input slope is therefore a click made
// by the plugin. Historical bugs caught by this harness: the instantaneous
// xfade jump at onset (MegaloHN, 70× the input slope) and the comp_level /
// envelope truncation step at LoopReady (Megalo, 6×).
//
// Build & run (audit target does this for both cores):
//   g++ -O2 -std=c++17 -Isrc tools/click_test.cpp src/megalo_dsp.cpp
//   g++ -O2 -std=c++17 -DMEGALO_HN_SYNTH -pthread -Isrc \
//       tools/click_test.cpp src/megaloHN_dsp.cpp
//
// Exits non-zero when the largest step exceeds PASS_RATIO × the input slope.
#include "megalo_dsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static constexpr float SR         = 48000.0f;
static constexpr float PASS_RATIO = 3.0f;

int main()
{
    MegaloParams p{};
    p.onset_threshold = 0.2f;  p.sample_ms = 150; p.attack_skip_ms = 30;
    p.blend = 0.8f;            p.grain_size_ms = 100; p.grain_xfade_ms = 40;
    p.pitch1_semi = -12;       p.pitch2_semi = 12;    p.chorus_rate = 0.5f;
    p.filter_type = 0;         p.filter_cutoff = 18000; p.filter_q = 0.7f;
    p.env_attack = 100;        p.env_decay = 200;     p.env_sustain = 0.8f;
    p.env_release = 2000;      p.dry_level = 1.0f;

    MegaloDsp* d = megalo_dsp_new(SR);
    const int N = (int)(SR * 3.0);
    std::vector<float> in(N), out(N);

    // Pluck-like pulses: 2 ms smooth rise, ~250 ms decay back to the bed
    // level, so the SECOND pulse re-triggers the onset detector too.
    auto pulse = [&](int i, int s) -> float {
        if (i < s) return 0.0f;
        const float t    = (i - s) / SR;
        const float rise = t < 0.002f ? t / 0.002f : 1.0f;
        return rise * std::exp(-t / 0.25f);
    };
    for (int i = 0; i < N; ++i) {
        const float a = 0.12f + 0.5f * (pulse(i, (int)(0.3f * SR)) +
                                        pulse(i, (int)(1.5f * SR)));
        in[i] = a * std::sin(2.0 * M_PI * 220.0 * (i / SR));
    }

    for (int i = 0; i < N; i += 64) {
        const int n = std::min(64, N - i);
        megalo_dsp_process(d, &p, in.data() + i, out.data() + i, n);
        megalo_dsp_flush_analysis(d);   // offline determinism (HN worker)
    }

    float din = 0.0f;
    for (int i = 1; i < N; ++i) din = std::max(din, std::abs(in[i] - in[i - 1]));

    // Top steps at least 5 ms apart, skipping the initial silence.
    std::vector<std::pair<float, int>> steps;
    for (int i = (int)(0.05f * SR); i < N; ++i)
        steps.push_back({ std::abs(out[i] - out[i - 1]), i });
    std::sort(steps.rbegin(), steps.rend());

    printf("max|dIn|=%.4f  top steps: ", din);
    std::vector<int> used;
    float worst_ratio = 0.0f;
    int shown = 0;
    for (auto& s : steps) {
        bool near = false;
        for (int u : used)
            if (std::abs(s.second - u) < (int)(0.005f * SR)) near = true;
        if (near) continue;
        const float ratio = s.first / din;
        worst_ratio = std::max(worst_ratio, ratio);
        printf("%.4f@%.3fs(x%.1f) ", s.first, s.second / SR, ratio);
        used.push_back(s.second);
        if (++shown >= 5) break;
    }
    printf("\n");
    megalo_dsp_free(d);

    if (worst_ratio > PASS_RATIO) {
        printf("FAIL: step ratio %.1f > %.1f\n", worst_ratio, PASS_RATIO);
        return 1;
    }
    printf("PASS (ratio %.1f <= %.1f)\n", worst_ratio, PASS_RATIO);
    return 0;
}
