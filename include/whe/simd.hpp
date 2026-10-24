// Runtime-dispatched SIMD kernels (scalar / AVX2+FMA / AVX-512F).
// Each ISA lives in its own translation unit compiled with that ISA enabled,
// and the best one the CPU *and OS* support is chosen once at startup.
// Override for testing with the environment variable WHE_SIMD=scalar|avx2|avx512.
#pragma once

#include "whe/core.hpp"

namespace whe {

enum class SimdLevel { Scalar = 0, Avx2 = 1, Avx512 = 2 };

// Bank of parallel two-pole resonators, structure-of-arrays, padded to 16 lanes.
struct ModalBankState {
    int modes = 0;
    int padded = 0;
    AlignedBuffer<float> a1, a2, gain, y1, y2;
    AlignedBuffer<float> scratch;  // kMaxBlock * 16
    void allocate(int modeCount) {
        modes = modeCount;
        padded = (modeCount + 15) & ~15;
        for (auto* b : {&a1, &a2, &gain, &y1, &y2}) b->resize(static_cast<std::size_t>(padded));
        scratch.resize(static_cast<std::size_t>(kMaxBlock) * 16);
    }
};

// One explicit time step of the damped 2-D wave equation (FDTD membrane):
//   next = A*u + B*u_prev + C*Lap(u) + D*Lap(u_prev),  then  next *= mask
struct MeshStepArgs {
    float* next;
    const float* cur;
    const float* prev;
    const float* mask;
    int n;       // interior cells per side
    int stride;  // floats per row (padded)
    float A, B, C, D;
};

struct SimdKernels {
    SimdLevel level;
    const char* name;
    void (*addScaled)(float* dst, const float* src, float gain, int n);
    // out[t] = sum over modes of resonator response to in[t]; replaces out.
    void (*modalBank)(ModalBankState& s, const float* in, float* out, int n);
    void (*meshStep)(const MeshStepArgs& a);
};

[[nodiscard]] SimdLevel detectSimdLevel() noexcept;
[[nodiscard]] const SimdKernels& simd() noexcept;  // best available, cached
[[nodiscard]] const SimdKernels* simdFor(SimdLevel level) noexcept;  // nullptr if not compiled/supported

namespace kernels {
const SimdKernels& scalar() noexcept;
const SimdKernels& avx2() noexcept;
const SimdKernels& avx512() noexcept;
}  // namespace kernels

}  // namespace whe
