// Direct-From-Disk streaming engine.
//
//   disk (.whs cache) --mmap--> DiskStreamer thread --copy--> per-voice double buffer --> voice
//
// * Every recording is memory-mapped once (SampleStore). Its first
//   `preloadFrames` live in locked RAM, so a note-on is served instantly.
// * Each voice owns a StreamCursor with TWO region buffers taken from a
//   lock-free BlockPool. While the voice reads region k from one buffer, the
//   disk thread fills region k+1 into the other (classic double buffering).
// * Requests travel through a lock-free MPMC queue. Each buffer carries a
//   (generation, region) ticket so a fill that finishes after a voice was
//   stolen and restarted is discarded instead of corrupting the new note.
// * Page faults only ever happen on the disk thread; the audio threads read
//   nothing but preload RAM and filled buffers.
// * Sustain loops embedded in the recording are honored by streaming a
//   "virtual timeline" that wraps inside the loop.
#pragma once

#include <thread>

#include "whe/lockfree.hpp"
#include "whe/sample_store.hpp"

namespace whe {

class DiskStreamer;

class StreamCursor {
public:
    StreamCursor() = default;
    StreamCursor(const StreamCursor&) = delete;
    StreamCursor& operator=(const StreamCursor&) = delete;

    // Load-time setup: takes two region buffers from the pool.
    [[nodiscard]] bool attach(DiskStreamer& streamer, BlockPool& pool);
    void detach(BlockPool& pool);

    // Real-time safe. Begins streaming `sample` from frame 0.
    void start(const StoredSample& sample, bool honorLoop);
    void stop() noexcept { sample_ = nullptr; }

    // Real-time safe. Makes sure the region containing `frame` and the next
    // one are requested. Call once per block before gather().
    void service(i64 frame) noexcept;

    // Real-time safe. Copies virtual frames [first, first+count) into dst
    // (interleaved, sample's channel count). Frames that are not yet on hand
    // are zero-filled and counted as an underrun. Returns false on underrun.
    bool gather(i64 first, int count, float* dst) noexcept;

    // True once `frame` lies past the end of a non-looping recording.
    [[nodiscard]] bool finishedAt(i64 frame) const noexcept;
    [[nodiscard]] const StoredSample* sample() const noexcept { return sample_; }

private:
    friend class DiskStreamer;
    struct Buffer {
        float* data = nullptr;
        std::atomic<u64> want{0};
        std::atomic<u64> ready{0};
    };
    static u64 ticket(u32 generation, i64 region) noexcept { return (static_cast<u64>(generation) << 32) | static_cast<u64>(region + 1); }

    DiskStreamer* streamer_ = nullptr;
    const StoredSample* sample_ = nullptr;
    Buffer buf_[2];
    u32 generation_ = 0;
    bool loop_ = false;
    u64 loopStart_ = 0, loopEnd_ = 0;
    u64 preloadLimit_ = 0;   // virtual frames [0, preloadLimit_) come from resident RAM
    i64 requestedUpTo_ = -1; // highest region already requested
};

struct StreamerStats {
    u64 fills = 0;
    u64 staleFills = 0;
    u64 bytesCopied = 0;
    u64 underruns = 0;
    u64 queueFull = 0;
};

class DiskStreamer {
public:
    // regionFrames: frames per double-buffer half (default 16384 ≈ 0.34 s @ 48 kHz).
    explicit DiskStreamer(u32 regionFrames = 16384, std::size_t queueCapacity = 8192);
    ~DiskStreamer();
    DiskStreamer(const DiskStreamer&) = delete;
    DiskStreamer& operator=(const DiskStreamer&) = delete;

    [[nodiscard]] u32 regionFrames() const noexcept { return regionFrames_; }
    // Floats needed per region buffer (stereo).
    [[nodiscard]] std::size_t bufferFloats() const noexcept { return static_cast<std::size_t>(regionFrames_) * 2; }

    // Blocks until every queued fill has been completed (offline rendering / tests).
    void drain() const;
    [[nodiscard]] StreamerStats stats() const noexcept;

private:
    friend class StreamCursor;
    struct Request {
        StreamCursor* cursor = nullptr;
        const StoredSample* sample = nullptr;
        u64 ticket = 0;
        i64 region = 0;
        u64 streamStart = 0;
        u64 loopStart = 0, loopEnd = 0;
        u8 buffer = 0;
        bool loop = false;
    };
    bool post(const Request& r) noexcept;
    void run(std::stop_token st);
    void fill(const Request& r) noexcept;

    u32 regionFrames_;
    MpmcQueue<Request> queue_;
    alignas(kCacheLine) std::atomic<u32> signal_{0};
    alignas(kCacheLine) std::atomic<u64> posted_{0};
    alignas(kCacheLine) std::atomic<u64> completed_{0};
    std::atomic<u64> fills_{0}, stale_{0}, bytes_{0}, underruns_{0}, queueFull_{0};
    std::jthread thread_;
};

}  // namespace whe
