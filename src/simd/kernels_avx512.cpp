// Compiled with AVX-512F enabled (-mavx512f -mfma / MSVC /arch:AVX512).
#include <immintrin.h>

#include "whe/simd.hpp"

namespace whe::kernels {

namespace {

void addScaled(float* dst, const float* src, float g, int n) {
    const __m512 vg = _mm512_set1_ps(g);
    int i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(dst + i, _mm512_fmadd_ps(vg, _mm512_loadu_ps(src + i), _mm512_loadu_ps(dst + i)));
    if (i < n) {
        const __mmask16 k = static_cast<__mmask16>((1u << (n - i)) - 1u);
        _mm512_mask_storeu_ps(dst + i, k, _mm512_fmadd_ps(vg, _mm512_maskz_loadu_ps(k, src + i), _mm512_maskz_loadu_ps(k, dst + i)));
    }
}

void modalBank(ModalBankState& s, const float* in, float* out, int n) {
    float* acc = s.scratch.data();
    for (int t = 0; t < n; ++t) _mm512_store_ps(acc + t * 16, _mm512_setzero_ps());
    for (int m = 0; m < s.padded; m += 16) {
        __m512 y1 = _mm512_load_ps(s.y1.data() + m), y2 = _mm512_load_ps(s.y2.data() + m);
        const __m512 a1 = _mm512_load_ps(s.a1.data() + m), a2 = _mm512_load_ps(s.a2.data() + m), g = _mm512_load_ps(s.gain.data() + m);
        for (int t = 0; t < n; ++t) {
            const __m512 x = _mm512_set1_ps(in[t]);
            const __m512 y = _mm512_fmadd_ps(g, x, _mm512_fnmadd_ps(a2, y2, _mm512_mul_ps(a1, y1)));
            y2 = y1;
            y1 = y;
            _mm512_store_ps(acc + t * 16, _mm512_add_ps(_mm512_load_ps(acc + t * 16), y));
        }
        _mm512_store_ps(s.y1.data() + m, y1);
        _mm512_store_ps(s.y2.data() + m, y2);
    }
    for (int t = 0; t < n; ++t) out[t] = _mm512_reduce_add_ps(_mm512_load_ps(acc + t * 16));
}

void meshStep(const MeshStepArgs& a) {
    const __m512 A = _mm512_set1_ps(a.A), B = _mm512_set1_ps(a.B), C = _mm512_set1_ps(a.C), D = _mm512_set1_ps(a.D);
    const __m512 four = _mm512_set1_ps(4.0f);
    const int W = (a.n + 15) & ~15;
    for (int r = 1; r <= a.n; ++r) {
        const int base = r * a.stride;
        for (int c = 1; c < 1 + W; c += 16) {
            const int i = base + c;
            const __m512 u = _mm512_loadu_ps(a.cur + i), p = _mm512_loadu_ps(a.prev + i);
            const __m512 nu = _mm512_add_ps(_mm512_add_ps(_mm512_loadu_ps(a.cur + i - 1), _mm512_loadu_ps(a.cur + i + 1)),
                                            _mm512_add_ps(_mm512_loadu_ps(a.cur + i - a.stride), _mm512_loadu_ps(a.cur + i + a.stride)));
            const __m512 np = _mm512_add_ps(_mm512_add_ps(_mm512_loadu_ps(a.prev + i - 1), _mm512_loadu_ps(a.prev + i + 1)),
                                            _mm512_add_ps(_mm512_loadu_ps(a.prev + i - a.stride), _mm512_loadu_ps(a.prev + i + a.stride)));
            const __m512 lu = _mm512_fnmadd_ps(four, u, nu);
            const __m512 lp = _mm512_fnmadd_ps(four, p, np);
            __m512 v = _mm512_mul_ps(A, u);
            v = _mm512_fmadd_ps(B, p, v);
            v = _mm512_fmadd_ps(C, lu, v);
            v = _mm512_fmadd_ps(D, lp, v);
            _mm512_storeu_ps(a.next + i, _mm512_mul_ps(v, _mm512_loadu_ps(a.mask + i)));
        }
    }
}

const SimdKernels kAvx512{SimdLevel::Avx512, "avx512f", &addScaled, &modalBank, &meshStep};

}  // namespace

const SimdKernels& avx512() noexcept { return kAvx512; }

}  // namespace whe::kernels
