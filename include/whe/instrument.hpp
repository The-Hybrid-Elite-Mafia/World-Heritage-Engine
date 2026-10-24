// InstrumentEngine — base of the "one instrument, one engine" architecture.
//
// Every concrete engine (piano, bowed string, plucked string, aerophone,
// membrane, …) is its own class with its own physics, but they all share one
// non-negotiable rule enforced here: the ONLY sound source is SampleVoice
// playback of real recordings from a SampleLibrary. The physics hooks receive
// that recorded audio and may only reshape it:
//
//   handleNoteOn  -> pick recorded zone / layer / round robin, start SampleVoice(s)
//   processVoice  -> per-voice physics driven by that voice's recording
//   postProcess   -> instrument-level physics (one body, one drumhead,
//                    one set of sympathetic strings) driven by the summed recordings
#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "whe/lockfree.hpp"
#include "whe/voice.hpp"

namespace whe {

struct NoteEvent {
    enum class Type : u8 { NoteOn, NoteOff, Sustain, AllNotesOff, PitchBend };
    Type type = Type::NoteOn;
    u8 note = 0;
    u8 velocity = 0;
    float value = 0.0f;  // Sustain: 0/1, PitchBend: semitones
};

struct ParamInfo {
    const char* name;
    float min, max, def;
    const char* unit;
    const char* physics;  // what the parameter changes physically
};

struct EngineConfig {
    int polyphony = 48;
    float releaseSeconds = 0.4f;
    std::string articulation;   // empty: library's primary articulation
    bool useReleaseSamples = true;
};

class InstrumentEngine {
public:
    virtual ~InstrumentEngine();
    InstrumentEngine(const InstrumentEngine&) = delete;
    InstrumentEngine& operator=(const InstrumentEngine&) = delete;

    // Control thread. Allocates voices, stream buffers and physics state.
    [[nodiscard]] Result<void> prepare(double sampleRate, int maxBlock);

    // Any thread, lock-free.
    bool post(const NoteEvent& e) noexcept { return events_.push(e); }
    void setParam(int index, float value) noexcept;
    [[nodiscard]] float paramTarget(int index) const noexcept;
    [[nodiscard]] int paramIndex(std::string_view name) const noexcept;

    // Render thread. Writes n frames (replaces content). Real-time safe.
    void render(float* outL, float* outR, int n) noexcept;

    [[nodiscard]] virtual const char* kind() const noexcept = 0;
    [[nodiscard]] std::span<const ParamInfo> params() const noexcept { return paramInfo_; }
    [[nodiscard]] const SampleLibrary& library() const noexcept { return *lib_; }
    [[nodiscard]] const Articulation& articulation() const noexcept { return *art_; }
    [[nodiscard]] int activeVoices() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

protected:
    InstrumentEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& streamer, EngineConfig cfg, std::span<const ParamInfo> params);

    struct VoiceInfo {
        int note = -1;
        int velocity = 0;
        u64 age = 0;
        bool held = false;
        bool sustained = false;
        bool releaseVoice = false;
        bool tail = false;          // recording finished; physics ringing out
        float tailEnv = 0.0f;
        int group = -1;             // voices started together for one note (layer morph)
        int layerSlot = 0;
        double baseRatio = 1.0;
        const Zone* zone = nullptr;
    };

    // --- hooks for concrete engines ---
    virtual void onPrepare() = 0;
    virtual void onVoiceStart(int /*voice*/, const VoiceInfo& /*info*/) noexcept {}
    virtual void onBlock(int /*n*/) noexcept {}
    virtual void processVoice(int /*voice*/, float* /*L*/, float* /*R*/, int /*n*/) noexcept {}
    virtual void postProcess(float* /*L*/, float* /*R*/, int /*n*/) noexcept {}
    [[nodiscard]] virtual float tailSeconds() const noexcept { return 0.0f; }
    // Default: one recorded zone per note (velocity layer + round robin).
    virtual void handleNoteOn(int note, int velocity) noexcept;

    // Starts a SampleVoice on a recorded zone; returns the voice index or -1.
    int startVoice(const Zone& zone, int note, int velocity, float gain, bool releaseVoice, int group = -1, int layerSlot = 0) noexcept;
    [[nodiscard]] float p(int i) const noexcept { return params_[static_cast<std::size_t>(i)].value(); }
    [[nodiscard]] float noteHz(int voice) const noexcept;  // note pitch incl. bend
    [[nodiscard]] SampleVoice& voice(int v) noexcept { return voices_[static_cast<std::size_t>(v)]; }
    [[nodiscard]] VoiceInfo& info(int v) noexcept { return info_[static_cast<std::size_t>(v)]; }
    [[nodiscard]] int voiceCount() const noexcept { return voiceCount_; }
    [[nodiscard]] float bendSemitones() const noexcept { return bend_; }
    [[nodiscard]] bool sustainPedal() const noexcept { return sustain_; }

    std::shared_ptr<const SampleLibrary> lib_;
    const Articulation* art_ = nullptr;
    const Articulation* releaseArt_ = nullptr;
    DiskStreamer& streamer_;
    EngineConfig cfg_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    int nextGroup_ = 0;

private:
    void handle(const NoteEvent& e) noexcept;
    void noteOff(int note) noexcept;
    void renderChunk(float* L, float* R, int n) noexcept;
    int allocateVoice() noexcept;

    std::span<const ParamInfo> paramInfo_;
    std::unique_ptr<SmoothedParam[]> params_;
    MpmcQueue<NoteEvent> events_{1024};
    std::unique_ptr<BlockPool> pool_;
    std::unique_ptr<SampleVoice[]> voices_;
    std::vector<VoiceInfo> info_;
    std::vector<u32> rrCounter_;
    AlignedBuffer<float> vL_, vR_;
    int voiceCount_ = 0;
    u64 clock_ = 0;
    float bend_ = 0.0f;
    bool sustain_ = false;
    bool prepared_ = false;
};

enum class EngineKind { Piano, BowedString, PluckedString, Aerophone, Membrane };

[[nodiscard]] std::optional<EngineKind> engineKindFromName(std::string_view name) noexcept;
[[nodiscard]] const char* engineKindName(EngineKind k) noexcept;

// The only way to build an instrument: from a SampleLibrary of real recordings.
[[nodiscard]] Result<std::unique_ptr<InstrumentEngine>> createInstrument(EngineKind kind, std::shared_ptr<const SampleLibrary> library,
                                                                         DiskStreamer& streamer, EngineConfig cfg = {});

}  // namespace whe
