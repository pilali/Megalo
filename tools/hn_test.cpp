// Offline test harness for the polyphonic H+N analyzer (hn_multif0_analyze).
//
// Synthesizes harmonic tones and chords — including notes down to A1 (55 Hz) —
// then measures how well the multi-F0 estimator recovers the fundamentals.
// This is the de-risking step: validate detection (hit rate, octave errors,
// cents accuracy) BEFORE wiring resynthesis into the plugin.
//
// Build & run:
//   g++ -std=c++17 -O3 -ffast-math -Isrc -DMEGALO_HN_QUALITY=2 \
//       tools/hn_test.cpp -o /tmp/hn_test && /tmp/hn_test

#include "hn_multif0.hpp"
#include "hn_nnls.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <random>
#include <algorithm>

static constexpr float SR = 48000.0f;

static float midi_to_freq(int midi) { return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f); }
static float cents(float a, float b) { return 1200.0f * std::log2(a / b); }

// Sum of harmonic tones (1/k amplitude, 8 partials) + optional white noise.
static std::vector<float> synth(const std::vector<float>& freqs, int len,
                                float noise_amp = 0.0f)
{
    std::vector<float> buf(len, 0.0f);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> ph(0.0f, 2.0f * float(M_PI));
    for (float f : freqs) {
        for (int k = 1; k <= 8; ++k) {
            const float fk = f * k;
            if (fk >= SR * 0.46f) break;
            const float amp = 1.0f / k;
            const float p0  = ph(rng);
            for (int i = 0; i < len; ++i)
                buf[i] += amp * std::sin(2.0f * float(M_PI) * fk * i / SR + p0);
        }
    }
    if (noise_amp > 0.0f) {
        std::normal_distribution<float> nz(0.0f, noise_amp);
        for (int i = 0; i < len; ++i) buf[i] += nz(rng);
    }
    // Normalize to ±1.
    float peak = 1e-9f;
    for (float v : buf) peak = std::max(peak, std::abs(v));
    for (float& v : buf) v /= peak;
    return buf;
}

struct Result { int hits, misses, extras; float worst_cents; };

static Result run_case(const std::string& name,
                       const std::vector<int>& midi, float noise_amp = 0.0f)
{
    std::vector<float> truth;
    for (int m : midi) truth.push_back(midi_to_freq(m));

    const int len = 24000;  // 0.5 s capture
    auto buf = synth(truth, len, noise_amp);
    MultiHNState st = hn_multif0_analyze(buf.data(), len, SR);

    std::vector<float> det;
    for (int i = 0; i < st.n_notes; ++i) det.push_back(st.notes[i].f0);
    std::sort(det.begin(), det.end());

    // Greedy match: each truth note to nearest detected within 50 cents.
    std::vector<bool> used(det.size(), false);
    int hits = 0; float worst = 0.0f;
    for (float t : truth) {
        int best = -1; float bestc = 1e9f;
        for (size_t j = 0; j < det.size(); ++j) {
            if (used[j]) continue;
            float c = std::abs(cents(det[j], t));
            if (c < bestc) { bestc = c; best = (int)j; }
        }
        if (best >= 0 && bestc <= 50.0f) { used[best] = true; ++hits; worst = std::max(worst, bestc); }
    }
    const int misses = (int)truth.size() - hits;
    int extras = 0;
    for (bool u : used) if (!u) {} // placeholder
    for (size_t j = 0; j < det.size(); ++j) if (!used[j]) ++extras;

    printf("  %-22s truth:", name.c_str());
    for (float t : truth) printf(" %.1f", t);
    printf("\n  %-22s det.: ", "");
    for (float d : det) printf(" %.1f", d);
    printf("\n  %-22s  -> hits %d/%d  extras %d  worst %.0f cents%s\n\n",
           "", hits, (int)truth.size(), extras, worst,
           (misses == 0 && extras == 0) ? "   OK" : "");

    return { hits, misses, extras, worst };
}

int main()
{
    printf("H+N polyphonic analyzer — tier MEGALO_HN_QUALITY=%d  "
           "(FFT %d, max notes %d, max partials %d)\n\n",
           MEGALO_HN_QUALITY, hnq::FFT_SIZE, hnq::MAX_NOTES, hnq::MAX_PARTIALS);

    int H = 0, M = 0, E = 0;
    auto acc = [&](Result r){ H += r.hits; M += r.misses; E += r.extras; };

    printf("Single notes:\n");
    acc(run_case("A1 (55 Hz)",   {33}));
    acc(run_case("E2 low-E",     {40}));
    acc(run_case("A2",           {45}));
    acc(run_case("A4 (440)",     {69}));

    printf("Two notes:\n");
    acc(run_case("octave A2+A3", {45, 57}));
    acc(run_case("fifth E2+B2",  {40, 47}));
    acc(run_case("low A1+E2",    {33, 40}));

    printf("Chords:\n");
    acc(run_case("power E2+B2+E3", {40, 47, 52}));
    acc(run_case("open E major",   {40, 47, 52, 56, 59, 64}));

    printf("With noise:\n");
    acc(run_case("A2 + 15%% noise", {45}, 0.15f));

    printf("==== TOTAL  hits %d  misses %d  extras %d ====\n", H, M, E);
    // CI gate: 19/20 baseline (the remaining miss is E4 at the top of the
    // 6-note open-E chord, fully shadowed by three lower notes' harmonics).
    return (H >= 19 && E == 0) ? 0 : 1;
}
