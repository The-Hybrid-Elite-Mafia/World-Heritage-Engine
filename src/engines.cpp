#include "whe/engines.hpp"

#include <limits>

namespace whe {

namespace {

template <std::size_t N>
void invalidate(std::array<float, N>& a) {
    a.fill(std::numeric_limits<float>::quiet_NaN());
}

// Linear ramp of the voice's release envelope across the block (physics damping).
struct EnvRamp {
    float a, step;
    EnvRamp(const SampleVoice& v, bool tail, int n)
        : a(tail ? 1.0f : v.envelopeAtBlockStart()), step(tail ? 0.0f : (v.envelopeAtBlockEnd() - v.envelopeAtBlockStart()) / static_cast<float>(n)) {}
    float next() noexcept {
        const float e = a;
        a += step;
        return e;
    }
};

// A strike window W samples long excites a string of period P coherently about W/P times.
float windowScale(float windowMs, float hz, double fs) {
    const float period = static_cast<float>(fs) / hz;
    const float w = windowMs * 0.001f * static_cast<float>(fs);
    return period / std::max(period, w);
}

float perNoteDecay(float t60AtMiddleC, float hz) { return clampf(t60AtMiddleC * std::pow(261.63f / hz, 0.6f), 0.15f, 60.0f); }

}  // namespace

// ============================================================================
// PianoEngine
// ============================================================================
static constexpr ParamInfo kPianoParams[] = {
    {"HammerHardness", 0, 1, 0.5f, "", "Low-pass on the recorded hammer strike before it excites the strings (worn felt … hard felt)"},
    {"StrikeWindowMs", 2, 40, 10, "ms", "How much of the recording's attack is treated as the hammer strike"},
    {"StringResonance", 0, 2, 0.25f, "x", "Level of the modeled string resonance excited by the real strike"},
    {"RecordingLevel", 0, 1, 1, "x", "Level of the original recording (0 = modeled strings driven only by the real strike)"},
    {"UnisonDetune", 0, 6, 1.2f, "cents", "Detune between the modeled unison strings (beating, aftersound)"},
    {"Stiffness", 0, 1, 0.3f, "", "String stiffness: allpass dispersion sharpens upper partials (inharmonicity)"},
    {"StringDecay", 0.5f, 30, 8, "s", "T60 of the modeled strings at middle C (shorter for treble, longer for bass)"},
    {"SympatheticResonance", 0, 1, 0.25f, "x", "Open-string sympathetic resonance while the sustain pedal is down"},
};

PianoEngine::PianoEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg)
    : InstrumentEngine(std::move(lib), s, std::move(cfg), kPianoParams) {}

void PianoEngine::onPrepare() {
    const float fs = static_cast<float>(sampleRate_);
    pv_.resize(static_cast<std::size_t>(voiceCount()));
    for (auto& v : pv_) {
        v.strike.prepare(fs);
        for (auto& s : v.strings) s.prepare(fs, 25.0f);
    }
    std::vector<int> open;
    for (int k = 36; k <= 84; k += 2) open.push_back(k);
    sympathetic_.prepare(fs, open, 6.0f);
    mono_.resize(static_cast<std::size_t>(maxBlock_));
    sym_.resize(static_cast<std::size_t>(maxBlock_));
    invalidate(applied_);
}

void PianoEngine::configure(int v) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    const float fs = static_cast<float>(sampleRate_);
    const float hz = noteHz(v);
    pv.strike.setWindow(p(StrikeWindowMs));
    pv.excScale = windowScale(p(StrikeWindowMs), hz, sampleRate_);
    pv.hammer.setLowpass(800.0f * std::pow(20.0f, p(HammerHardness)), 0.707f, fs);  // 800 Hz … 16 kHz
    const float detune = p(UnisonDetune);
    const float offsets[3] = {0.0f, -detune, +detune};
    for (int i = 0; i < pv.count; ++i) {
        auto& s = pv.strings[static_cast<std::size_t>(i)];
        s.setFrequency(hz * std::exp2(offsets[i] / 1200.0f));
        s.setStiffness(p(Stiffness) * clampf((hz - 27.5f) / 2000.0f + 0.25f, 0.0f, 1.0f));
        s.setBrightness(0.55f + 0.4f * p(HammerHardness));
        s.setExcitePosition(1.0f / 8.0f);  // hammers strike near 1/8 of the speaking length
        s.setDecay(perNoteDecay(p(StringDecay), hz));
    }
}

void PianoEngine::onVoiceStart(int v, const VoiceInfo& in) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    pv.count = in.releaseVoice ? 0 : (in.note < 35 ? 1 : (in.note < 48 ? 2 : 3));  // single, bichord, trichord
    pv.strike.trigger();
    pv.hammer.reset();
    for (auto& s : pv.strings) s.reset();
    configure(v);
}

void PianoEngine::onBlock(int n) noexcept {
    bool dirty = bendSemitones() != appliedBend_;
    for (int i : {HammerHardness, StrikeWindowMs, UnisonDetune, Stiffness, StringDecay})
        if (p(i) != applied_[static_cast<std::size_t>(i)]) dirty = true;
    if (dirty) {
        for (int v = 0; v < voiceCount(); ++v)
            if (info(v).note >= 0 && pv_[static_cast<std::size_t>(v)].count > 0) configure(v);
        for (int i = 0; i < kParamCount; ++i) applied_[static_cast<std::size_t>(i)] = p(i);
        appliedBend_ = bendSemitones();
    }
    const float target = sustainPedal() ? 1.0f : 0.0f;
    pedalGain_ += (target - pedalGain_) * std::min(1.0f, n / (0.08f * static_cast<float>(sampleRate_)));
}

void PianoEngine::processVoice(int v, float* L, float* R, int n) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    const float dry = p(RecordingLevel);
    if (pv.count == 0) {
        for (int i = 0; i < n; ++i) {
            L[i] *= dry;
            R[i] *= dry;
        }
        return;
    }
    const float mix = p(StringResonance) / static_cast<float>(pv.count);
    EnvRamp env(voice(v), info(v).tail, n);
    for (int i = 0; i < n; ++i) {
        const float strike = pv.hammer.process(pv.strike.process(0.5f * (L[i] + R[i]))) * pv.excScale;
        float s = 0.0f;
        for (int k = 0; k < pv.count; ++k) s += pv.strings[static_cast<std::size_t>(k)].process(strike);
        const float add = s * mix * env.next();
        L[i] = L[i] * dry + add;
        R[i] = R[i] * dry + add;
    }
}

void PianoEngine::postProcess(float* L, float* R, int n) noexcept {
    const float amt = p(SympatheticResonance);
    if (amt <= 0.0f) return;
    for (int i = 0; i < n; ++i) mono_[static_cast<std::size_t>(i)] = 0.5f * (L[i] + R[i]) * pedalGain_ * 0.2f;
    sympathetic_.process(mono_.data(), sym_.data(), n);
    for (int i = 0; i < n; ++i) {
        L[i] += sym_[static_cast<std::size_t>(i)] * amt;
        R[i] += sym_[static_cast<std::size_t>(i)] * amt;
    }
}

// ============================================================================
// BowedStringEngine
// ============================================================================
static constexpr ParamInfo kBowedParams[] = {
    {"BowPressure", 0, 1, 0.5f, "", "Bow normal force: flattens the friction curve so the string sticks longer"},
    {"BowPosition", 0.02f, 0.4f, 0.12f, "", "Contact point as a fraction of string length from the bridge (ponticello … tasto)"},
    {"Rosin", 0, 1, 0.5f, "", "Rosin grab: widens the stick region of the friction curve"},
    {"FrictionAmount", 0, 2, 0.5f, "x", "Level of the friction-waveguide response layered onto the recording"},
    {"RecordingLevel", 0, 1, 1, "x", "Level of the original recording"},
    {"Dynamics", 0, 1, 0.6f, "", "Crossfade position across the RECORDED dynamic layers (map to CC1)"},
    {"UseDynamics", 0, 1, 1, "", "1: Dynamics selects/crossfades layers; 0: note velocity selects the layer"},
    {"WoodDensity", 0.5f, 2, 1, "x", "Body wood density relative to the recording (modes scale with 1/sqrt(rho))"},
    {"WoodStiffness", 0.5f, 2, 1, "x", "Body wood stiffness relative to the recording (modes scale with sqrt(E))"},
    {"BodyAmount", 0, 2, 1, "x", "Strength of the body re-voicing"},
    {"StringDecay", 0.2f, 5, 1.5f, "s", "T60 of the friction waveguide"},
};

BowedStringEngine::BowedStringEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg)
    : InstrumentEngine(std::move(lib), s, std::move(cfg), kBowedParams) {}

void BowedStringEngine::onPrepare() {
    const float fs = static_cast<float>(sampleRate_);
    pv_.resize(static_cast<std::size_t>(voiceCount()));
    for (auto& v : pv_) v.bow.prepare(fs, 25.0f);
    modes_ = hybrid::estimateBodyModes(*lib_, 24);
    body_.prepare(fs, modes_);
    mono_.resize(static_cast<std::size_t>(maxBlock_));
    corr_.resize(static_cast<std::size_t>(maxBlock_));
    invalidate(applied_);
}

void BowedStringEngine::handleNoteOn(int note, int velocity) noexcept {
    if (p(UseDynamics) < 0.5f) {
        InstrumentEngine::handleNoteOn(note, velocity);
        return;
    }
    // Start the two recorded dynamic layers that bracket the Dynamics position;
    // onBlock() equal-power crossfades them as Dynamics moves during the note.
    const auto layers = art_->layersAt(note);
    if (layers.empty()) return;
    const float x = p(Dynamics) * static_cast<float>(layers.size() - 1);
    const int lower = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(layers.size()) - 1);
    const int upper = std::min(lower + 1, static_cast<int>(layers.size()) - 1);
    const int group = ++nextGroup_;
    for (int li : {lower, upper}) {
        const Zone& z = *layers[static_cast<std::size_t>(li)];
        const int v = startVoice(z, note, z.hiVel, 0.0f, false, group, li);
        if (v >= 0) {
            pv_[static_cast<std::size_t>(v)].layer = li;
            pv_[static_cast<std::size_t>(v)].layerCount = static_cast<int>(layers.size());
        }
        if (upper == lower) break;
    }
    onBlock(0);
}

void BowedStringEngine::configure(int v) noexcept {
    auto& b = pv_[static_cast<std::size_t>(v)].bow;
    b.setFrequency(noteHz(v));
    b.setBowPosition(p(BowPosition));
    b.setPressure(p(BowPressure));
    b.setRosin(p(Rosin));
    b.setDecay(p(StringDecay));
}

void BowedStringEngine::onVoiceStart(int v, const VoiceInfo& in) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    pv.bow.reset();
    pv.layer = 0;
    pv.layerCount = 1;
    if (in.group < 0) voice(v).setGain(1.0f);
    configure(v);
}

void BowedStringEngine::onBlock(int) noexcept {
    bool dirty = bendSemitones() != appliedBend_;
    for (int i : {BowPressure, BowPosition, Rosin, StringDecay})
        if (p(i) != applied_[static_cast<std::size_t>(i)]) dirty = true;
    if (dirty)
        for (int v = 0; v < voiceCount(); ++v)
            if (info(v).note >= 0) configure(v);
    body_.setMaterial(p(WoodDensity), p(WoodStiffness));
    body_.setAmount(p(BodyAmount));
    // Layer morph gains (equal power) for grouped voices.
    for (int v = 0; v < voiceCount(); ++v) {
        const VoiceInfo& in = info(v);
        if (in.group < 0 || in.note < 0 || in.releaseVoice) continue;
        const PerVoice& pv = pv_[static_cast<std::size_t>(v)];
        const float x = p(Dynamics) * static_cast<float>(pv.layerCount - 1);
        const float w = clampf(1.0f - std::fabs(x - static_cast<float>(pv.layer)), 0.0f, 1.0f);
        // Keep the outer layers at full level beyond the recorded range.
        float gain = std::sin(w * std::numbers::pi_v<float> * 0.5f);
        bool partner = false;
        for (int u = 0; u < voiceCount(); ++u)
            if (u != v && info(u).group == in.group) partner = true;
        if (!partner) gain = 1.0f;
        voice(v).setGain(gain);
    }
    for (int i = 0; i < kParamCount; ++i) applied_[static_cast<std::size_t>(i)] = p(i);
    appliedBend_ = bendSemitones();
}

void BowedStringEngine::processVoice(int v, float* L, float* R, int n) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    const float dry = p(RecordingLevel), amt = p(FrictionAmount);
    EnvRamp env(voice(v), info(v).tail, n);
    for (int i = 0; i < n; ++i) {
        const float f = pv.bow.process(0.5f * (L[i] + R[i])) * amt * env.next();
        L[i] = L[i] * dry + f;
        R[i] = R[i] * dry + f;
    }
}

void BowedStringEngine::postProcess(float* L, float* R, int n) noexcept {
    if (!body_.active()) return;
    for (int i = 0; i < n; ++i) mono_[static_cast<std::size_t>(i)] = 0.5f * (L[i] + R[i]);
    body_.process(mono_.data(), corr_.data(), n);
    for (int i = 0; i < n; ++i) {
        L[i] += corr_[static_cast<std::size_t>(i)];
        R[i] += corr_[static_cast<std::size_t>(i)];
    }
}

// ============================================================================
// PluckedStringEngine
// ============================================================================
static constexpr ParamInfo kPluckedParams[] = {
    {"PluckPosition", 0.05f, 0.5f, 0.18f, "", "Pluck point as a fraction of string length (comb filtering of the excitation)"},
    {"StringDecay", 0.3f, 15, 3, "s", "T60 of the modeled string at middle C"},
    {"Brightness", 0, 1, 0.6f, "", "Loop low-pass: string material and damping of upper partials"},
    {"ResonatorAmount", 0, 2, 0.5f, "x", "Level of the modeled string excited by the real pluck"},
    {"RecordingLevel", 0, 1, 1, "x", "Level of the original recording"},
    {"PickWindowMs", 2, 30, 6, "ms", "How much of the recording's attack is treated as the pluck"},
    {"Sympathetic", 0, 1, 0.2f, "x", "Open-string sympathetic resonance"},
    {"WoodDensity", 0.5f, 2, 1, "x", "Body wood density relative to the recording"},
    {"WoodStiffness", 0.5f, 2, 1, "x", "Body wood stiffness relative to the recording"},
    {"BodyAmount", 0, 2, 1, "x", "Strength of the body re-voicing"},
};

PluckedStringEngine::PluckedStringEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg)
    : InstrumentEngine(std::move(lib), s, std::move(cfg), kPluckedParams) {}

void PluckedStringEngine::onPrepare() {
    const float fs = static_cast<float>(sampleRate_);
    pv_.resize(static_cast<std::size_t>(voiceCount()));
    for (auto& v : pv_) {
        v.pick.prepare(fs);
        v.string.prepare(fs, 25.0f);
    }
    sympathetic_.prepare(fs, openStrings_, 4.0f);
    modes_ = hybrid::estimateBodyModes(*lib_, 24);
    body_.prepare(fs, modes_);
    mono_.resize(static_cast<std::size_t>(maxBlock_));
    sym_.resize(static_cast<std::size_t>(maxBlock_));
    corr_.resize(static_cast<std::size_t>(maxBlock_));
    invalidate(applied_);
}

void PluckedStringEngine::configure(int v) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    const float hz = noteHz(v);
    pv.pick.setWindow(p(PickWindowMs));
    pv.excScale = windowScale(p(PickWindowMs), hz, sampleRate_);
    pv.string.setFrequency(hz);
    pv.string.setExcitePosition(p(PluckPosition));
    pv.string.setBrightness(p(Brightness));
    pv.string.setDecay(perNoteDecay(p(StringDecay), hz));
}

void PluckedStringEngine::onVoiceStart(int v, const VoiceInfo&) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    pv.pick.trigger();
    pv.string.reset();
    configure(v);
}

void PluckedStringEngine::onBlock(int) noexcept {
    bool dirty = bendSemitones() != appliedBend_;
    for (int i : {PluckPosition, StringDecay, Brightness, PickWindowMs})
        if (p(i) != applied_[static_cast<std::size_t>(i)]) dirty = true;
    if (dirty)
        for (int v = 0; v < voiceCount(); ++v)
            if (info(v).note >= 0) configure(v);
    body_.setMaterial(p(WoodDensity), p(WoodStiffness));
    body_.setAmount(p(BodyAmount));
    for (int i = 0; i < kParamCount; ++i) applied_[static_cast<std::size_t>(i)] = p(i);
    appliedBend_ = bendSemitones();
}

void PluckedStringEngine::processVoice(int v, float* L, float* R, int n) noexcept {
    PerVoice& pv = pv_[static_cast<std::size_t>(v)];
    const float dry = p(RecordingLevel), amt = p(ResonatorAmount);
    const bool isRelease = info(v).releaseVoice;
    EnvRamp env(voice(v), info(v).tail, n);
    for (int i = 0; i < n; ++i) {
        float add = 0.0f;
        if (!isRelease) add = pv.string.process(pv.pick.process(0.5f * (L[i] + R[i])) * pv.excScale) * amt * env.next();
        L[i] = L[i] * dry + add;
        R[i] = R[i] * dry + add;
    }
}

void PluckedStringEngine::postProcess(float* L, float* R, int n) noexcept {
    const float amt = p(Sympathetic);
    for (int i = 0; i < n; ++i) mono_[static_cast<std::size_t>(i)] = 0.5f * (L[i] + R[i]);
    if (amt > 0.0f) {
        for (int i = 0; i < n; ++i) sym_[static_cast<std::size_t>(i)] = mono_[static_cast<std::size_t>(i)] * 0.15f;
        sympathetic_.process(sym_.data(), sym_.data(), n);
        for (int i = 0; i < n; ++i) {
            L[i] += sym_[static_cast<std::size_t>(i)] * amt;
            R[i] += sym_[static_cast<std::size_t>(i)] * amt;
        }
    }
    if (body_.active()) {
        body_.process(mono_.data(), corr_.data(), n);
        for (int i = 0; i < n; ++i) {
            L[i] += corr_[static_cast<std::size_t>(i)];
            R[i] += corr_[static_cast<std::size_t>(i)];
        }
    }
}

// ============================================================================
// AerophoneEngine
// ============================================================================
static constexpr ParamInfo kAeroParams[] = {
    {"BoreLengthScale", 0.5f, 2, 1, "x", "Bore length relative to the length that resonates at the played note"},
    {"BoreType", 0, 1, 0, "", "0 = open–open bore (flute, ney); 1 = closed–open (clarinet, panpipe)"},
    {"Overblow", 0, 1, 0, "", "Shifts excitation to the register loop (octave for open bores, twelfth for closed)"},
    {"Turbulence", 0, 1, 0.3f, "", "Emphasis of the recording's own breath noise injected into the bore"},
    {"Tonguing", 0, 1, 0.4f, "", "Emphasis of the recording's own articulation transient"},
    {"BellRadius", 0.005f, 0.2f, 0.03f, "m", "Bell radius: sets the diffraction split (ka = 1) between reflected and radiated sound"},
    {"ColumnAmount", 0, 2, 0.5f, "x", "Level of the radiated bore response layered onto the recording"},
    {"RecordingLevel", 0, 1, 1, "x", "Level of the original recording"},
    {"ColumnDecay", 0.05f, 2, 0.4f, "s", "T60 of the bore resonance"},
    {"Nonlinearity", 0, 1, 0.3f, "", "Pressure saturation inside the bore (passive tanh)"},
};

AerophoneEngine::AerophoneEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg)
    : InstrumentEngine(std::move(lib), s, std::move(cfg), kAeroParams) {}

void AerophoneEngine::onPrepare() {
    column_.resize(static_cast<std::size_t>(voiceCount()));
    for (auto& c : column_) c.prepare(static_cast<float>(sampleRate_), 25.0f);
    invalidate(applied_);
}

void AerophoneEngine::configure(int v) noexcept {
    auto& c = column_[static_cast<std::size_t>(v)];
    const auto bore = p(BoreType) >= 0.5f ? hybrid::AirColumnShaper::Bore::Closed : hybrid::AirColumnShaper::Bore::Open;
    c.setBore(bore);
    c.setBoreLength(hybrid::AirColumnShaper::boreLengthFor(noteHz(v), bore) * p(BoreLengthScale));
    c.setBellRadius(p(BellRadius));
    c.setOverblow(p(Overblow));
    c.setTurbulence(p(Turbulence));
    c.setTonguing(p(Tonguing));
    c.setNonlinearity(p(Nonlinearity));
    c.setDecay(p(ColumnDecay));
}

void AerophoneEngine::onVoiceStart(int v, const VoiceInfo&) noexcept {
    auto& c = column_[static_cast<std::size_t>(v)];
    c.reset();
    configure(v);
    c.noteOn();
}

void AerophoneEngine::onBlock(int) noexcept {
    bool dirty = bendSemitones() != appliedBend_;
    for (int i = 0; i < kParamCount; ++i)
        if (i != ColumnAmount && i != RecordingLevel && p(i) != applied_[static_cast<std::size_t>(i)]) dirty = true;
    if (dirty)
        for (int v = 0; v < voiceCount(); ++v)
            if (info(v).note >= 0) configure(v);
    for (int i = 0; i < kParamCount; ++i) applied_[static_cast<std::size_t>(i)] = p(i);
    appliedBend_ = bendSemitones();
}

void AerophoneEngine::processVoice(int v, float* L, float* R, int n) noexcept {
    auto& c = column_[static_cast<std::size_t>(v)];
    const float dry = p(RecordingLevel), amt = p(ColumnAmount);
    EnvRamp env(voice(v), info(v).tail, n);
    for (int i = 0; i < n; ++i) {
        const float add = c.process(0.5f * (L[i] + R[i])) * amt * env.next();
        L[i] = L[i] * dry + add;
        R[i] = R[i] * dry + add;
    }
}

// ============================================================================
// MembraneEngine
// ============================================================================
static constexpr ParamInfo kMembraneParams[] = {
    {"Tension", 0, 1, 0.35f, "", "Head tension: wave speed on the membrane (pitch of the modeled head)"},
    {"Damping", 0.05f, 3, 0.5f, "s", "T60 of the head (muffling, moon-gel, tape)"},
    {"HighDamping", 0, 1, 0.4f, "", "Frequency-dependent loss: upper modes die first"},
    {"StrikeX", -1, 1, 0.35f, "", "Strike point across the head (0 = center, ±1 = edge)"},
    {"StrikeY", -1, 1, 0, "", "Strike point across the head"},
    {"MalletHardness", 0, 1, 0.6f, "", "Low-pass on the injected recording transient: felt … wood"},
    {"HeadAmount", 0, 4, 1, "x", "Level of the modeled head response layered onto the recordings"},
    {"RecordingLevel", 0, 1, 1, "x", "Level of the original recordings"},
    {"ExciteWindowMs", 1, 40, 8, "ms", "How much of each recorded hit excites the head"},
};

MembraneEngine::MembraneEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& s, EngineConfig cfg, int meshCells)
    : InstrumentEngine(std::move(lib), s, std::move(cfg), kMembraneParams), cells_(meshCells) {}

void MembraneEngine::onPrepare() {
    const float fs = static_cast<float>(sampleRate_);
    strike_.resize(static_cast<std::size_t>(voiceCount()));
    for (auto& s : strike_) s.prepare(fs);
    mesh_.prepare(fs, cells_);
    excite_.resize(static_cast<std::size_t>(maxBlock_));
    head_.resize(static_cast<std::size_t>(maxBlock_));
    invalidate(applied_);
}

void MembraneEngine::onVoiceStart(int v, const VoiceInfo&) noexcept {
    auto& s = strike_[static_cast<std::size_t>(v)];
    s.setWindow(p(ExciteWindowMs));
    s.trigger();
}

void MembraneEngine::onBlock(int n) noexcept {
    auto ch = [&](int i) { return p(i) != applied_[static_cast<std::size_t>(i)]; };
    if (ch(Tension)) mesh_.setTension(p(Tension));
    if (ch(Damping)) mesh_.setDamping(p(Damping));
    if (ch(HighDamping)) mesh_.setHighDamping(p(HighDamping));
    if (ch(StrikeX) || ch(StrikeY)) mesh_.setStrike(p(StrikeX), p(StrikeY));
    if (ch(MalletHardness)) mesh_.setMalletHardness(p(MalletHardness));
    for (int i = 0; i < kParamCount; ++i) applied_[static_cast<std::size_t>(i)] = p(i);
    std::memset(excite_.data(), 0, sizeof(float) * static_cast<std::size_t>(std::max(n, 0)));
}

void MembraneEngine::processVoice(int v, float* L, float* R, int n) noexcept {
    auto& s = strike_[static_cast<std::size_t>(v)];
    const float dry = p(RecordingLevel);
    const bool isRelease = info(v).releaseVoice;
    for (int i = 0; i < n; ++i) {
        if (!isRelease) excite_[static_cast<std::size_t>(i)] += s.process(0.5f * (L[i] + R[i]));
        L[i] *= dry;
        R[i] *= dry;
    }
}

void MembraneEngine::postProcess(float* L, float* R, int n) noexcept {
    mesh_.process(excite_.data(), head_.data(), n);
    const float amt = p(HeadAmount);
    for (int i = 0; i < n; ++i) {
        L[i] += head_[static_cast<std::size_t>(i)] * amt;
        R[i] += head_[static_cast<std::size_t>(i)] * amt;
    }
}

// ============================================================================
// Factory
// ============================================================================
std::optional<EngineKind> engineKindFromName(std::string_view n) noexcept {
    if (n == "piano") return EngineKind::Piano;
    if (n == "bowed" || n == "bowedstring" || n == "strings") return EngineKind::BowedString;
    if (n == "plucked" || n == "pluckedstring" || n == "guitar") return EngineKind::PluckedString;
    if (n == "aerophone" || n == "wind" || n == "winds") return EngineKind::Aerophone;
    if (n == "membrane" || n == "drum" || n == "drums" || n == "percussion") return EngineKind::Membrane;
    return std::nullopt;
}

const char* engineKindName(EngineKind k) noexcept {
    switch (k) {
        case EngineKind::Piano: return "piano";
        case EngineKind::BowedString: return "bowed";
        case EngineKind::PluckedString: return "plucked";
        case EngineKind::Aerophone: return "aerophone";
        case EngineKind::Membrane: return "membrane";
    }
    return "?";
}

Result<std::unique_ptr<InstrumentEngine>> createInstrument(EngineKind kind, std::shared_ptr<const SampleLibrary> lib, DiskStreamer& streamer,
                                                           EngineConfig cfg) {
    if (!lib) return fail("an instrument needs a SampleLibrary of real recordings — none was given");
    const Articulation* art = cfg.articulation.empty() ? lib->primary() : lib->articulation(cfg.articulation);
    if (!art) return fail("articulation '" + cfg.articulation + "' not found in " + lib->name());
    if (art->zones.empty()) return fail("library " + lib->name() + " has no mapped recordings");
    const bool needsPitch = kind != EngineKind::Membrane;
    if (needsPitch && !art->pitched)
        return fail(std::string("the ") + engineKindName(kind) + " engine needs pitched recordings (note names in file names or root keys in the files); '" +
                    art->name + "' in " + lib->name() + " is unpitched");
    std::unique_ptr<InstrumentEngine> e;
    switch (kind) {
        case EngineKind::Piano: e = std::make_unique<PianoEngine>(lib, streamer, cfg); break;
        case EngineKind::BowedString: e = std::make_unique<BowedStringEngine>(lib, streamer, cfg); break;
        case EngineKind::PluckedString: e = std::make_unique<PluckedStringEngine>(lib, streamer, cfg); break;
        case EngineKind::Aerophone: e = std::make_unique<AerophoneEngine>(lib, streamer, cfg); break;
        case EngineKind::Membrane: e = std::make_unique<MembraneEngine>(lib, streamer, cfg); break;
    }
    return e;
}

}  // namespace whe
