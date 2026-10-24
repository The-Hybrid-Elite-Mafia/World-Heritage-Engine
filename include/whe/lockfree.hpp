// Lock-free building blocks used between the audio, worker, control and disk threads.
#pragma once

#include <memory>

#include "whe/core.hpp"

namespace whe {

// Bounded multi-producer / multi-consumer queue (Dmitry Vyukov's algorithm).
// push/pop never block and never allocate.
template <class T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacityPow2) {
        std::size_t cap = 2;
        while (cap < capacityPow2) cap <<= 1;
        cells_ = std::make_unique<Cell[]>(cap);
        mask_ = cap - 1;
        for (std::size_t i = 0; i < cap; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    bool push(const T& v) noexcept {
        std::size_t pos = enq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.value = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enq_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T& out) noexcept {
        std::size_t pos = deq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = c.value;
                    c.seq.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = deq_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Cell {
        std::atomic<std::size_t> seq{0};
        T value{};
    };
    std::unique_ptr<Cell[]> cells_;
    std::size_t mask_ = 0;
    alignas(kCacheLine) std::atomic<std::size_t> enq_{0};
    alignas(kCacheLine) std::atomic<std::size_t> deq_{0};
};

// Fixed-size block pool carved from one aligned allocation. acquire/release are
// lock-free (Treiber stack with a 32-bit ABA tag packed next to the index).
class BlockPool {
public:
    BlockPool(std::size_t blockFloats, u32 blockCount)
        : blockFloats_((blockFloats + 15) & ~std::size_t{15}), count_(blockCount), storage_(blockFloats_ * blockCount),
          next_(std::make_unique<std::atomic<u32>[]>(blockCount)) {
        for (u32 i = 0; i < blockCount; ++i) next_[i].store(i + 1 < blockCount ? i + 1 : kNil, std::memory_order_relaxed);
        head_.store(blockCount ? 0 : pack(0, kNil), std::memory_order_relaxed);
    }

    [[nodiscard]] float* acquire() noexcept {
        u64 h = head_.load(std::memory_order_acquire);
        for (;;) {
            const u32 idx = static_cast<u32>(h);
            if (idx == kNil) return nullptr;
            const u32 nxt = next_[idx].load(std::memory_order_relaxed);
            const u64 nh = pack(static_cast<u32>(h >> 32) + 1, nxt);
            if (head_.compare_exchange_weak(h, nh, std::memory_order_acq_rel, std::memory_order_acquire))
                return storage_.data() + static_cast<std::size_t>(idx) * blockFloats_;
        }
    }

    void release(float* block) noexcept {
        if (!block) return;
        const u32 idx = static_cast<u32>((block - storage_.data()) / static_cast<std::ptrdiff_t>(blockFloats_));
        u64 h = head_.load(std::memory_order_acquire);
        for (;;) {
            next_[idx].store(static_cast<u32>(h), std::memory_order_relaxed);
            const u64 nh = pack(static_cast<u32>(h >> 32) + 1, idx);
            if (head_.compare_exchange_weak(h, nh, std::memory_order_acq_rel, std::memory_order_acquire)) return;
        }
    }

    [[nodiscard]] std::size_t blockFloats() const noexcept { return blockFloats_; }
    [[nodiscard]] u32 blockCount() const noexcept { return count_; }

private:
    static constexpr u32 kNil = 0xFFFFFFFFu;
    static u64 pack(u32 tag, u32 idx) noexcept { return (static_cast<u64>(tag) << 32) | idx; }
    std::size_t blockFloats_;
    u32 count_;
    AlignedBuffer<float> storage_;
    std::unique_ptr<std::atomic<u32>[]> next_;
    alignas(kCacheLine) std::atomic<u64> head_{0};
};

// Lock-free real-time parameter: any thread writes the target, the render
// thread glides toward it per sample (no zipper noise while sculpting).
class SmoothedParam {
public:
    SmoothedParam() = default;
    explicit SmoothedParam(float initial) { reset(initial); }
    void reset(float v) noexcept {
        target_.store(v, std::memory_order_relaxed);
        current_ = v;
    }
    void set(float v) noexcept { target_.store(v, std::memory_order_relaxed); }
    [[nodiscard]] float target() const noexcept { return target_.load(std::memory_order_relaxed); }
    void setGlide(double sampleRate, double seconds) noexcept {
        coef_ = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, sampleRate * seconds)));
    }
    // Advance one block; returns the value at the end of the block.
    float advance(int samples) noexcept {
        const float t = target_.load(std::memory_order_relaxed);
        const float k = 1.0f - std::pow(1.0f - coef_, static_cast<float>(samples));
        current_ += (t - current_) * k;
        if (std::fabs(t - current_) < 1e-7f) current_ = t;
        return current_;
    }
    [[nodiscard]] float value() const noexcept { return current_; }

private:
    std::atomic<float> target_{0.0f};
    float current_ = 0.0f;
    float coef_ = 0.01f;
};

}  // namespace whe
