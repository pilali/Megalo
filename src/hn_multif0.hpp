#pragma once
#include "hn_analyzer.hpp"   // HNState, HN_MAX_PARTIALS
#include "hn_quality.hpp"    // hnq::FFT_SIZE, MAX_NOTES, MAX_PARTIALS, F0_*
#include "hn_fft.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Polyphonic harmonic+noise analyzer ────────────────────────────────────
//
// Replaces the monophonic YIN front-end (hn_analyzer.hpp) with a spectral
// multi-F0 estimator. Runs ONCE on LoopReady, never per sample.
//
// Method (a pragmatic, guitar-constrained subset of Klapuri iterative
// cancellation):
//   1. Hann-window the captured loop (as many samples as available, up to
//      FFT_SIZE) and take one FFT → magnitude spectrum.
//   2. Score every candidate F0 on a log grid by its harmonic-comb salience,
//      with a 1/sqrt(k) harmonic weighting that intrinsically disfavours the
//      sub-octave (f0/2) and octave-up (2·f0) traps.
//   3. Greedily pick the strongest candidate, refine it to sub-bin accuracy
//      from its loudest harmonic peak, record an HNState for it, then subtract
//      its harmonic energy from the spectrum and repeat — up to MAX_NOTES, or
//      until the residual salience falls below a fraction of the first note.
//   4. Whatever spectral energy remains after all combs → shared noise model.
//
// Extending F0_MIN to 55 Hz is why FFT_SIZE is large: at 48 kHz a 16384-point
// transform gives ~2.9 Hz bins, and the sub-bin parabolic refinement (driven by
// a high harmonic, which carries m× the frequency resolution of the fundamental)
// recovers the cents-level accuracy needed to separate low semitones.

struct MultiHNState {
    int     n_notes   = 0;
    HNState notes[hnq::MAX_NOTES];
    float   noise_rms = 0.0f;   // broadband residual shared by all notes
    bool    valid     = false;  // false → no pitched content found
};

namespace hnmf {

// Linear-interpolated spectral magnitude at an arbitrary (fractional) bin.
static inline float mag_at(const float* mag, int nbins, float bin) noexcept
{
    if (bin < 0.0f || bin >= static_cast<float>(nbins - 1)) return 0.0f;
    const int   i0   = static_cast<int>(bin);
    const float frac = bin - static_cast<float>(i0);
    return mag[i0] + frac * (mag[i0 + 1] - mag[i0]);
}

// Harmonic-comb salience of a candidate fundamental f0.
//
// A 1/sqrt(k) weighting already disfavours the octave-up trap. The extra
// sub-octave GUARD handles the opposite error seen on real audio: a phantom
// f0 = F/2 whose comb (F/2, F, 3F/2, 2F …) lands its EVEN harmonics on a real
// note F's partials and scores high. A genuine note has energy on its
// fundamental and odd harmonics (g, 3g, 5g); a phantom sub-octave has ~none
// there (the real source sits at 2g, 4g …). We scale salience by how much of
// the comb's energy actually sits on the odd harmonics, so a phantom — whose
// own fundamental is essentially absent — is pushed below the real note.
static inline float salience(const float* mag, int nbins, float f0,
                             float sr, int fft_n) noexcept
{
    const float bin_per_hz = static_cast<float>(fft_n) / sr;
    const int   kmax       = std::min(hnq::MAX_PARTIALS,
                                      static_cast<int>(0.46f * sr / f0));
    float s = 0.0f, odd = 0.0f, even = 0.0f;
    for (int k = 1; k <= kmax; ++k) {
        const float m = mag_at(mag, nbins, f0 * static_cast<float>(k) * bin_per_hz);
        s += m / std::sqrt(static_cast<float>(k));
        if (k & 1) odd += m; else even += m;
    }
    // guard ≈ 1 for a real harmonic note, → small when the odd harmonics
    // (fundamental included) are missing, i.e. a sub-octave phantom.
    const float guard = (odd + even > 1e-12f) ? odd / (odd + even) : 0.0f;
    return s * (0.25f + 0.75f * std::min(1.0f, 2.0f * guard));
}

} // namespace hnmf

// Optional second-stage amplitude attribution (definition in hn_nnls.hpp).
// Only referenced when hnq::USE_NNLS is true, so a translation unit that does
// not need it (dwarf tier) need not include hn_nnls.hpp.
inline void hn_refine_nnls(const float* mag, int nbins, float sr, int fft_n,
                           float amp_scale, MultiHNState& st) noexcept;

static MultiHNState hn_multif0_analyze(const float* loop, int loop_len,
                                       float sr) noexcept
{
    MultiHNState out;
    if (loop_len < 256) return out;

    const int   N     = hnq::FFT_SIZE;
    const int   NB    = N / 2;                      // usable bins (0..N/2-1)
    const int   L     = std::min(loop_len, N);      // samples actually analysed
    const float bin_per_hz = static_cast<float>(N) / sr;

    // ── 1. Window + FFT ───────────────────────────────────────────────────
    static thread_local float re[hnq::FFT_SIZE];
    static thread_local float im[hnq::FFT_SIZE];
    static thread_local float mag[hnq::FFT_SIZE / 2];
    static thread_local float mag_orig[hnq::FFT_SIZE / 2];   // kept for NNLS

    float win_sum = 0.0f;
    for (int i = 0; i < L; ++i) {
        // Hann window over the L analysed samples.
        const float w = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) *
                          static_cast<float>(i) / static_cast<float>(L - 1)));
        re[i] = loop[i] * w;
        im[i] = 0.0f;
        win_sum += w;
    }
    for (int i = L; i < N; ++i) { re[i] = 0.0f; im[i] = 0.0f; }

    hnfft::fft(re, im, N);

    float total_energy = 0.0f;
    for (int i = 0; i < NB; ++i) {
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        mag_orig[i] = mag[i];               // greedy pass destroys mag[]
        total_energy += mag[i] * mag[i];
    }
    // Single-sided sinusoid amplitude ≈ 2·peak_mag / Σwindow.
    const float amp_scale = (win_sum > 0.0f) ? (2.0f / win_sum) : 0.0f;

    // ── 2. Candidate F0 grid (log-spaced, ~20 cents) ──────────────────────
    static constexpr float CENTS_STEP = 20.0f;
    const float ratio_step = std::pow(2.0f, CENTS_STEP / 1200.0f);
    int n_cand = 0;
    for (float f = hnq::F0_MIN; f <= hnq::F0_MAX; f *= ratio_step) ++n_cand;

    static thread_local float cand_f0 [4096];
    static thread_local float cand_sal[4096];
    n_cand = std::min(n_cand, 4096);
    {
        float f = hnq::F0_MIN;
        for (int c = 0; c < n_cand; ++c, f *= ratio_step) {
            cand_f0 [c] = f;
            cand_sal[c] = hnmf::salience(mag, NB, f, sr, N);
        }
    }

    // ── 3. Greedy multi-F0 extraction with harmonic subtraction ───────────
    static constexpr float REL_THRESH = 0.18f;  // stop below this × first note
    float first_sal = 0.0f;

    for (int note = 0; note < hnq::MAX_NOTES; ++note) {
        // Pick the most salient remaining candidate.
        int   best_c = -1;
        float best_s = 0.0f;
        for (int c = 0; c < n_cand; ++c)
            if (cand_sal[c] > best_s) { best_s = cand_sal[c]; best_c = c; }
        if (best_c < 0) break;
        if (note == 0) first_sal = best_s;
        else if (best_s < REL_THRESH * first_sal) break;

        float f0 = cand_f0[best_c];

        // Sub-bin refinement: parabolic interpolation on the loudest harmonic
        // peak (harmonic m resolves f0 m× finer than the fundamental bin).
        {
            float best_amp = 0.0f; int best_m = 1; float best_bin = f0 * bin_per_hz;
            const int kmax = std::min(hnq::MAX_PARTIALS,
                                      static_cast<int>(0.46f * sr / f0));
            for (int k = 1; k <= kmax; ++k) {
                const float b = f0 * static_cast<float>(k) * bin_per_hz;
                const float a = hnmf::mag_at(mag, NB, b);
                if (a > best_amp) { best_amp = a; best_m = k; best_bin = b; }
            }
            const int ib = static_cast<int>(std::lround(best_bin));
            if (ib >= 1 && ib < NB - 1) {
                const float a = mag[ib - 1], b = mag[ib], c = mag[ib + 1];
                const float denom = 2.0f * (a - 2.0f * b + c);
                const float frac  = (std::abs(denom) > 1e-12f)
                                    ? (a - c) / denom : 0.0f;
                const float peak_freq = (static_cast<float>(ib) + frac) / bin_per_hz;
                f0 = peak_freq / static_cast<float>(best_m);
            }
        }

        // Record the note's harmonic amplitudes/phases, and subtract its comb.
        HNState& st = out.notes[out.n_notes];
        st = HNState{};
        st.f0 = f0;
        const int kmax = std::min(hnq::MAX_PARTIALS,
                                  static_cast<int>(0.46f * sr / f0));
        float harm_ss = 0.0f;
        for (int k = 1; k <= kmax; ++k) {
            const float fb = f0 * static_cast<float>(k) * bin_per_hz;
            const int   ib = static_cast<int>(std::lround(fb));
            if (ib < 1 || ib >= NB - 1) break;
            const float a   = hnmf::mag_at(mag, NB, fb);
            const float amp = a * amp_scale;
            st.harm_amp [k - 1] = amp;
            st.harm_phase[k - 1] = std::atan2(im[ib], re[ib]);
            st.n_partials = k;
            harm_ss += 0.5f * amp * amp;

            // Soft-subtract a small triangular bump (±2 bins) so this comb is
            // not redetected and shared harmonics are partly freed for others.
            for (int d = -2; d <= 2; ++d) {
                const int j = ib + d;
                if (j < 0 || j >= NB) continue;
                const float wd = 1.0f - 0.25f * static_cast<float>(std::abs(d));
                mag[j] = std::max(0.0f, mag[j] - a * wd);
            }
        }
        st.confidence = best_s / (first_sal + 1e-12f);
        st.noise_rms  = 0.0f;   // filled globally below
        st.valid      = true;
        ++out.n_notes;

        // Refresh salience of candidates near this f0 and its octaves so the
        // next pick reflects the subtraction.
        for (int c = 0; c < n_cand; ++c)
            cand_sal[c] = hnmf::salience(mag, NB, cand_f0[c], sr, N);
    }

    // ── 4. Residual → shared noise RMS ────────────────────────────────────
    float residual = 0.0f;
    for (int i = 0; i < NB; ++i) residual += mag[i] * mag[i];
    const float noise_frac = (total_energy > 0.0f)
                             ? std::sqrt(residual / total_energy) : 0.0f;
    // Scale to a time-domain RMS estimate comparable to the old monophonic path.
    out.noise_rms = noise_frac;
    for (int i = 0; i < out.n_notes; ++i) out.notes[i].noise_rms = noise_frac;

    // Second stage: joint amplitude attribution to recover octave notes whose
    // partials are shared with an already-detected lower note (uses the
    // pre-subtraction spectrum). Greedy-only on the dwarf tier.
    if constexpr (hnq::USE_NNLS)
        hn_refine_nnls(mag_orig, NB, sr, N, amp_scale, out);

    out.valid = (out.n_notes > 0);
    return out;
}
