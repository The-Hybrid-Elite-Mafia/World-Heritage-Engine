// SampleVoice — plays ONE real recording, streamed direct-from-disk, with
// pitch (cubic Hermite interpolation), velocity gain and release envelope.
// This is the only sound source in the engine. Every physical model is fed
// from a SampleVoice's output.
#pragma once

#include "whe/dfd_stream.hpp"
#include "whe/sample_library.hpp"

namespace whe {

class SampleVoice {
public:
    SampleVoice() = default;
    SampleVoice(const SampleVoice&) = delete;
    SampleVoice& operator=(const SampleVoice&) = delete;

    // Load time: allocates the interpolation window and takes stream buffers.
    [[nodiscard]] bool prepare(DiskStreamer& streamer, BlockPool& pool, int maxBlock);
    void unprepare(BlockPool& pool);

    // Real-time safe.
    void start(const StoredSample& sample, double playbackRatio, float gain, bool honorLoop) noexcept;
    void release(float seconds, double outRate) noexcept;
    void kill(double outRate) noexcept;  // ~4 ms fade for voice stealing
    void setGain(float g) noexcept { gainTarget_ = g; }
    void setRatio(double r) noexcept { ratio_ = std::min(r, kMaxPitchRatio); }

    // Writes n frames of the recording into outL/outR (replaces their content).
    // Returns false when the voice has finished (outputs are then silent).
    bool render(float* outL, float* outR, int n) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool releasing() const noexcept { return releasing_; }
    [[nodiscard]] double position() const noexcept { return pos_; }
    [[nodiscard]] const StoredSample* sample() const noexcept { return cursor_.sample(); }
    // Release-envelope values at the start / end of the last rendered block.
    // Engines apply the same ramp to their physics so a key release damps the
    // modeled string exactly like it damps the recording.
    [[nodiscard]] float envelopeAtBlockStart() const noexcept { return envStartOfBlock_; }
    [[nodiscard]] float envelopeAtBlockEnd() const noexcept { return env_; }

private:
    StreamCursor cursor_;
    AlignedBuffer<float> window_;
    int windowFrames_ = 0;
    double pos_ = 0.0;
    double ratio_ = 1.0;
    float gain_ = 1.0f, gainTarget_ = 1.0f;
    float env_ = 1.0f;
    float envStartOfBlock_ = 1.0f;
    float releaseMul_ = 1.0f;
    bool active_ = false;
    bool releasing_ = false;
};

}  // namespace whe
