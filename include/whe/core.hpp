// The World Heritage Engine — core types shared by every subsystem.
// Pure C++23, no third-party code.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define WHE_X86 1
#endif

namespace whe {

namespace fs = std::filesystem;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

struct Error {
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(std::string message) {
    return std::unexpected(Error{std::move(message)});
}

inline constexpr std::size_t kCacheLine = 64;
inline constexpr int kMaxBlock = 4096;        // largest audio block any engine accepts
inline constexpr double kMaxPitchRatio = 8.0;  // max playback-rate ratio (≈ +3 octaves incl. SR conversion)

// ---------------------------------------------------------------------------
// Aligned heap buffer (64-byte, AVX-512 friendly). Allocation happens only at
// prepare/load time, never on the audio thread.
// ---------------------------------------------------------------------------
template <class T, std::size_t Align = 64>
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(std::size_t count) { resize(count); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& o) noexcept : ptr_(std::exchange(o.ptr_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            release();
            ptr_ = std::exchange(o.ptr_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    ~AlignedBuffer() { release(); }

    void resize(std::size_t count) {
        release();
        if (count == 0) return;
        const std::size_t bytes = ((count * sizeof(T) + Align - 1) / Align) * Align;
        ptr_ = static_cast<T*>(::operator new(bytes, std::align_val_t{Align}));
        size_ = count;
        std::memset(static_cast<void*>(ptr_), 0, bytes);
    }
    void zero() noexcept {
        if (ptr_) std::memset(static_cast<void*>(ptr_), 0, size_ * sizeof(T));
    }

    T* data() noexcept { return ptr_; }
    const T* data() const noexcept { return ptr_; }
    std::size_t size() const noexcept { return size_; }
    T& operator[](std::size_t i) noexcept { return ptr_[i]; }
    const T& operator[](std::size_t i) const noexcept { return ptr_[i]; }
    std::span<T> span() noexcept { return {ptr_, size_}; }

private:
    void release() noexcept {
        if (ptr_) ::operator delete(static_cast<void*>(ptr_), std::align_val_t{Align});
        ptr_ = nullptr;
        size_ = 0;
    }
    T* ptr_ = nullptr;
    std::size_t size_ = 0;
};

// Flush-to-zero / denormals-are-zero for the lifetime of the guard. Every
// render thread installs one so decaying resonators never hit denormal stalls.
class DenormalGuard {
public:
    DenormalGuard() noexcept {
#ifdef WHE_X86
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);  // FTZ (bit 15) | DAZ (bit 6)
#endif
    }
    ~DenormalGuard() {
#ifdef WHE_X86
        _mm_setcsr(saved_);
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
    unsigned saved_ = 0;
};

inline void cpuRelax() noexcept {
#ifdef WHE_X86
    _mm_pause();
#endif
}

[[nodiscard]] inline double midiToHz(double note) noexcept { return 440.0 * std::exp2((note - 69.0) / 12.0); }
[[nodiscard]] inline float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
[[nodiscard]] inline float clampf(float v, float lo, float hi) noexcept { return std::min(hi, std::max(lo, v)); }

// Speed of sound in air at ~20 °C, used by the aerophone bore model.
inline constexpr double kSpeedOfSound = 343.0;

}  // namespace whe
