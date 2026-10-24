#include "whe/voice.hpp"

namespace whe {

bool SampleVoice::prepare(DiskStreamer& streamer, BlockPool& pool, int maxBlock) {
    windowFrames_ = static_cast<int>(std::ceil(kMaxPitchRatio * maxBlock)) + 8;
    if (windowFrames_ > static_cast<int>(streamer.regionFrames())) return false;  // window must fit inside one region
    window_.resize(static_cast<std::size_t>(windowFrames_) * 2);
    return cursor_.attach(streamer, pool);
}

void SampleVoice::unprepare(BlockPool& pool) { cursor_.detach(pool); }

void SampleVoice::start(const StoredSample& sample, double ratio, float gain, bool honorLoop) noexcept {
    cursor_.start(sample, honorLoop);
    pos_ = 0.0;
    ratio_ = std::min(ratio, kMaxPitchRatio);
    gain_ = gainTarget_ = gain;
    env_ = 1.0f;
    envStartOfBlock_ = 1.0f;
    releaseMul_ = 1.0f;
    releasing_ = false;
    active_ = true;
}

void SampleVoice::release(float seconds, double outRate) noexcept {
    if (!active_ || releasing_) return;
    releasing_ = true;
    // exp decay reaching -60 dB after `seconds`
    releaseMul_ = static_cast<float>(std::pow(1e-3, 1.0 / std::max(1.0, seconds * outRate)));
}

void SampleVoice::kill(double outRate) noexcept {
    if (!active_) return;
    releasing_ = true;
    releaseMul_ = static_cast<float>(std::pow(1e-3, 1.0 / std::max(1.0, 0.004 * outRate)));
}

bool SampleVoice::render(float* outL, float* outR, int n) noexcept {
    if (!active_) {
        std::memset(outL, 0, sizeof(float) * static_cast<std::size_t>(n));
        std::memset(outR, 0, sizeof(float) * static_cast<std::size_t>(n));
        return false;
    }
    const StoredSample* s = cursor_.sample();
    const u32 C = s->channels();
    // Window of source frames needed for 4-point Hermite over this block.
    const i64 first = static_cast<i64>(std::floor(pos_)) - 1;
    const i64 last = static_cast<i64>(std::floor(pos_ + ratio_ * (n - 1))) + 2;
    const int count = static_cast<int>(last - first + 1);
    cursor_.service(std::max<i64>(first, 0));
    cursor_.gather(first, count, window_.data());

    envStartOfBlock_ = env_;
    const float* w = window_.data();
    const float gStep = (gainTarget_ - gain_) / static_cast<float>(n);
    double p = pos_;
    for (int i = 0; i < n; ++i) {
        const i64 ip = static_cast<i64>(std::floor(p));
        const float f = static_cast<float>(p - static_cast<double>(ip));
        const std::size_t base = static_cast<std::size_t>(ip - 1 - first);
        float out[2] = {0.0f, 0.0f};
        for (u32 c = 0; c < std::min<u32>(C, 2); ++c) {
            const float xm1 = w[(base + 0) * C + c], x0 = w[(base + 1) * C + c], x1 = w[(base + 2) * C + c], x2 = w[(base + 3) * C + c];
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            out[c] = ((c3 * f + c2) * f + c1) * f + x0;
        }
        if (C == 1) out[1] = out[0];
        gain_ += gStep;
        if (releasing_) env_ *= releaseMul_;
        const float g = gain_ * env_;
        outL[i] = out[0] * g;
        outR[i] = out[1] * g;
        p += ratio_;
    }
    gain_ = gainTarget_;
    pos_ = p;
    if ((releasing_ && env_ < 1e-5f) || cursor_.finishedAt(static_cast<i64>(std::floor(pos_)) - 1)) {
        active_ = false;
        cursor_.stop();
    }
    return true;
}

}  // namespace whe
