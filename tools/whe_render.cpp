// whe_render — offline proof-of-sound: import a real sample pack, build an
// instrument with one of the bespoke engines, play a demo phrase, write a WAV.
//
//   whe_render <sample-folder> <piano|bowed|plucked|aerophone|membrane> <out.wav>
//              [--set Name=value ...] [--seconds N] [--rate 48000] [--block 256]
//              [--notes 60,64,67] [--middle-c 3|4]
#include <cstdio>
#include <sstream>
#include <string>

#include "whe/engines.hpp"
#include "whe/instrument_manager.hpp"
#include "whe/simd.hpp"

using namespace whe;

namespace {

struct Hit {
    double at;
    NoteEvent ev;
};

std::vector<Hit> demoPhrase(EngineKind kind, const Articulation& art, std::vector<int> notes, double seconds) {
    std::vector<Hit> h;
    auto on = [&](double t, int n, int v) { h.push_back({t, {NoteEvent::Type::NoteOn, static_cast<u8>(n), static_cast<u8>(v), 0}}); };
    auto off = [&](double t, int n) { h.push_back({t, {NoteEvent::Type::NoteOff, static_cast<u8>(n), 0, 0}}); };
    if (notes.empty()) {
        if (!art.pitched) {
            notes = {art.zones.front().rootKey};
        } else {
            std::vector<int> roots;
            for (const auto& z : art.zones) roots.push_back(z.rootKey);
            std::ranges::sort(roots);
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            const int mid = roots[roots.size() / 2];
            notes = {mid, mid + 4, mid + 7, mid + 12};
        }
    }
    if (kind == EngineKind::Membrane) {
        const int n = static_cast<int>(seconds / 0.25);
        for (int i = 0; i < n; ++i) {
            const int vel = 20 + (107 * (i % 8)) / 7;
            on(0.1 + i * 0.25, notes[static_cast<std::size_t>(i) % notes.size()], vel);
            off(0.1 + i * 0.25 + 0.1, notes[static_cast<std::size_t>(i) % notes.size()]);
        }
        return h;
    }
    if (kind == EngineKind::Piano || kind == EngineKind::PluckedString) {
        const double step = std::min(0.5, seconds / (notes.size() * 2.0 + 2));
        for (std::size_t i = 0; i < notes.size(); ++i) {
            on(0.1 + i * step, notes[i], 70 + static_cast<int>(i) * 10);
            off(0.1 + i * step + step * 0.9, notes[i]);
        }
        const double chord = 0.2 + notes.size() * step;
        for (int n : notes) on(chord, n, 96);
        for (int n : notes) off(seconds - 1.0, n);
        return h;
    }
    // sustained instruments: long notes
    const double len = (seconds - 0.5) / static_cast<double>(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        on(0.1 + i * len, notes[i], 100);
        off(0.1 + i * len + len * 0.9, notes[i]);
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: whe_render <sample-folder> <piano|bowed|plucked|aerophone|membrane> <out.wav> [--set Name=value] [--seconds N] [--notes a,b,c] [--middle-c 3|4]\n");
        return 2;
    }
    const auto kind = engineKindFromName(argv[2]);
    if (!kind) {
        std::fprintf(stderr, "unknown engine '%s'\n", argv[2]);
        return 2;
    }
    ImportOptions io;
    double seconds = 8.0, rate = 48000.0;
    int block = 256;
    std::vector<std::pair<std::string, float>> sets;
    std::vector<int> notes;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq != std::string::npos) sets.emplace_back(kv.substr(0, eq), std::stof(kv.substr(eq + 1)));
        } else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate = std::stod(argv[++i]);
        else if (a == "--block" && i + 1 < argc) block = std::stoi(argv[++i]);
        else if (a == "--middle-c" && i + 1 < argc) io.naming.middleCOctave = std::stoi(argv[++i]);
        else if (a == "--notes" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string t;
            while (std::getline(ss, t, ',')) notes.push_back(std::stoi(t));
        }
    }

    auto lib = SampleLibrary::import(argv[1], io);
    if (!lib) {
        std::fprintf(stderr, "import failed: %s\n", lib.error().message.c_str());
        return 1;
    }
    std::fputs((*lib)->report().c_str(), stdout);

    InstrumentManager mgr(1);
    mgr.setAudioFormat(rate, block);
    auto inst = createInstrument(*kind, *lib, mgr.streamer());
    if (!inst) {
        std::fprintf(stderr, "cannot build instrument: %s\n", inst.error().message.c_str());
        return 1;
    }
    InstrumentEngine* eng = inst->get();
    for (auto& [name, val] : sets) {
        const int idx = eng->paramIndex(name);
        if (idx < 0) {
            std::fprintf(stderr, "unknown parameter '%s' for %s engine. Available:\n", name.c_str(), eng->kind());
            for (const auto& p : eng->params()) std::fprintf(stderr, "  %-22s %g..%g (default %g %s) — %s\n", p.name, p.min, p.max, p.def, p.unit, p.physics);
            return 2;
        }
        eng->setParam(idx, val);
    }
    auto id = mgr.add(std::move(*inst));
    if (!id) {
        std::fprintf(stderr, "add failed: %s\n", id.error().message.c_str());
        return 1;
    }

    auto phrase = demoPhrase(*kind, eng->articulation(), notes, seconds);
    std::ranges::sort(phrase, {}, &Hit::at);
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    std::vector<float> out(frames * 2);
    std::vector<float> L(static_cast<std::size_t>(block)), R(static_cast<std::size_t>(block));
    std::size_t next = 0;
    double peak = 0, sumSq = 0;
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), frames - pos));
        while (next < phrase.size() && phrase[next].at * rate <= static_cast<double>(pos)) eng->post(phrase[next++].ev);
        mgr.process(L.data(), R.data(), n);
        mgr.streamer().drain();  // offline: let the disk thread finish this block's prefetch before the next
        for (int i = 0; i < n; ++i) {
            out[(pos + static_cast<std::size_t>(i)) * 2] = L[static_cast<std::size_t>(i)];
            out[(pos + static_cast<std::size_t>(i)) * 2 + 1] = R[static_cast<std::size_t>(i)];
            sumSq += static_cast<double>(L[static_cast<std::size_t>(i)]) * L[static_cast<std::size_t>(i)];
            peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[static_cast<std::size_t>(i)]), std::fabs(R[static_cast<std::size_t>(i)]))));
        }
    }
    // Normalize only if the render would clip.
    if (peak > 0.98) {
        const float g = static_cast<float>(0.98 / peak);
        for (float& s : out) s *= g;
    }
    if (auto w = writeWav24(argv[3], static_cast<u32>(rate), 2, out); !w) {
        std::fprintf(stderr, "%s\n", w.error().message.c_str());
        return 1;
    }
    const auto st = mgr.streamer().stats();
    std::printf("Rendered %.1f s with the %s engine (SIMD: %s) -> %s\n", seconds, eng->kind(), simd().name, argv[3]);
    std::printf("Peak %.3f, RMS %.4f%s | disk fills %llu, stale %llu, underruns %llu, streamed %.1f MB\n", peak, std::sqrt(sumSq / static_cast<double>(frames)), peak > 0.98 ? " (normalized)" : "",
                static_cast<unsigned long long>(st.fills), static_cast<unsigned long long>(st.staleFills), static_cast<unsigned long long>(st.underruns),
                st.bytesCopied / 1048576.0);
    return 0;
}
