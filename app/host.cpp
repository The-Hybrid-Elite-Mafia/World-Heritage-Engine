#include "host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>

namespace whe::ui {

namespace {

constexpr int kMaxBlock = 1024;  // engine render chunk; fixed so slot buffers never change size while playing

void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    if (msg == MIM_DATA && inst) reinterpret_cast<AudioHost*>(inst)->onMidi(static_cast<unsigned long>(p1));
}

}  // namespace

AudioHost::AudioHost() : mgr_(std::make_unique<InstrumentManager>()) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart > 0 ? f.QuadPart : 1;
}

AudioHost::~AudioHost() {
    closeMidi();
    closeDevice();
    live_.store(nullptr);
    if (instId_ >= 0) mgr_->remove(instId_);
    mgr_.reset();
}

bool AudioHost::asioBuilt() noexcept {
#ifdef WHE_HAS_ASIO
    return true;
#else
    return false;
#endif
}

std::vector<std::string> AudioHost::asioDrivers() {
#ifdef WHE_HAS_ASIO
    return AsioOutput::listDrivers();
#else
    return {};
#endif
}

void AudioHost::render(float* L, float* R, int n) noexcept {
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    mgr_->process(L, R, n);
    // Meters show the level before protection, so a hot setting is visible.
    float pl = 0, pr = 0;
    for (int i = 0; i < n; ++i) {
        pl = std::max(pl, std::abs(L[i]));
        pr = std::max(pr, std::abs(R[i]));
    }
    // Safety limiter: instant attack, 150 ms release, ceiling -0.5 dBFS. Untouched below the ceiling.
    constexpr float kCeiling = 0.944f;
    const float rel = 1.0f - std::exp(-1.0f / (0.15f * static_cast<float>(std::max(1.0, rate_))));
    float g = limGain_, gMin = 1.0f;
    for (int i = 0; i < n; ++i) {
        const float pk = std::max(std::abs(L[i]), std::abs(R[i]));
        const float target = pk > kCeiling ? kCeiling / pk : 1.0f;
        g = target < g ? target : g + (1.0f - g) * rel;
        L[i] *= g;
        R[i] *= g;
        gMin = std::min(gMin, g);
    }
    limGain_ = g;
    float lm = limMin_.load(std::memory_order_relaxed);
    while (gMin < lm && !limMin_.compare_exchange_weak(lm, gMin, std::memory_order_relaxed)) {
    }
    // Keep the highest peak until the UI reads it.
    float cur = peakL_.load(std::memory_order_relaxed);
    while (pl > cur && !peakL_.compare_exchange_weak(cur, pl, std::memory_order_relaxed)) {
    }
    cur = peakR_.load(std::memory_order_relaxed);
    while (pr > cur && !peakR_.compare_exchange_weak(cur, pr, std::memory_order_relaxed)) {
    }
    QueryPerformanceCounter(&t1);
    const double used = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(qpcFreq_);
    const double avail = n / std::max(1.0, rate_);
    const float load = static_cast<float>(used / avail);
    const float prev = cpu_.load(std::memory_order_relaxed);
    cpu_.store(prev + (load - prev) * 0.1f, std::memory_order_relaxed);
}

Result<void> AudioHost::startStream() {
    auto cb = [this](float* L, float* R, int n) { render(L, R, n); };
#ifdef WHE_HAS_ASIO
    if (usingAsio_) return asio_.start(cb);
#endif
    return wasapi_.start(cb);
}

void AudioHost::stopStream() {
#ifdef WHE_HAS_ASIO
    if (usingAsio_) {
        asio_.stop();
        asio_.close();
        return;
    }
#endif
    wasapi_.stop();
}

void AudioHost::closeDevice() {
    if (!running_) return;
    stopStream();
    running_ = false;
}

Result<void> AudioHost::openDevice(const DeviceChoice& choice) {
    closeDevice();
    choice_ = choice;
    lastError_.clear();
    usingAsio_ = false;
    if (choice.asio) {
#ifdef WHE_HAS_ASIO
        AsioOutput::Options o;
        o.driver = choice.asioDriver;
        o.bufferFrames = choice.bufferFrames;
        o.sampleRate = choice.sampleRate;
        if (auto r = asio_.open(o); !r) {
            lastError_ = "ASIO: " + r.error().message;
            return std::unexpected(r.error());
        }
        usingAsio_ = true;
        rate_ = asio_.sampleRate();
        buffer_ = asio_.bufferFrames();
        deviceName_ = asio_.description();
#else
        lastError_ = "This build has no ASIO support.";
        return fail(lastError_);
#endif
    } else {
        WasapiOutput::Options o;
        o.exclusive = choice.exclusive;
        o.periodFrames = choice.exclusive ? choice.bufferFrames : 0;
        if (auto r = wasapi_.open(o); !r) {
            lastError_ = "Windows audio: " + r.error().message;
            return std::unexpected(r.error());
        }
        rate_ = wasapi_.sampleRate();
        buffer_ = wasapi_.bufferFrames();
        deviceName_ = wasapi_.description();
    }
    mgr_->setAudioFormat(rate_, kMaxBlock);
    // The instrument was prepared for the old rate: rebuild it before audio starts.
    if (lib_) {
        if (auto r = setInstrument(kind_, lib_); !r) lastError_ = "instrument: " + r.error().message;
    }
    if (auto r = startStream(); !r) {
        lastError_ = r.error().message;
        stopStream();
        return r;
    }
    running_ = true;
    return {};
}

Result<void> AudioHost::setInstrument(EngineKind kind, std::shared_ptr<const SampleLibrary> lib) {
    if (!lib) return fail("no library");
    mgr_->setAudioFormat(rate_, kMaxBlock);
    auto inst = createInstrument(kind, lib, mgr_->streamer());
    if (!inst) return std::unexpected(inst.error());
    InstrumentEngine* raw = inst->get();
    auto id = mgr_->add(std::move(*inst));
    if (!id) return std::unexpected(id.error());
    live_.store(nullptr, std::memory_order_release);
    if (instId_ >= 0) mgr_->remove(instId_);  // retired only after the audio thread has moved on
    instId_ = *id;
    engine_ = raw;
    kind_ = kind;
    lib_ = std::move(lib);
    live_.store(raw, std::memory_order_release);
    held_[0].store(0);
    held_[1].store(0);
    return {};
}

void AudioHost::setHeld(int note, bool on) noexcept {
    if (note < 0 || note > 127) return;
    const u64 bit = u64{1} << (note & 63);
    if (on) held_[note >> 6].fetch_or(bit, std::memory_order_relaxed);
    else held_[note >> 6].fetch_and(~bit, std::memory_order_relaxed);
}

bool AudioHost::noteHeld(int note) const noexcept {
    if (note < 0 || note > 127) return false;
    return (held_[note >> 6].load(std::memory_order_relaxed) >> (note & 63)) & 1u;
}

void AudioHost::noteOn(int note, int velocity) {
    if (note < 0 || note > 127) return;
    if (auto* e = live_.load(std::memory_order_acquire))
        e->post({NoteEvent::Type::NoteOn, static_cast<u8>(note), static_cast<u8>(std::clamp(velocity, 1, 127)), 0});
    setHeld(note, true);
}

void AudioHost::noteOff(int note) {
    if (note < 0 || note > 127) return;
    if (auto* e = live_.load(std::memory_order_acquire)) e->post({NoteEvent::Type::NoteOff, static_cast<u8>(note), 0, 0});
    setHeld(note, false);
}

void AudioHost::sustain(bool down) {
    if (auto* e = live_.load(std::memory_order_acquire)) e->post({NoteEvent::Type::Sustain, 0, 0, down ? 1.0f : 0.0f});
}

void AudioHost::allNotesOff() {
    if (auto* e = live_.load(std::memory_order_acquire)) e->post({NoteEvent::Type::AllNotesOff, 0, 0, 0});
    held_[0].store(0);
    held_[1].store(0);
}

void AudioHost::onMidi(unsigned long p1) noexcept {
    const u8 status = static_cast<u8>(p1 & 0xFF), d1 = static_cast<u8>((p1 >> 8) & 0x7F), d2 = static_cast<u8>((p1 >> 16) & 0x7F);
    InstrumentEngine* e = live_.load(std::memory_order_acquire);
    bool notify = false;
    switch (status & 0xF0) {
        case 0x90:
            if (e) e->post({d2 ? NoteEvent::Type::NoteOn : NoteEvent::Type::NoteOff, d1, d2, 0});
            setHeld(d1, d2 != 0);
            notify = true;
            break;
        case 0x80:
            if (e) e->post({NoteEvent::Type::NoteOff, d1, 0, 0});
            setHeld(d1, false);
            notify = true;
            break;
        case 0xE0:
            if (e) e->post({NoteEvent::Type::PitchBend, 0, 0, ((d2 << 7 | d1) - 8192) / 8192.0f * 2.0f});
            break;
        case 0xB0:
            if (d1 == 64) {
                if (e) e->post({NoteEvent::Type::Sustain, 0, 0, d2 >= 64 ? 1.0f : 0.0f});
                midiSustain_.store(d2 >= 64, std::memory_order_relaxed);
                notify = true;
            } else if (d1 == 123 && e) {
                e->post({NoteEvent::Type::AllNotesOff, 0, 0, 0});
            }
            break;
        default: break;
    }
    if (notify && notifyWnd_) PostMessageW(static_cast<HWND>(notifyWnd_), notifyMsg_, 0, 0);
}

void AudioHost::openMidi(void* notifyWnd, unsigned notifyMsg) {
    closeMidi();
    notifyWnd_ = notifyWnd;
    notifyMsg_ = notifyMsg;
    const UINT n = midiInGetNumDevs();
    for (UINT d = 0; d < n; ++d) {
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, d, reinterpret_cast<DWORD_PTR>(&midiProc), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            midiInStart(h);
            midiIn_.push_back(h);
        }
    }
}

void AudioHost::closeMidi() {
    for (void* h : midiIn_) {
        midiInStop(static_cast<HMIDIIN>(h));
        midiInClose(static_cast<HMIDIIN>(h));
    }
    midiIn_.clear();
}

AudioHost::Live AudioHost::readLive() {
    Live l;
    l.peakL = peakL_.exchange(0.0f, std::memory_order_relaxed);
    l.peakR = peakR_.exchange(0.0f, std::memory_order_relaxed);
    l.cpu = running_ ? cpu_.load(std::memory_order_relaxed) : 0.0f;
    l.limitDb = 20.0f * std::log10(std::max(1e-6f, limMin_.exchange(1.0f, std::memory_order_relaxed)));
    if (engine_) l.voices = engine_->activeVoices();
    const auto st = mgr_->streamer().stats();
    l.diskBytes = st.bytesCopied;
    l.underruns = st.underruns;
    return l;
}

}  // namespace whe::ui
