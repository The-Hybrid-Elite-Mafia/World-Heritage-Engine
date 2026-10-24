// InstrumentManager — runs thousands of independent instrument engines in
// parallel, every audio block, without locks.
//
// * Instruments live in fixed slots published with atomic pointers; adding or
//   removing one never blocks the audio thread (removal is deferred until two
//   blocks have passed, so no worker can still be touching it).
// * Each block the audio thread publishes a (sequence, index) job counter.
//   Worker threads and the audio thread itself claim instruments with a CAS
//   on that counter, render them into per-slot buffers, and the audio thread
//   sums the finished instrument outputs onto the device buffer.
// * Workers sleep on std::atomic::wait between blocks (futex /
//   WaitOnAddress), so idle CPU stays idle.
// This is output summing for the device only — there is no mixer, no tracks,
// no stems.
#pragma once

#include <thread>

#include "whe/instrument.hpp"

namespace whe {

class InstrumentManager {
public:
    // workers: render threads in addition to the audio thread (0 = hardware_concurrency - 1)
    explicit InstrumentManager(unsigned workers = 0, int maxInstruments = 4096, u32 streamRegionFrames = 16384);
    ~InstrumentManager();
    InstrumentManager(const InstrumentManager&) = delete;
    InstrumentManager& operator=(const InstrumentManager&) = delete;

    void setAudioFormat(double sampleRate, int maxBlock) noexcept {
        sampleRate_ = sampleRate;
        maxBlock_ = maxBlock;
    }
    [[nodiscard]] DiskStreamer& streamer() noexcept { return streamer_; }

    // Control thread: prepares the engine for the current format and publishes it.
    [[nodiscard]] Result<int> add(std::unique_ptr<InstrumentEngine> engine);
    void remove(int id);
    void collectGarbage();
    [[nodiscard]] InstrumentEngine* get(int id) noexcept;
    [[nodiscard]] int count() const noexcept;

    // Audio thread. Renders every instrument and writes the summed output (replaces content).
    void process(float* outL, float* outR, int n) noexcept;

    [[nodiscard]] unsigned workerCount() const noexcept { return static_cast<unsigned>(workers_.size()); }

private:
    struct Slot {
        std::atomic<InstrumentEngine*> engine{nullptr};
        AlignedBuffer<float> L, R;
        bool rendered = false;
    };
    void workerLoop(std::stop_token st);
    void renderJobs(u32 seq) noexcept;

    DiskStreamer streamer_;  // declared first: destroyed after every engine
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    int maxInstruments_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::unique_ptr<InstrumentEngine>> owned_;  // control-thread ownership, index = slot
    struct Retired {
        std::unique_ptr<InstrumentEngine> engine;
        u64 afterBlock;
    };
    std::vector<Retired> retired_;
    std::atomic<int> highWater_{0};

    alignas(kCacheLine) std::atomic<u64> jobs_{0};      // (seq << 32) | next index
    alignas(kCacheLine) std::atomic<int> done_{0};
    alignas(kCacheLine) std::atomic<u32> blockSeq_{0};
    std::atomic<u64> processed_{0};
    int blockN_ = 0;
    int blockTotal_ = 0;
    std::vector<std::jthread> workers_;
};

}  // namespace whe
