// whe_play — real-time Windows host: WASAPI out + MIDI in (+ PC keyboard).
//
//   whe_play <sample-folder> <piano|bowed|plucked|aerophone|membrane>
//            [--asio [driver]] [--list-asio] [--rate Hz] [--buffer frames]
//            [--exclusive] [--period <frames>] [--midi <device#>] [--set Name=value ...] [--middle-c 3|4]
// Output: WASAPI by default; --asio uses the named (or first) ASIO driver when
// this build includes the Steinberg ASIO SDK.
//
// MIDI: notes, CC64 sustain, pitch bend (±2 semitones), CC1 -> "Dynamics" when the
// engine has it, CC20..CC29 -> the engine's parameters 0..9 (scaled to their range).
// PC keyboard: A W S E D F T G Y H U J K = one octave from middle C, Z/X = octave down/up,
// 1..9,0 = select parameter, -/+ = change it, Space = sustain toggle, Q = quit.
#include <conio.h>
#include <windows.h>
#include <mmsystem.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "whe/engines.hpp"
#include "whe/instrument_manager.hpp"
#include "whe/platform/wasapi_output.hpp"
#ifdef WHE_HAS_ASIO
#include "whe/platform/asio_output.hpp"
#endif
#include "whe/simd.hpp"

using namespace whe;

static InstrumentEngine* g_engine = nullptr;
static int g_dynamicsParam = -1;

static void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA || !g_engine) return;
    const u8 status = static_cast<u8>(p1 & 0xFF), d1 = static_cast<u8>((p1 >> 8) & 0x7F), d2 = static_cast<u8>((p1 >> 16) & 0x7F);
    switch (status & 0xF0) {
        case 0x90: g_engine->post({d2 ? NoteEvent::Type::NoteOn : NoteEvent::Type::NoteOff, d1, d2, 0}); break;
        case 0x80: g_engine->post({NoteEvent::Type::NoteOff, d1, 0, 0}); break;
        case 0xE0: g_engine->post({NoteEvent::Type::PitchBend, 0, 0, ((d2 << 7 | d1) - 8192) / 8192.0f * 2.0f}); break;
        case 0xB0:
            if (d1 == 64) g_engine->post({NoteEvent::Type::Sustain, 0, 0, d2 >= 64 ? 1.0f : 0.0f});
            else if (d1 == 1 && g_dynamicsParam >= 0) g_engine->setParam(g_dynamicsParam, d2 / 127.0f);
            else if (d1 == 123) g_engine->post({NoteEvent::Type::AllNotesOff, 0, 0, 0});
            else if (d1 >= 20 && d1 <= 29) {
                const int idx = d1 - 20;
                const auto ps = g_engine->params();
                if (idx < static_cast<int>(ps.size())) g_engine->setParam(idx, ps[idx].min + (ps[idx].max - ps[idx].min) * d2 / 127.0f);
            }
            break;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: whe_play <sample-folder> <piano|bowed|plucked|aerophone|membrane> [--asio [driver]] [--list-asio] [--buffer N] [--rate Hz] [--exclusive] [--period N] [--midi N] [--set Name=value]\n");
        return 2;
    }
    const auto kind = engineKindFromName(argv[2]);
    if (!kind) {
        std::fprintf(stderr, "unknown engine '%s'\n", argv[2]);
        return 2;
    }
    WasapiOutput::Options ao;
    bool useAsio = false;
    std::string asioDriver;
    int asioBuffer = 0;
    double asioRate = 0;
    ImportOptions io;
    int midiDev = 0;
    std::vector<std::pair<std::string, float>> sets;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list-asio") {
#ifdef WHE_HAS_ASIO
            const auto drivers = AsioOutput::listDrivers();
            if (drivers.empty()) std::printf("No 64-bit ASIO drivers installed.\n");
            for (const auto& d : drivers) std::printf("ASIO driver: %s\n", d.c_str());
#else
            std::printf("This build has no ASIO support (built without the Steinberg ASIO SDK).\n");
#endif
            return 0;
        } else if (a == "--asio") {
            useAsio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') asioDriver = argv[++i];
        } else if (a == "--buffer" && i + 1 < argc) asioBuffer = std::stoi(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) asioRate = std::stod(argv[++i]);
        else if (a == "--exclusive") ao.exclusive = true;
        else if (a == "--period" && i + 1 < argc) ao.periodFrames = std::stoi(argv[++i]);
        else if (a == "--midi" && i + 1 < argc) midiDev = std::stoi(argv[++i]);
        else if (a == "--middle-c" && i + 1 < argc) io.naming.middleCOctave = std::stoi(argv[++i]);
        else if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq != std::string::npos) sets.emplace_back(kv.substr(0, eq), std::stof(kv.substr(eq + 1)));
        }
    }

    std::printf("Importing %s ...\n", argv[1]);
    auto lib = SampleLibrary::import(argv[1], io);
    if (!lib) {
        std::fprintf(stderr, "import failed: %s\n", lib.error().message.c_str());
        return 1;
    }
    std::fputs((*lib)->report().c_str(), stdout);

    // One small adapter so the rest of the host does not care which driver model plays.
    WasapiOutput wasapi;
#ifdef WHE_HAS_ASIO
    AsioOutput asio;
#endif
    double deviceRate = 0;
    int deviceBuffer = 0;
    std::string deviceDesc;
    if (useAsio) {
#ifdef WHE_HAS_ASIO
        AsioOutput::Options opt;
        opt.driver = asioDriver;
        opt.bufferFrames = asioBuffer;
        opt.sampleRate = asioRate;
        if (auto r = asio.open(opt); !r) {
            std::fprintf(stderr, "ASIO: %s\n", r.error().message.c_str());
            return 1;
        }
        deviceRate = asio.sampleRate();
        deviceBuffer = asio.bufferFrames();
        deviceDesc = asio.description();
#else
        std::fprintf(stderr, "This build has no ASIO support (built without the Steinberg ASIO SDK). Use the ASIO build or drop --asio.\n");
        return 1;
#endif
    } else {
        if (auto r = wasapi.open(ao); !r) {
            std::fprintf(stderr, "audio: %s\n", r.error().message.c_str());
            return 1;
        }
        deviceRate = wasapi.sampleRate();
        deviceBuffer = wasapi.bufferFrames();
        deviceDesc = wasapi.description();
    }
    std::printf("Audio: %s | SIMD: %s\n", deviceDesc.c_str(), simd().name);

    InstrumentManager mgr;
    mgr.setAudioFormat(deviceRate, std::min(deviceBuffer, 1024));
    auto inst = createInstrument(*kind, *lib, mgr.streamer());
    if (!inst) {
        std::fprintf(stderr, "instrument: %s\n", inst.error().message.c_str());
        return 1;
    }
    for (auto& [n, v] : sets) {
        const int idx = (*inst)->paramIndex(n);
        if (idx >= 0) (*inst)->setParam(idx, v);
        else std::fprintf(stderr, "unknown parameter %s (ignored)\n", n.c_str());
    }
    InstrumentEngine* eng = inst->get();
    auto id = mgr.add(std::move(*inst));
    if (!id) {
        std::fprintf(stderr, "%s\n", id.error().message.c_str());
        return 1;
    }
    g_engine = eng;
    g_dynamicsParam = eng->paramIndex("Dynamics");

    HMIDIIN midi = nullptr;
    const UINT nMidi = midiInGetNumDevs();
    for (UINT d = 0; d < nMidi; ++d) {
        MIDIINCAPSW caps{};
        midiInGetDevCapsW(d, &caps, sizeof caps);
        std::wprintf(L"MIDI in %u: %ls%ls\n", d, caps.szPname, static_cast<int>(d) == midiDev ? L"  <- using" : L"");
    }
    if (nMidi > 0 && midiInOpen(&midi, static_cast<UINT>(midiDev), reinterpret_cast<DWORD_PTR>(&midiProc), 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
        midiInStart(midi);

    auto render = [&](float* L, float* R, int n) { mgr.process(L, R, n); };
    Result<void> started = fail("no output");
#ifdef WHE_HAS_ASIO
    if (useAsio) started = asio.start(render);
    else started = wasapi.start(render);
#else
    started = wasapi.start(render);
#endif
    if (auto& r = started; !r) {
        std::fprintf(stderr, "%s\n", r.error().message.c_str());
        return 1;
    }

    std::printf("\nPlaying the %s engine. Parameters:\n", eng->kind());
    const auto ps = eng->params();
    for (std::size_t i = 0; i < ps.size(); ++i) std::printf("  [%zu] %-22s %.3g (%g..%g %s)\n", (i + 1) % 10, ps[i].name, eng->paramTarget(static_cast<int>(i)), ps[i].min, ps[i].max, ps[i].unit);
    std::printf("Keys: A W S E D F T G Y H U J K play, Z/X octave, 1-0 pick param, -/+ adjust, Space sustain, Q quit\n");

    const char* keys = "awsedftgyhujk";
    int octave = 0, sel = 0;
    bool sustain = false;
    // Console keys have no key-up event: each key-press note is released after a short hold.
    using clock = std::chrono::steady_clock;
    std::vector<std::pair<clock::time_point, int>> pendingOff;
    // Keys are read on their own thread (console or redirected stdin) so note-offs keep flowing.
    MpmcQueue<int> keyQueue(64);
    const bool console = GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
    std::jthread reader([&keyQueue, console] {
        for (;;) {
            int k = console ? _getch() : std::getchar();
            if (k == EOF) k = 'q';
            while (!keyQueue.push(k)) Sleep(1);
            if (k == 'q' || k == 'Q') return;
        }
    });
    for (;;) {
        const auto now = clock::now();
        std::erase_if(pendingOff, [&](const auto& p) {
            if (p.first > now) return false;
            eng->post({NoteEvent::Type::NoteOff, static_cast<u8>(p.second), 0, 0});
            return true;
        });
        int c = 0;
        if (!keyQueue.pop(c)) {
            Sleep(5);
            continue;
        }
        if (c == 'q' || c == 'Q') break;
        if (c == 'z') octave = std::max(-4, octave - 1);
        else if (c == 'x') octave = std::min(4, octave + 1);
        else if (c == ' ') {
            sustain = !sustain;
            eng->post({NoteEvent::Type::Sustain, 0, 0, sustain ? 1.0f : 0.0f});
            std::printf("sustain %s\n", sustain ? "down" : "up");
        } else if (c >= '0' && c <= '9') {
            sel = (c == '0') ? 9 : c - '1';
            if (sel < static_cast<int>(ps.size())) std::printf("selected %s = %.3g\n", ps[sel].name, eng->paramTarget(sel));
        } else if ((c == '-' || c == '+' || c == '=') && sel < static_cast<int>(ps.size())) {
            const float step = (ps[sel].max - ps[sel].min) / 20.0f * (c == '-' ? -1.0f : 1.0f);
            eng->setParam(sel, eng->paramTarget(sel) + step);
            std::printf("%s = %.3g\n", ps[sel].name, eng->paramTarget(sel));
        } else if (const char* k = std::strchr(keys, std::tolower(c)); k && c) {
            const int note = 60 + static_cast<int>(k - keys) + 12 * octave;
            eng->post({NoteEvent::Type::NoteOn, static_cast<u8>(note), 100, 0});
            pendingOff.emplace_back(clock::now() + std::chrono::milliseconds(600), note);
        }
    }
    if (midi) {
        midiInStop(midi);
        midiInClose(midi);
    }
    g_engine = nullptr;
    wasapi.stop();
#ifdef WHE_HAS_ASIO
    asio.close();
#endif
    const auto st = mgr.streamer().stats();
    std::printf("disk fills %llu, underruns %llu\n", static_cast<unsigned long long>(st.fills), static_cast<unsigned long long>(st.underruns));
    return 0;
}
