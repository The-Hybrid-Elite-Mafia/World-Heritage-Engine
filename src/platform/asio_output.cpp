// ASIO render client on top of the Steinberg ASIO SDK host layer.
// Compiled only when WHE_HAS_ASIO is defined (SDK found by CMake).
#include "whe/platform/asio_output.hpp"

#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#include <atomic>

// Provided by the Steinberg SDK (host/asiodrivers.cpp).
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace whe {

struct AsioOutput::Impl {
    ASIOBufferInfo infos[2]{};
    ASIOSampleType types[2]{};
    long frames = 0;
    bool postOutput = false;
    bool driverLoaded = false;
    bool buffersCreated = false;
    bool comInit = false;
    std::vector<float> L, R;
    Callback callback;
    std::atomic<bool> running{false};

    static bool supported(ASIOSampleType t) noexcept {
        switch (t) {
            case ASIOSTInt16LSB:
            case ASIOSTInt24LSB:
            case ASIOSTInt32LSB:
            case ASIOSTFloat32LSB:
            case ASIOSTFloat64LSB:
            case ASIOSTInt32LSB16:
            case ASIOSTInt32LSB18:
            case ASIOSTInt32LSB20:
            case ASIOSTInt32LSB24: return true;
            default: return false;
        }
    }

    static void fromFloat(ASIOSampleType t, const float* src, void* dst, long n) noexcept {
        auto clip = [](float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); };
        switch (t) {
            case ASIOSTFloat32LSB: std::memcpy(dst, src, sizeof(float) * static_cast<std::size_t>(n)); break;
            case ASIOSTFloat64LSB:
                for (long i = 0; i < n; ++i) static_cast<double*>(dst)[i] = src[i];
                break;
            case ASIOSTInt16LSB:
                for (long i = 0; i < n; ++i) static_cast<i16*>(dst)[i] = static_cast<i16>(clip(src[i]) * 32767.0f);
                break;
            case ASIOSTInt24LSB: {
                u8* d = static_cast<u8*>(dst);
                for (long i = 0; i < n; ++i) {
                    const i32 x = static_cast<i32>(clip(src[i]) * 8388607.0f);
                    d[i * 3] = static_cast<u8>(x);
                    d[i * 3 + 1] = static_cast<u8>(x >> 8);
                    d[i * 3 + 2] = static_cast<u8>(x >> 16);
                }
                break;
            }
            case ASIOSTInt32LSB:
                for (long i = 0; i < n; ++i) static_cast<i32*>(dst)[i] = static_cast<i32>(static_cast<double>(clip(src[i])) * 2147483647.0);
                break;
            case ASIOSTInt32LSB16:
            case ASIOSTInt32LSB18:
            case ASIOSTInt32LSB20:
            case ASIOSTInt32LSB24: {
                // 32-bit container, right-justified sample of 16/18/20/24 bits
                const int bits = t == ASIOSTInt32LSB16 ? 16 : t == ASIOSTInt32LSB18 ? 18 : t == ASIOSTInt32LSB20 ? 20 : 24;
                const double scale = static_cast<double>((1 << (bits - 1)) - 1);
                for (long i = 0; i < n; ++i) static_cast<i32*>(dst)[i] = static_cast<i32>(clip(src[i]) * scale);
                break;
            }
            default: break;
        }
    }

    void bufferSwitch(long index) noexcept {
        DenormalGuard ftz;
        callback(L.data(), R.data(), static_cast<int>(frames));
        fromFloat(types[0], L.data(), infos[0].buffers[index], frames);
        fromFloat(types[1], R.data(), infos[1].buffers[index], frames);
        if (postOutput) ASIOOutputReady();
    }
};

namespace {

std::atomic<AsioOutput::Impl*> gActive{nullptr};

void onBufferSwitch(long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->bufferSwitch(index);
}
ASIOTime* onBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->bufferSwitch(index);
    return time;
}
void onSampleRateChanged(ASIOSampleRate) {}
long onAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                    value == kAsioSupportsTimeInfo)
                       ? 1L
                       : 0L;
        case kAsioEngineVersion: return 2L;
        case kAsioResetRequest: return 1L;     // host should reopen; whe_play reports it on restart
        case kAsioResyncRequest: return 1L;
        case kAsioLatenciesChanged: return 1L;
        case kAsioSupportsTimeInfo: return 1L;
        case kAsioSupportsTimeCode: return 0L;
        default: return 0L;
    }
}
ASIOCallbacks gCallbacks{&onBufferSwitch, &onSampleRateChanged, &onAsioMessage, &onBufferSwitchTimeInfo};

std::string err(const char* stage, long code) { return std::string(stage) + " failed (ASIO error " + std::to_string(code) + ")"; }

}  // namespace

std::vector<std::string> AsioOutput::listDrivers() {
    std::vector<std::string> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        const LSTATUS st = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        char narrow[512];
        if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr) > 0) out.emplace_back(narrow);
    }
    RegCloseKey(root);
    return out;
}

AsioOutput::~AsioOutput() {
    close();
    delete impl_;
}

Result<void> AsioOutput::open(const Options& opts) {
    close();
    if (!impl_) impl_ = new Impl();
    Impl& s = *impl_;
    Impl* expected = nullptr;
    if (!gActive.compare_exchange_strong(expected, &s)) return fail("another ASIO stream is already open in this process");

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // ASIO drivers are COM objects
    s.comInit = SUCCEEDED(hr);

    std::string name = opts.driver;
    if (name.empty()) {
        const auto all = listDrivers();
        if (all.empty()) {
            close();
            return fail("no 64-bit ASIO driver is installed");
        }
        name = all.front();
    }
    std::vector<char> buf(name.begin(), name.end());
    buf.push_back('\0');
    if (!loadAsioDriver(buf.data())) {
        close();
        return fail("could not load ASIO driver '" + name + "'");
    }
    s.driverLoaded = true;

    ASIODriverInfo info{};
    info.asioVersion = 2;
    HWND owner = GetConsoleWindow();
    info.sysRef = owner ? owner : GetDesktopWindow();
    ASIOError e = ASIOInit(&info);
    if (e != ASE_OK) {
        close();
        return fail(err("ASIOInit", e) + std::string(": ") + info.errorMessage);
    }

    if (opts.sampleRate > 0) {
        if ((e = ASIOCanSampleRate(opts.sampleRate)) != ASE_OK) {
            close();
            return fail("driver cannot run at " + std::to_string(static_cast<int>(opts.sampleRate)) + " Hz");
        }
        if ((e = ASIOSetSampleRate(opts.sampleRate)) != ASE_OK) {
            close();
            return fail(err("ASIOSetSampleRate", e));
        }
    }
    ASIOSampleRate rate = 0;
    if ((e = ASIOGetSampleRate(&rate)) != ASE_OK || rate <= 0) {
        close();
        return fail(err("ASIOGetSampleRate", e));
    }

    long ins = 0, outs = 0;
    if ((e = ASIOGetChannels(&ins, &outs)) != ASE_OK) {
        close();
        return fail(err("ASIOGetChannels", e));
    }
    if (outs < opts.firstOutput + 2) {
        close();
        return fail("driver has " + std::to_string(outs) + " outputs; need outputs " + std::to_string(opts.firstOutput + 1) + "/" +
                    std::to_string(opts.firstOutput + 2));
    }

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if ((e = ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity)) != ASE_OK) {
        close();
        return fail(err("ASIOGetBufferSize", e));
    }
    long size = preferred;
    if (opts.bufferFrames > 0) {
        const long req = opts.bufferFrames;
        const bool inRange = req >= minSize && req <= maxSize;
        const bool fits = granularity == -1 ? ((req & (req - 1)) == 0) : (granularity <= 0 || ((req - minSize) % granularity) == 0);
        if (inRange && fits) size = req;
    }

    for (int c = 0; c < 2; ++c) {
        s.infos[c].isInput = ASIOFalse;
        s.infos[c].channelNum = opts.firstOutput + c;
        s.infos[c].buffers[0] = s.infos[c].buffers[1] = nullptr;
    }
    if ((e = ASIOCreateBuffers(s.infos, 2, size, &gCallbacks)) != ASE_OK) {
        close();
        return fail(err("ASIOCreateBuffers", e));
    }
    s.buffersCreated = true;
    for (int c = 0; c < 2; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = s.infos[c].channelNum;
        ci.isInput = ASIOFalse;
        if ((e = ASIOGetChannelInfo(&ci)) != ASE_OK) {
            close();
            return fail(err("ASIOGetChannelInfo", e));
        }
        if (!Impl::supported(ci.type)) {
            close();
            return fail("unsupported ASIO sample type " + std::to_string(ci.type));
        }
        s.types[c] = ci.type;
    }
    s.frames = size;
    s.L.assign(static_cast<std::size_t>(size), 0.0f);
    s.R.assign(static_cast<std::size_t>(size), 0.0f);
    s.postOutput = ASIOOutputReady() == ASE_OK;

    long inLat = 0, outLat = 0;
    ASIOGetLatencies(&inLat, &outLat);
    sampleRate_ = rate;
    bufferFrames_ = static_cast<int>(size);
    description_ = std::string("ASIO '") + info.name + "', " + std::to_string(static_cast<int>(rate)) + " Hz, buffer " + std::to_string(size) +
                   " frames, outputs " + std::to_string(opts.firstOutput + 1) + "/" + std::to_string(opts.firstOutput + 2) + ", output latency " +
                   std::to_string(1000.0 * outLat / rate).substr(0, 5) + " ms";
    return {};
}

Result<void> AsioOutput::start(Callback cb) {
    if (!impl_ || !impl_->buffersCreated) return fail("ASIO output not open");
    impl_->callback = std::move(cb);
    impl_->running.store(true, std::memory_order_release);
    if (const ASIOError e = ASIOStart(); e != ASE_OK) {
        impl_->running.store(false, std::memory_order_release);
        return fail(err("ASIOStart", e));
    }
    return {};
}

void AsioOutput::stop() noexcept {
    if (impl_ && impl_->running.exchange(false, std::memory_order_acq_rel)) ASIOStop();
}

void AsioOutput::close() noexcept {
    if (!impl_) return;
    stop();
    Impl& s = *impl_;
    if (s.buffersCreated) {
        ASIODisposeBuffers();
        s.buffersCreated = false;
    }
    if (s.driverLoaded) {
        ASIOExit();
        s.driverLoaded = false;
    }
    Impl* self = &s;
    gActive.compare_exchange_strong(self, nullptr);
    if (s.comInit) {
        CoUninitialize();
        s.comInit = false;
    }
}

}  // namespace whe
