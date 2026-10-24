// Bare-metal ASIO output (Windows only). Built only when the Steinberg ASIO SDK
// is present at configure time (WHE_HAS_ASIO=1); the SDK is downloaded from
// Steinberg by tools/fetch-asio-sdk.ps1 and is never committed.
#pragma once

#include <functional>
#include <vector>

#include "whe/core.hpp"

namespace whe {

class AsioOutput {
public:
    // Called on the driver's buffer-switch thread; must be real-time safe.
    using Callback = std::function<void(float* L, float* R, int frames)>;

    struct Options {
        std::string driver;      // registry name of the driver; empty = first installed driver
        int bufferFrames = 0;    // 0 = driver's preferred size
        double sampleRate = 0;   // 0 = keep the driver's current rate
        int firstOutput = 0;     // output channel pair to use (0 = outputs 1/2)
    };

    // Installed 64-bit ASIO drivers (HKLM\SOFTWARE\ASIO).
    [[nodiscard]] static std::vector<std::string> listDrivers();

    AsioOutput() = default;
    ~AsioOutput();
    AsioOutput(const AsioOutput&) = delete;
    AsioOutput& operator=(const AsioOutput&) = delete;

    [[nodiscard]] Result<void> open(const Options& opts);
    [[nodiscard]] Result<void> start(Callback cb);
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int bufferFrames() const noexcept { return bufferFrames_; }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }

    struct Impl;  // public so the driver's C callbacks can reach it

private:
    Impl* impl_ = nullptr;
    double sampleRate_ = 0;
    int bufferFrames_ = 0;
    std::string description_;
};

}  // namespace whe
