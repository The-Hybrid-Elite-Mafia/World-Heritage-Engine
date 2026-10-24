#include <algorithm>
// World Heritage Engine test suite (no framework, no dependencies).
//
//   whe_tests [real-pack-folder ...]
//
// Synthetic WAV/AIFF files written by these tests are FIXTURES for the parser,
// decoder and streaming checks only. Instrument sound tests run on the real
// recorded packs passed on the command line.
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>

#include "whe/engines.hpp"
#include "whe/instrument_manager.hpp"
#include "whe/simd.hpp"

using namespace whe;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) {                                                                    \
            ++g_pass;                                                                  \
        } else {                                                                       \
            ++g_fail;                                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

static void section(const char* name) { std::printf("[%s]\n", name); }

// ----------------------------------------------------------------------------
// Fixture writers
// ----------------------------------------------------------------------------
static void put16(std::vector<u8>& b, u16 v, bool be = false) {
    if (be) { b.push_back(u8(v >> 8)); b.push_back(u8(v)); }
    else { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
}
static void put32(std::vector<u8>& b, u32 v, bool be = false) {
    if (be) for (int i = 3; i >= 0; --i) b.push_back(u8(v >> (8 * i)));
    else for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
static void putTag(std::vector<u8>& b, const char* t) { b.insert(b.end(), t, t + 4); }

static std::vector<i16> fixtureSignal(std::size_t frames, int channels, double hz, double sr) {
    std::vector<i16> s(frames * static_cast<std::size_t>(channels));
    for (std::size_t f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c)
            s[f * channels + c] = static_cast<i16>(std::lround(12000.0 * std::sin(2 * std::numbers::pi * hz * (f + c * 7) / sr)));
    return s;
}

static void writeWav16(const fs::path& p, const std::vector<i16>& s, int channels, u32 sr, int smplRoot = -1, u32 loopStart = 0, u32 loopEnd = 0) {
    std::vector<u8> b;
    putTag(b, "RIFF"); put32(b, 0); putTag(b, "WAVE");
    putTag(b, "fmt "); put32(b, 16); put16(b, 1); put16(b, u16(channels)); put32(b, sr); put32(b, sr * channels * 2); put16(b, u16(channels * 2)); put16(b, 16);
    if (smplRoot >= 0) {
        putTag(b, "smpl"); put32(b, 36 + (loopEnd ? 24 : 0));
        put32(b, 0); put32(b, 0); put32(b, 1000000000u / sr); put32(b, u32(smplRoot)); put32(b, 0); put32(b, 0); put32(b, 0);
        put32(b, loopEnd ? 1 : 0); put32(b, 0);
        if (loopEnd) { put32(b, 0); put32(b, 0); put32(b, loopStart); put32(b, loopEnd - 1); put32(b, 0); put32(b, 0); }
    }
    putTag(b, "data"); put32(b, u32(s.size() * 2));
    for (i16 v : s) put16(b, u16(v));
    const u32 riff = u32(b.size() - 8);
    std::memcpy(b.data() + 4, &riff, 4);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static void writeAiff16(const fs::path& p, const std::vector<i16>& s, int channels, int baseNote, u32 loopStart, u32 loopEnd) {
    std::vector<u8> b;
    putTag(b, "FORM"); put32(b, 0, true); putTag(b, "AIFF");
    putTag(b, "COMM"); put32(b, 18, true); put16(b, u16(channels), true); put32(b, u32(s.size() / channels), true); put16(b, 16, true);
    const u8 rate48k[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};  // 48000 as 80-bit extended
    b.insert(b.end(), rate48k, rate48k + 10);
    putTag(b, "MARK"); put32(b, 2 + 2 * 8, true); put16(b, 2, true);
    put16(b, 1, true); put32(b, loopStart, true); b.push_back(0); b.push_back(0);
    put16(b, 2, true); put32(b, loopEnd, true); b.push_back(0); b.push_back(0);
    putTag(b, "INST"); put32(b, 20, true);
    b.push_back(u8(baseNote)); b.push_back(0); b.push_back(0); b.push_back(127); b.push_back(1); b.push_back(127); put16(b, 0, true);
    put16(b, 1, true); put16(b, 1, true); put16(b, 2, true);  // sustain loop forward, markers 1..2
    put16(b, 0, true); put16(b, 0, true); put16(b, 0, true);
    putTag(b, "SSND"); put32(b, u32(8 + s.size() * 2), true); put32(b, 0, true); put32(b, 0, true);
    for (i16 v : s) put16(b, u16(v), true);
    const u32 sz = u32(b.size() - 8);
    b[4] = u8(sz >> 24); b[5] = u8(sz >> 16); b[6] = u8(sz >> 8); b[7] = u8(sz);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// ----------------------------------------------------------------------------
static void testMd5() {
    section("md5");
    auto hex = [](std::array<u8, 16> d) {
        char s[33];
        for (int i = 0; i < 16; ++i) std::snprintf(s + 2 * i, 3, "%02x", d[i]);
        return std::string(s);
    };
    Md5 a;
    CHECK(hex(a.finish()) == "d41d8cd98f00b204e9800998ecf8427e");
    Md5 b;
    b.update(reinterpret_cast<const u8*>("abc"), 3);
    CHECK(hex(b.finish()) == "900150983cd24fb0d6963f7d28e17f72");
    Md5 c;
    const std::string q = "The quick brown fox jumps over the lazy dog";
    c.update(reinterpret_cast<const u8*>(q.data()), q.size());
    CHECK(hex(c.finish()) == "9e107d9d372bb6826bd81d3542a419d6");
}

static void testParser() {
    section("file-name parser");
    auto p = parseSampleName("Snare_Hard_v127_RR3");
    CHECK(p.words == std::vector<std::string>{"snare"});
    CHECK(p.dynamicRank == 7);
    CHECK(p.ambiguousV == 127);
    CHECK(p.roundRobin == 3);
    CHECK(!p.note);

    p = parseSampleName("Cello_Sustain_C3_v100");
    CHECK(p.note == 48);
    CHECK(p.ambiguousV == 100);
    CHECK((p.words == std::vector<std::string>{"cello", "sustain"}));
    CHECK(parseSampleName("Cello_Sustain_C3_v100", NamingOptions{3}).note == 60);  // C3 = 60 convention

    p = parseSampleName("C4v12");  // Salamander Grand Piano style
    CHECK(p.note == 60);
    CHECK(p.ambiguousV == 12);

    p = parseSampleName("F#-1_vel64");
    CHECK(p.note == 6);
    CHECK(p.ambiguousV == 64);

    p = parseSampleName("Db4 pp rr2");
    CHECK(p.note == 61);
    CHECK(p.dynamicRank == 2);
    CHECK(p.roundRobin == 2);

    p = parseSampleName("harmLA0");
    CHECK(p.note == 21);
    CHECK(p.words == std::vector<std::string>{"harml"});

    p = parseSampleName("Hard3");  // must NOT be read as D3
    CHECK(!p.note);

    p = parseSampleName("snaremic_snare_center_vl17");
    CHECK(p.layerIndex == 17);
    CHECK((p.words == std::vector<std::string>{"snaremic", "snare", "center"}));

    p = parseSampleName("Piano_A#2_mf_rel");
    CHECK(p.note == 46);
    CHECK(p.release);
    CHECK(p.dynamicRank == 5);

    p = parseSampleName("Violin_Legato_G3_key55");
    CHECK(p.note == 55);  // explicit key tag wins (last note token)

    CHECK(gmPercussionKey({"kick"}) == 36);
    CHECK(gmPercussionKey({"snaremic", "snare", "center"}) == 38);
    CHECK(gmPercussionKey({"hihat", "open"}) == 46);
    CHECK(gmPercussionKey({"ride", "bell"}) == 53);
}

static void testDecoders(const fs::path& dir) {
    section("decoders (fixtures)");
    const auto sig = fixtureSignal(5000, 2, 440, 48000);
    writeWav16(dir / "fixture.wav", sig, 2, 48000, 64, 1000, 4000);
    auto w = decodeAudioFile(dir / "fixture.wav");
    CHECK(w.has_value());
    if (w) {
        CHECK(w->channels == 2 && w->frames == 5000 && w->sampleRate == 48000);
        CHECK(w->meta.rootKey == 64);
        CHECK(w->meta.loop && w->meta.loop->start == 1000 && w->meta.loop->end == 4000);
        bool exact = true;
        for (std::size_t i = 0; i < sig.size(); ++i) exact &= w->interleaved[i] == sig[i] / 32768.0f;
        CHECK(exact);
    }
    writeAiff16(dir / "fixture.aif", sig, 2, 57, 500, 3000);
    auto a = decodeAudioFile(dir / "fixture.aif");
    CHECK(a.has_value());
    if (a) {
        CHECK(a->frames == 5000 && a->sampleRate == 48000 && a->channels == 2);
        CHECK(a->meta.rootKey == 57);
        CHECK(a->meta.loop && a->meta.loop->start == 500 && a->meta.loop->end == 3000);
        bool exact = true;
        for (std::size_t i = 0; i < sig.size(); ++i) exact &= a->interleaved[i] == sig[i] / 32768.0f;
        CHECK(exact);
    }
    std::ofstream(dir / "junk.wav") << "this is not audio";
    CHECK(!decodeAudioFile(dir / "junk.wav").has_value());
}

static void testMapper(const fs::path& dir) {
    section("auto mapper (fixtures)");
    const fs::path pack = dir / "MapperPack";
    fs::remove_all(pack);
    fs::create_directories(pack);
    const auto sig = fixtureSignal(2000, 1, 220, 48000);
    for (const char* n : {"Cello_Sustain_C3_v40", "Cello_Sustain_C3_v100", "Cello_Sustain_C3_v127", "Cello_Sustain_G3_v40", "Cello_Sustain_G3_v100",
                          "Cello_Sustain_G3_v127", "Cello_Sustain_C4_v100", "Cello_Pizz_C3_RR1", "Cello_Pizz_C3_RR2", "Cello_Pizz_C3_RR3"})
        writeWav16(pack / (std::string(n) + ".wav"), sig, 1, 48000);
    auto lib = SampleLibrary::import(pack);
    CHECK(lib.has_value());
    if (!lib) return;
    const Articulation* sus = (*lib)->articulation("sustain");
    const Articulation* piz = (*lib)->articulation("pizz");
    CHECK(sus && piz);
    if (!sus || !piz) return;
    CHECK(sus->pitched);
    // key zones split at midpoints between roots 48, 55, 60
    auto z = sus->find(51, 90);
    CHECK(z && z->rootKey == 48);
    z = sus->find(52, 90);
    CHECK(z && z->rootKey == 55);
    z = sus->find(57, 90);
    CHECK(z && z->rootKey == 55);
    z = sus->find(58, 90);
    CHECK(z && z->rootKey == 60);
    CHECK(sus->find(0, 64) && sus->find(127, 64));  // outer zones stretch to the ends
    // velocity layers from MIDI velocities 40 / 100 / 127
    z = sus->find(48, 40);
    CHECK(z && z->loVel == 1 && z->hiVel == 40);
    z = sus->find(48, 41);
    CHECK(z && z->loVel == 41 && z->hiVel == 100);
    z = sus->find(48, 127);
    CHECK(z && z->loVel == 101 && z->hiVel == 127);
    z = sus->find(60, 10);
    CHECK(z && z->loVel == 1 && z->hiVel == 127);  // single layer covers all velocities
    // round robins
    z = piz->find(48, 64);
    CHECK(z && z->roundRobins.size() == 3);
    // ingest cache reuse
    auto again = SampleLibrary::import(pack);
    CHECK(again && (*again)->ingestStats().cacheHits == 10 && (*again)->ingestStats().decoded == 0);

    // layer-index convention: v1..v4 contiguous -> evenly spread layers
    const fs::path pack2 = dir / "LayerPack";
    fs::remove_all(pack2);
    fs::create_directories(pack2);
    for (int v = 1; v <= 4; ++v) writeWav16(pack2 / ("A4v" + std::to_string(v) + ".wav"), sig, 1, 48000);
    auto lib2 = SampleLibrary::import(pack2);
    CHECK(lib2.has_value());
    if (lib2) {
        const Articulation* a = (*lib2)->primary();
        CHECK(a && a->zones.size() == 4);
        if (a) {
            CHECK(a->find(69, 1)->layer == 0);
            CHECK(a->find(69, 32)->layer == 0);
            CHECK(a->find(69, 33)->layer == 1);
            CHECK(a->find(69, 127)->layer == 3);
        }
    }
    // MIDI note numbers in file names (HV_40 … = notes, HV/MV = velocity layers)
    {
        const fs::path pk = dir / "MidiNumPack";
        fs::remove_all(pk);
        fs::create_directories(pk);
        for (int n : {40, 45, 50, 55, 60}) writeWav16(pk / ("HV_" + std::to_string(n) + ".wav"), sig, 1, 48000);
        for (int n : {40, 50, 60}) writeWav16(pk / ("MV_" + std::to_string(n) + ".wav"), sig, 1, 48000);
        auto l = SampleLibrary::import(pk);
        CHECK(l.has_value());
        if (l) {
            const Articulation* a = (*l)->primary();
            CHECK(a && a->pitched && (*l)->articulations().size() == 1);
            CHECK(a && a->find(40, 127) && a->find(40, 127)->rootKey == 40);
            CHECK(a && a->find(55, 100) && a->find(55, 100)->rootKey == 55);
            CHECK(a && a->find(40, 1) && a->find(40, 1)->layerCount == 2);  // MV + HV layers on 40
        }
    }
    // Takes "Da_Test01..04": strokes on separate keys, takes are round robins, one playable articulation
    {
        const fs::path pk = dir / "StrokePack";
        fs::remove_all(pk);
        fs::create_directories(pk);
        for (const char* s : {"Da", "Ge", "Na"})
            for (int t = 1; t <= 4; ++t) writeWav16(pk / (std::string(s) + "_Test0" + std::to_string(t) + ".wav"), sig, 1, 48000);
        auto l = SampleLibrary::import(pk);
        CHECK(l.has_value());
        if (l) {
            const Articulation* a = (*l)->primary();
            CHECK(a && !a->pitched && a->zones.size() == 3);
            CHECK(a && std::ranges::all_of(a->zones, [](const Zone& z) { return z.roundRobins.size() == 4; }));
        }
    }
    // Unpitched extras next to real notes go above the notes, never on top of them
    {
        const fs::path pk = dir / "UkePack";
        fs::remove_all(pk);
        fs::create_directories(pk);
        for (const char* n : {"C4", "E4", "G4", "C5", "body-tap", "chuck"}) writeWav16(pk / (std::string(n) + ".wav"), sig, 1, 48000);
        auto l = SampleLibrary::import(pk);
        CHECK(l.has_value());
        if (l) {
            const Articulation* a = (*l)->primary();
            CHECK(a && a->pitched);
            CHECK(a && a->find(60, 100) && a->find(60, 100)->rootKey == 60);
            CHECK(a && a->find(74, 100) && !a->find(74, 100)->pitched);  // first hit: C5 (72) + 2
            CHECK(a && a->find(75, 100) && !a->find(75, 100)->pitched);
        }
    }
    // MappingChart.txt: number in the name -> key
    {
        const fs::path pk = dir / "ChartPack";
        fs::remove_all(pk);
        fs::create_directories(pk);
        for (int i : {0, 2, 4}) {
            char nm[64];
            std::snprintf(nm, sizeof nm, "Player_dyn1_rr1_%03d.wav", i);
            writeWav16(pk / nm, sig, 1, 48000);
        }
        std::ofstream(pk / "MappingChart.txt") << "Notation=KeyNumber\n000=21\n002=25\n004=29";
        auto l = SampleLibrary::import(pk);
        CHECK(l.has_value());
        if (l) {
            const Articulation* a = (*l)->primary();
            CHECK(a && a->find(21, 64) && a->find(21, 64)->rootKey == 21);
            CHECK(a && a->find(29, 64) && a->find(29, 64)->rootKey == 29);
            CHECK(!(*l)->mappingGuessed());
        }
    }
    // SFZ wins over file names: #define, #include, groups, seq_position, release trigger, keyswitch default
    {
        const fs::path pk = dir / "SfzPack";
        fs::remove_all(pk);
        fs::create_directories(pk / "samples");
        fs::create_directories(pk / "inc");
        for (const char* n : {"a1", "a2", "b1", "rel", "alt"}) writeWav16(pk / "samples" / (std::string(n) + ".wav"), sig, 1, 48000);
        std::ofstream(pk / "inc" / "notes.txt") << "<region> sample=$P1.wav seq_position=1\n<region> sample=$P2.wav seq_position=2\n";
        std::ofstream(pk / "pack.sfz") << "// test\n#define $P1 a1\n#define $P2 a2\n<control> default_path=samples/\n"
                                          "<global> sw_default=c0\n<master> sw_last=c0\n"
                                          "<group> lokey=c4 hikey=d4 pitch_keycenter=c4 seq_length=2 volume=-6\n#include \"inc/notes.txt\"\n"
                                          "<group> key=70 lovel=1 hivel=127 <region> sample=b1.wav\n"
                                          "<master> sw_last=c#0\n<group> key=70 <region> sample=alt.wav\n"
                                          "<global> trigger=release <region> key=60 sample=rel.wav\n"
                                          "<global> <region> key=61 on_locc64=127 sample=b1.wav\n";
        auto l = SampleLibrary::import(pk);
        CHECK(l.has_value());
        if (l) {
            const Articulation* a = (*l)->primary();
            CHECK((*l)->mappingSource() == "pack.sfz");
            CHECK(a && a->find(61, 100) && a->find(61, 100)->roundRobins.size() == 2 && a->find(61, 100)->rootKey == 60);
            CHECK(a && a->find(61, 100) && std::abs(a->find(61, 100)->gain - 0.501f) < 0.01f);
            CHECK(a && a->find(70, 100) && a->find(70, 100)->roundRobins.size() == 1);  // c#0 keyswitch region left out
            CHECK((*l)->releaseArticulation() && (*l)->releaseArticulation()->find(60, 64));
            CHECK((*l)->store().size() == 4);  // alt.wav (other keyswitch) never decoded
        }
    }
    // no recordings -> no instrument
    const fs::path empty = dir / "EmptyPack";
    fs::create_directories(empty);
    CHECK(!SampleLibrary::import(empty).has_value());
}

static void testDfd(const fs::path& dir) {
    section("direct-from-disk streaming");
    const fs::path pack = dir / "StreamPack";
    fs::remove_all(pack);
    fs::create_directories(pack);
    // 200k frames with an embedded loop: exercises preload, many regions, and loop wrap.
    std::vector<i16> s(200000 * 2);
    std::mt19937 rng(7);
    for (auto& v : s) v = static_cast<i16>(static_cast<int>(rng() % 60000) - 30000);
    writeWav16(pack / "Tone_A3.wav", s, 2, 48000, 57, 150000, 190000);
    writeWav16(pack / "Tone_A4.wav", std::vector<i16>(s.begin(), s.begin() + 2 * 30000), 2, 48000);
    ImportOptions io;
    io.preloadFrames = 3000;
    auto lib = SampleLibrary::import(pack, io);
    CHECK(lib.has_value());
    if (!lib) return;
    const StoredSample* looped = nullptr;
    const StoredSample* plain = nullptr;
    for (std::size_t i = 0; i < (*lib)->store().size(); ++i) {
        const auto& ss = (*lib)->store().get(static_cast<u32>(i));
        (ss.hasLoop() ? looped : plain) = &ss;
    }
    CHECK(looped && plain);
    if (!looped || !plain) return;
    CHECK((*lib)->store().residentBytes() < 2 * 3000 * 2 * sizeof(float) + 1024);  // only heads are in RAM

    DiskStreamer streamer(4096);
    BlockPool pool(streamer.bufferFloats(), 4);
    for (const StoredSample* smp : {plain, looped}) {
        StreamCursor cur;
        CHECK(cur.attach(streamer, pool));
        cur.start(*smp, true);
        const u32 C = smp->channels();
        std::vector<float> got(700 * C);
        bool same = true;
        i64 pos = 0;
        const i64 end = smp->hasLoop() ? 400000 : static_cast<i64>(smp->frameCount()) + 2000;
        int blocks = 0;
        while (pos < end) {
            cur.service(pos);
            streamer.drain();
            cur.gather(pos, 700, got.data());
            for (int f = 0; f < 700; ++f) {
                const i64 v = pos + f;
                i64 fileFrame = v;
                if (smp->hasLoop() && v >= static_cast<i64>(smp->header.loopEnd))
                    fileFrame = smp->header.loopStart + (v - smp->header.loopStart) % (smp->header.loopEnd - smp->header.loopStart);
                for (u32 c = 0; c < C; ++c) {
                    const float want = (!smp->hasLoop() && v >= static_cast<i64>(smp->frameCount())) ? 0.0f : smp->frames[fileFrame * C + c];
                    same &= got[static_cast<std::size_t>(f) * C + c] == want;
                }
            }
            pos += 683;  // window advances by less than its length, like a voice does
            ++blocks;
        }
        CHECK(same);
        cur.stop();
        streamer.drain();
        cur.detach(pool);
    }
    const auto st = streamer.stats();
    CHECK(st.underruns == 0);
    CHECK(st.fills > 50);

    // restarting a voice mid-stream must never serve the old generation's data
    StreamCursor cur;
    CHECK(cur.attach(streamer, pool));
    cur.start(*looped, true);
    cur.service(5000);
    cur.start(*plain, true);  // steal before the fills land
    streamer.drain();
    std::vector<float> got(100 * 2);
    cur.service(3000);
    streamer.drain();
    cur.gather(3000, 100, got.data());
    bool ok = true;
    for (int f = 0; f < 100; ++f)
        for (int c = 0; c < 2; ++c) ok &= got[static_cast<std::size_t>(f) * 2 + c] == plain->frames[(3000 + f) * 2 + c];
    CHECK(ok);
    cur.stop();
    streamer.drain();
    cur.detach(pool);

    // lock-free pool exhausts cleanly and recycles
    BlockPool tiny(16, 3);
    float* a = tiny.acquire();
    float* b = tiny.acquire();
    float* c = tiny.acquire();
    CHECK(a && b && c && !tiny.acquire());
    tiny.release(b);
    CHECK(tiny.acquire() == b);
}

// Every hybrid hook must be input-driven: silence in -> exact silence out,
// and once the recording stops the response must decay away.
static void testNoSynthesisInvariants() {
    section("hybrid hooks never synthesize");
    const float fs = 48000;
    auto probe = [&](const char* name, const std::function<float(float)>& f, int decaySeconds = 8) {
        bool silent = true;
        for (int i = 0; i < 20000; ++i) silent &= f(0.0f) == 0.0f;
        float peak = 0;
        bool finite = true;
        // a burst of "recording" then silence
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> d(-0.8f, 0.8f);
        for (int i = 0; i < 4800; ++i) {
            const float y = f(d(rng));
            finite &= std::isfinite(y);
            peak = std::max(peak, std::fabs(y));
        }
        float tail = 0;
        const int total = decaySeconds * 48000;
        for (int i = 0; i < total; ++i) {
            const float y = f(0.0f);
            finite &= std::isfinite(y);
            peak = std::max(peak, std::fabs(y));
            if (i > total - 4800) tail = std::max(tail, std::fabs(y));
        }
        const bool ok = silent && finite && peak < 1e3f && tail < 1e-4f * std::max(peak, 1e-3f);
        std::printf("  %-34s silent-in=%s finite=%s peak=%.3g tail=%.2g %s\n", name, silent ? "yes" : "NO", finite ? "yes" : "NO", peak, tail,
                    ok ? "ok" : "FAIL");
        CHECK(ok);
    };

    for (float stiff : {0.0f, 1.0f})
        for (float bright : {0.0f, 1.0f}) {
            hybrid::StringResonator s;
            s.prepare(fs, 20);
            s.setFrequency(55);
            s.setDecay(2.0f);
            s.setStiffness(stiff);
            s.setBrightness(bright);
            s.setExcitePosition(0.13f);
            char n[64];
            std::snprintf(n, sizeof n, "StringResonator stiff=%.0f bright=%.0f", stiff, bright);
            probe(n, [&](float x) { return s.process(x); }, 10);
        }
    for (float pressure : {0.0f, 1.0f})
        for (float rosin : {0.0f, 1.0f}) {
            hybrid::BowFrictionShaper b;
            b.prepare(fs, 20);
            b.setFrequency(65.4f);
            b.setPressure(pressure);
            b.setRosin(rosin);
            b.setBowPosition(0.02f);
            b.setDecay(5.0f);
            char n[64];
            std::snprintf(n, sizeof n, "BowFriction p=%.0f rosin=%.0f", pressure, rosin);
            probe(n, [&](float x) { return b.process(x); }, 10);
        }
    for (int bore = 0; bore < 2; ++bore)
        for (float ob : {0.0f, 1.0f}) {
            hybrid::AirColumnShaper a;
            a.prepare(fs, 20);
            a.setBore(bore ? hybrid::AirColumnShaper::Bore::Closed : hybrid::AirColumnShaper::Bore::Open);
            a.setBoreLength(2.0f);
            a.setBellRadius(0.2f);
            a.setOverblow(ob);
            a.setTurbulence(1);
            a.setTonguing(1);
            a.setNonlinearity(1);
            a.setDecay(2.0f);
            a.noteOn();
            char n[64];
            std::snprintf(n, sizeof n, "AirColumn %s overblow=%.0f", bore ? "closed" : "open", ob);
            probe(n, [&](float x) { return a.process(x); });
        }
    for (float tension : {0.0f, 1.0f})
        for (float high : {0.0f, 1.0f}) {
            hybrid::MembraneMesh m;
            m.prepare(fs, 24);
            m.setTension(tension);
            m.setHighDamping(high);
            m.setDamping(3.0f);
            m.setMalletHardness(1.0f);
            m.setStrike(0.9f, 0.4f);
            char n[64];
            std::snprintf(n, sizeof n, "MembraneMesh T=%.0f highDamp=%.0f", tension, high);
            probe(n, [&](float x) {
                float y;
                m.process(&x, &y, 1);
                return y;
            }, 12);
        }
    {
        hybrid::SympatheticBank bank;
        const int notes[] = {40, 45, 50, 55, 59, 64};
        bank.prepare(fs, notes, 4.0f);
        probe("SympatheticBank", [&](float x) {
            float y;
            bank.process(&x, &y, 1);
            return y;
        }, 10);
    }
    {
        const hybrid::BodyMode modes[] = {{180, 0.2f, 0.12f}, {280, 0.2f, 0.12f}, {450, 0.1f, 0.06f}, {1200, 0.1f, 0.06f}};
        hybrid::BodyRevoicer body;
        body.prepare(fs, modes);
        body.setMaterial(2.0f, 0.5f);
        probe("BodyRevoicer rho=2 E=0.5", [&](float x) {
            float c;
            body.process(&x, &c, 1);
            return x;
        });
        // At reference material the recording must pass bit-for-bit.
        hybrid::BodyRevoicer ref;
        ref.prepare(fs, modes);
        ref.setMaterial(1.0f, 1.0f);
        std::vector<float> sig(4096), orig;
        std::mt19937 rng(3);
        for (auto& v : sig) v = static_cast<float>(rng() % 2000) / 1000.0f - 1.0f;
        orig = sig;
        std::vector<float> corr(4096);
        ref.process(sig.data(), corr.data(), 4096);
        CHECK(sig == orig);
    }
}

static void testSimdEquivalence() {
    section("SIMD kernels agree");
    std::printf("  CPU best: %s\n", simd().name);
    for (SimdLevel lvl : {SimdLevel::Avx2, SimdLevel::Avx512}) {
        const SimdKernels* k = simdFor(lvl);
        if (!k) {
            std::printf("  %s not supported on this CPU, skipped\n", lvl == SimdLevel::Avx2 ? "avx2" : "avx512");
            continue;
        }
        // modal bank
        auto setup = [](ModalBankState& s) {
            s.allocate(21);
            for (int i = 0; i < 21; ++i) {
                const float r = 0.999f - 0.0005f * i, th = 0.01f + 0.03f * i;
                s.a1[i] = 2 * r * std::cos(th);
                s.a2[i] = r * r;
                s.gain[i] = 0.01f * (i + 1);
            }
        };
        ModalBankState a, b;
        setup(a);
        setup(b);
        std::vector<float> in(1024), o1(1024), o2(1024);
        std::mt19937 rng(9);
        for (auto& v : in) v = static_cast<float>(rng() % 2000) / 1000.0f - 1.0f;
        double maxErr = 0, maxVal = 0;
        for (int blk = 0; blk < 8; ++blk) {
            kernels::scalar().modalBank(a, in.data(), o1.data(), 1024);
            k->modalBank(b, in.data(), o2.data(), 1024);
            for (int i = 0; i < 1024; ++i) {
                maxErr = std::max(maxErr, static_cast<double>(std::fabs(o1[i] - o2[i])));
                maxVal = std::max(maxVal, static_cast<double>(std::fabs(o1[i])));
            }
        }
        std::printf("  %-8s modal bank max |err| %.3g (signal peak %.3g)\n", k->name, maxErr, maxVal);
        CHECK(maxErr < 1e-4 * std::max(1.0, maxVal));

        // membrane step
        const int n = 24, stride = ((n + 2 + 15) & ~15) + 16;
        const std::size_t size = static_cast<std::size_t>(stride) * (n + 2);
        std::vector<float> u0(size, 0), u1(size, 0), mask(size, 0), n1(size, 0), n2(size, 0);
        for (int r = 1; r <= n; ++r)
            for (int c = 1; c <= n; ++c) {
                mask[static_cast<std::size_t>(r * stride + c)] = 1;
                u1[static_cast<std::size_t>(r * stride + c)] = static_cast<float>(rng() % 2000) / 1000.0f - 1.0f;
                u0[static_cast<std::size_t>(r * stride + c)] = static_cast<float>(rng() % 2000) / 1000.0f - 1.0f;
            }
        MeshStepArgs m1{n1.data(), u1.data(), u0.data(), mask.data(), n, stride, 1.99f, -0.99f, 0.3f, -0.004f};
        MeshStepArgs m2 = m1;
        m2.next = n2.data();
        kernels::scalar().meshStep(m1);
        k->meshStep(m2);
        double err = 0;
        for (std::size_t i = 0; i < size; ++i) err = std::max(err, static_cast<double>(std::fabs(n1[i] - n2[i])));
        bool padClean = true;
        for (int r = 0; r < n + 2; ++r)
            for (int c = 0; c < stride; ++c)
                if (r == 0 || r == n + 1 || c == 0 || c > n) padClean &= n2[static_cast<std::size_t>(r * stride + c)] == 0.0f;
        std::printf("  %-8s mesh step max |err| %.3g, boundary stays zero: %s\n", k->name, err, padClean ? "yes" : "NO");
        CHECK(err < 1e-5);
        CHECK(padClean);

        std::vector<float> d1(37, 1.0f), d2(37, 1.0f), src(37);
        for (int i = 0; i < 37; ++i) src[static_cast<std::size_t>(i)] = static_cast<float>(i);
        kernels::scalar().addScaled(d1.data(), src.data(), 0.5f, 37);
        k->addScaled(d2.data(), src.data(), 0.5f, 37);
        CHECK(d1 == d2);
    }
}

static void testEngineGuards(const fs::path& dir) {
    section("instruments require real recordings");
    DiskStreamer streamer;
    CHECK(!createInstrument(EngineKind::Piano, nullptr, streamer).has_value());
    // unpitched pack cannot become a piano, but can drive a membrane
    const fs::path pack = dir / "HitPack";
    fs::remove_all(pack);
    fs::create_directories(pack);
    const auto sig = fixtureSignal(4000, 1, 180, 48000);
    writeWav16(pack / "Tom_Low_v60.wav", sig, 1, 48000);
    writeWav16(pack / "Tom_Low_v127.wav", sig, 1, 48000);
    auto lib = SampleLibrary::import(pack);
    CHECK(lib.has_value());
    if (!lib) return;
    CHECK((*lib)->primary() && !(*lib)->primary()->pitched);
    CHECK((*lib)->primary()->zones.front().rootKey == 45);  // GM low tom
    auto piano = createInstrument(EngineKind::Piano, *lib, streamer);
    CHECK(!piano.has_value());
    auto drum = createInstrument(EngineKind::Membrane, *lib, streamer);
    CHECK(drum.has_value());
}

static double renderOnce(InstrumentEngine& e, DiskStreamer& st, std::vector<float>& out, double seconds, const std::vector<NoteEvent>& atStart,
                         bool& finite) {
    const int B = 256;
    const std::size_t frames = static_cast<std::size_t>(seconds * 48000);
    out.assign(frames, 0.0f);
    std::vector<float> L(B), R(B);
    for (const auto& ev : atStart) e.post(ev);
    double sumSq = 0;
    finite = true;
    for (std::size_t pos = 0; pos < frames; pos += B) {
        const int n = static_cast<int>(std::min<std::size_t>(B, frames - pos));
        e.render(L.data(), R.data(), n);
        st.drain();
        for (int i = 0; i < n; ++i) {
            out[pos + static_cast<std::size_t>(i)] = L[static_cast<std::size_t>(i)];
            finite &= std::isfinite(L[static_cast<std::size_t>(i)]) && std::isfinite(R[static_cast<std::size_t>(i)]);
            sumSq += static_cast<double>(L[static_cast<std::size_t>(i)]) * L[static_cast<std::size_t>(i)];
        }
    }
    return std::sqrt(sumSq / static_cast<double>(frames));
}

static void testRealPack(const fs::path& folder) {
    section(("real pack: " + folder.filename().string()).c_str());
    auto lib = SampleLibrary::import(folder);
    CHECK(lib.has_value());
    if (!lib) {
        std::printf("  import failed: %s\n", lib.error().message.c_str());
        return;
    }
    std::fputs((*lib)->report().c_str(), stdout);
    const Articulation* art = (*lib)->primary();
    const bool pitched = art->pitched;
    const int note = pitched ? art->zones[art->zones.size() / 2].rootKey : art->zones.front().rootKey;
    DiskStreamer st;
    const std::vector<EngineKind> kinds = pitched ? std::vector<EngineKind>{EngineKind::Piano, EngineKind::BowedString, EngineKind::PluckedString,
                                                                             EngineKind::Aerophone, EngineKind::Membrane}
                                                  : std::vector<EngineKind>{EngineKind::Membrane};
    for (EngineKind k : kinds) {
        auto inst = createInstrument(k, *lib, st);
        CHECK(inst.has_value());
        if (!inst) continue;
        CHECK((*inst)->prepare(48000, 256).has_value());
        std::vector<float> out;
        bool finite = true;
        // 1) No notes: nothing may sound, from any engine, ever.
        const double silent = renderOnce(**inst, st, out, 1.0, {}, finite);
        const bool exactZero = std::ranges::all_of(out, [](float v) { return v == 0.0f; });
        // 2) One note from a real recording.
        const double rms = renderOnce(**inst, st, out, 3.0, {{NoteEvent::Type::NoteOn, static_cast<u8>(note), 100, 0}}, finite);
        float peak = 0;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        std::printf("  %-10s no-notes output exactly zero: %s | note %d: rms %.4f peak %.3f finite %s\n", engineKindName(k), exactZero ? "yes" : "NO",
                    note, rms, peak, finite ? "yes" : "NO");
        CHECK(silent == 0.0 && exactZero);
        CHECK(rms > 1e-4 && finite && peak < 8.0f);
    }
    CHECK(st.stats().underruns == 0);

    // Many independent instruments through the parallel manager
    InstrumentManager mgr(2, 256);
    mgr.setAudioFormat(48000, 256);
    const int count = 64;
    for (int i = 0; i < count; ++i) {
        auto inst = createInstrument(pitched ? EngineKind::PluckedString : EngineKind::Membrane, *lib, mgr.streamer(), EngineConfig{8});
        CHECK(inst.has_value());
        auto id = mgr.add(std::move(*inst));
        CHECK(id.has_value());
        mgr.get(*id)->post({NoteEvent::Type::NoteOn, static_cast<u8>(note), 90, 0});
    }
    std::vector<float> L(256), R(256), single;
    double sumMgr = 0;
    for (int b = 0; b < 200; ++b) {
        mgr.process(L.data(), R.data(), 256);
        mgr.streamer().drain();
        for (float v : L) sumMgr += static_cast<double>(v) * v;
    }
    // compare with one instrument rendered alone
    DiskStreamer st2;
    auto one = createInstrument(pitched ? EngineKind::PluckedString : EngineKind::Membrane, *lib, st2, EngineConfig{8});
    CHECK(one.has_value());
    CHECK((*one)->prepare(48000, 256).has_value());
    bool finite = true;
    const double rmsOne = renderOnce(**one, st2, single, 200 * 256 / 48000.0, {{NoteEvent::Type::NoteOn, static_cast<u8>(note), 90, 0}}, finite);
    const double rmsMgr = std::sqrt(sumMgr / (200.0 * 256));
    std::printf("  manager: %d instruments on %u workers + audio thread, rms %.4f vs %d x single %.4f\n", count, mgr.workerCount(), rmsMgr, count,
                count * rmsOne);
    CHECK(std::fabs(rmsMgr - count * rmsOne) < 1e-3 * count * rmsOne + 1e-9);
    mgr.remove(0);
    mgr.process(L.data(), R.data(), 256);
    mgr.process(L.data(), R.data(), 256);
    mgr.process(L.data(), R.data(), 256);
    mgr.collectGarbage();
    CHECK(mgr.count() == count - 1);
}

int main(int argc, char** argv) {
    const fs::path dir = fs::temp_directory_path() / "whe_tests";
    fs::create_directories(dir);
    testMd5();
    testParser();
    testDecoders(dir);
    testMapper(dir);
    testDfd(dir);
    testNoSynthesisInvariants();
    testSimdEquivalence();
    testEngineGuards(dir);
    for (int i = 1; i < argc; ++i) testRealPack(argv[i]);
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
