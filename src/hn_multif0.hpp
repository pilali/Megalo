#pragma once
#include "hn_state.hpp"      // HNState, HN_MAX_PARTIALS
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

// True peak magnitude of a partial near `bin`: snap to the local maximum
// within ±2 bins, then parabolic-interpolate the vertex. Recovers the real
// amplitude of an off-bin partial (a single interpolated sample loses up to
// ~36% to Hann scalloping), so the harmonic power is not under-counted and
// mis-attributed to the noise residual. Used for amplitude estimation; the
// salience comb keeps the cheaper mag_at.
static inline float peak_mag(const float* mag, int nbins, float bin) noexcept
{
    const int c0 = static_cast<int>(std::lround(bin));
    if (c0 < 2 || c0 >= nbins - 2) return mag_at(mag, nbins, bin);
    int c = c0;                                 // snap to local max in ±1 only:
    for (int d = -1; d <= 1; ++d)               // a real partial peaks within a
        if (mag[c0 + d] > mag[c]) c = c0 + d;   // bin of k·f0; ±2 grabbed noise.
    const float a = mag[c - 1], b = mag[c], cc = mag[c + 1];
    const float denom = a - 2.0f * b + cc;
    if (std::abs(denom) < 1e-12f) return b;
    const float p = 0.5f * (a - cc) / denom;    // sub-bin offset [-0.5,0.5]
    return b - 0.25f * (a - cc) * p;            // parabola vertex value
}

// Refined frequency (Hz) of a partial near fractional bin `bin`: snap to the
// local maximum within ±1 bin, then parabolic-interpolate the vertex
// POSITION (peak_mag interpolates the vertex VALUE). Recovers the true,
// slightly stretched frequency of an inharmonic string partial.
static inline float peak_freq(const float* mag, int nbins, float bin,
                              float bin_per_hz) noexcept
{
    const int c0 = static_cast<int>(std::lround(bin));
    if (c0 < 4 || c0 >= nbins - 4) return bin / bin_per_hz;
    int c = c0;
    // ±3-bin snap: high partials of a stiff string drift more than one bin
    // above k·f0 (peak_mag keeps ±1 for amplitudes). The caller's ±3 % clamp
    // bounds the damage if this grabs a foreign peak in a dense chord.
    for (int d = -3; d <= 3; ++d)
        if (mag[c0 + d] > mag[c]) c = c0 + d;
    const float a = mag[c - 1], b = mag[c], cc = mag[c + 1];
    const float denom = a - 2.0f * b + cc;
    if (std::abs(denom) < 1e-12f) return static_cast<float>(c) / bin_per_hz;
    const float p = std::min(0.5f, std::max(-0.5f, 0.5f * (a - cc) / denom));
    return (static_cast<float>(c) + p) / bin_per_hz;
}

// ── Detection-robustness tuning (grouped so they can be set by ear) ────────
// Whitening (④) curbs the "loudest harmonic wins" bias; the two-way mismatch
// (③) replaces the old odd/even guard and kills octave-UP errors; the
// sub-harmonic descent (①) is a safety net applied at pick time; the attack
// down-weight (⑤) lives in the windowing step of hn_multif0_analyze().
static constexpr float WHITEN_AMOUNT      = 0.5f;   // ④ 0 = off … 1 = full whitening
static constexpr float WHITEN_SMOOTH_HZ   = 220.0f; // ④ envelope smoothing width (Hz)
static constexpr float SUBOCT_PENALTY     = 0.8f;   // ③ two-way mismatch strength [0,1]
static constexpr float SUBHARM_KEEP       = 0.85f;  // ① descend an octave when the
                                                    //   sub-multiple keeps ≥ this × salience
static constexpr float ATTACK_SUPPRESS_MS = 30.0f;  // ⑤ attack-region down-weight span
static constexpr float ATTACK_FLOOR       = 0.3f;   // ⑤ residual weight inside that span

// Whitened, linear-interpolated magnitude at a fractional bin. The per-bin gain
// (precomputed once from the pristine spectrum) flattens the broadband tilt so
// the comb score reflects partial PRESENCE, not absolute loudness — without
// touching the raw mag[] used for amplitude/noise estimation.
static inline float mag_at_w(const float* mag, const float* wg,
                             int nbins, float bin) noexcept
{
    if (bin < 0.0f || bin >= static_cast<float>(nbins - 1)) return 0.0f;
    const int   i0   = static_cast<int>(bin);
    const float frac = bin - static_cast<float>(i0);
    const float m = mag[i0] + frac * (mag[i0 + 1] - mag[i0]);
    const float g = wg [i0] + frac * (wg [i0 + 1] - wg [i0]);
    return m * g;
}

// Harmonic-comb salience of a candidate fundamental f0 (whitened), with a
// two-way mismatch penalty (③) that replaces the former odd/even guard.
//
// The comb sum credits energy on f0, 2·f0, 3·f0 … with a 1/sqrt(k) weighting.
// The mismatch term measures energy on the HALF-teeth — the odd multiples of
// f0/2 (f0/2, 3·f0/2, 5·f0/2 …) that a genuine fundamental does NOT excite but
// an octave-UP phantom does (those are the real lower note's partials the
// phantom skips). The candidate is scaled down in proportion to how much of its
// neighbourhood energy falls on those skipped teeth, so the real, lower
// fundamental wins even when its own fundamental bin is weak — the typical
// guitar case the old guard mis-handled (it penalised exactly those notes).
//
// A genuinely low note has nothing below it → half ≈ 0 → no penalty; only a
// candidate that is itself a harmonic of something lower gets pushed down.
static inline float salience(const float* mag, const float* wg, int nbins,
                             float f0, float sr, int fft_n) noexcept
{
    const float bin_per_hz = static_cast<float>(fft_n) / sr;
    const int   kmax       = std::min(hnq::MAX_PARTIALS,
                                      static_cast<int>(0.46f * sr / f0));
    float s = 0.0f;
    for (int k = 1; k <= kmax; ++k)
        s += mag_at_w(mag, wg, nbins, f0 * static_cast<float>(k) * bin_per_hz)
             / std::sqrt(static_cast<float>(k));

    // Energy on the skipped half-teeth (odd harmonics of f0/2).
    float half = 0.0f;
    const float half_bin = 0.5f * f0 * bin_per_hz;
    for (int t = 1; t <= 2 * kmax; t += 2) {
        const float b = half_bin * static_cast<float>(t);
        if (b >= static_cast<float>(nbins - 1)) break;
        half += mag_at_w(mag, wg, nbins, b)
                / std::sqrt(0.5f * static_cast<float>(t));
    }
    const float mism = (s + half > 1e-12f) ? half / (s + half) : 0.0f;
    return s * (1.0f - SUBOCT_PENALTY * mism);
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

    // ⑤ Attack-suppression: an ASYMMETRIC window. A normal Hann tapers both ends
    // (clean leakage) but still weights the pluck transient sitting tens of ms
    // into the loop — its broadband energy tilts the spectrum bright and feeds
    // the octave-up error. We keep the Hann (all samples → full frequency
    // resolution, no added latency: the audio is already captured) and multiply
    // an extra raised-cosine ramp over the first ATTACK_SUPPRESS_MS so the attack
    // is down-weighted toward ATTACK_FLOOR without being discarded. Only applied
    // when the loop is comfortably longer than the ramp; otherwise plain Hann.
    const int  atk_n   = std::min(L / 3, static_cast<int>(
                             hnmf::ATTACK_SUPPRESS_MS * 0.001f * sr));
    const bool use_atk = (atk_n > 0 && L > 4 * atk_n);
    float win_sum = 0.0f, sig_ss = 0.0f;
    for (int i = 0; i < L; ++i) {
        // Hann window over the L analysed samples.
        float w = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) *
                          static_cast<float>(i) / static_cast<float>(L - 1)));
        if (use_atk && i < atk_n) {
            const float r = 0.5f * (1.0f - std::cos(float(M_PI) *
                              static_cast<float>(i) / static_cast<float>(atk_n)));
            w *= hnmf::ATTACK_FLOOR + (1.0f - hnmf::ATTACK_FLOOR) * r;   // floor→1
        }
        re[i] = loop[i] * w;
        im[i] = 0.0f;
        win_sum += w;
        sig_ss += loop[i] * loop[i];        // raw signal power (for noise RMS)
    }
    sig_ss = (L > 0) ? sig_ss / static_cast<float>(L) : 0.0f;
    for (int i = L; i < N; ++i) { re[i] = 0.0f; im[i] = 0.0f; }

    hnfft::fft(re, im, N);

    float mag_sum = 0.0f;
    for (int i = 0; i < NB; ++i) {
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        mag_orig[i] = mag[i];               // greedy pass destroys mag[]
        mag_sum += mag[i];
    }
    const float mean_mag = (NB > 0) ? mag_sum / static_cast<float>(NB) : 0.0f;
    // Single-sided sinusoid amplitude ≈ 2·peak_mag / Σwindow.
    const float amp_scale = (win_sum > 0.0f) ? (2.0f / win_sum) : 0.0f;

    // ── ④ Whitening gain (SCORING path only) ──────────────────────────────
    // A smoothed spectral envelope → per-bin gain 1/env^WHITEN_AMOUNT that
    // flattens the broadband tilt for the salience/mismatch scoring, so a
    // candidate is judged on partial PRESENCE rather than on whichever harmonic
    // happens to be loudest (the bright-attack bias). mag[] itself — used for
    // amplitude extraction, the noise residual and NNLS — is left untouched, so
    // the resynthesis stays calibrated. The score uses mag[bin]·wgain[bin], and
    // since the greedy pass subtracts from mag[] the whitened score tracks the
    // subtraction automatically. (Scale-invariant: only relative scores matter.)
    static thread_local float  wgain[hnq::FFT_SIZE / 2];
    static thread_local double wpref[hnq::FFT_SIZE / 2 + 1];
    {
        wpref[0] = 0.0;
        for (int i = 0; i < NB; ++i) wpref[i + 1] = wpref[i] + mag_orig[i];
        const int half_w = std::max(4, static_cast<int>(
            hnmf::WHITEN_SMOOTH_HZ * static_cast<float>(N) / sr * 0.5f));
        for (int i = 0; i < NB; ++i) {
            const int lo = std::max(0, i - half_w);
            const int hi = std::min(NB - 1, i + half_w);
            const float env = static_cast<float>(
                (wpref[hi + 1] - wpref[lo]) / static_cast<double>(hi - lo + 1));
            wgain[i] = 1.0f / std::pow(env + 1e-9f, hnmf::WHITEN_AMOUNT);
        }
    }

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
            cand_sal[c] = hnmf::salience(mag, wgain, NB, f, sr, N);
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

        // ① Octave-down verification (safety net over the ③ ranking). The
        // whitened two-way-mismatch salience already favours the true low
        // fundamental, but a pick can still slip an octave high on a very
        // weak-fundamental note; here we test f0/2 and f0/3 and descend to the
        // lowest sub-multiple that keeps ≥ SUBHARM_KEEP × the current salience.
        // The mismatch term inside salience() blocks descending onto a genuine
        // sub-octave phantom (its own teeth are absent → low salience), so this
        // only fires when the lower fundamental really carries energy.
        {
            float chosen   = f0;
            float chosen_s = hnmf::salience(mag, wgain, NB, f0, sr, N);
            for (int sub = 2; sub <= 3; ++sub) {
                const float fs = f0 / static_cast<float>(sub);
                if (fs < hnq::F0_MIN) break;
                const float ss = hnmf::salience(mag, wgain, NB, fs, sr, N);
                if (ss > hnmf::SUBHARM_KEEP * chosen_s) { chosen = fs; chosen_s = ss; }
            }
            f0 = chosen;
        }

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
            const float a   = hnmf::peak_mag(mag, NB, fb);
            const float amp = a * amp_scale;
            st.harm_amp [k - 1] = amp;
            st.harm_phase[k - 1] = std::atan2(im[ib], re[ib]);
            // Measured partial frequency (inharmonicity). Clamped to ±3 % of
            // the ideal k·f0 so a neighbouring note's partial is never grabbed.
            {
                const float ideal = f0 * static_cast<float>(k);
                float fmeas = hnmf::peak_freq(mag, NB, fb, bin_per_hz);
                if (std::abs(fmeas - ideal) > 0.03f * ideal) fmeas = ideal;
                st.harm_freq[k - 1] = fmeas;
            }
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
            cand_sal[c] = hnmf::salience(mag, wgain, NB, cand_f0[c], sr, N);
    }

    // Second stage: joint amplitude attribution to recover octave notes whose
    // partials are shared with an already-detected lower note (uses the
    // pre-subtraction spectrum). Greedy-only on the dwarf tier.
    if constexpr (hnq::USE_NNLS)
        hn_refine_nnls(mag_orig, NB, sr, N, amp_scale, out);

    // Partial gate: drop partials at the noise floor (peak_mag can inflate a
    // high partial out of the noise → harsh treble). Keep a partial only if its
    // magnitude exceeds PARTIAL_FLOOR_X × the mean spectral magnitude.
    const float amp_floor = hnq::PARTIAL_FLOOR_X * mean_mag * amp_scale;
    for (int i = 0; i < out.n_notes; ++i) {
        HNState& s = out.notes[i];
        int last = 0;
        for (int k = 0; k < s.n_partials; ++k) {
            if (s.harm_amp[k] < amp_floor) s.harm_amp[k] = 0.0f;
            else last = k + 1;
        }
        s.n_partials = last;                    // trim trailing zeros
    }

    // ── 4. Shared noise RMS = residual after the harmonics ────────────────
    // A real RMS amplitude (NOT a spectral ratio): the leftover signal power
    // once the harmonic power Σ½·amp² of every detected note is removed, so it
    // sits at the right level next to the calibrated harm_amp values. The old
    // spectral-ratio estimate counted each partial's window leakage as noise
    // and, scaled in the synth, drowned the notes under pink noise.
    float harm_ss = 0.0f;
    for (int i = 0; i < out.n_notes; ++i)
        for (int k = 0; k < out.notes[i].n_partials; ++k)
            harm_ss += 0.5f * out.notes[i].harm_amp[k] * out.notes[i].harm_amp[k];
    // Pure residual RMS; the user "Noise" control scales it in the synth.
    const float noise_rms = std::sqrt(std::max(0.0f, sig_ss - harm_ss));
    out.noise_rms = noise_rms;
    for (int i = 0; i < out.n_notes; ++i) out.notes[i].noise_rms = noise_rms;

    out.valid = (out.n_notes > 0);
    return out;
}
