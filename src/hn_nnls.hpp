#pragma once
#include "hn_multif0.hpp"
#include <cmath>
#include <algorithm>

// ── Non-negative amplitude attribution (octave disambiguation) ─────────────
//
// The greedy harmonic-comb pass (hn_multif0_analyze) finds fundamentals well
// but starves any note whose partials are fully shared with an already-detected
// lower note — typically the octave (A3 hides inside A2's even harmonics).
//
// This pass treats the greedy notes as a CANDIDATE set (extended with each
// note's octave), then jointly re-estimates every partial amplitude under two
// constraints:
//   • non-negativity (EM / multiplicative-style redistribution), and
//   • spectral-envelope smoothness per note.
//
// Why it resolves the octave: a note's harmonics that land on a frequency no
// other candidate explains are UNIQUE — they pin that note's amplitude exactly
// (the odd harmonics of A2: 1·f, 3·f, 5·f …). The smoothness prior then
// predicts what its SHARED (even) harmonics should be; any excess energy at
// those shared frequencies is forced onto the octave candidate. A candidate
// that ends up with ~no energy (a spurious octave) is pruned.
//
// Pure, perfectly-harmonic octaves where the upper note has NO unique partial
// remain fundamentally ambiguous from magnitude alone — no method recovers
// those without a timbre prior — but real notes almost always leave a trace.

namespace hnnnls {

static constexpr int   MAXC      = 2 * hnq::MAX_NOTES;  // notes + their octaves
static constexpr int   ITERS     = 40;
static constexpr float PRUNE_REL = 0.08f;   // drop notes below this × loudest
static constexpr float COINCIDE_BINS = 1.5f; // partials within → shared group

} // namespace hnnnls

inline void hn_refine_nnls(const float* mag, int nbins, float sr, int fft_n,
                           float amp_scale, MultiHNState& st) noexcept
{
    using namespace hnnnls;
    if (st.n_notes == 0) return;

    const float bin_per_hz = static_cast<float>(fft_n) / sr;

    // ── Build candidate fundamentals: detected notes + their octaves ───────
    float cf0[MAXC];
    int   nc = 0;
    auto add_cand = [&](float f) {
        if (f < hnq::F0_MIN || f > hnq::F0_MAX || nc >= MAXC) return;
        for (int i = 0; i < nc; ++i)                 // dedup within ¼ tone
            if (std::abs(1200.0f * std::log2(f / cf0[i])) < 25.0f) return;
        cf0[nc++] = f;
    };
    for (int i = 0; i < st.n_notes; ++i) add_cand(st.notes[i].f0);
    const int n_detected = nc;
    for (int i = 0; i < n_detected; ++i) add_cand(cf0[i] * 2.0f);

    // ── Atoms: one per (candidate, harmonic) with its observed amplitude ────
    static thread_local float a    [MAXC][HN_MAX_PARTIALS];
    static thread_local float env  [MAXC][HN_MAX_PARTIALS];
    static thread_local float obs  [MAXC][HN_MAX_PARTIALS];
    static thread_local float freq [MAXC][HN_MAX_PARTIALS];
    static thread_local int   np   [MAXC];

    for (int c = 0; c < nc; ++c) {
        const int kmax = std::min(hnq::MAX_PARTIALS,
                                  static_cast<int>(0.46f * sr / cf0[c]));
        np[c] = 0;
        for (int k = 1; k <= kmax; ++k) {
            const float f = cf0[c] * static_cast<float>(k);
            const float o = hnmf::peak_mag(mag, nbins, f * bin_per_hz) * amp_scale;
            freq[c][k - 1] = f;
            obs [c][k - 1] = o;
            a   [c][k - 1] = o;          // init; shared atoms over-count, EM fixes
            np  [c]        = k;
        }
    }

    // ── Coincidence groups: atoms sharing a frequency (within COINCIDE_BINS)
    // Group id per atom; representative observation = the shared magnitude.
    const float tol_hz = COINCIDE_BINS / bin_per_hz;

    // ── EM: redistribute each group's observed amplitude by smoothed envelope
    for (int it = 0; it < ITERS; ++it) {
        // 1. Smooth each note's current envelope across k (3-tap, edge-clamped).
        for (int c = 0; c < nc; ++c) {
            for (int k = 0; k < np[c]; ++k) {
                const float lo = a[c][std::max(0, k - 1)];
                const float hi = a[c][std::min(np[c] - 1, k + 1)];
                env[c][k] = 0.25f * lo + 0.5f * a[c][k] + 0.25f * hi + 1e-9f;
            }
        }
        // 2. For every atom, split its frequency's observed amplitude across all
        //    atoms landing there, in proportion to the smoothed envelope.
        for (int c = 0; c < nc; ++c) {
            for (int k = 0; k < np[c]; ++k) {
                float sum_env = 0.0f;
                for (int c2 = 0; c2 < nc; ++c2)
                    for (int k2 = 0; k2 < np[c2]; ++k2)
                        if (std::abs(freq[c2][k2] - freq[c][k]) < tol_hz)
                            sum_env += env[c2][k2];
                a[c][k] = (sum_env > 1e-12f)
                          ? obs[c][k] * env[c][k] / sum_env : 0.0f;
            }
        }
    }

    // ── Collect refined notes, prune inactive candidates ───────────────────
    float energy[MAXC]; float max_e = 0.0f;
    for (int c = 0; c < nc; ++c) {
        float e = 0.0f;
        for (int k = 0; k < np[c]; ++k) e += a[c][k] * a[c][k];
        energy[c] = e;
        max_e = std::max(max_e, e);
    }

    MultiHNState refined;
    for (int c = 0; c < nc && refined.n_notes < hnq::MAX_NOTES; ++c) {
        if (energy[c] < PRUNE_REL * max_e) continue;
        HNState& s = refined.notes[refined.n_notes];
        s = HNState{};
        s.f0 = cf0[c];
        for (int k = 0; k < np[c]; ++k) {
            s.harm_amp[k] = a[c][k];
            if (a[c][k] > 1e-6f) s.n_partials = k + 1;
        }
        s.confidence = energy[c] / (max_e + 1e-12f);
        s.noise_rms  = st.noise_rms;
        s.valid      = true;
        ++refined.n_notes;
    }
    refined.noise_rms = st.noise_rms;
    refined.valid     = (refined.n_notes > 0);
    if (refined.n_notes > 0) st = refined;
}
