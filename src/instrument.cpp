#include "whe/instrument.hpp"

#include "whe/simd.hpp"

namespace whe {

InstrumentEngine::InstrumentEngine(std::shared_ptr<const SampleLibrary> lib, DiskStreamer& streamer, EngineConfig cfg,
                                   std::span<const ParamInfo> params)
    : lib_(std::move(lib)), streamer_(streamer), cfg_(std::move(cfg)), paramInfo_(params) {
    art_ = cfg_.articulation.empty() ? lib_->primary() : lib_->articulation(cfg_.articulation);
    if (!art_) art_ = lib_->primary();
    releaseArt_ = cfg_.useReleaseSamples ? lib_->releaseArticulation() : nullptr;
    params_ = std::make_unique<SmoothedParam[]>(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) params_[i].reset(params[i].def);
}

InstrumentEngine::~InstrumentEngine() {
    // Stop every voice, then wait for in-flight disk fills that still point at
    // our stream cursors before their buffers are released.
    for (int v = 0; v < voiceCount_; ++v) voices_[static_cast<std::size_t>(v)].kill(sampleRate_);
    streamer_.drain();
    if (pool_)
        for (int v = 0; v < voiceCount_; ++v) voices_[static_cast<std::size_t>(v)].unprepare(*pool_);
}

Result<void> InstrumentEngine::prepare(double sampleRate, int maxBlock) {
    if (prepared_) return fail("instrument already prepared");
    if (sampleRate < 8000 || sampleRate > 384000) return fail("unsupported sample rate");
    sampleRate_ = sampleRate;
    maxBlock_ = std::clamp(maxBlock, 16, kMaxBlock);
    voiceCount_ = std::max(1, cfg_.polyphony) + 8;  // +8 so stolen voices can fade instead of clicking
    pool_ = std::make_unique<BlockPool>(streamer_.bufferFloats(), static_cast<u32>(voiceCount_ * 2));
    voices_ = std::make_unique<SampleVoice[]>(static_cast<std::size_t>(voiceCount_));
    info_.assign(static_cast<std::size_t>(voiceCount_), VoiceInfo{});
    for (int v = 0; v < voiceCount_; ++v)
        if (!voices_[static_cast<std::size_t>(v)].prepare(streamer_, *pool_, maxBlock_))
            return fail("stream buffers too small for block size " + std::to_string(maxBlock_));
    rrCounter_.assign(lib_->zoneCount() + 1, 0);
    vL_.resize(static_cast<std::size_t>(maxBlock_));
    vR_.resize(static_cast<std::size_t>(maxBlock_));
    for (std::size_t i = 0; i < paramInfo_.size(); ++i) params_[i].setGlide(sampleRate_ / maxBlock_, 0.03);
    onPrepare();
    prepared_ = true;
    return {};
}

void InstrumentEngine::setParam(int index, float value) noexcept {
    if (index < 0 || index >= static_cast<int>(paramInfo_.size())) return;
    const ParamInfo& pi = paramInfo_[static_cast<std::size_t>(index)];
    params_[static_cast<std::size_t>(index)].set(clampf(value, pi.min, pi.max));
}
float InstrumentEngine::paramTarget(int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(paramInfo_.size())) return 0.0f;
    return params_[static_cast<std::size_t>(index)].target();
}
int InstrumentEngine::paramIndex(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < paramInfo_.size(); ++i)
        if (name == paramInfo_[i].name) return static_cast<int>(i);
    return -1;
}
int InstrumentEngine::activeVoices() const noexcept {
    int n = 0;
    for (int v = 0; v < voiceCount_; ++v) n += (voices_[static_cast<std::size_t>(v)].active() || info_[static_cast<std::size_t>(v)].tail) ? 1 : 0;
    return n;
}

float InstrumentEngine::noteHz(int v) const noexcept {
    return static_cast<float>(midiToHz(info_[static_cast<std::size_t>(v)].note + bend_));
}

int InstrumentEngine::allocateVoice() noexcept {
    int freeV = -1, oldest = -1, oldestReleasing = -1;
    int sounding = 0;
    for (int v = 0; v < voiceCount_; ++v) {
        const auto& in = info_[static_cast<std::size_t>(v)];
        const bool busy = voices_[static_cast<std::size_t>(v)].active() || in.tail;
        if (!busy) {
            if (freeV < 0) freeV = v;
            continue;
        }
        if (!voices_[static_cast<std::size_t>(v)].releasing() || in.held) ++sounding;
        if (oldest < 0 || in.age < info_[static_cast<std::size_t>(oldest)].age) oldest = v;
        if (voices_[static_cast<std::size_t>(v)].releasing() && (oldestReleasing < 0 || in.age < info_[static_cast<std::size_t>(oldestReleasing)].age))
            oldestReleasing = v;
    }
    // Over the polyphony budget: fade the oldest (released first) so it cannot click.
    if (sounding >= cfg_.polyphony) {
        const int victim = oldestReleasing >= 0 ? oldestReleasing : oldest;
        if (victim >= 0) voices_[static_cast<std::size_t>(victim)].kill(sampleRate_);
    }
    if (freeV >= 0) return freeV;
    // Every physical voice busy: hard steal the oldest.
    return oldestReleasing >= 0 ? oldestReleasing : oldest;
}

int InstrumentEngine::startVoice(const Zone& zone, int note, int velocity, float gain, bool releaseVoice, int group, int layerSlot) noexcept {
    if (zone.roundRobins.empty()) return -1;
    const int v = allocateVoice();
    if (v < 0) return -1;
    u32& rr = rrCounter_[zone.index];
    const u32 sampleId = zone.roundRobins[rr % zone.roundRobins.size()];
    ++rr;
    const StoredSample& s = lib_->store().get(sampleId);
    double ratio = static_cast<double>(s.header.sampleRate) / sampleRate_;
    if (zone.pitched) ratio *= std::exp2((note + bend_ - zone.rootKey - zone.tuneCents / 100.0) / 12.0);
    // Velocity inside one recorded layer: gentle scaling; a single-layer zone gets a wider range.
    const float span = std::max(1, zone.hiVel - zone.loVel + 1);
    // (Each recorded layer already carries its own loudness, so only a 3 dB trim inside it.)
    const float rangeDb = zone.layerCount > 1 ? 3.0f : 24.0f;
    const float within = clampf(static_cast<float>(zone.hiVel - velocity) / span, 0.0f, 1.0f);
    const float velGain = zone.layerCount > 1 ? dbToGain(-rangeDb * within) : dbToGain(-rangeDb * (1.0f - velocity / 127.0f));

    SampleVoice& sv = voices_[static_cast<std::size_t>(v)];
    sv.start(s, ratio, gain * velGain * zone.gain * lib_->levelGain(), !releaseVoice);
    VoiceInfo& in = info_[static_cast<std::size_t>(v)];
    in = VoiceInfo{};
    in.note = note;
    in.velocity = velocity;
    in.age = ++clock_;
    in.held = !releaseVoice;
    in.releaseVoice = releaseVoice;
    in.group = group;
    in.layerSlot = layerSlot;
    in.baseRatio = ratio / std::exp2(bend_ / 12.0);
    in.zone = &zone;
    onVoiceStart(v, in);
    return v;
}

void InstrumentEngine::handleNoteOn(int note, int velocity) noexcept {
    if (const Zone* z = art_->find(note, velocity)) startVoice(*z, note, velocity, 1.0f, false);
}

void InstrumentEngine::noteOff(int note) noexcept {
    bool released = false;
    int velocity = 64;
    for (int v = 0; v < voiceCount_; ++v) {
        VoiceInfo& in = info_[static_cast<std::size_t>(v)];
        if (in.note != note || !in.held || in.releaseVoice) continue;
        in.held = false;
        velocity = in.velocity;
        if (sustain_) {
            in.sustained = true;
        } else {
            voices_[static_cast<std::size_t>(v)].release(cfg_.releaseSeconds, sampleRate_);
            released = true;
        }
    }
    const float relLevel = releaseLevel_.load(std::memory_order_relaxed);
    if (released && releaseArt_ && relLevel > 0.0f)
        if (const Zone* z = releaseArt_->find(note, velocity)) startVoice(*z, note, velocity, relLevel, true);
}

void InstrumentEngine::handle(const NoteEvent& e) noexcept {
    switch (e.type) {
        case NoteEvent::Type::NoteOn:
            if (e.velocity == 0) noteOff(e.note);
            else handleNoteOn(e.note, e.velocity);
            break;
        case NoteEvent::Type::NoteOff: noteOff(e.note); break;
        case NoteEvent::Type::Sustain: {
            const bool down = e.value >= 0.5f;
            if (sustain_ && !down) {
                for (int v = 0; v < voiceCount_; ++v) {
                    VoiceInfo& in = info_[static_cast<std::size_t>(v)];
                    if (in.sustained) {
                        in.sustained = false;
                        voices_[static_cast<std::size_t>(v)].release(cfg_.releaseSeconds, sampleRate_);
                    }
                }
            }
            sustain_ = down;
            break;
        }
        case NoteEvent::Type::AllNotesOff:
            for (int v = 0; v < voiceCount_; ++v) {
                voices_[static_cast<std::size_t>(v)].release(cfg_.releaseSeconds, sampleRate_);
                info_[static_cast<std::size_t>(v)].held = false;
                info_[static_cast<std::size_t>(v)].sustained = false;
            }
            break;
        case NoteEvent::Type::PitchBend:
            bend_ = clampf(e.value, -24.0f, 24.0f);
            for (int v = 0; v < voiceCount_; ++v) {
                VoiceInfo& in = info_[static_cast<std::size_t>(v)];
                if (in.zone && in.zone->pitched) voices_[static_cast<std::size_t>(v)].setRatio(in.baseRatio * std::exp2(bend_ / 12.0));
            }
            break;
    }
}

void InstrumentEngine::render(float* outL, float* outR, int n) noexcept {
    DenormalGuard ftz;
    int done = 0;
    while (done < n) {
        const int chunk = std::min(maxBlock_, n - done);
        renderChunk(outL + done, outR + done, chunk);
        done += chunk;
    }
}

void InstrumentEngine::renderChunk(float* L, float* R, int n) noexcept {
    NoteEvent e;
    while (events_.pop(e)) handle(e);
    for (std::size_t i = 0; i < paramInfo_.size(); ++i) params_[i].advance(n);
    onBlock(n);
    std::memset(L, 0, sizeof(float) * static_cast<std::size_t>(n));
    std::memset(R, 0, sizeof(float) * static_cast<std::size_t>(n));
    const SimdKernels& k = simd();
    const float tailMul = static_cast<float>(std::pow(1e-3, 1.0 / std::max(1.0, static_cast<double>(tailSeconds()) * sampleRate_)));
    for (int v = 0; v < voiceCount_; ++v) {
        SampleVoice& sv = voices_[static_cast<std::size_t>(v)];
        VoiceInfo& in = info_[static_cast<std::size_t>(v)];
        if (sv.active()) {
            sv.render(vL_.data(), vR_.data(), n);
            if (!sv.active() && !sv.releasing() && tailSeconds() > 0.0f) {
                in.tail = true;  // recording ended naturally: let the physics ring out
                in.tailEnv = 1.0f;
            }
        } else if (in.tail) {
            std::memset(vL_.data(), 0, sizeof(float) * static_cast<std::size_t>(n));
            std::memset(vR_.data(), 0, sizeof(float) * static_cast<std::size_t>(n));
        } else {
            continue;
        }
        processVoice(v, vL_.data(), vR_.data(), n);
        if (in.tail && !sv.active()) {
            float g = in.tailEnv;
            for (int i = 0; i < n; ++i) {
                vL_[static_cast<std::size_t>(i)] *= g;
                vR_[static_cast<std::size_t>(i)] *= g;
                g *= tailMul;
            }
            in.tailEnv = g;
            if (g < 1e-5f) in.tail = false;
        }
        k.addScaled(L, vL_.data(), 1.0f, n);
        k.addScaled(R, vR_.data(), 1.0f, n);
        if (!sv.active() && !in.tail) in = VoiceInfo{};
    }
    postProcess(L, R, n);
}

}  // namespace whe
