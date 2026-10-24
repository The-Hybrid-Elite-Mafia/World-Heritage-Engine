#include "whe/dfd_stream.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace whe {

// ---------------------------------------------------------------------------
// StreamCursor
// ---------------------------------------------------------------------------
bool StreamCursor::attach(DiskStreamer& streamer, BlockPool& pool) {
    streamer_ = &streamer;
    if (pool.blockFloats() < streamer.bufferFloats()) return false;
    for (auto& b : buf_) {
        b.data = pool.acquire();
        if (!b.data) return false;
    }
    return true;
}

void StreamCursor::detach(BlockPool& pool) {
    for (auto& b : buf_) {
        pool.release(b.data);
        b.data = nullptr;
    }
}

void StreamCursor::start(const StoredSample& s, bool honorLoop) {
    sample_ = &s;
    ++generation_;
    if (generation_ == 0) ++generation_;
    loop_ = honorLoop && s.hasLoop();
    loopStart_ = s.header.loopStart;
    loopEnd_ = s.header.loopEnd;
    const u64 limit = loop_ ? loopEnd_ : s.frameCount();
    preloadLimit_ = std::min<u64>(s.preloadFrames, limit);
    requestedUpTo_ = -1;
    service(0);
}

bool StreamCursor::finishedAt(i64 frame) const noexcept {
    if (!sample_) return true;
    if (loop_) return false;
    return frame >= static_cast<i64>(sample_->frameCount());
}

void StreamCursor::service(i64 frame) noexcept {
    if (!sample_ || !streamer_) return;
    const i64 B = streamer_->regionFrames_;
    const i64 S = static_cast<i64>(preloadLimit_);
    const i64 total = static_cast<i64>(sample_->frameCount());
    if (!loop_ && S >= total) return;  // whole recording is resident
    const i64 cur = frame < S ? -1 : (frame - S) / B;
    for (i64 r = std::max<i64>(cur, 0); r <= cur + 1; ++r) {
        if (r <= requestedUpTo_) continue;
        if (!loop_ && S + r * B >= total) break;
        const u8 b = static_cast<u8>(r & 1);
        const u64 t = ticket(generation_, r);
        buf_[b].want.store(t, std::memory_order_release);
        DiskStreamer::Request req;
        req.cursor = this;
        req.sample = sample_;
        req.ticket = t;
        req.region = r;
        req.streamStart = preloadLimit_;
        req.loop = loop_;
        req.loopStart = loopStart_;
        req.loopEnd = loopEnd_;
        req.buffer = b;
        if (!streamer_->post(req)) return;  // queue full: retried next block
        requestedUpTo_ = r;
    }
}

bool StreamCursor::gather(i64 first, int count, float* dst) noexcept {
    const StoredSample* s = sample_;
    if (!s) {
        std::memset(dst, 0, sizeof(float) * static_cast<std::size_t>(count) * 2);
        return true;
    }
    const u32 C = s->channels();
    const i64 B = streamer_->regionFrames_;
    const i64 S = static_cast<i64>(preloadLimit_);
    const i64 total = static_cast<i64>(s->frameCount());
    bool ok = true;
    i64 v = first;
    int remaining = count;
    float* out = dst;
    while (remaining > 0) {
        int run;
        if (v < 0) {
            run = static_cast<int>(std::min<i64>(remaining, -v));
            std::memset(out, 0, sizeof(float) * static_cast<std::size_t>(run) * C);
        } else if (v < S) {
            run = static_cast<int>(std::min<i64>(remaining, S - v));
            std::memcpy(out, s->preload + v * C, sizeof(float) * static_cast<std::size_t>(run) * C);
        } else if (!loop_ && v >= total) {
            run = remaining;
            std::memset(out, 0, sizeof(float) * static_cast<std::size_t>(run) * C);
        } else {
            const i64 region = (v - S) / B;
            const i64 offset = (v - S) % B;
            run = static_cast<int>(std::min<i64>(remaining, B - offset));
            const Buffer& b = buf_[region & 1];
            if (b.ready.load(std::memory_order_acquire) == ticket(generation_, region)) {
                std::memcpy(out, b.data + offset * C, sizeof(float) * static_cast<std::size_t>(run) * C);
            } else {
                std::memset(out, 0, sizeof(float) * static_cast<std::size_t>(run) * C);
                ok = false;
            }
        }
        v += run;
        out += static_cast<std::size_t>(run) * C;
        remaining -= run;
    }
    if (!ok) streamer_->underruns_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// ---------------------------------------------------------------------------
// DiskStreamer
// ---------------------------------------------------------------------------
DiskStreamer::DiskStreamer(u32 regionFrames, std::size_t queueCapacity)
    : regionFrames_(std::max<u32>(regionFrames, 1024)), queue_(queueCapacity) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

DiskStreamer::~DiskStreamer() {
    thread_.request_stop();
    signal_.fetch_add(1, std::memory_order_release);
    signal_.notify_one();
}

bool DiskStreamer::post(const Request& r) noexcept {
    if (!queue_.push(r)) {
        queueFull_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    posted_.fetch_add(1, std::memory_order_release);
    signal_.fetch_add(1, std::memory_order_release);
    signal_.notify_one();  // futex / WakeByAddress: a non-blocking kernel wake, never a lock
    return true;
}

void DiskStreamer::run(std::stop_token st) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    Request r;
    while (!st.stop_requested()) {
        const u32 seen = signal_.load(std::memory_order_acquire);
        bool worked = false;
        while (queue_.pop(r)) {
            fill(r);
            completed_.fetch_add(1, std::memory_order_release);
            completed_.notify_all();
            worked = true;
        }
        if (!worked && !st.stop_requested()) signal_.wait(seen, std::memory_order_acquire);
    }
}

void DiskStreamer::fill(const Request& r) noexcept {
    StreamCursor::Buffer& b = r.cursor->buf_[r.buffer];
    if (b.want.load(std::memory_order_acquire) != r.ticket) {
        stale_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const StoredSample& s = *r.sample;
    const u32 C = s.channels();
    const u64 total = s.frameCount();
    const u64 B = regionFrames_;
    const u64 v0 = r.streamStart + static_cast<u64>(r.region) * B;
    const u64 loopLen = r.loopEnd - r.loopStart;
    float* dst = b.data;
    u64 i = 0;
    u64 firstFileFrame = 0;
    while (i < B) {
        const u64 v = v0 + i;
        u64 fileFrame;
        u64 run;
        if (r.loop && v >= r.loopEnd) {
            fileFrame = r.loopStart + (v - r.loopStart) % loopLen;
            run = std::min(B - i, r.loopEnd - fileFrame);
        } else if (r.loop) {
            fileFrame = v;
            run = std::min(B - i, r.loopEnd - v);
        } else if (v >= total) {
            std::memset(dst + i * C, 0, sizeof(float) * (B - i) * C);
            break;
        } else {
            fileFrame = v;
            run = std::min(B - i, total - v);
        }
        if (i == 0) firstFileFrame = fileFrame;
        std::memcpy(dst + i * C, s.frames + fileFrame * C, sizeof(float) * run * C);  // page faults land here, on the disk thread
        i += run;
    }
    bytes_.fetch_add(B * C * sizeof(float), std::memory_order_relaxed);
    // Read-ahead for the region after this one.
    const u64 nextOffset = s.header.dataOffset + (firstFileFrame + B) * C * sizeof(float);
    s.map.prefetch(static_cast<std::size_t>(nextOffset), static_cast<std::size_t>(B * C * sizeof(float)));

    if (b.want.load(std::memory_order_acquire) == r.ticket) {
        b.ready.store(r.ticket, std::memory_order_release);
        fills_.fetch_add(1, std::memory_order_relaxed);
    } else {
        stale_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DiskStreamer::drain() const {
    for (;;) {
        const u64 c = completed_.load(std::memory_order_acquire);
        if (c >= posted_.load(std::memory_order_acquire)) return;
        completed_.wait(c, std::memory_order_acquire);
    }
}

StreamerStats DiskStreamer::stats() const noexcept {
    return {fills_.load(), stale_.load(), bytes_.load(), underruns_.load(), queueFull_.load()};
}

}  // namespace whe
