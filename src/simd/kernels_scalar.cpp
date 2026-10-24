#include "whe/simd.hpp"

namespace whe::kernels {

namespace {

void addScaled(float* dst, const float* src, float g, int n) {
    for (int i = 0; i < n; ++i) dst[i] += g * src[i];
}

void modalBank(ModalBankState& s, const float* in, float* out, int n) {
    for (int t = 0; t < n; ++t) out[t] = 0.0f;
    for (int m = 0; m < s.modes; ++m) {
        float y1 = s.y1[m], y2 = s.y2[m];
        const float a1 = s.a1[m], a2 = s.a2[m], g = s.gain[m];
        for (int t = 0; t < n; ++t) {
            const float y = a1 * y1 - a2 * y2 + g * in[t];
            y2 = y1;
            y1 = y;
            out[t] += y;
        }
        s.y1[m] = y1;
        s.y2[m] = y2;
    }
}

void meshStep(const MeshStepArgs& a) {
    for (int r = 1; r <= a.n; ++r) {
        const int base = r * a.stride;
        for (int c = 1; c <= a.n; ++c) {
            const int i = base + c;
            const float u = a.cur[i], p = a.prev[i];
            const float lu = a.cur[i - 1] + a.cur[i + 1] + a.cur[i - a.stride] + a.cur[i + a.stride] - 4.0f * u;
            const float lp = a.prev[i - 1] + a.prev[i + 1] + a.prev[i - a.stride] + a.prev[i + a.stride] - 4.0f * p;
            a.next[i] = (a.A * u + a.B * p + a.C * lu + a.D * lp) * a.mask[i];
        }
    }
}

const SimdKernels kScalar{SimdLevel::Scalar, "scalar", &addScaled, &modalBank, &meshStep};

}  // namespace

const SimdKernels& scalar() noexcept { return kScalar; }

}  // namespace whe::kernels
