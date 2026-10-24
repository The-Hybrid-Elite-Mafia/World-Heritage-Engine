#include <cstdlib>
#include <string_view>

#include "whe/simd.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(WHE_X86)
#include <cpuid.h>
#endif

namespace whe {

namespace {

#ifdef WHE_X86
void cpuid(int leaf, int sub, unsigned r[4]) {
#if defined(_MSC_VER)
    int t[4];
    __cpuidex(t, leaf, sub);
    for (int i = 0; i < 4; ++i) r[i] = static_cast<unsigned>(t[i]);
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}
unsigned long long xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
}
#endif

}  // namespace

SimdLevel detectSimdLevel() noexcept {
#ifdef WHE_X86
    unsigned r1[4], r7[4];
    cpuid(0, 0, r1);
    if (r1[0] < 7) return SimdLevel::Scalar;
    cpuid(1, 0, r1);
    cpuid(7, 0, r7);
    const bool osxsave = (r1[2] >> 27) & 1;
    const bool fma = (r1[2] >> 12) & 1;
    if (!osxsave) return SimdLevel::Scalar;
    const unsigned long long xcr0 = xgetbv0();
    const bool ymm = (xcr0 & 0x6) == 0x6;
    const bool zmm = (xcr0 & 0xE6) == 0xE6;
    const bool avx2 = (r7[1] >> 5) & 1;
    const bool avx512f = (r7[1] >> 16) & 1;
    if (avx512f && zmm && fma) return SimdLevel::Avx512;
    if (avx2 && ymm && fma) return SimdLevel::Avx2;
#endif
    return SimdLevel::Scalar;
}

const SimdKernels* simdFor(SimdLevel level) noexcept {
    if (static_cast<int>(level) > static_cast<int>(detectSimdLevel())) return nullptr;
    switch (level) {
        case SimdLevel::Avx512: return &kernels::avx512();
        case SimdLevel::Avx2: return &kernels::avx2();
        default: return &kernels::scalar();
    }
}

const SimdKernels& simd() noexcept {
    static const SimdKernels* chosen = [] {
        SimdLevel lvl = detectSimdLevel();
        if (const char* env = std::getenv("WHE_SIMD")) {
            const std::string_view e(env);
            SimdLevel want = lvl;
            if (e == "scalar") want = SimdLevel::Scalar;
            else if (e == "avx2") want = SimdLevel::Avx2;
            else if (e == "avx512") want = SimdLevel::Avx512;
            if (static_cast<int>(want) <= static_cast<int>(lvl)) lvl = want;
        }
        return simdFor(lvl);
    }();
    return *chosen;
}

}  // namespace whe
