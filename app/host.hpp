// AudioHost — owns the sound card connection (ASIO or WASAPI), the instrument
// manager, the one instrument being played, MIDI input, and the live numbers the
// status bar and meters show. Everything here is plain Win32 + the engine.
#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "whe/engines.hpp"
#include "whe/instrument_manager.hpp"
#include "whe/platform/wasapi_output.hpp"
#ifdef WHE_HAS_ASIO
#include "whe/platform/asio_output.hpp"
#endif

namespace whe::ui {

struct DeviceChoice {
    bool asio = true;           // false: Windows audio (WASAPI)
    std::string asioDriver;     // empty: first installed driver
    bool exclusive = false;     // WASAPI exclusive mode
    int bufferFrames = 0;       // 0: driver default
    double sampleRate = 0;      // 0: keep the driver's rate
};

class AudioHost {
public:
    AudioHost();
    ~AudioHost();

    [[nodiscard]] static bool asioBuilt() noexcept;
    [[nodiscard]] static std::vector<std::string> asioDrivers();

    // Opens (or re-opens) the sound card. If an instrument is loaded it is
    // rebuilt for the new sample rate. On failure the host is left closed.
    Result<void> openDevice(const DeviceChoice& choice);
    void closeDevice();
    [[nodiscard]] bool deviceOpen() const noexcept { return running_; }
    [[nodiscard]] const DeviceChoice& device() const noexcept { return choice_; }
    [[nodiscard]] double sampleRate() const noexcept { return rate_; }
    [[nodiscard]] int bufferFrames() const noexcept { return buffer_; }
    [[nodiscard]] const std::string& deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    // UI thread: makes `lib` the playing instrument (old one is retired safely).
    Result<void> setInstrument(EngineKind kind, std::shared_ptr<const SampleLibrary> lib);
    [[nodiscard]] InstrumentEngine* engine() noexcept { return engine_; }
    [[nodiscard]] EngineKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::shared_ptr<const SampleLibrary>& library() const noexcept { return lib_; }

    // Notes from the UI (mouse, PC keyboard) — any thread.
    void noteOn(int note, int velocity);
    void noteOff(int note);
    void sustain(bool down);
    void allNotesOff();

    // MIDI input (all devices). Fills `heldNotes` and posts `notifyMsg` to `notifyWnd`.
    void openMidi(void* notifyWnd, unsigned notifyMsg);
    void closeMidi();
    [[nodiscard]] int midiDeviceCount() const noexcept { return static_cast<int>(midiIn_.size()); }

    // Drum kits: keys without a recording play the kit's hits in turn, so every key sounds.
    [[nodiscard]] int mapNote(int note) const noexcept {
        return note < 0 || note > 127 ? note : static_cast<int>(remap_[static_cast<std::size_t>(note)].load(std::memory_order_relaxed));
    }

    // Notes currently down from any source (UI or MIDI), for lighting keys.
    [[nodiscard]] bool noteHeld(int note) const noexcept;
    [[nodiscard]] bool midiSustain() const noexcept { return midiSustain_.load(std::memory_order_relaxed); }

    // Live numbers (UI thread reads).
    struct Live {
        float peakL = 0, peakR = 0;  // linear, since last read
        float cpu = 0;               // audio callback time / buffer time, 0..1+
        int voices = 0;
        int polyphony = 48;
        u64 diskBytes = 0;
        u64 underruns = 0;
        float limitDb = 0;           // deepest safety-limiter gain reduction since last read (dB, <= 0)
    };
    [[nodiscard]] Live readLive();

    void collectGarbage() { mgr_->collectGarbage(); }

    // MIDI callback entry (public for the C callback).
    void onMidi(unsigned long msg) noexcept;

private:
    void render(float* L, float* R, int n) noexcept;
    Result<void> startStream();
    void stopStream();
    void setHeld(int note, bool on) noexcept;

    std::unique_ptr<InstrumentManager> mgr_;
    WasapiOutput wasapi_;
#ifdef WHE_HAS_ASIO
    AsioOutput asio_;
#endif
    DeviceChoice choice_;
    bool running_ = false;
    bool usingAsio_ = false;
    double rate_ = 48000;
    int buffer_ = 512;
    std::string deviceName_, lastError_;

    std::shared_ptr<const SampleLibrary> lib_;
    EngineKind kind_ = EngineKind::Piano;
    int instId_ = -1;
    InstrumentEngine* engine_ = nullptr;
    std::atomic<InstrumentEngine*> live_{nullptr};  // what MIDI/UI threads post to

    std::atomic<float> peakL_{0}, peakR_{0}, cpu_{0};
    // Output safety limiter: never lets the sum exceed -0.5 dBFS at the sound card.
    float limGain_ = 1.0f;
    std::atomic<float> limMin_{1.0f};
    std::atomic<u64> held_[2]{};  // 128-bit held-note mask
    std::array<std::atomic<u8>, 128> remap_{};
    std::atomic<bool> midiSustain_{false};
    std::vector<void*> midiIn_;
    void* notifyWnd_ = nullptr;
    unsigned notifyMsg_ = 0;
    long long qpcFreq_ = 1;
};

}  // namespace whe::ui
