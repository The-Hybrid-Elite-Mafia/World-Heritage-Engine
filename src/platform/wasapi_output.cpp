// WASAPI render client. Windows only.
#include "whe/platform/wasapi_output.hpp"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

namespace whe {

namespace {

// GUIDs spelled out so no import library or INITGUID trickery is needed.
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string hr(const char* what, HRESULT h) {
    char b[128];
    std::snprintf(b, sizeof b, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(h));
    return b;
}

enum class SampleKind { Float32, Int32, Int24, Int16 };

WAVEFORMATEXTENSIBLE makeFormat(SampleKind k, DWORD rate, WORD channels) {
    WAVEFORMATEXTENSIBLE f{};
    const WORD bits = k == SampleKind::Int16 ? 16 : (k == SampleKind::Int24 ? 24 : 32);
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = channels;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = bits;
    f.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    f.SubFormat = k == SampleKind::Float32 ? kSubtypeFloat : kSubtypePcm;
    return f;
}

}  // namespace

struct WasapiOutput::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    bool exclusive = false;
    bool comInit = false;
    SampleKind kind = SampleKind::Float32;
    int channels = 2;
    UINT32 bufferFrames = 0;
    std::vector<float> L, R;

    ~Impl() {
        safeRelease(render);
        safeRelease(client);
        safeRelease(device);
        safeRelease(enumerator);
        if (event) CloseHandle(event);
        if (comInit) CoUninitialize();
    }
};

WasapiOutput::~WasapiOutput() {
    stop();
    delete impl_;
}

Result<void> WasapiOutput::open(const Options& opts) {
    if (impl_) return fail("WASAPI output already open");
    impl_ = new Impl();
    Impl& m = *impl_;
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m.comInit = SUCCEEDED(h);
    h = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&m.enumerator));
    if (FAILED(h)) return fail(hr("CoCreateInstance(MMDeviceEnumerator)", h));
    h = m.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m.device);
    if (FAILED(h)) return fail(hr("GetDefaultAudioEndpoint", h));
    auto activate = [&]() -> HRESULT {
        safeRelease(m.client);
        return m.device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m.client));
    };
    h = activate();
    if (FAILED(h)) return fail(hr("IMMDevice::Activate(IAudioClient)", h));

    WAVEFORMATEX* mix = nullptr;
    h = m.client->GetMixFormat(&mix);
    if (FAILED(h) || !mix) return fail(hr("GetMixFormat", h));
    const DWORD rate = mix->nSamplesPerSec;
    WAVEFORMATEXTENSIBLE chosen{};
    std::memcpy(&chosen, mix, std::min<std::size_t>(sizeof chosen, sizeof(WAVEFORMATEX) + mix->cbSize));
    CoTaskMemFree(mix);
    m.exclusive = opts.exclusive;

    if (!m.exclusive) {
        // Shared mode: the engine renders in the mix format (almost always float32).
        m.channels = chosen.Format.nChannels;
        const bool isFloat = (chosen.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                             (chosen.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(chosen.SubFormat, kSubtypeFloat));
        if (isFloat && chosen.Format.wBitsPerSample == 32) m.kind = SampleKind::Float32;
        else if (chosen.Format.wBitsPerSample == 16) m.kind = SampleKind::Int16;
        else if (chosen.Format.wBitsPerSample == 24) m.kind = SampleKind::Int24;
        else m.kind = SampleKind::Int32;
        h = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &chosen.Format, nullptr);
        if (FAILED(h)) return fail(hr("IAudioClient::Initialize(shared)", h));
    } else {
        // Exclusive mode: negotiate the best stereo format the hardware takes at the mix rate.
        m.channels = 2;
        bool found = false;
        for (SampleKind k : {SampleKind::Float32, SampleKind::Int32, SampleKind::Int24, SampleKind::Int16}) {
            WAVEFORMATEXTENSIBLE f = makeFormat(k, rate, 2);
            if (m.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK) {
                chosen = f;
                m.kind = k;
                found = true;
                break;
            }
        }
        if (!found) return fail("device accepts no stereo float32/int32/int24/int16 format in exclusive mode at " + std::to_string(rate) + " Hz");
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        m.client->GetDevicePeriod(&defPeriod, &minPeriod);
        REFERENCE_TIME period = minPeriod;
        if (opts.periodFrames > 0) period = std::max<REFERENCE_TIME>(minPeriod, static_cast<REFERENCE_TIME>(10000000.0 * opts.periodFrames / rate + 0.5));
        h = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &chosen.Format, nullptr);
        if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            m.client->GetBufferSize(&frames);
            period = static_cast<REFERENCE_TIME>(10000000.0 * frames / rate + 0.5);
            h = activate();
            if (FAILED(h)) return fail(hr("IMMDevice::Activate (re-align)", h));
            h = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &chosen.Format, nullptr);
        }
        if (FAILED(h)) return fail(hr("IAudioClient::Initialize(exclusive)", h));
    }

    m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m.event) return fail("CreateEvent failed");
    h = m.client->SetEventHandle(m.event);
    if (FAILED(h)) return fail(hr("SetEventHandle", h));
    h = m.client->GetBufferSize(&m.bufferFrames);
    if (FAILED(h)) return fail(hr("GetBufferSize", h));
    h = m.client->GetService(kIidIAudioRenderClient, reinterpret_cast<void**>(&m.render));
    if (FAILED(h)) return fail(hr("GetService(IAudioRenderClient)", h));
    m.L.assign(m.bufferFrames, 0.0f);
    m.R.assign(m.bufferFrames, 0.0f);

    sampleRate_ = rate;
    bufferFrames_ = static_cast<int>(m.bufferFrames);
    static const char* kinds[] = {"float32", "int32", "int24", "int16"};
    description_ = std::string(m.exclusive ? "WASAPI exclusive " : "WASAPI shared ") + kinds[static_cast<int>(m.kind)] + ", " +
                   std::to_string(rate) + " Hz, " + std::to_string(m.channels) + " ch, buffer " + std::to_string(m.bufferFrames) + " frames (" +
                   std::to_string(1000.0 * m.bufferFrames / rate).substr(0, 5) + " ms)";
    return {};
}

Result<void> WasapiOutput::start(Callback cb) {
    if (!impl_ || !impl_->client) return fail("WASAPI output not open");
    cb_ = std::move(cb);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    return {};
}

void WasapiOutput::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        if (impl_ && impl_->event) SetEvent(impl_->event);
        thread_.join();
    }
}

void WasapiOutput::run(std::stop_token st) {
    Impl& m = *impl_;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    auto fill = [&](UINT32 frames) {
        BYTE* data = nullptr;
        if (FAILED(m.render->GetBuffer(frames, &data)) || !data) return;
        cb_(m.L.data(), m.R.data(), static_cast<int>(frames));
        const int C = m.channels;
        for (UINT32 i = 0; i < frames; ++i) {
            for (int c = 0; c < C; ++c) {
                const float v = c == 0 ? m.L[i] : (c == 1 ? m.R[i] : 0.0f);
                const float s = std::clamp(v, -1.0f, 1.0f);
                const std::size_t idx = static_cast<std::size_t>(i) * C + c;
                switch (m.kind) {
                    case SampleKind::Float32: reinterpret_cast<float*>(data)[idx] = v; break;
                    case SampleKind::Int32: reinterpret_cast<i32*>(data)[idx] = static_cast<i32>(s * 2147483520.0f); break;
                    case SampleKind::Int16: reinterpret_cast<i16*>(data)[idx] = static_cast<i16>(s * 32767.0f); break;
                    case SampleKind::Int24: {
                        const i32 x = static_cast<i32>(s * 8388607.0f);
                        BYTE* p = data + idx * 3;
                        p[0] = static_cast<BYTE>(x);
                        p[1] = static_cast<BYTE>(x >> 8);
                        p[2] = static_cast<BYTE>(x >> 16);
                        break;
                    }
                }
            }
        }
        m.render->ReleaseBuffer(frames, 0);
    };

    fill(m.bufferFrames);  // pre-roll so the first period is not silence-glitched
    m.client->Start();
    while (!st.stop_requested()) {
        if (WaitForSingleObject(m.event, 200) != WAIT_OBJECT_0) continue;
        if (st.stop_requested()) break;
        UINT32 frames = m.bufferFrames;
        if (!m.exclusive) {
            UINT32 padding = 0;
            if (FAILED(m.client->GetCurrentPadding(&padding))) continue;
            frames = m.bufferFrames - padding;
        }
        if (frames) fill(frames);
    }
    m.client->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace whe
