// Compiled with AVX2 + FMA enabled (-mavx2 -mfma / MSVC /arch:AVX2).
#include <immintrin.h>

#include "whe/simd.hpp"

namespace whe::kernels {

namespace {

void addScaled(float* dst, const float* src, float g, int n) {
    const __m256 vg = _mm256_set1_ps(g);
    int i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(vg, _mm256_loadu_ps(src + i), _mm256_loadu_ps(dst + i)));
    for (; i < n; ++i) dst[i] += g * src[i];
}

// Each 8-mode vector runs the whole block in registers; per-sample lane sums
// are accumulated in scratch and reduced once at the end.
void modalBank(ModalBankState& s, const float* in, float* out, int n) {
    float* acc = s.scratch.data();
    for (int t = 0; t < n; ++t) _mm256_store_ps(acc + t * 8, _mm256_setzero_ps());
    for (int m = 0; m < s.padded; m += 8) {
        __m256 y1 = _mm256_load_ps(s.y1.data() + m), y2 = _mm256_load_ps(s.y2.data() + m);
        const __m256 a1 = _mm256_load_ps(s.a1.data() + m), a2 = _mm256_load_ps(s.a2.data() + m), g = _mm256_load_ps(s.gain.data() + m);
        for (int t = 0; t < n; ++t) {
            const __m256 x = _mm256_set1_ps(in[t]);
            const __m256 y = _mm256_fmadd_ps(g, x, _mm256_fnmadd_ps(a2, y2, _mm256_mul_ps(a1, y1)));
            y2 = y1;
            y1 = y;
            _mm256_store_ps(acc + t * 8, _mm256_add_ps(_mm256_load_ps(acc + t * 8), y));
        }
        _mm256_store_ps(s.y1.data() + m, y1);
        _mm256_store_ps(s.y2.data() + m, y2);
    }
    for (int t = 0; t < n; ++t) {
        const __m256 v = _mm256_load_ps(acc + t * 8);
        __m128 lo = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
        lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
        lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 1));
        out[t] = _mm_cvtss_f32(lo);
    }
}

void meshStep(const MeshStepArgs& a) {
    const __m256 A = _mm256_set1_ps(a.A), B = _mm256_set1_ps(a.B), C = _mm256_set1_ps(a.C), D = _mm256_set1_ps(a.D);
    const __m256 four = _mm256_set1_ps(4.0f);
    const int W = (a.n + 7) & ~7;
    for (int r = 1; r <= a.n; ++r) {
        const int base = r * a.stride;
        for (int c = 1; c < 1 + W; c += 8) {
            const int i = base + c;
            const __m256 u = _mm256_loadu_ps(a.cur + i), p = _mm256_loadu_ps(a.prev + i);
            const __m256 nu = _mm256_add_ps(_mm256_add_ps(_mm256_loadu_ps(a.cur + i - 1), _mm256_loadu_ps(a.cur + i + 1)),
                                            _mm256_add_ps(_mm256_loadu_ps(a.cur + i - a.stride), _mm256_loadu_ps(a.cur + i + a.stride)));
            const __m256 np = _mm256_add_ps(_mm256_add_ps(_mm256_loadu_ps(a.prev + i - 1), _mm256_loadu_ps(a.prev + i + 1)),
                                            _mm256_add_ps(_mm256_loadu_ps(a.prev + i - a.stride), _mm256_loadu_ps(a.prev + i + a.stride)));
            const __m256 lu = _mm256_fnmadd_ps(four, u, nu);
            const __m256 lp = _mm256_fnmadd_ps(four, p, np);
            __m256 v = _mm256_mul_ps(A, u);
            v = _mm256_fmadd_ps(B, p, v);
            v = _mm256_fmadd_ps(C, lu, v);
            v = _mm256_fmadd_ps(D, lp, v);
            _mm256_storeu_ps(a.next + i, _mm256_mul_ps(v, _mm256_loadu_ps(a.mask + i)));
        }
    }
}

const SimdKernels kAvx2{SimdLevel::Avx2, "avx2+fma", &addScaled, &modalBank, &meshStep};

}  // namespace

const SimdKernels& avx2() noexcept { return kAvx2; }

}  // namespace whe::kernels
