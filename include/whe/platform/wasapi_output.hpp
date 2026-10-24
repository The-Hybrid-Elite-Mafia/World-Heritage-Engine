// Bare-metal WASAPI output (Windows only): event-driven, shared or exclusive
// mode, MMCSS "Pro Audio" thread, no framework.
#pragma once

#include <functional>
#include <thread>

#include "whe/core.hpp"

namespace whe {

class WasapiOutput {
public:
    // Called on the audio thread; must be real-time safe. Write `frames` samples to L and R.
    using Callback = std::function<void(float* L, float* R, int frames)>;

    struct Options {
        bool exclusive = false;
        int periodFrames = 0;  // exclusive mode: 0 = device minimum period
    };

    WasapiOutput() = default;
    ~WasapiOutput();
    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    [[nodiscard]] Result<void> open(const Options& opts);
    [[nodiscard]] Result<void> start(Callback cb);
    void stop();

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int bufferFrames() const noexcept { return bufferFrames_; }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }

private:
    void run(std::stop_token st);
    struct Impl;
    Impl* impl_ = nullptr;
    double sampleRate_ = 0;
    int bufferFrames_ = 0;
    std::string description_;
    Callback cb_;
    std::jthread thread_;
};

}  // namespace whe
