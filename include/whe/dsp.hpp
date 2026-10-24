// Small DSP building blocks shared by the hybrid micro-engines.
// None of these generate signal on their own: with zero input and zero
// state every one of them outputs exact zeros.
#pragma once

#include "whe/core.hpp"

namespace whe::dsp {

inline constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

struct OnePole {
    float a = 1.0f;
    float y = 0.0f;
    void setCutoff(float hz, float sampleRate) noexcept {
        a = 1.0f - std::exp(-kTwoPi * clampf(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
    }
    float lp(float x) noexcept { return y += a * (x - y); }
    float hp(float x) noexcept { return x - lp(x); }
    void reset() noexcept { y = 0.0f; }
};

struct DcBlocker {
    float x1 = 0.0f, y1 = 0.0f, r = 0.995f;
    void setCutoff(float hz, float sampleRate) noexcept { r = 1.0f - kTwoPi * hz / sampleRate; }
    float process(float x) noexcept {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
    void reset() noexcept { x1 = y1 = 0.0f; }
};

// Transposed direct form II biquad (RBJ cookbook designs).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float process(float x) noexcept {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0f; }
    void setLowpass(float fc, float q, float fs) noexcept { design(fc, q, fs, 0); }
    void setHighpass(float fc, float q, float fs) noexcept { design(fc, q, fs, 1); }
    void setBandpass(float fc, float q, float fs) noexcept { design(fc, q, fs, 2); }

private:
    void design(float fc, float q, float fs, int type) noexcept {
        const float w = kTwoPi * clampf(fc, 5.0f, 0.49f * fs) / fs;
        const float c = std::cos(w), s = std::sin(w), alpha = s / (2.0f * std::max(q, 0.05f));
        const float a0 = 1.0f + alpha;
        float nb0, nb1, nb2;
        if (type == 0) {
            nb0 = (1 - c) * 0.5f; nb1 = 1 - c; nb2 = nb0;
        } else if (type == 1) {
            nb0 = (1 + c) * 0.5f; nb1 = -(1 + c); nb2 = nb0;
        } else {
            nb0 = alpha; nb1 = 0; nb2 = -alpha;
        }
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0;
        a1 = -2.0f * c / a0; a2 = (1.0f - alpha) / a0;
    }
};

// Power-of-two circular delay with 3rd-order Lagrange fractional reads.
// read(d) returns the sample written d samples ago (d >= 2 for fractional reads).
class DelayLine {
public:
    void prepare(int maxDelay) {
        int size = 16;
        while (size < maxDelay + 8) size <<= 1;
        buf_.resize(static_cast<std::size_t>(size));
        mask_ = size - 1;
        w_ = 0;
    }
    void reset() noexcept { buf_.zero(); }
    void write(float x) noexcept {
        buf_[static_cast<std::size_t>(w_)] = x;
        w_ = (w_ + 1) & mask_;
    }
    [[nodiscard]] float tap(int d) const noexcept { return buf_[static_cast<std::size_t>((w_ - d) & mask_)]; }
    [[nodiscard]] float read(float d) const noexcept {
        d = clampf(d, 2.0f, static_cast<float>(mask_ - 4));
        const int D = static_cast<int>(d);
        const float t = d - static_cast<float>(D);
        const float pm1 = tap(D - 1), p0 = tap(D), p1 = tap(D + 1), p2 = tap(D + 2);
        const float wm1 = -t * (t - 1.0f) * (t - 2.0f) / 6.0f;
        const float w0 = (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
        const float w1 = -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
        const float w2 = (t + 1.0f) * t * (t - 1.0f) / 6.0f;
        return wm1 * pm1 + w0 * p0 + w1 * p1 + w2 * p2;
    }
    [[nodiscard]] int capacity() const noexcept { return mask_ - 4; }

private:
    AlignedBuffer<float> buf_;
    int mask_ = 0;
    int w_ = 0;
};

struct EnvelopeFollower {
    float att = 0.1f, rel = 0.001f, env = 0.0f;
    void setTimes(float attackMs, float releaseMs, float fs) noexcept {
        att = 1.0f - std::exp(-1.0f / std::max(1.0f, attackMs * 0.001f * fs));
        rel = 1.0f - std::exp(-1.0f / std::max(1.0f, releaseMs * 0.001f * fs));
    }
    float process(float x) noexcept {
        const float a = std::fabs(x);
        env += (a > env ? att : rel) * (a - env);
        return env;
    }
    void reset() noexcept { env = 0.0f; }
};

// Isolates the attack of a REAL recording — the hammer strike, the pluck,
// the stick hit, the tongue — so it can excite a physical resonator.
// Two mechanisms, combined with max():
//   * onset window: the first `windowMs` of the note pass, then fade out (Hann tail)
//   * flux gate: wherever the fast envelope outruns the slow envelope (a new
//     attack inside the recording), the signal passes in proportion.
class TransientExtractor {
public:
    void prepare(float fs) noexcept {
        fs_ = fs;
        fast_.setTimes(0.3f, 8.0f, fs);
        slow_.setTimes(25.0f, 120.0f, fs);
        setWindow(windowMs_);
    }
    void setWindow(float ms) noexcept {
        windowMs_ = ms;
        window_ = std::max(1, static_cast<int>(ms * 0.001f * fs_));
        fade_ = std::max(1, window_ / 2);
    }
    void setSensitivity(float s) noexcept { sensitivity_ = clampf(s, 0.0f, 4.0f); }
    void trigger() noexcept {
        pos_ = 0;
        fast_.reset();
        slow_.reset();
    }
    void reset() noexcept { trigger(); pos_ = window_ + fade_; }

    float process(float x) noexcept {
        float w = 0.0f;
        if (pos_ < window_) {
            w = 1.0f;
        } else if (pos_ < window_ + fade_) {
            const float t = static_cast<float>(pos_ - window_) / static_cast<float>(fade_);
            w = 0.5f * (1.0f + std::cos(std::numbers::pi_v<float> * t));
        }
        if (pos_ < window_ + fade_) ++pos_;
        const float f = fast_.process(x), s = slow_.process(x);
        const float flux = f > 1e-9f ? clampf((f - 1.2f * s) / f * sensitivity_, 0.0f, 1.0f) : 0.0f;
        return x * std::max(w, flux);
    }

private:
    EnvelopeFollower fast_, slow_;
    float fs_ = 48000.0f;
    float windowMs_ = 12.0f;
    float sensitivity_ = 1.0f;
    int window_ = 576, fade_ = 288;
    int pos_ = 1 << 30;
};

}  // namespace whe::dsp
