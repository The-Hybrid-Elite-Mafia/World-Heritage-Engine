#include "catalog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <tuple>

namespace whe::ui {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Dials per engine. Every label/help line is what the dial physically does to
// the real recording, in plain words.
// ---------------------------------------------------------------------------
namespace {

const std::vector<EngineUi>& allEngines() {
    static const std::vector<EngineUi> engines = {
        {EngineKind::Piano, L"PIANO ENGINE · REAL HAMMER → MODELED STRINGS",
         {DialPage{{L"HAMMER", L"STRINGS", L"BODY & PEDAL"},
                   {L"The real recorded strike", L"Modeled strings rung by the strike", L"The instrument around the strings"},
                   {DialSpec{"HammerHardness", L"HARDNESS", L"Soft felt ↔ hard felt"},
                    DialSpec{"StrikeWindowMs", L"STRIKE WINDOW", L"Attack that hits the strings"},
                    DialSpec{"StringResonance", L"RESONANCE", L"How loud the strings ring"},
                    DialSpec{"UnisonDetune", L"DETUNE", L"Unison beating"},
                    DialSpec{"Stiffness", L"STIFFNESS", L"Steel-like overtones"},
                    DialSpec{"StringDecay", L"DECAY", L"How long the strings ring"},
                    DialSpec{"SympatheticResonance", L"SYMPATHETIC", L"Open strings, pedal down"},
                    DialSpec{"@release", L"RELEASE NOISE", L"Real key-off thumps"}}}}},
        {EngineKind::BowedString, L"BOWED ENGINE · REAL BOWING → FRICTION STRING + WOOD BODY",
         {DialPage{{L"BOW", L"STRING", L"WOOD BODY"},
                   {L"How the bow meets the string", L"Modeled string driven by the recording", L"Re-voices the instrument's body"},
                   {DialSpec{"BowPressure", L"PRESSURE", L"Light ↔ heavy bow"},
                    DialSpec{"BowPosition", L"BOW POINT", L"Near bridge ↔ over the neck"},
                    DialSpec{"Rosin", L"ROSIN", L"How hard the hair grabs"},
                    DialSpec{"FrictionAmount", L"FRICTION", L"How loud the modeled string is"},
                    DialSpec{"StringDecay", L"DECAY", L"How long the string rings"},
                    DialSpec{"Dynamics", L"DYNAMICS", L"0 = your playing picks the layer"},
                    DialSpec{"WoodDensity", L"DENSITY", L"Light wood ↔ heavy wood"},
                    DialSpec{"WoodStiffness", L"STIFFNESS", L"Soft wood ↔ stiff wood"}}},
          DialPage{{L"BODY LEVEL", L"", L"RELEASE"},
                   {L"How much the body is re-voiced", L"", L"Recorded release noises"},
                   {DialSpec{"BodyAmount", L"BODY", L"Strength of the new body"}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{},
                    DialSpec{"@release", L"RELEASE NOISE", L"Real bow-off sounds"}, DialSpec{}}}}},
        {EngineKind::PluckedString, L"PLUCKED ENGINE · REAL PLUCK → MODELED STRING + WOOD BODY",
         {DialPage{{L"PLUCK", L"STRING", L"WOOD BODY"},
                   {L"The real recorded pluck", L"Modeled string rung by the pluck", L"Re-voices the instrument's body"},
                   {DialSpec{"PickWindowMs", L"PICK WINDOW", L"Attack that hits the string"},
                    DialSpec{"PluckPosition", L"PLUCK POINT", L"Near bridge ↔ middle"},
                    DialSpec{"ResonatorAmount", L"RESONANCE", L"How loud the string rings"},
                    DialSpec{"Brightness", L"BRIGHTNESS", L"Dull ↔ bright string"},
                    DialSpec{"StringDecay", L"DECAY", L"How long the string rings"},
                    DialSpec{"Sympathetic", L"SYMPATHETIC", L"Other open strings ringing"},
                    DialSpec{"WoodDensity", L"DENSITY", L"Light wood ↔ heavy wood"},
                    DialSpec{"WoodStiffness", L"STIFFNESS", L"Soft wood ↔ stiff wood"}}},
          DialPage{{L"BODY LEVEL", L"", L"RELEASE"},
                   {L"How much the body is re-voiced", L"", L"Recorded release noises"},
                   {DialSpec{"BodyAmount", L"BODY", L"Strength of the new body"}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{},
                    DialSpec{"@release", L"RELEASE NOISE", L"Real finger-off sounds"}, DialSpec{}}}}},
        {EngineKind::Aerophone, L"WIND ENGINE · REAL BREATH → MODELED AIR COLUMN",
         {DialPage{{L"BREATH", L"AIR COLUMN", L"BELL"},
                   {L"What the recording puts into the bore", L"Modeled tube driven by the recording", L"Where the sound leaves"},
                   {DialSpec{"Turbulence", L"BREATH NOISE", L"Air noise sent into the tube"},
                    DialSpec{"Tonguing", L"TONGUING", L"Attack sent into the tube"},
                    DialSpec{"ColumnAmount", L"RESONANCE", L"How loud the tube rings"},
                    DialSpec{"BoreLengthScale", L"BORE LENGTH", L"Short tube ↔ long tube"},
                    DialSpec{"ColumnDecay", L"DECAY", L"How long the tube rings"},
                    DialSpec{"Overblow", L"OVERBLOW", L"Jump to the upper register"},
                    DialSpec{"BellRadius", L"BELL SIZE", L"Small bell ↔ wide bell"},
                    DialSpec{"Nonlinearity", L"PRESSURE", L"Soft ↔ pushed air"}}},
          DialPage{{L"BORE", L"", L"RELEASE"},
                   {L"Shape of the tube", L"", L"Recorded release noises"},
                   {DialSpec{"BoreType", L"BORE TYPE", L"Open tube ↔ closed tube"}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{}, DialSpec{},
                    DialSpec{"@release", L"RELEASE NOISE", L"Real breath-off sounds"}, DialSpec{}}}}},
        {EngineKind::Membrane, L"DRUM ENGINE · REAL HITS → MODELED DRUMHEAD",
         {DialPage{{L"STRIKE", L"DRUMHEAD", L"STRIKE POINT"},
                   {L"The real recorded hit", L"Modeled head rung by every hit", L"Where the stick lands"},
                   {DialSpec{"MalletHardness", L"HARDNESS", L"Felt ↔ wood stick"},
                    DialSpec{"ExciteWindowMs", L"HIT WINDOW", L"Attack that hits the head"},
                    DialSpec{"HeadAmount", L"RESONANCE", L"How loud the head rings"},
                    DialSpec{"Tension", L"TENSION", L"Loose head ↔ tight head"},
                    DialSpec{"Damping", L"DECAY", L"How long the head rings"},
                    DialSpec{"HighDamping", L"MUFFLING", L"Highs die first"},
                    DialSpec{"StrikeX", L"ACROSS", L"Center at 12 o'clock"},
                    DialSpec{"StrikeY", L"UP / DOWN", L"Center at 12 o'clock"}}}}},
    };
    return engines;
}

}  // namespace

const EngineUi& engineUi(EngineKind k) {
    for (const auto& e : allEngines())
        if (e.kind == k) return e;
    return allEngines().front();
}

const wchar_t* engineShortName(EngineKind k) {
    switch (k) {
        case EngineKind::Piano: return L"Piano";
        case EngineKind::BowedString: return L"Bowed strings";
        case EngineKind::PluckedString: return L"Plucked strings";
        case EngineKind::Aerophone: return L"Wind";
        case EngineKind::Membrane: return L"Drum";
    }
    return L"Piano";
}

const char* engineId(EngineKind k) {
    switch (k) {
        case EngineKind::Piano: return "piano";
        case EngineKind::BowedString: return "bowed";
        case EngineKind::PluckedString: return "plucked";
        case EngineKind::Aerophone: return "wind";
        case EngineKind::Membrane: return "drum";
    }
    return "piano";
}

EngineKind guessEngine(const std::wstring& folderName) {
    std::wstring s = folderName;
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    auto has = [&](std::initializer_list<const wchar_t*> words) {
        return std::ranges::any_of(words, [&](const wchar_t* w) { return s.find(w) != std::wstring::npos; });
    };
    if (has({L"hang", L"handpan", L"steelpan", L"steel pan", L"steel drum", L"tongue drum", L"tank drum"})) return EngineKind::Membrane;
    if (has({L"snare", L"drum", L"tom", L"kick", L"tabla", L"djembe", L"timpani", L"cajon", L"conga", L"bongo", L"taiko", L"perc", L"bodhran", L"darbuka"}))
        return EngineKind::Membrane;
    if (has({L"violin", L"viola", L"cello", L"contrabass", L"double bass", L"fiddle", L"erhu", L"bowed", L"sarangi", L"strings"}))
        return EngineKind::BowedString;
    if (has({L"guitar", L"harp", L"oud", L"koto", L"sitar", L"banjo", L"mandolin", L"ukulele", L"lute", L"pluck", L"bouzouki", L"kora", L"guzheng"}))
        return EngineKind::PluckedString;
    if (has({L"flute", L"clarinet", L"sax", L"trumpet", L"horn", L"trombone", L"tuba", L"oboe", L"bassoon", L"ney", L"shakuhachi", L"didgeridoo",
             L"recorder", L"whistle", L"pipe", L"duduk", L"bagpipe", L"cornet", L"brass", L"wind"}))
        return EngineKind::Aerophone;
    return EngineKind::Piano;
}

// ---------------------------------------------------------------------------
bool dialIsBipolar(const ParamInfo& p) { return p.min < 0.0f && p.max > 0.0f; }

float dialRest(const ParamInfo& p) {
    if (dialIsBipolar(p)) return (0.0f - p.min) / (p.max - p.min);
    return 0.0f;
}

float dialToValue(const ParamInfo& p, float pos) { return p.min + std::clamp(pos, 0.0f, 1.0f) * (p.max - p.min); }

std::wstring formatValue(const ParamInfo& p, float v) {
    wchar_t buf[64];
    const std::string unit = p.unit ? p.unit : "";
    if (unit == "ms") std::swprintf(buf, 64, L"%.0f ms", v);
    else if (unit == "s") std::swprintf(buf, 64, v < 1.0f ? L"%.2f s" : L"%.1f s", v);
    else if (unit == "x") std::swprintf(buf, 64, L"%.2f×", v);
    else if (unit == "cents") std::swprintf(buf, 64, L"%.1f ¢", v);
    else if (unit == "m") std::swprintf(buf, 64, L"%.0f mm", v * 1000.0f);
    else std::swprintf(buf, 64, L"%.2f", v);
    return buf;
}

// ---------------------------------------------------------------------------
int countRecordings(const fs::path& folder) {
    int n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec) && it->path().filename() == L".whe_cache") {
            it.disable_recursion_pending();
            continue;
        }
        std::wstring ext = it->path().extension().wstring();
        std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (ext == L".wav" || ext == L".wave" || ext == L".flac" || ext == L".aif" || ext == L".aiff" || ext == L".aifc") ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
void logLine(const std::string& line) {
    static bool first = true;
    // Next to the program (…\World Heritage Engine\studio.log) when that folder is writable,
    // otherwise in the settings folder.
    static const fs::path file = [] {
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        const fs::path beside = fs::path(std::wstring(buf, n)).parent_path().parent_path() / L"studio.log";
        std::ofstream probe(beside, std::ios::binary | std::ios::app);
        return probe ? beside : settingsDir() / L"studio.log";
    }();
    std::error_code ec;
    // Start a fresh log each run once it grows past 1 MB.
    if (first && fs::exists(file, ec) && fs::file_size(file, ec) > (1u << 20)) fs::remove(file, ec);
    first = false;
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d.%03d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    out << stamp << line << "\r\n";
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring noteName(int n) {
    static const wchar_t* names[12] = {L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"};
    if (n < 0 || n > 127) return L"?";
    return std::wstring(names[n % 12]) + std::to_wstring(n / 12 - 1);
}

std::wstring packKey(const fs::path& folder) {
    std::wstring s = folder.lexically_normal().wstring();
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring soundKey(const fs::path& folder, EngineKind k) { return packKey(folder) + L"|" + widen(engineId(k)); }

namespace {
std::wstring fnv(const std::wstring& s) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ull;
    }
    wchar_t buf[24];
    std::swprintf(buf, 24, L"%016llx", h);
    return buf;
}
fs::path localDir() {
    PWSTR p = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = fs::path(p) / L"HEM" / L"World Heritage Engine";
    CoTaskMemFree(p);
    if (dir.empty()) dir = fs::temp_directory_path() / L"World Heritage Engine";
    return dir;
}
}  // namespace

fs::path cacheDirFor(const fs::path& folder) {
    const fs::path d = localDir() / L"cache" / (folder.filename().wstring() + L"-" + fnv(packKey(folder)).substr(0, 8));
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

fs::path mappingsDir() {
    const fs::path d = settingsDir() / L"mappings";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

bool writeMappingSfz(const fs::path& sfz, const std::vector<MapRow>& rows) {
    // Round-robin groups: rows with identical keys/velocities/root/trigger, ordered by rr.
    std::map<std::tuple<bool, int, int, int, int, int>, std::vector<const MapRow*>> groups;
    for (const MapRow& r : rows) groups[{r.release, r.lo, r.hi, r.loVel, r.hiVel, r.root}].push_back(&r);
    std::ofstream out(sfz, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "// World Heritage Engine — mapping edited in the File Map. Delete this file to go back to automatic mapping.\n";
    for (auto& [k, list] : groups) {
        std::ranges::stable_sort(list, [](const MapRow* a, const MapRow* b) { return a->rr < b->rr; });
        out << "<group> lokey=" << std::get<1>(k) << " hikey=" << std::get<2>(k) << " pitch_keycenter=" << std::get<5>(k) << " lovel=" << std::get<3>(k)
            << " hivel=" << std::get<4>(k) << " seq_length=" << list.size() << (std::get<0>(k) ? " trigger=release" : "");
        const MapRow& first = *list.front();
        if (!first.release && first.pitched && !first.articulation.empty()) out << " group_label=" << narrow(first.articulation);
        out << "\n";
        int pos = 1;
        for (const MapRow* r : list) {
            out << "<region> seq_position=" << pos++;
            if (!r->pitched) out << " pitch_keytrack=0";
            if (r->tuneCents != 0.0f) out << " tune=" << -r->tuneCents;
            if (r->gainDb != 0.0f) out << " volume=" << r->gainDb;
            out << " sample=" << narrow(r->file.wstring()) << "\n";
        }
    }
    return static_cast<bool>(out);
}

fs::path settingsDir() {
    PWSTR p = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / L"HEM" / L"World Heritage Engine";
    CoTaskMemFree(p);
    if (dir.empty()) dir = fs::temp_directory_path() / L"World Heritage Engine";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

namespace {

std::optional<EngineKind> kindFromInt(int v) {
    if (v < 0 || v > 4) return std::nullopt;
    return static_cast<EngineKind>(v);
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        const auto b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

}  // namespace

Settings loadSettings() {
    Settings s;
    std::ifstream in(settingsDir() / L"settings.txt", std::ios::binary);
    if (!in) return s;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = splitTabs(line);
        if (f.size() < 2) continue;
        try {
            if (f[0] == "asio") s.asio = f[1] == "1";
            else if (f[0] == "asioDriver") s.asioDriver = f[1];
            else if (f[0] == "exclusive") s.exclusive = f[1] == "1";
            else if (f[0] == "buffer") s.bufferFrames = std::stoi(f[1]);
            else if (f[0] == "rate") s.sampleRate = std::stod(f[1]);
            else if (f[0] == "lastPack") s.lastPack = fs::path(widen(f[1]));
            else if (f[0] == "baseNote") s.baseOctaveNote = std::clamp(std::stoi(f[1]), 12, 84);
            else if (f[0] == "velocity") s.velocity = std::clamp(std::stoi(f[1]), 1, 127);
            else if (f[0] == "pack" && f.size() >= 3) {
                if (auto k = kindFromInt(std::stoi(f[2]))) s.userPacks.emplace_back(fs::path(widen(f[1])), *k);
            } else if (f[0] == "dial" && f.size() >= 4) {
                s.sounds[widen(f[1])].dials[f[2]] = std::clamp(std::stof(f[3]), 0.0f, 1.0f);
            } else if (f[0] == "blend" && f.size() >= 3) {
                s.sounds[widen(f[1])].blend = std::clamp(std::stof(f[2]), 0.0f, 1.0f);
            } else if (f[0] == "physics" && f.size() >= 3) {
                s.sounds[widen(f[1])].physicsOn = f[2] == "1";
            } else if (f[0] == "hidden") {
                s.hiddenPacks.emplace_back(widen(f[1]));
            } else if (f[0] == "mapping" && f.size() >= 3) {
                s.mappings[widen(f[1])] = fs::path(widen(f[2]));
            }
        } catch (...) {
            // A damaged line is skipped; the rest of the settings still load.
        }
    }
    return s;
}

bool saveSettings(const Settings& s) {
    const fs::path dir = settingsDir();
    const fs::path tmp = dir / L"settings.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "asio\t" << (s.asio ? 1 : 0) << "\n";
        out << "asioDriver\t" << s.asioDriver << "\n";
        out << "exclusive\t" << (s.exclusive ? 1 : 0) << "\n";
        out << "buffer\t" << s.bufferFrames << "\n";
        out << "rate\t" << s.sampleRate << "\n";
        out << "lastPack\t" << narrow(s.lastPack.wstring()) << "\n";
        out << "baseNote\t" << s.baseOctaveNote << "\n";
        out << "velocity\t" << s.velocity << "\n";
        for (const auto& [p, k] : s.userPacks) out << "pack\t" << narrow(p.wstring()) << "\t" << static_cast<int>(k) << "\n";
        for (const auto& p : s.hiddenPacks) out << "hidden\t" << narrow(p.wstring()) << "\n";
        for (const auto& [k, p] : s.mappings) out << "mapping\t" << narrow(k) << "\t" << narrow(p.wstring()) << "\n";
        for (const auto& [key, snd] : s.sounds) {
            const std::string k = narrow(key);
            for (const auto& [param, v] : snd.dials) out << "dial\t" << k << "\t" << param << "\t" << v << "\n";
            out << "blend\t" << k << "\t" << snd.blend << "\n";
            out << "physics\t" << k << "\t" << (snd.physicsOn ? 1 : 0) << "\n";
        }
        if (!out) return false;
    }
    // MoveFileEx replaces the old file in one step (std::rename does not on every runtime).
    return MoveFileExW(tmp.c_str(), (dir / L"settings.txt").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace whe::ui
