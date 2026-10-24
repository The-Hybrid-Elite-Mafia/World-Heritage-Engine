// Bespoke instrument engines. Each one starts from real recordings and adds
// only the physics that belongs to its instrument family.
#pragma once

#include "whe/hybrid.hpp"
#include "whe/instrument.hpp"

namespace whe {

// ---------------------------------------------------------------------------
// Piano: the transient of the REAL recorded hammer strike excites 1–3 modeled
// unison strings (with stiffness dispersion and beating), layered onto the
// recording; sustain pedal opens a bank of sympathetic strings.
// RecordingLevel = 0 leaves only modeled strings driven by the real strike.
// ---------------------------------------------------------------------------
class PianoEngine final : public InstrumentEngine {
public:
    enum Param { HammerHardness, StrikeWindowMs, StringResonance, RecordingLevel, UnisonDetune, Stiffness, StringDecay, SympatheticResonance, kParamCount };
    PianoEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg);
    const char* kind() const noexcept override { return "piano"; }

protected:
    void onPrepare() override;
    void onVoiceStart(int v, const VoiceInfo& in) noexcept override;
    void onBlock(int n) noexcept override;
    void processVoice(int v, float* L, float* R, int n) noexcept override;
    void postProcess(float* L, float* R, int n) noexcept override;
    float tailSeconds() const noexcept override { return 0.5f; }

private:
    struct PerVoice {
        dsp::TransientExtractor strike;
        dsp::Biquad hammer;
        std::array<hybrid::StringResonator, 3> strings;
        int count = 0;
        float excScale = 1.0f;  // normalises coherent build-up of a strike window spanning many periods
    };
    void configure(int v) noexcept;
    std::vector<PerVoice> pv_;
    hybrid::SympatheticBank sympathetic_;
    AlignedBuffer<float> mono_, sym_;
    std::array<float, kParamCount> applied_{};
    float pedalGain_ = 0.0f, appliedBend_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Bowed strings (violin, viola, cello, bass, erhu, …): the REAL multi-sampled
// bowed recording drives a friction waveguide (bow pressure, position, rosin);
// Dynamics crossfades between adjacent RECORDED dynamic layers; the body is
// re-voiced by changing wood density / stiffness of modes estimated from the
// recordings themselves.
// ---------------------------------------------------------------------------
class BowedStringEngine final : public InstrumentEngine {
public:
    enum Param { BowPressure, BowPosition, Rosin, FrictionAmount, RecordingLevel, Dynamics, UseDynamics, WoodDensity, WoodStiffness, BodyAmount, StringDecay, kParamCount };
    BowedStringEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg);
    const char* kind() const noexcept override { return "bowed"; }
    [[nodiscard]] const std::vector<hybrid::BodyMode>& bodyModes() const noexcept { return modes_; }

protected:
    void onPrepare() override;
    void handleNoteOn(int note, int velocity) noexcept override;
    void onVoiceStart(int v, const VoiceInfo& in) noexcept override;
    void onBlock(int n) noexcept override;
    void processVoice(int v, float* L, float* R, int n) noexcept override;
    void postProcess(float* L, float* R, int n) noexcept override;
    float tailSeconds() const noexcept override { return 0.3f; }

private:
    struct PerVoice {
        hybrid::BowFrictionShaper bow;
        int layer = 0;       // index into the recorded layers at this key
        int layerCount = 1;
    };
    void configure(int v) noexcept;
    std::vector<PerVoice> pv_;
    std::vector<hybrid::BodyMode> modes_;
    hybrid::BodyRevoicer body_;
    AlignedBuffer<float> mono_, corr_;
    std::array<float, kParamCount> applied_{};
    float appliedBend_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Plucked strings (guitar, harp, oud, koto, sitar, …): the REAL recorded pluck
// transient excites a Karplus–Strong waveguide (pluck position, decay,
// brightness); open strings resonate sympathetically; body re-voicing.
// ---------------------------------------------------------------------------
class PluckedStringEngine final : public InstrumentEngine {
public:
    enum Param { PluckPosition, StringDecay, Brightness, ResonatorAmount, RecordingLevel, PickWindowMs, Sympathetic, WoodDensity, WoodStiffness, BodyAmount, kParamCount };
    PluckedStringEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg);
    const char* kind() const noexcept override { return "plucked"; }
    // Open-string tunings for the sympathetic bank (MIDI notes). Call before prepare().
    void setOpenStrings(std::vector<int> notes) { openStrings_ = std::move(notes); }

protected:
    void onPrepare() override;
    void onVoiceStart(int v, const VoiceInfo& in) noexcept override;
    void onBlock(int n) noexcept override;
    void processVoice(int v, float* L, float* R, int n) noexcept override;
    void postProcess(float* L, float* R, int n) noexcept override;
    float tailSeconds() const noexcept override { return 1.0f; }

private:
    struct PerVoice {
        dsp::TransientExtractor pick;
        hybrid::StringResonator string;
        float excScale = 1.0f;
    };
    void configure(int v) noexcept;
    std::vector<PerVoice> pv_;
    std::vector<int> openStrings_{40, 45, 50, 55, 59, 64};
    hybrid::SympatheticBank sympathetic_;
    std::vector<hybrid::BodyMode> modes_;
    hybrid::BodyRevoicer body_;
    AlignedBuffer<float> mono_, sym_, corr_;
    std::array<float, kParamCount> applied_{};
    float appliedBend_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Aerophones (flute, clarinet, sax, trumpet, ney, shakuhachi, didgeridoo, …):
// the REAL recorded breath/tone excites a bore waveguide whose length, bore
// type, bell radius (diffraction), overblowing, turbulence and tonguing are
// live physical parameters.
// ---------------------------------------------------------------------------
class AerophoneEngine final : public InstrumentEngine {
public:
    enum Param { BoreLengthScale, BoreType, Overblow, Turbulence, Tonguing, BellRadius, ColumnAmount, RecordingLevel, ColumnDecay, Nonlinearity, kParamCount };
    AerophoneEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg);
    const char* kind() const noexcept override { return "aerophone"; }

protected:
    void onPrepare() override;
    void onVoiceStart(int v, const VoiceInfo& in) noexcept override;
    void onBlock(int n) noexcept override;
    void processVoice(int v, float* L, float* R, int n) noexcept override;
    float tailSeconds() const noexcept override { return 0.2f; }

private:
    void configure(int v) noexcept;
    std::vector<hybrid::AirColumnShaper> column_;
    std::array<float, kParamCount> applied_{};
    float appliedBend_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Membranes (snare, toms, kick, frame drums, tabla, djembe, timpani, …): every
// REAL recorded hit's transient is injected into ONE shared 2-D drumhead mesh
// whose tension, damping, strike point and mallet hardness are live.
// ---------------------------------------------------------------------------
class MembraneEngine final : public InstrumentEngine {
public:
    enum Param { Tension, Damping, HighDamping, StrikeX, StrikeY, MalletHardness, HeadAmount, RecordingLevel, ExciteWindowMs, kParamCount };
    MembraneEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg, int meshCells = 24);
    const char* kind() const noexcept override { return "membrane"; }
    [[nodiscard]] float headFundamentalHz() const noexcept { return mesh_.fundamentalEstimateHz(); }

protected:
    void onPrepare() override;
    void onVoiceStart(int v, const VoiceInfo& in) noexcept override;
    void onBlock(int n) noexcept override;
    void processVoice(int v, float* L, float* R, int n) noexcept override;
    void postProcess(float* L, float* R, int n) noexcept override;

private:
    std::vector<dsp::TransientExtractor> strike_;
    hybrid::MembraneMesh mesh_;
    int cells_;
    AlignedBuffer<float> excite_, head_;
    std::array<float, kParamCount> applied_{};
};

}  // namespace whe
