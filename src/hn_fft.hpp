#pragma once
#include <cmath>
#include <utility>

// ── Minimal self-contained FFT ────────────────────────────────────────────
//
// Iterative radix-2 Cooley-Tukey, in-place, no external dependency. Used only
// by the one-shot H+N analyzer at LoopReady, so clarity beats micro-tuning; a
// 16384-point transform here is sub-millisecond and runs once per capture.
//
// n MUST be a power of two. re[]/im[] hold the complex signal (im = 0 for real
// input on entry). Forward transform (exp(-j…)).

namespace hnfft {

static inline void fft(float* re, float* im, int n) noexcept
{
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }

    // Butterflies.
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / static_cast<float>(len);
        const float wr  = std::cos(ang);
        const float wi  = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;       // running twiddle
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const int a = i + k;
                const int b = i + k + half;
                const float vr = re[b] * cwr - im[b] * cwi;
                const float vi = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] = re[a] + vr; im[a] = im[a] + vi;
                const float ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = ncwr;
            }
        }
    }
}

} // namespace hnfft
