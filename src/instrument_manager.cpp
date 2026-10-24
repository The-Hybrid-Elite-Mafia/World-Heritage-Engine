#include "whe/instrument_manager.hpp"

#include "whe/simd.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#endif

namespace whe {

InstrumentManager::InstrumentManager(unsigned workers, int maxInstruments, u32 streamRegionFrames)
    : streamer_(streamRegionFrames), maxInstruments_(std::max(1, maxInstruments)) {
    slots_.reserve(static_cast<std::size_t>(maxInstruments_));
    for (int i = 0; i < maxInstruments_; ++i) slots_.push_back(std::make_unique<Slot>());
    owned_.resize(static_cast<std::size_t>(maxInstruments_));
    if (workers == 0) workers = std::max(1u, std::thread::hardware_concurrency()) - 1;
    for (unsigned w = 0; w < workers; ++w) workers_.emplace_back([this](std::stop_token st) { workerLoop(st); });
}

InstrumentManager::~InstrumentManager() {
    for (auto& w : workers_) w.request_stop();
    blockSeq_.fetch_add(1, std::memory_order_release);
    blockSeq_.notify_all();
    workers_.clear();  // joins
    for (auto& s : slots_) s->engine.store(nullptr);
    retired_.clear();
    owned_.clear();
}

Result<int> InstrumentManager::add(std::unique_ptr<InstrumentEngine> engine) {
    if (!engine) return fail("null instrument");
    int slot = -1;
    for (int i = 0; i < maxInstruments_; ++i)
        if (!owned_[static_cast<std::size_t>(i)]) {
            slot = i;
            break;
        }
    if (slot < 0) return fail("instrument limit reached");
    if (auto r = engine->prepare(sampleRate_, maxBlock_); !r) return std::unexpected(r.error());
    Slot& s = *slots_[static_cast<std::size_t>(slot)];
    s.L.resize(static_cast<std::size_t>(maxBlock_));
    s.R.resize(static_cast<std::size_t>(maxBlock_));
    InstrumentEngine* raw = engine.get();
    owned_[static_cast<std::size_t>(slot)] = std::move(engine);
    s.engine.store(raw, std::memory_order_release);
    int hw = highWater_.load();
    while (hw < slot + 1 && !highWater_.compare_exchange_weak(hw, slot + 1)) {
    }
    return slot;
}

void InstrumentManager::remove(int id) {
    if (id < 0 || id >= maxInstruments_ || !owned_[static_cast<std::size_t>(id)]) return;
    slots_[static_cast<std::size_t>(id)]->engine.store(nullptr, std::memory_order_release);
    retired_.push_back({std::move(owned_[static_cast<std::size_t>(id)]), processed_.load() + 2});
}

void InstrumentManager::collectGarbage() {
    const u64 now = processed_.load();
    std::erase_if(retired_, [&](const Retired& r) { return now >= r.afterBlock; });
}

InstrumentEngine* InstrumentManager::get(int id) noexcept {
    if (id < 0 || id >= maxInstruments_) return nullptr;
    return owned_[static_cast<std::size_t>(id)].get();
}

int InstrumentManager::count() const noexcept {
    int n = 0;
    for (const auto& o : owned_) n += o ? 1 : 0;
    return n;
}

void InstrumentManager::renderJobs(u32 seq) noexcept {
    DenormalGuard ftz;
    u64 j = jobs_.load(std::memory_order_acquire);
    for (;;) {
        if (static_cast<u32>(j >> 32) != seq) return;
        const int idx = static_cast<int>(j & 0xFFFFFFFFu);
        if (idx >= blockTotal_) return;
        if (!jobs_.compare_exchange_weak(j, j + 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
        Slot& s = *slots_[static_cast<std::size_t>(idx)];
        InstrumentEngine* e = s.engine.load(std::memory_order_acquire);
        s.rendered = e != nullptr;
        if (e) e->render(s.L.data(), s.R.data(), blockN_);
        done_.fetch_add(1, std::memory_order_acq_rel);
        j = jobs_.load(std::memory_order_acquire);
    }
}

void InstrumentManager::workerLoop(std::stop_token st) {
#ifdef _WIN32
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
#endif
    u32 last = blockSeq_.load(std::memory_order_acquire);
    while (!st.stop_requested()) {
        blockSeq_.wait(last, std::memory_order_acquire);
        if (st.stop_requested()) break;
        last = blockSeq_.load(std::memory_order_acquire);
        renderJobs(last);
    }
#ifdef _WIN32
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
#endif
}

void InstrumentManager::process(float* outL, float* outR, int n) noexcept {
    std::memset(outL, 0, sizeof(float) * static_cast<std::size_t>(n));
    std::memset(outR, 0, sizeof(float) * static_cast<std::size_t>(n));
    const SimdKernels& k = simd();
    int done = 0;
    while (done < n) {
        const int chunk = std::min(maxBlock_, n - done);
        blockN_ = chunk;
        blockTotal_ = highWater_.load(std::memory_order_acquire);
        done_.store(0, std::memory_order_relaxed);
        const u32 seq = blockSeq_.load(std::memory_order_relaxed) + 1;
        jobs_.store(static_cast<u64>(seq) << 32, std::memory_order_release);
        blockSeq_.store(seq, std::memory_order_release);
        if (!workers_.empty() && blockTotal_ > 1) blockSeq_.notify_all();
        renderJobs(seq);  // the audio thread works too
        while (done_.load(std::memory_order_acquire) < blockTotal_) cpuRelax();
        for (int i = 0; i < blockTotal_; ++i) {
            Slot& s = *slots_[static_cast<std::size_t>(i)];
            if (!s.rendered) continue;
            k.addScaled(outL + done, s.L.data(), 1.0f, chunk);
            k.addScaled(outR + done, s.R.data(), 1.0f, chunk);
        }
        processed_.fetch_add(1, std::memory_order_release);
        done += chunk;
    }
}

}  // namespace whe
