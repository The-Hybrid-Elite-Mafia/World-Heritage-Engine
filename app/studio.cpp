// World Heritage Engine — the instrument window.
//
// One window, drawn entirely with Direct2D over the rendered HEM faceplate:
//   * status bar  — sound card, sample rate, buffer/latency, CPU, disk stream, voices, underruns
//   * library     — the sample packs (real recordings) you can play; import new ones
//   * engine      — which recording is loaded and how files were read (FILE MAP)
//   * key map     — which recording plays where; tap a zone to hear it
//   * dials       — the physics that reshapes the recording, with a plain-English line each
//   * blend       — recording vs physics, physics on/bypass, output meters
//   * keyboard    — play with the mouse, the PC keys or any MIDI keyboard
// Import page    — every file of a pack and how its name was read (note, velocity, round robin, articulation).
//
// Rules of the faceplate: a lit light is ON, an unlit light is OFF; every dial
// starts at zero (dials whose range runs through zero start at the middle).
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "catalog.hpp"
#include "gfx.hpp"
#include "host.hpp"
#include "layout_gen.hpp"

namespace fs = std::filesystem;
using namespace whe;
using namespace whe::ui;
namespace L = whe::ui::layout;

namespace {

constexpr UINT WM_APP_LOADED = WM_APP + 1;
constexpr UINT WM_APP_MIDI = WM_APP + 2;
constexpr UINT_PTR kTimerUi = 1;
constexpr const wchar_t* kVersion = L"v0.2";

constexpr Color kCyan{0.247f, 0.847f, 0.918f, 1};
constexpr Color kDarkText{0.012f, 0.086f, 0.102f, 1};
constexpr Color kDarkSub{0.02f, 0.196f, 0.227f, 1};
constexpr Color kLabel{0.867f, 0.878f, 0.894f, 1};
constexpr Color kWhite{0.925f, 0.933f, 0.941f, 1};
constexpr Color kGray{0.604f, 0.627f, 0.659f, 1};
constexpr Color kDim{0.498f, 0.529f, 0.565f, 1};
constexpr Color kRed{1.0f, 0.42f, 0.42f, 1};

enum class Act {
    None, Pack, PackPlaceholder, Import, Save, Reset, Dial, Blend, Physics, Sustain, OctDown, OctUp, Fader, Keys, Zone,
    Driver, Buffer, Rate, FileMap, Page, ImpEngine, ImpAdd, ImpBack, ImpTable, Library
};

struct Hit {
    Rect r;
    Act act = Act::None;
    int arg = 0;
};

struct LoadResult {
    int packIndex = -1;
    fs::path folder;
    EngineKind kind = EngineKind::Piano;
    bool forImport = false;
    std::shared_ptr<const SampleLibrary> lib;
    std::string error;
};

struct ZoneSeg {
    int lo = 0, hi = 0, root = 60;
    bool mapped = false;
    float x = 0, w = 0;
};

struct ImportRow {
    std::wstring file, art, root, keys, vel, rr;
    bool skipped = false;
};

TextStyle st(float size, int weight, Color c, Align a = Align::Left, bool glow = false, bool engrave = false) {
    TextStyle t;
    t.size = size;
    t.weight = weight;
    t.color = c;
    t.align = a;
    t.glow = glow;
    t.engrave = engrave;
    return t;
}

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

// The physics-layer levels per engine: zeroed when physics is bypassed.
std::vector<const char*> physicsLevels(EngineKind k) {
    switch (k) {
        case EngineKind::Piano: return {"StringResonance", "SympatheticResonance"};
        case EngineKind::BowedString: return {"FrictionAmount", "BodyAmount"};
        case EngineKind::PluckedString: return {"ResonatorAmount", "Sympathetic", "BodyAmount"};
        case EngineKind::Aerophone: return {"ColumnAmount"};
        case EngineKind::Membrane: return {"HeadAmount"};
    }
    return {};
}

const ParamInfo kReleaseInfo{"@release", 0.0f, 1.0f, 0.0f, "x", "Level of the recorded release noises"};

std::wstring shortDriver(std::wstring s) {
    for (const wchar_t* cut : {L" ASIO Driver", L" ASIO driver", L" ASIO"}) {
        if (auto p = s.find(cut); p != std::wstring::npos) s.erase(p, std::wcslen(cut));
    }
    if (s.size() > 24) s = s.substr(0, 23) + L"…";
    return s;
}

class App {
public:
    int run(HINSTANCE inst, int show);

private:
    static LRESULT CALLBACK wndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT wndProc(UINT m, WPARAM w, LPARAM l);

    // setup
    void discoverPacks();
    void openAudioFromSettings();
    void startLoad(int packIndex, const fs::path& folder, EngineKind kind, bool forImport);
    void onLoaded(std::unique_ptr<LoadResult> r);
    bool activateLibrary(int packIndex, EngineKind kind, std::shared_ptr<const SampleLibrary> lib);
    void rememberEngine(const fs::path& folder, EngineKind kind);
    void buildKeyMap();
    void buildImportRows(const SampleLibrary& lib);

    // engine state
    float& dialPos(const std::string& key, const ParamInfo& info);
    const ParamInfo* paramFor(const DialSpec& d) const;
    void applyAll();
    void applyDial(const DialSpec& d);
    void resetSound();
    void saveSound();
    void loadSoundForPack();

    // drawing
    void updateView();
    void paint();
    void drawStatic();
    void drawDynamic();
    void drawHeader();
    void drawLibrary();
    void drawEngineGlass();
    void drawDials();
    void drawBlend();
    void drawKeyboardRow();
    void drawImportPage();
    void drawStatusValues();
    void drawKeyMap();
    void drawMeters();
    void drawKeys();
    void drawFader();
    void button(const Rect& r, bool lit, std::wstring_view text, float size, Act act, int arg, bool dynamic = false, Color textColor = {0, 0, 0, 0});
    void ledRing(float cx, float cy, float radius, int count, float pos, float rest, float ledSize);
    void T(float x, float y, std::wstring_view s, const TextStyle& t) { gfx_.text(s, Rect{x, y, 1400.0f, t.size * 1.5f}, t); }
    void TC(float cx, float y, float w, std::wstring_view s, const TextStyle& t) {
        TextStyle c = t;
        c.align = Align::Center;
        gfx_.text(s, Rect{cx - w / 2, y, w, t.size * 1.5f}, c);
    }
    void addHit(const Rect& r, Act a, int arg, bool dynamic) { (dynamic ? dynHits_ : statHits_).push_back(Hit{r, a, arg}); }

    // input
    std::optional<Hit> hitAt(float x, float y) const;
    void mouseDown(float x, float y, bool dbl);
    void mouseMove(float x, float y, bool shift);
    void mouseUp();
    void wheel(float x, float y, int delta);
    void keyDown(WPARAM vk, LPARAM lp);
    void keyUp(WPARAM vk);
    int keyAt(float x, float y) const;
    void playMouseNote(int note);
    void setVelocityFromX(float x);
    void showDriverMenu();
    void showBufferMenu();
    void showRateMenu();
    void chooseImportFolder();
    void importFolder(const fs::path& folder);
    void tick();
    void invalidate(bool statics) {
        if (statics) gfx_.invalidateLayer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void setMessage(std::wstring m, bool error = false) {
        message_ = std::move(m);
        messageError_ = error;
        messageUntil_ = GetTickCount64() + 8000;
        invalidate(true);
    }

    HWND hwnd_ = nullptr;
    Gfx gfx_;
    AudioHost host_;
    Settings settings_;
    std::vector<Pack> packs_;
    int selPack_ = -1;
    int listScroll_ = 0;

    // loading
    bool loading_ = false;
    std::wstring loadingName_;
    ULONGLONG loadingSince_ = 0;
    std::jthread loader_;

    // current sound (dial positions by param name; "@release" for release noise)
    std::map<std::string, float> dials_;
    float blend_ = 0.0f;
    bool physicsOn_ = true;
    bool sustainOn_ = false;
    int page_ = 0;
    std::vector<ZoneSeg> zoneSegs_;
    int zoneLo_ = 21, zoneHi_ = 108;
    int lastNote_ = 60;

    // import page
    bool importPage_ = false;
    bool importIsNew_ = false;
    bool importCancelled_ = false;
    fs::path importFolder_;
    EngineKind importKind_ = EngineKind::Piano;
    std::shared_ptr<const SampleLibrary> importLib_;
    std::vector<ImportRow> importRows_;
    int importScroll_ = 0;
    std::wstring importSummary_;

    // view + interaction
    float scale_ = 1, ox_ = 0, oy_ = 0;
    std::vector<Hit> statHits_, dynHits_;
    Act drag_ = Act::None;
    int dragArg_ = 0;
    float dragY_ = 0, dragStart_ = 0;
    int mouseNote_ = -1;
    std::set<int> pcNotes_;
    std::vector<std::pair<ULONGLONG, int>> pendingOff_;

    // live numbers
    float meterL_ = -90, meterR_ = -90, peakHold_ = -90;
    ULONGLONG peakHoldUntil_ = 0;
    float cpuShown_ = 0, diskMBs_ = 0;
    int voicesShown_ = 0;
    u64 underrunsShown_ = 0, lastDiskBytes_ = 0;
    ULONGLONG lastStatsTick_ = 0, lastGcTick_ = 0;

    std::wstring message_;
    bool messageError_ = false;
    ULONGLONG messageUntil_ = 0;
};

// ===========================================================================
// setup
// ===========================================================================

int App::run(HINSTANCE inst, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    settings_ = loadSettings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &App::wndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"WHEStudioWindow";
    RegisterClassExW(&wc);

    // 16:9 window filling most of the work area (1366x768 laptops get ~1229x691).
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    int cw = static_cast<int>(waW * 0.92), ch = cw * 9 / 16;
    if (ch > waH * 0.9) {
        ch = static_cast<int>(waH * 0.9);
        cw = ch * 16 / 9;
    }
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"World Heritage Engine", WS_OVERLAPPEDWINDOW, wa.left + (waW - ww) / 2,
                            wa.top + std::max(0, (waH - wh) / 2), ww, wh, nullptr, nullptr, inst, this);
    if (!hwnd_) return 1;

    const fs::path base = exeDir();
    fs::path res = base / L"res";
    if (!fs::exists(res / L"art")) res = base.parent_path() / L"res";
    if (auto err = gfx_.init(hwnd_, res / L"art", res / L"brand", res / L"fonts"); !err.empty()) {
        MessageBoxW(hwnd_, (L"The faceplate art could not be loaded.\n\n" + err).c_str(), L"World Heritage Engine", MB_ICONERROR);
        return 1;
    }

    discoverPacks();
    openAudioFromSettings();
    host_.openMidi(hwnd_, WM_APP_MIDI);

    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTimerUi, 33, nullptr);
    DragAcceptFiles(hwnd_, TRUE);  // drop a sample-pack folder on the window to import it

    // Start with the last instrument played (or the first one found).
    int start = -1;
    for (int i = 0; i < static_cast<int>(packs_.size()); ++i)
        if (packKey(packs_[i].folder) == packKey(settings_.lastPack)) start = i;
    if (start < 0 && !packs_.empty()) start = 0;
    // "whe_studio.exe <folder>" (or dropping a folder on the program icon) opens the import page for it.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path importArg;
    if (argv && argc > 1) importArg = argv[1];
    if (argv) LocalFree(argv);
    if (!importArg.empty()) importFolder(importArg);
    else if (start >= 0) startLoad(start, packs_[start].folder, packs_[start].kind, false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    host_.allNotesOff();
    saveSettings(settings_);
    return 0;
}

void App::discoverPacks() {
    packs_.clear();
    std::set<std::wstring> seen;
    auto addPack = [&](const fs::path& folder, std::optional<EngineKind> kind, bool builtIn) {
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) return;
        const auto key = packKey(folder);
        if (seen.contains(key)) return;
        seen.insert(key);
        Pack p;
        p.folder = folder;
        p.name = folder.filename().wstring();
        p.kind = kind.value_or(guessEngine(p.name));
        p.recordings = countRecordings(folder);
        p.builtIn = builtIn;
        if (p.recordings > 0) packs_.push_back(std::move(p));
    };
    // Engine choices the user made (also for packs next to the program) win.
    std::map<std::wstring, EngineKind> chosen;
    for (const auto& [f, k] : settings_.userPacks) chosen[packKey(f)] = k;
    auto kindFor = [&](const fs::path& f) -> std::optional<EngineKind> {
        if (auto it = chosen.find(packKey(f)); it != chosen.end()) return it->second;
        return std::nullopt;
    };
    const fs::path base = exeDir();
    for (const fs::path& samples : {base.parent_path() / L"Samples", base / L"Samples"}) {
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(samples, ec))
            if (e.is_directory(ec)) dirs.push_back(e.path());
        std::ranges::sort(dirs);
        for (const auto& d : dirs) addPack(d, kindFor(d), true);
    }
    for (const auto& [f, k] : settings_.userPacks) addPack(f, k, false);
}

void App::openAudioFromSettings() {
    DeviceChoice c;
    const auto drivers = AudioHost::asioDrivers();
    c.asio = settings_.asio && AudioHost::asioBuilt() && !drivers.empty();
    c.asioDriver = settings_.asioDriver;
    if (c.asio && !c.asioDriver.empty() && std::ranges::find(drivers, c.asioDriver) == drivers.end()) c.asioDriver.clear();
    c.exclusive = settings_.exclusive;
    c.bufferFrames = settings_.bufferFrames;
    c.sampleRate = settings_.sampleRate;
    if (auto r = host_.openDevice(c); !r) {
        const std::wstring why = widen(host_.lastError());
        if (c.asio) {
            DeviceChoice w;
            w.asio = false;
            if (host_.openDevice(w)) {
                setMessage(L"ASIO did not start (" + why + L"). Playing through Windows audio.", true);
                return;
            }
        }
        setMessage(L"No sound card connection: " + why + L" Tap the first box in the status bar to pick one.", true);
    }
}

void App::startLoad(int packIndex, const fs::path& folder, EngineKind kind, bool forImport) {
    if (loading_) return;
    loading_ = true;
    loadingName_ = folder.filename().wstring();
    loadingSince_ = GetTickCount64();
    if (loader_.joinable()) loader_.join();
    HWND hwnd = hwnd_;
    loader_ = std::jthread([hwnd, packIndex, folder, kind, forImport] {
        auto res = std::make_unique<LoadResult>();
        res->packIndex = packIndex;
        res->folder = folder;
        res->kind = kind;
        res->forImport = forImport;
        ImportOptions io;
        // The fast-stream cache lives next to the recordings when that folder is
        // writable, otherwise under %LOCALAPPDATA%.
        std::error_code ec;
        const fs::path probe = folder / L".whe_cache";
        fs::create_directories(probe, ec);
        if (ec || !fs::is_directory(probe, ec)) {
            wchar_t local[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
                io.cacheDir = fs::path(local) / L"HEM" / L"World Heritage Engine" / L"cache" /
                              std::to_wstring(std::hash<std::wstring>{}(packKey(folder)));
                fs::create_directories(io.cacheDir, ec);
            }
        }
        auto lib = SampleLibrary::import(folder, io);
        if (lib) res->lib = *lib;
        else res->error = lib.error().message;
        PostMessageW(hwnd, WM_APP_LOADED, 0, reinterpret_cast<LPARAM>(res.release()));
    });
    invalidate(true);
}

void App::onLoaded(std::unique_ptr<LoadResult> r) {
    loading_ = false;
    if (!r->lib) {
        if (r->forImport) {
            importPage_ = false;
            importCancelled_ = false;
        }
        setMessage(L"Could not read " + r->folder.filename().wstring() + L": " + widen(r->error), true);
        return;
    }
    if (r->forImport) {
        if (importCancelled_) {
            importCancelled_ = false;
            return;
        }
        importLib_ = r->lib;
        importFolder_ = r->folder;
        importKind_ = r->kind;
        importIsNew_ = true;
        importPage_ = true;
        importScroll_ = 0;
        buildImportRows(*importLib_);
        invalidate(true);
        return;
    }
    activateLibrary(r->packIndex, r->kind, r->lib);
}

void App::rememberEngine(const fs::path& folder, EngineKind kind) {
    std::erase_if(settings_.userPacks, [&](const auto& p) { return packKey(p.first) == packKey(folder); });
    settings_.userPacks.emplace_back(folder, kind);
}

bool App::activateLibrary(int packIndex, EngineKind kind, std::shared_ptr<const SampleLibrary> lib) {
    host_.allNotesOff();
    if (auto res = host_.setInstrument(kind, std::move(lib)); !res) {
        setMessage(L"Could not build the instrument: " + widen(res.error().message), true);
        return false;
    }
    selPack_ = packIndex;
    if (packIndex >= 0 && packIndex < static_cast<int>(packs_.size())) {
        packs_[packIndex].kind = kind;
        settings_.lastPack = packs_[packIndex].folder;
    }
    page_ = 0;
    sustainOn_ = false;
    loadSoundForPack();
    buildKeyMap();
    // Make sure the on-screen keyboard shows keys that have recordings (drums sit low, e.g. snare on D2).
    if (const SampleLibrary* l = host_.library().get(); l && l->primary() && !l->primary()->zones.empty()) {
        const Articulation& a = *l->primary();
        const int lo = std::ranges::min(a.zones, {}, &Zone::loKey).loKey;
        const int hi = std::ranges::max(a.zones, {}, &Zone::hiKey).hiKey;
        const int base = settings_.baseOctaveNote;
        if (hi < base || lo > base + 35) settings_.baseOctaveNote = std::clamp(lo / 12 * 12, 12, 84);
        lastNote_ = a.pitched ? 60 : a.zones.front().rootKey;
        if (a.pitched && (60 < lo || 60 > hi)) lastNote_ = a.zones.front().rootKey;
    }
    applyAll();
    saveSettings(settings_);
    invalidate(true);
    return true;
}

void App::buildKeyMap() {
    zoneSegs_.clear();
    const SampleLibrary* lib = host_.library().get();
    if (!lib || !lib->primary()) return;
    const Articulation& art = *lib->primary();
    std::map<std::pair<int, int>, int> ranges;  // (lo, hi) -> root
    int lo = 127, hi = 0;
    for (const Zone& z : art.zones) {
        ranges[{z.loKey, z.hiKey}] = z.rootKey;
        lo = std::min(lo, z.loKey);
        hi = std::max(hi, z.hiKey);
    }
    if (ranges.empty()) return;
    if (art.pitched) {
        zoneLo_ = std::max(lo, 21);
        zoneHi_ = std::min(hi, 108);
        if (zoneLo_ > zoneHi_) {
            zoneLo_ = lo;
            zoneHi_ = hi;
        }
    } else {
        zoneLo_ = lo;
        zoneHi_ = hi;
    }
    int cursor = zoneLo_;
    for (const auto& [range, root] : ranges) {
        const int a = std::max(range.first, zoneLo_), b = std::min(range.second, zoneHi_);
        if (a > b || b < cursor) continue;
        if (a > cursor) zoneSegs_.push_back(ZoneSeg{cursor, a - 1, 0, false});
        zoneSegs_.push_back(ZoneSeg{std::max(a, cursor), b, root, true});
        cursor = b + 1;
    }
    if (cursor <= zoneHi_) zoneSegs_.push_back(ZoneSeg{cursor, zoneHi_, 0, false});

    // Widths: proportional to the keys each zone covers, but every mapped zone
    // stays wide enough to tap (44 px) when there is room for that.
    const float total = 1120.0f - 6.0f * static_cast<float>(zoneSegs_.size() - 1);
    std::vector<float> w(zoneSegs_.size());
    std::vector<bool> fixed(zoneSegs_.size(), false);
    const float minW = 44.0f;
    const bool roomForMin = static_cast<float>(zoneSegs_.size()) * minW <= total;
    for (int iter = 0; iter < 8; ++iter) {
        float fixedSum = 0, freeKeys = 0;
        for (std::size_t i = 0; i < zoneSegs_.size(); ++i) {
            if (fixed[i]) fixedSum += minW;
            else freeKeys += static_cast<float>(zoneSegs_[i].hi - zoneSegs_[i].lo + 1);
        }
        bool changed = false;
        for (std::size_t i = 0; i < zoneSegs_.size(); ++i) {
            if (fixed[i]) {
                w[i] = minW;
                continue;
            }
            w[i] = (total - fixedSum) * static_cast<float>(zoneSegs_[i].hi - zoneSegs_[i].lo + 1) / std::max(1.0f, freeKeys);
            if (roomForMin && zoneSegs_[i].mapped && w[i] < minW) {
                fixed[i] = true;
                changed = true;
            }
        }
        if (!changed) break;
    }
    float x = 400.0f;
    for (std::size_t i = 0; i < zoneSegs_.size(); ++i) {
        zoneSegs_[i].x = x;
        zoneSegs_[i].w = w[i];
        x += w[i] + 6.0f;
    }
}

void App::buildImportRows(const SampleLibrary& lib) {
    importRows_.clear();
    int files = 0;
    std::set<int> roots;
    int maxLayers = 0, maxRr = 0;
    for (const Articulation& art : lib.articulations()) {
        std::wstring artName = widen(art.name);
        if (art.releaseTrigger) artName += L" (release)";
        for (const Zone& z : art.zones) {
            roots.insert(z.rootKey);
            maxLayers = std::max(maxLayers, z.layerCount);
            maxRr = std::max(maxRr, static_cast<int>(z.roundRobins.size()));
            for (std::size_t j = 0; j < z.roundRobins.size(); ++j) {
                ImportRow row;
                row.file = lib.store().get(z.roundRobins[j]).source.filename().wstring();
                row.art = artName;
                row.root = z.pitched ? noteName(z.rootKey) : noteName(z.rootKey) + L" (hit)";
                row.keys = z.loKey == z.hiKey ? noteName(z.loKey) : noteName(z.loKey) + L" – " + noteName(z.hiKey);
                row.vel = std::to_wstring(z.loVel) + L" – " + std::to_wstring(z.hiVel) + L"  (" + std::to_wstring(z.layer + 1) + L"/" +
                          std::to_wstring(z.layerCount) + L")";
                row.rr = std::to_wstring(j + 1) + L" of " + std::to_wstring(z.roundRobins.size());
                importRows_.push_back(std::move(row));
                ++files;
            }
        }
    }
    for (const std::string& w : lib.warnings()) {
        ImportRow row;
        row.skipped = true;
        row.file = widen(w);
        importRows_.push_back(std::move(row));
    }
    wchar_t buf[256];
    const bool pitched = lib.primary() && lib.primary()->pitched;
    const std::wstring skipped = lib.warnings().empty() ? L"" : L" · " + std::to_wstring(lib.warnings().size()) + L" skipped";
    std::swprintf(buf, 256, L"%d recordings → %zu %ls · up to %d velocity layer%ls · up to %d round robin%ls · %zu articulation%ls%ls", files,
                  roots.size(), pitched ? (roots.size() == 1 ? L"note" : L"notes") : (roots.size() == 1 ? L"drum key" : L"drum keys"), maxLayers,
                  maxLayers == 1 ? L"" : L"s", maxRr, maxRr == 1 ? L"" : L"s", lib.articulations().size(),
                  lib.articulations().size() == 1 ? L"" : L"s", skipped.c_str());
    importSummary_ = buf;
}

// ===========================================================================
// sound state
// ===========================================================================

const ParamInfo* App::paramFor(const DialSpec& d) const {
    if (!d.param) return nullptr;
    if (std::string_view(d.param) == "@release") return &kReleaseInfo;
    InstrumentEngine* e = const_cast<AudioHost&>(host_).engine();
    if (!e) return nullptr;
    const int idx = e->paramIndex(d.param);
    if (idx < 0) return nullptr;
    return &e->params()[static_cast<std::size_t>(idx)];
}

float& App::dialPos(const std::string& key, const ParamInfo& info) {
    auto it = dials_.find(key);
    if (it == dials_.end()) it = dials_.emplace(key, dialRest(info)).first;
    return it->second;
}

void App::applyDial(const DialSpec& d) {
    InstrumentEngine* e = host_.engine();
    const ParamInfo* info = paramFor(d);
    if (!e || !info) return;
    const float pos = dialPos(d.param, *info);
    if (std::string_view(d.param) == "@release") {
        e->setReleaseLevel(pos);
        return;
    }
    const int idx = e->paramIndex(d.param);
    float v = dialToValue(*info, pos);
    if (!physicsOn_) {
        for (const char* lvl : physicsLevels(host_.kind()))
            if (std::string_view(lvl) == d.param) v = 0.0f;
    }
    e->setParam(idx, v);
    if (std::string_view(d.param) == "Dynamics") {
        // At zero your playing (velocity) picks the recorded layer; above zero the dial crossfades layers.
        if (const int u = e->paramIndex("UseDynamics"); u >= 0) e->setParam(u, pos > 0.0f ? 1.0f : 0.0f);
    }
}

void App::applyAll() {
    InstrumentEngine* e = host_.engine();
    if (!e) return;
    for (const DialPage& pg : engineUi(host_.kind()).pages)
        for (const DialSpec& d : pg.dials) applyDial(d);
    if (!engineUi(host_.kind()).pages.empty()) {
        // Engines without a release dial still honour "zero": no extra release noise.
        bool hasRelease = false;
        for (const DialPage& pg : engineUi(host_.kind()).pages)
            for (const DialSpec& d : pg.dials)
                if (d.param && std::string_view(d.param) == "@release") hasRelease = true;
        if (!hasRelease) e->setReleaseLevel(0.0f);
    }
    if (const int rec = e->paramIndex("RecordingLevel"); rec >= 0) e->setParam(rec, physicsOn_ ? 1.0f - blend_ : 1.0f);
}

void App::resetSound() {
    dials_.clear();
    blend_ = 0.0f;
    physicsOn_ = true;
    applyAll();
    setMessage(L"Every dial is back at zero: you hear the untouched recording.");
}

void App::saveSound() {
    if (selPack_ < 0) return;
    SavedSound s;
    s.blend = blend_;
    s.physicsOn = physicsOn_;
    for (const DialPage& pg : engineUi(host_.kind()).pages)
        for (const DialSpec& d : pg.dials)
            if (const ParamInfo* info = paramFor(d)) s.dials[d.param] = dialPos(d.param, *info);
    settings_.sounds[packKey(packs_[selPack_].folder)] = std::move(s);
    if (saveSettings(settings_)) setMessage(L"Saved. This instrument opens with these settings next time.");
    else setMessage(L"Could not write the settings file.", true);
}

void App::loadSoundForPack() {
    dials_.clear();
    blend_ = 0.0f;
    physicsOn_ = true;
    if (selPack_ < 0) return;
    auto it = settings_.sounds.find(packKey(packs_[selPack_].folder));
    if (it == settings_.sounds.end()) return;
    for (const auto& [k, v] : it->second.dials) dials_[k] = v;
    blend_ = it->second.blend;
    physicsOn_ = it->second.physicsOn;
}

// ===========================================================================
// drawing
// ===========================================================================

void App::updateView() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(rc.right - rc.left), h = static_cast<float>(rc.bottom - rc.top);
    scale_ = std::min(w / L::W, h / L::H);
    ox_ = std::floor((w - L::W * scale_) * 0.5f);
    oy_ = std::floor((h - L::H * scale_) * 0.5f);
    gfx_.setView(scale_, ox_, oy_);
}

void App::paint() {
    updateView();
    if (!gfx_.layerValid()) {
        if (gfx_.beginLayer()) {
            statHits_.clear();
            drawStatic();
            gfx_.endLayer();
        }
    }
    if (!gfx_.beginFrame()) return;
    gfx_.clear(Color{0, 0, 0, 1});
    gfx_.drawLayer();
    dynHits_.clear();
    drawDynamic();
    gfx_.endFrame();
}

void App::button(const Rect& r, bool lit, std::wstring_view text, float size, Act act, int arg, bool dynamic, Color textColor) {
    gfx_.nineSlice(lit ? Sprite::ButtonOn : Sprite::ButtonOff, r, 128.0f, 24.0f, 15.0f);
    if (!text.empty()) {
        TextStyle t = st(size, 800, lit ? kDarkText : kLabel, Align::Center);
        if (textColor.a > 0) {
            t.color = textColor;
            t.glow = !lit;
        }
        gfx_.text(text, Rect{r.x, r.y + (r.h - size * 1.25f) / 2.0f, r.w, size * 1.4f}, t);
    }
    if (act != Act::None) addHit(r, act, arg, dynamic);
}

void App::ledRing(float cx, float cy, float radius, int count, float pos, float rest, float ledSize) {
    for (int i = 0; i < count; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(count - 1);
        const float a = (-135.0f + 270.0f * f) * 3.14159265f / 180.0f;
        const float lo = std::min(pos, rest), hi = std::max(pos, rest);
        // Lit = ON: only the stretch the dial has moved away from its resting point.
        const bool on = std::abs(pos - rest) > 1e-4f && f >= lo - 1e-4f && f <= hi + 1e-4f;
        const float x = cx + radius * std::sin(a), y = cy - radius * std::cos(a);
        gfx_.sprite(on ? Sprite::LedOn : Sprite::LedOff, Rect{x - ledSize / 2, y - ledSize / 2, ledSize, ledSize});
    }
}

void App::drawStatic() {
    gfx_.clear(Color{0, 0, 0, 1});
    gfx_.sprite(Sprite::Plate, Rect{0, 0, L::W, L::H});
    drawHeader();
    // status bar labels (values are dynamic)
    const bool asio = host_.deviceOpen() && host_.device().asio;
    const wchar_t* labels[7] = {asio ? L"ASIO DRIVER" : L"AUDIO OUTPUT", L"SAMPLE RATE", L"BUFFER · LATENCY", L"CPU", L"DISK STREAM", L"VOICES", L"UNDERRUNS"};
    for (int i = 0; i < 7; ++i) {
        T(70.0f + i * 258.0f, 164, labels[i], st(13, 700, Color{0.498f, 0.529f, 0.565f, 1}));
        const Act a = i == 0 ? Act::Driver : i == 1 ? Act::Rate : i == 2 ? Act::Buffer : Act::None;
        if (a != Act::None) addHit(Rect{60.0f + i * 258.0f, 158, 250, 58}, a, 0, false);
    }
    drawLibrary();
    if (importPage_) {
        drawImportPage();
    } else {
        drawEngineGlass();
        T(400, 338, L"KEY MAP · TAP A ZONE TO HEAR ITS RECORDING", st(14, 800, kCyan, Align::Left, true));
        drawDials();
    }
    drawBlend();
    drawKeyboardRow();
}

void App::drawHeader() {
    gfx_.sprite(Sprite::Crown, Rect{52, 36, 96, 96});
    gfx_.sprite(Sprite::Ghost, Rect{1774, 36, 92, 96});
    T(170, 54, L"WORLD HERITAGE", st(24, 800, Color{0.902f, 0.910f, 0.922f, 1}, Align::Left, false, true));
    T(170, 84, std::wstring(L"ENGINE · ") + kVersion, st(17, 700, Color{0.604f, 0.627f, 0.659f, 1}, Align::Left, false, true));
    TC(960, 38, 900, L"HYBRID ELITE MAFIA", st(46, 800, Color{0.925f, 0.933f, 0.941f, 1}, Align::Center, false, true));
    TC(960, 94, 900, L"REAL INSTRUMENT CREATOR", st(21, 700, kCyan, Align::Center, true));
    T(1590, 54, L"HEM AUDIO", st(24, 800, Color{0.902f, 0.910f, 0.922f, 1}, Align::Left, false, true));
    T(1640, 84, L"EST. 2024", st(17, 700, kCyan, Align::Left, true));
}

void App::drawLibrary() {
    T(64, 252, L"INSTRUMENTS", st(19, 800, kCyan, Align::Left, true));
    const int n = static_cast<int>(packs_.size());
    if (n > 3) {
        wchar_t buf[32];
        std::swprintf(buf, 32, L"%d–%d of %d", listScroll_ + 1, std::min(n, listScroll_ + 3), n);
        gfx_.text(buf, Rect{200, 256, 140, 20}, st(13, 700, kDim, Align::Right));
    }
    addHit(Rect{44, 238, 316, 290}, Act::Library, 0, false);
    for (int slot = 0; slot < 3; ++slot) {
        const int i = listScroll_ + slot;
        const Rect r{62, 286.0f + slot * 80.0f, 280, 68};
        if (i < n) {
            const Pack& p = packs_[i];
            const bool lit = i == selPack_;
            button(r, lit, L"", 0, Act::Pack, i);
            std::wstring name = p.name;
            std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
            gfx_.text(name, Rect{84, r.y + 14, 250, 26}, st(18, 800, lit ? kDarkText : kLabel));
            const std::wstring sub = std::wstring(engineShortName(p.kind)) + L" · " + std::to_wstring(p.recordings) + L" recordings";
            gfx_.text(sub, Rect{84, r.y + 38, 250, 22}, st(14, 700, lit ? kDarkSub : kGray));
        } else if (i == n) {
            button(r, false, L"", 0, Act::PackPlaceholder, 0);
            gfx_.text(L"[YOUR NEXT PACK]", Rect{84, r.y + 14, 250, 26}, st(18, 800, kLabel));
            gfx_.text(L"Cello, guitar, flute, any", Rect{84, r.y + 38, 250, 22}, st(14, 700, kGray));
        }
    }
    button(Rect{62, 538, 280, 56}, importPage_ && importIsNew_, L"+  IMPORT SAMPLE PACK", 16, Act::Import, 0, false,
           importPage_ && importIsNew_ ? Color{0, 0, 0, 0} : kCyan);
    const TextStyle help = st(14, 600, kGray);
    T(64, 608, L"Any folder of real recordings —", help);
    T(64, 628, L"keys, layers and round robins", help);
    T(64, 648, L"mapped from the file names.", help);
    button(Rect{62, 690, 280, 44}, false, L"SAVE INSTRUMENT", 15, Act::Save, 0);
    button(Rect{62, 742, 280, 44}, false, L"RESET TO RECORDING", 15, Act::Reset, 0);
}

void App::drawEngineGlass() {
    const EngineUi& ui = engineUi(host_.kind());
    const SampleLibrary* lib = host_.library().get();
    const bool messageOn = !message_.empty() && GetTickCount64() < messageUntil_;
    if (loading_) {
        T(398, 250, L"READING RECORDINGS…", st(15, 700, kCyan, Align::Left, true));
        std::wstring nm = loadingName_;
        std::ranges::transform(nm, nm.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        gfx_.text(nm, Rect{398, 270, 820, 40}, st(32, 800, Color{0.949f, 0.957f, 0.965f, 1}));
    } else if (lib && selPack_ >= 0) {
        T(398, 250, ui.title, st(15, 700, kCyan, Align::Left, true));
        std::wstring nm = packs_[selPack_].name;
        std::ranges::transform(nm, nm.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        gfx_.text(nm, Rect{398, 270, 560, 40}, st(32, 800, Color{0.949f, 0.957f, 0.965f, 1}));
    } else {
        T(398, 250, L"NO INSTRUMENT LOADED", st(15, 700, kCyan, Align::Left, true));
        gfx_.text(L"IMPORT A SAMPLE PACK", Rect{398, 270, 820, 40}, st(32, 800, Color{0.949f, 0.957f, 0.965f, 1}));
    }
    const bool pages = ui.pages.size() > 1;
    const float rightEdge = pages ? 1250.0f : 1385.0f;
    if (messageOn) {
        TextStyle t = st(14, 700, messageError_ ? kRed : kCyan, Align::Right);
        t.wrap = true;
        t.lineSpacing = 17;
        gfx_.pushClip(Rect{700, 244, rightEdge - 700, 66});
        gfx_.text(message_, Rect{760, 248, rightEdge - 760, 64}, t);
        gfx_.popClip();
    } else if (lib && !loading_) {
        const Articulation* art = lib->primary();
        std::set<int> roots;
        int layers = 0;
        if (art)
            for (const Zone& z : art->zones) {
                roots.insert(z.rootKey);
                layers = std::max(layers, z.layerCount);
            }
        wchar_t buf[160];
        std::swprintf(buf, 160, L"%zu recordings · %zu %ls · %d layer%ls", lib->store().size(), roots.size(),
                      art && art->pitched ? (roots.size() == 1 ? L"note" : L"notes") : (roots.size() == 1 ? L"drum key" : L"drum keys"), layers,
                      layers == 1 ? L"" : L"s");
        gfx_.text(buf, Rect{900, 262, rightEdge - 900, 26}, st(18, 600, Color{0.663f, 0.690f, 0.722f, 1}, Align::Right));
    }
    if (pages) {
        wchar_t buf[48];
        std::swprintf(buf, 48, L"DIALS %d/%zu ▶", page_ + 1, ui.pages.size());
        button(Rect{1262, 256, 126, 42}, false, buf, 14, Act::Page, 0, false, kCyan);
    }
    button(Rect{1404, 256, 126, 42}, false, L"FILE MAP", 14, Act::FileMap, 0, false, kCyan);
}

void App::drawDials() {
    const EngineUi& ui = engineUi(host_.kind());
    if (ui.pages.empty()) return;
    const DialPage& pg = ui.pages[static_cast<std::size_t>(std::clamp(page_, 0, static_cast<int>(ui.pages.size()) - 1))];
    const L::Box pockets[3] = {L::pocket_hammer, L::pocket_strings, L::pocket_body};
    for (int s = 0; s < 3; ++s) {
        if (!pg.titles[s] || !*pg.titles[s]) continue;
        const float cx = pockets[s].x + pockets[s].w / 2;
        TC(cx, 454, pockets[s].w, pg.titles[s], st(21, 800, kCyan, Align::Center, true));
        TC(cx, 482, pockets[s].w, pg.subtitles[s], st(14, 600, kGray, Align::Center));
    }
    const bool haveEngine = host_.engine() != nullptr;
    for (int i = 0; i < 8; ++i) {
        const DialSpec& d = pg.dials[static_cast<std::size_t>(i)];
        const L::KnobPos& kp = L::knobWells[i];
        const ParamInfo* info = paramFor(d);
        if (!d.param || (haveEngine && !info)) continue;
        const ParamInfo& pi = info ? *info : kReleaseInfo;
        const float rest = dialRest(pi);
        const float pos = info ? dialPos(d.param, pi) : rest;
        ledRing(kp.cx, kp.cy, kp.ring, 21, pos, rest, 22);
        const float size = kp.dia * 1.5f;
        const Rect kr{kp.cx - size / 2, kp.cy - size / 2, size, size};
        gfx_.sprite(Sprite::KnobBody, kr);
        gfx_.sprite(Sprite::KnobPointer, kr, -135.0f + 270.0f * pos);  // only the pointer layer turns
        TC(kp.cx, 528, 140, d.label, st(15, 800, Color{0.867f, 0.878f, 0.894f, 1}, Align::Center, false, true));
        const std::wstring value = info ? formatValue(pi, std::string_view(d.param) == "@release" ? pos : dialToValue(pi, pos)) : L"—";
        TC(kp.cx, 686, 140, value, st(21, 800, kCyan, Align::Center, true));
        TextStyle h = st(13, 600, kGray, Align::Center);
        h.wrap = true;
        h.lineSpacing = 17.5f;
        gfx_.text(d.help, Rect{kp.cx - 68, 716, 136, 40}, h);
        addHit(Rect{kp.cx - 66, 520, 132, 160}, Act::Dial, i, false);
    }
}

void App::drawBlend() {
    const L::KnobPos& b = L::blendKnob;
    TC(b.cx, 252, 300, L"BLEND", st(21, 800, kCyan, Align::Center, true));
    ledRing(b.cx, b.cy, b.ring, 29, physicsOn_ ? blend_ : 0.0f, 0.0f, 24);
    const float size = b.dia * 1.5f;
    const Rect kr{b.cx - size / 2, b.cy - size / 2, size, size};
    gfx_.sprite(Sprite::KnobBigBody, kr);
    gfx_.sprite(Sprite::KnobBigPointer, kr, -135.0f + 270.0f * blend_);
    addHit(Rect{b.cx - 100, b.cy - 100, 200, 200}, Act::Blend, 0, false);
    T(1576, 442, L"REAL REC", st(15, 800, kLabel, Align::Left, false, true));
    T(1786, 442, L"PHYSICS", st(15, 800, kLabel, Align::Left, false, true));
    std::wstring v;
    if (!physicsOn_) v = L"PHYSICS BYPASSED";
    else if (blend_ <= 0.0f) v = L"FULL RECORDING";
    else if (blend_ >= 1.0f) v = L"PHYSICS ONLY";
    else v = L"RECORDING " + std::to_wstring(static_cast<int>(std::lround((1.0f - blend_) * 100))) + L" %";
    TC(b.cx, 466, 300, v, st(22, 800, kCyan, Align::Center, true));
    TC(b.cx, 496, 300, L"Left = full recording · Right = physics only", st(13, 600, kGray, Align::Center));
    const L::KnobPos& pb = L::physicsButton;
    const float rs = pb.dia * 1.4f;
    gfx_.sprite(physicsOn_ ? Sprite::RoundOn : Sprite::RoundOff, Rect{pb.cx - rs / 2, pb.cy - rs / 2, rs, rs});
    TC(pb.cx, pb.cy - 14, 100, physicsOn_ ? L"ON" : L"OFF", st(22, 800, physicsOn_ ? kDarkText : kDim, Align::Center));
    addHit(Rect{pb.cx - 50, pb.cy - 50, 100, 100}, Act::Physics, 0, false);
    TC(pb.cx, 610, 300, L"PHYSICS ON / BYPASS", st(14, 800, kLabel, Align::Center, false, true));
    T(1664, 668, L"L", st(16, 800, kDim));
    T(1664, 702, L"R", st(16, 800, kDim));
    T(1664, 740, L"PEAK", st(14, 800, kDim));
}

void App::drawKeyboardRow() {
    button(Rect{62, 824, 58, 42}, false, L"◀", 20, Act::OctDown, 0, false, kCyan);
    button(Rect{224, 824, 58, 42}, false, L"▶", 20, Act::OctUp, 0, false, kCyan);
    const std::wstring range = noteName(settings_.baseOctaveNote) + L" – " + noteName(settings_.baseOctaveNote + 35);
    TC(177, 832, 100, range, st(19, 800, kWhite, Align::Center));
    T(960, 834, L"VELOCITY", st(17, 800, kLabel, Align::Left, false, true));
    T(1660, 836, L"PC keys A–K · Z / X octave", st(14, 600, kGray));
}

void App::drawImportPage() {
    const Rect panel{376, 240, 1168, 558};
    gfx_.fillRect(panel, Color{0.010f, 0.012f, 0.015f, 0.97f}, 10);
    gfx_.strokeRect(panel, Color{0.247f, 0.847f, 0.918f, 0.35f}, 1.5f, 10);
    const SampleLibrary* lib = importLib_.get();
    T(398, 254, importIsNew_ ? L"IMPORT · HOW EACH FILE WAS READ" : L"FILE MAP · HOW EACH FILE WAS READ", st(15, 700, kCyan, Align::Left, true));
    if (loading_) {
        std::wstring nm = loadingName_;
        std::ranges::transform(nm, nm.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        gfx_.text(nm, Rect{398, 274, 1100, 38}, st(28, 800, kWhite));
        T(398, 330, L"Reading every recording and building the fast-stream cache…", st(17, 600, kGray));
        T(398, 356, L"Big packs take a while the first time; after that they open in seconds.", st(15, 600, kGray));
        button(Rect{1240, 752, 290, 40}, false, L"CANCEL", 15, Act::ImpBack, 0);
        return;
    }
    if (!lib) return;
    std::wstring nm = importFolder_.filename().wstring();
    std::ranges::transform(nm, nm.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    gfx_.text(nm, Rect{398, 274, 1120, 38}, st(28, 800, kWhite));
    gfx_.text(importSummary_, Rect{398, 314, 1120, 22}, st(15, 600, kGray));
    T(398, 352, L"ENGINE", st(14, 800, kLabel, Align::Left, false, true));
    const EngineKind kinds[5] = {EngineKind::Piano, EngineKind::BowedString, EngineKind::PluckedString, EngineKind::Aerophone, EngineKind::Membrane};
    for (int i = 0; i < 5; ++i) {
        std::wstring n = engineShortName(kinds[i]);
        std::ranges::transform(n, n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        button(Rect{480.0f + i * 172.0f, 342, 160, 38}, importKind_ == kinds[i], n, 14, Act::ImpEngine, i);
    }
    const float cols[6] = {398, 770, 950, 1080, 1250, 1420};
    const wchar_t* heads[6] = {L"FILE", L"ARTICULATION", L"ROOT NOTE", L"KEYS", L"VELOCITY (LAYER)", L"ROUND ROBIN"};
    for (int c = 0; c < 6; ++c) T(cols[c], 398, heads[c], st(13, 800, kCyan, Align::Left, true));
    gfx_.line(392, 420, 1528, 420, Color{0.247f, 0.847f, 0.918f, 0.35f}, 1);
    const int visible = 11;
    const int nRows = static_cast<int>(importRows_.size());
    importScroll_ = std::clamp(importScroll_, 0, std::max(0, nRows - visible));
    gfx_.pushClip(Rect{392, 422, 1140, visible * 28.0f});
    for (int k = 0; k < visible; ++k) {
        const int i = importScroll_ + k;
        if (i >= nRows) break;
        const ImportRow& row = importRows_[static_cast<std::size_t>(i)];
        const float y = 424.0f + k * 28.0f;
        if (k % 2 == 0) gfx_.fillRect(Rect{392, y - 2, 1140, 28}, Color{1, 1, 1, 0.03f});
        if (row.skipped) {
            gfx_.text(L"SKIPPED: " + row.file, Rect{cols[0], y + 2, 1120, 22}, st(14, 600, kRed));
            continue;
        }
        const TextStyle cell = st(14, 600, kLabel);
        gfx_.text(row.file, Rect{cols[0], y + 2, cols[1] - cols[0] - 12, 22}, cell);
        gfx_.text(row.art, Rect{cols[1], y + 2, cols[2] - cols[1] - 12, 22}, cell);
        gfx_.text(row.root, Rect{cols[2], y + 2, cols[3] - cols[2] - 12, 22}, st(14, 800, kCyan));
        gfx_.text(row.keys, Rect{cols[3], y + 2, cols[4] - cols[3] - 12, 22}, cell);
        gfx_.text(row.vel, Rect{cols[4], y + 2, cols[5] - cols[4] - 12, 22}, cell);
        gfx_.text(row.rr, Rect{cols[5], y + 2, 110, 22}, cell);
    }
    gfx_.popClip();
    addHit(Rect{392, 422, 1140, visible * 28.0f}, Act::ImpTable, 0, false);
    wchar_t buf[96];
    std::swprintf(buf, 96, L"Rows %d–%d of %d · scroll with the mouse wheel", nRows ? importScroll_ + 1 : 0, std::min(nRows, importScroll_ + visible), nRows);
    T(398, 762, buf, st(14, 600, kGray));
    if (importIsNew_) {
        button(Rect{1080, 752, 140, 40}, false, L"CANCEL", 15, Act::ImpBack, 0);
        button(Rect{1240, 752, 290, 40}, true, L"ADD TO INSTRUMENTS", 15, Act::ImpAdd, 0);
    } else {
        button(Rect{1240, 752, 290, 40}, true, L"BACK TO PLAYING", 15, Act::ImpBack, 0);
    }
}

void App::drawDynamic() {
    drawStatusValues();
    if (!importPage_) drawKeyMap();
    drawMeters();
    drawFader();
    // SUSTAIN lights from the button or a MIDI sustain pedal.
    const bool sus = sustainOn_ || host_.midiSustain();
    button(Rect{310, 824, 176, 42}, sus, sus ? L"SUSTAIN ON" : L"SUSTAIN OFF", 17, Act::Sustain, 0, true);
    drawKeys();
}

void App::drawStatusValues() {
    std::wstring v[7];
    if (host_.deviceOpen()) {
        if (host_.device().asio) {
            std::wstring name = widen(host_.device().asioDriver);
            if (name.empty()) {
                const auto d = AudioHost::asioDrivers();
                name = d.empty() ? widen(host_.deviceName()) : widen(d.front());
            }
            v[0] = shortDriver(name);
        } else {
            v[0] = host_.device().exclusive ? L"Windows · exclusive" : L"Windows audio";
        }
        wchar_t b[64];
        const double r = host_.sampleRate();
        if (std::fmod(r, 1000.0) == 0.0) std::swprintf(b, 64, L"%.0f kHz", r / 1000.0);
        else std::swprintf(b, 64, L"%.1f kHz", r / 1000.0);
        v[1] = b;
        const double ms = host_.bufferFrames() * 1000.0 / std::max(1.0, r);
        std::swprintf(b, 64, ms < 10 ? L"%d · %.1f ms" : L"%d · %.0f ms", host_.bufferFrames(), ms);
        v[2] = b;
        std::swprintf(b, 64, L"%d %%", static_cast<int>(std::lround(cpuShown_ * 100)));
        v[3] = b;
        std::swprintf(b, 64, L"%.1f MB/s", diskMBs_);
        v[4] = b;
        std::swprintf(b, 64, L"%d / 48", voicesShown_);
        v[5] = b;
        v[6] = std::to_wstring(underrunsShown_);
        for (int i = 0; i < 7; ++i) T(70.0f + i * 258.0f, 182, v[i], st(22, 800, kCyan, Align::Left, true));
    } else {
        T(70, 182, L"OFF · TAP TO PICK", st(22, 800, kDim));
        for (int i = 1; i < 7; ++i) T(70.0f + i * 258.0f, 182, L"—", st(22, 800, kDim));
    }
}

void App::drawKeyMap() {
    for (const ZoneSeg& z : zoneSegs_) {
        bool lit = false;
        for (int n = z.lo; n <= z.hi && !lit; ++n) lit = host_.noteHeld(n);
        const Rect r{z.x, 362, z.w, 44};
        if (!z.mapped) {
            gfx_.strokeRect(r, Color{1, 1, 1, 0.06f}, 1, 5);
            continue;
        }
        if (lit) {
            gfx_.fillRect(Rect{r.x - 3, r.y - 3, r.w + 6, r.h + 6}, Color{0.247f, 0.847f, 0.918f, 0.25f}, 7);
            gfx_.fillGradientV(r, Color{0.314f, 0.902f, 0.961f, 0.95f}, Color{0.118f, 0.667f, 0.745f, 0.95f}, 5);
        } else {
            gfx_.fillRect(r, Color{0.247f, 0.847f, 0.918f, 0.06f}, 5);
            gfx_.strokeRect(r, Color{0.247f, 0.847f, 0.918f, 0.35f}, 1, 5);
        }
        std::wstring label;
        if (z.w > 150 && z.lo != z.hi) label = noteName(z.root) + L" · " + noteName(z.lo) + L"–" + noteName(z.hi);
        else if (z.w >= 40) label = noteName(z.root);
        if (!label.empty()) {
            gfx_.pushClip(r);
            gfx_.text(label, Rect{r.x, r.y + 11, r.w, 22}, st(16, 800, lit ? kDarkText : Color{0.498f, 0.867f, 0.918f, 1}, Align::Center));
            gfx_.popClip();
        }
        addHit(r, Act::Zone, z.root, true);
    }
    if (!zoneSegs_.empty()) {
        T(400, 410, noteName(zoneLo_), st(12, 700, Color{0.435f, 0.467f, 0.502f, 1}));
        gfx_.text(noteName(zoneHi_), Rect{1420, 410, 100, 16}, st(12, 700, Color{0.435f, 0.467f, 0.502f, 1}, Align::Right));
        for (const ZoneSeg& z : zoneSegs_)
            if (60 >= z.lo && 60 <= z.hi && 60 > zoneLo_ && 60 < zoneHi_) {
                const float x = z.x + z.w * (static_cast<float>(60 - z.lo) + 0.5f) / static_cast<float>(z.hi - z.lo + 1);
                TC(x, 410, 160, L"C4 · MIDDLE C", st(12, 700, Color{0.435f, 0.467f, 0.502f, 1}, Align::Center));
            }
    }
}

void App::drawMeters() {
    auto bar = [&](float x, float db) {
        const int lit = static_cast<int>(std::floor((db + 48.0f) / 48.0f * 18.0f + 0.5f));
        for (int i = 0; i < 18; ++i) {
            const float y = 774.0f - i * 7.2f;
            Color c = i >= 16 ? Color{1.0f, 0.353f, 0.353f, 1} : i >= 13 ? Color{0.75f, 0.95f, 1.0f, 1} : kCyan;
            if (i < lit) {
                Color halo = c;
                halo.a = 0.35f;
                gfx_.fillRect(Rect{x - 2, y - 2, 24, 9}, halo, 2);
                gfx_.fillRect(Rect{x, y, 20, 5}, c, 1);
            } else {
                c.a = 0.08f;
                gfx_.fillRect(Rect{x, y, 20, 5}, c, 1);
            }
        }
    };
    bar(1598, meterL_);
    bar(1624, meterR_);
    auto fmt = [](float db) {
        wchar_t b[32];
        if (db <= -89.0f) return std::wstring(L"−∞ dB");
        std::swprintf(b, 32, L"%.1f dB", db);
        std::wstring s = b;
        if (!s.empty() && s[0] == L'-') s[0] = L'−';
        return s;
    };
    T(1690, 664, fmt(meterL_), st(20, 800, kCyan, Align::Left, true));
    T(1690, 698, fmt(meterR_), st(20, 800, kCyan, Align::Left, true));
    std::wstring pk = fmt(peakHold_);
    if (auto p = pk.find(L" dB"); p != std::wstring::npos) pk.erase(p);
    T(1716, 736, pk, st(20, 800, kCyan, Align::Left, true));
}

void App::drawFader() {
    const L::Box& f = L::faderSlot;
    const float v = static_cast<float>(settings_.velocity - 1) / 126.0f;
    const float capW = 26.0f * 1.72f, capH = 40.0f * 1.67f;
    const float cx = f.x + f.w * v, cy = f.y + f.h / 2;
    gfx_.sprite(Sprite::FaderCap, Rect{cx - capW / 2, cy - capH / 2, capW, capH});
    addHit(Rect{f.x - 24, f.y - 14, f.w + 48, f.h + 28}, Act::Fader, 0, true);
    std::wstring layer;
    if (const SampleLibrary* lib = host_.library().get(); lib && lib->primary()) {
        if (const Zone* z = lib->primary()->find(lastNote_, settings_.velocity))
            layer = L" · LAYER " + std::to_wstring(z->layer + 1) + L" / " + std::to_wstring(z->layerCount);
    }
    T(1420, 832, std::to_wstring(settings_.velocity) + layer, st(19, 800, kCyan, Align::Left, true));
}

void App::drawKeys() {
    const L::Box& k = L::keys;
    const float ww = k.w / 21.0f;
    const int base = settings_.baseOctaveNote;
    static constexpr bool isBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    int wi = 0;
    struct BK {
        Rect r;
        int note;
    };
    std::vector<BK> blacks;
    for (int n = base; n < base + 36; ++n) {
        const int pc = n % 12;
        if (!isBlack[pc]) {
            const Rect r{k.x + wi * ww, k.y, ww + 1.0f, k.h};
            gfx_.sprite(host_.noteHeld(n) ? Sprite::KeyWhiteDown : Sprite::KeyWhite, r);
            if (pc == 0) gfx_.text(noteName(n), Rect{r.x, k.y + k.h - 30, ww, 20}, st(16, 800, Color{0.29f, 0.306f, 0.329f, 1}, Align::Center));
            ++wi;
        } else {
            const float bw = ww * 0.62f;
            blacks.push_back(BK{Rect{k.x + wi * ww - bw / 2, k.y - 2, bw, std::round(k.h * 0.62f)}, n});
        }
    }
    for (const BK& b : blacks) gfx_.sprite(host_.noteHeld(b.note) ? Sprite::KeyBlackDown : Sprite::KeyBlack, b.r);
    addHit(Rect{k.x, k.y - 2, k.w, k.h + 2}, Act::Keys, 0, true);
}

// ===========================================================================
// input
// ===========================================================================

std::optional<Hit> App::hitAt(float x, float y) const {
    for (auto it = dynHits_.rbegin(); it != dynHits_.rend(); ++it)
        if (it->r.contains(x, y)) return *it;
    for (auto it = statHits_.rbegin(); it != statHits_.rend(); ++it)
        if (it->r.contains(x, y)) return *it;
    return std::nullopt;
}

int App::keyAt(float x, float y) const {
    const L::Box& k = L::keys;
    const float ww = k.w / 21.0f;
    const int base = settings_.baseOctaveNote;
    static constexpr bool isBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    if (y < k.y + k.h * 0.62f) {
        int wi = 0;
        for (int n = base; n < base + 36; ++n) {
            if (!isBlack[n % 12]) {
                ++wi;
                continue;
            }
            const float bw = ww * 0.62f;
            const float bx = k.x + wi * ww - bw / 2;
            if (x >= bx && x < bx + bw) return n;
        }
    }
    const int wi = static_cast<int>(std::floor((x - k.x) / ww));
    if (wi < 0 || wi >= 21) return -1;
    int count = 0;
    for (int n = base; n < base + 36; ++n) {
        if (isBlack[n % 12]) continue;
        if (count == wi) return n;
        ++count;
    }
    return -1;
}

void App::playMouseNote(int note) {
    if (note == mouseNote_) return;
    if (mouseNote_ >= 0) host_.noteOff(mouseNote_);
    mouseNote_ = note;
    if (note >= 0) {
        host_.noteOn(note, settings_.velocity);
        lastNote_ = note;
    }
    invalidate(false);
}

void App::setVelocityFromX(float x) {
    const L::Box& f = L::faderSlot;
    settings_.velocity = std::clamp(1 + static_cast<int>(std::lround((x - f.x) / f.w * 126.0f)), 1, 127);
    invalidate(false);
}

void App::mouseDown(float x, float y, bool dbl) {
    auto hit = hitAt(x, y);
    if (!hit) return;
    const EngineUi& ui = engineUi(host_.kind());
    switch (hit->act) {
        case Act::Dial: {
            if (ui.pages.empty()) break;
            const DialSpec& d = ui.pages[static_cast<std::size_t>(page_)].dials[static_cast<std::size_t>(hit->arg)];
            const ParamInfo* info = paramFor(d);
            if (!info) break;
            if (dbl) {
                dialPos(d.param, *info) = dialRest(*info);
                applyDial(d);
                invalidate(true);
                break;
            }
            drag_ = Act::Dial;
            dragArg_ = hit->arg;
            dragY_ = y;
            dragStart_ = dialPos(d.param, *info);
            SetCapture(hwnd_);
            break;
        }
        case Act::Blend:
            if (dbl) {
                blend_ = 0.0f;
                applyAll();
                invalidate(true);
                break;
            }
            drag_ = Act::Blend;
            dragY_ = y;
            dragStart_ = blend_;
            SetCapture(hwnd_);
            break;
        case Act::Physics:
            physicsOn_ = !physicsOn_;
            applyAll();
            invalidate(true);
            break;
        case Act::Sustain:
            sustainOn_ = !sustainOn_;
            host_.sustain(sustainOn_);
            invalidate(false);
            break;
        case Act::OctDown:
            settings_.baseOctaveNote = std::max(12, settings_.baseOctaveNote - 12);
            invalidate(true);
            break;
        case Act::OctUp:
            settings_.baseOctaveNote = std::min(84, settings_.baseOctaveNote + 12);
            invalidate(true);
            break;
        case Act::Fader:
            drag_ = Act::Fader;
            setVelocityFromX(x);
            SetCapture(hwnd_);
            break;
        case Act::Keys:
            drag_ = Act::Keys;
            playMouseNote(keyAt(x, y));
            SetCapture(hwnd_);
            break;
        case Act::Zone:
            host_.noteOn(hit->arg, settings_.velocity);
            lastNote_ = hit->arg;
            pendingOff_.emplace_back(GetTickCount64() + 1500, hit->arg);
            invalidate(false);
            break;
        case Act::Pack:
            if (!loading_ && hit->arg != selPack_) {
                importPage_ = false;
                startLoad(hit->arg, packs_[hit->arg].folder, packs_[hit->arg].kind, false);
            }
            break;
        case Act::PackPlaceholder:
        case Act::Import: chooseImportFolder(); break;
        case Act::Save: saveSound(); break;
        case Act::Reset: resetSound(); break;
        case Act::Driver: showDriverMenu(); break;
        case Act::Buffer: showBufferMenu(); break;
        case Act::Rate: showRateMenu(); break;
        case Act::Page:
            if (!ui.pages.empty()) {
                page_ = (page_ + 1) % static_cast<int>(ui.pages.size());
                invalidate(true);
            }
            break;
        case Act::FileMap:
            if (host_.library() && selPack_ >= 0) {
                importLib_ = host_.library();
                importFolder_ = packs_[selPack_].folder;
                importKind_ = host_.kind();
                importIsNew_ = false;
                importPage_ = true;
                importScroll_ = 0;
                buildImportRows(*importLib_);
                invalidate(true);
            }
            break;
        case Act::ImpEngine: {
            const EngineKind kinds[5] = {EngineKind::Piano, EngineKind::BowedString, EngineKind::PluckedString, EngineKind::Aerophone, EngineKind::Membrane};
            importKind_ = kinds[std::clamp(hit->arg, 0, 4)];
            invalidate(true);
            break;
        }
        case Act::ImpAdd: {
            if (!importLib_) break;
            // Build it first: an engine that cannot play these recordings keeps the import page open.
            const auto lib = importLib_;
            const int before = selPack_;
            if (!activateLibrary(-1, importKind_, lib)) {
                selPack_ = before;
                break;
            }
            rememberEngine(importFolder_, importKind_);
            discoverPacks();
            int idx = -1;
            for (int i = 0; i < static_cast<int>(packs_.size()); ++i)
                if (packKey(packs_[i].folder) == packKey(importFolder_)) idx = i;
            selPack_ = idx;
            if (idx >= 0) {
                packs_[idx].kind = importKind_;
                settings_.lastPack = packs_[idx].folder;
                loadSoundForPack();
                applyAll();
            }
            saveSettings(settings_);
            importPage_ = false;
            importLib_.reset();
            if (idx >= 3) listScroll_ = idx - 2;
            setMessage(L"Added. Play it on the keyboard below.");
            break;
        }
        case Act::ImpBack: {
            if (loading_) {
                // The read cannot be interrupted mid-file; its result is dropped when it arrives.
                importCancelled_ = true;
                importPage_ = false;
                invalidate(true);
                break;
            }
            const bool engineChanged = !importIsNew_ && importLib_ && importKind_ != host_.kind();
            importPage_ = false;
            if (engineChanged && selPack_ >= 0) {
                // Rebuild with the chosen engine; remember the choice only if it can play these recordings.
                if (activateLibrary(selPack_, importKind_, importLib_)) {
                    rememberEngine(importFolder_, importKind_);
                    saveSettings(settings_);
                } else {
                    importPage_ = true;  // stay here so another engine can be picked
                    break;
                }
            }
            importLib_.reset();
            invalidate(true);
            break;
        }
        default: break;
    }
}

void App::mouseMove(float x, float y, bool shift) {
    if (drag_ == Act::None) return;
    const float sens = shift ? 1200.0f : 260.0f;  // layout px for a full turn; Shift = fine
    if (drag_ == Act::Dial) {
        const EngineUi& ui = engineUi(host_.kind());
        const DialSpec& d = ui.pages[static_cast<std::size_t>(page_)].dials[static_cast<std::size_t>(dragArg_)];
        if (const ParamInfo* info = paramFor(d)) {
            float& p = dialPos(d.param, *info);
            p = std::clamp(dragStart_ + (dragY_ - y) / sens, 0.0f, 1.0f);
            applyDial(d);
            invalidate(true);
        }
    } else if (drag_ == Act::Blend) {
        blend_ = std::clamp(dragStart_ + (dragY_ - y) / sens, 0.0f, 1.0f);
        applyAll();
        invalidate(true);
    } else if (drag_ == Act::Fader) {
        setVelocityFromX(x);
    } else if (drag_ == Act::Keys) {
        playMouseNote(keyAt(x, y));
    }
}

void App::mouseUp() {
    if (drag_ == Act::Keys) playMouseNote(-1);
    if (drag_ != Act::None) ReleaseCapture();
    drag_ = Act::None;
}

void App::wheel(float x, float y, int delta) {
    auto hit = hitAt(x, y);
    if (!hit) return;
    const float step = static_cast<float>(delta) / 120.0f / 50.0f;
    switch (hit->act) {
        case Act::Dial: {
            const EngineUi& ui = engineUi(host_.kind());
            const DialSpec& d = ui.pages[static_cast<std::size_t>(page_)].dials[static_cast<std::size_t>(hit->arg)];
            if (const ParamInfo* info = paramFor(d)) {
                float& p = dialPos(d.param, *info);
                p = std::clamp(p + step, 0.0f, 1.0f);
                applyDial(d);
                invalidate(true);
            }
            break;
        }
        case Act::Blend:
            blend_ = std::clamp(blend_ + step, 0.0f, 1.0f);
            applyAll();
            invalidate(true);
            break;
        case Act::Fader:
            settings_.velocity = std::clamp(settings_.velocity + (delta > 0 ? 1 : -1) * 4, 1, 127);
            invalidate(false);
            break;
        case Act::Library:
        case Act::Pack:
        case Act::PackPlaceholder:
            listScroll_ = std::clamp(listScroll_ + (delta > 0 ? -1 : 1), 0, std::max(0, static_cast<int>(packs_.size()) - 2));
            invalidate(true);
            break;
        case Act::ImpTable:
            importScroll_ += delta > 0 ? -3 : 3;
            invalidate(true);
            break;
        default: break;
    }
}

void App::keyDown(WPARAM vk, LPARAM lp) {
    if (lp & (1 << 30)) return;  // auto-repeat
    static constexpr int keys[] = {'A', 'W', 'S', 'E', 'D', 'F', 'T', 'G', 'Y', 'H', 'U', 'J', 'K', 'O', 'L', 'P', VK_OEM_1};
    for (int i = 0; i < static_cast<int>(std::size(keys)); ++i)
        if (static_cast<int>(vk) == keys[i]) {
            const int note = settings_.baseOctaveNote + 12 + i;
            if (pcNotes_.insert(static_cast<int>(vk)).second) {
                host_.noteOn(note, settings_.velocity);
                lastNote_ = note;
                invalidate(false);
            }
            return;
        }
    if (vk == 'Z') {
        settings_.baseOctaveNote = std::max(12, settings_.baseOctaveNote - 12);
        invalidate(true);
    } else if (vk == 'X') {
        settings_.baseOctaveNote = std::min(84, settings_.baseOctaveNote + 12);
        invalidate(true);
    } else if (vk == VK_SPACE) {
        sustainOn_ = !sustainOn_;
        host_.sustain(sustainOn_);
        invalidate(false);
    } else if (vk == VK_ESCAPE) {
        host_.allNotesOff();
        pcNotes_.clear();
        invalidate(false);
    }
}

void App::keyUp(WPARAM vk) {
    static constexpr int keys[] = {'A', 'W', 'S', 'E', 'D', 'F', 'T', 'G', 'Y', 'H', 'U', 'J', 'K', 'O', 'L', 'P', VK_OEM_1};
    for (int i = 0; i < static_cast<int>(std::size(keys)); ++i)
        if (static_cast<int>(vk) == keys[i] && pcNotes_.erase(static_cast<int>(vk))) {
            // The octave may have changed while the key was held: release every octave of it.
            for (int base = 12; base <= 84; base += 12) host_.noteOff(base + 12 + i);
            invalidate(false);
        }
}

void App::chooseImportFolder() {
    if (loading_) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Pick a folder of real instrument recordings (.wav / .flac / .aiff)");
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.GetAddressOf()))) return;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    const fs::path folder(path);
    CoTaskMemFree(path);
    importFolder(folder);
}

void App::importFolder(const fs::path& folder) {
    if (loading_) return;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        setMessage(L"Drop a folder of recordings (not single files) to import a pack.", true);
        return;
    }
    if (countRecordings(folder) == 0) {
        setMessage(L"No .wav, .flac or .aiff recordings in " + folder.filename().wstring() + L".", true);
        return;
    }
    importFolder_ = folder;
    importIsNew_ = true;
    importPage_ = true;
    importCancelled_ = false;
    importLib_.reset();
    importRows_.clear();
    startLoad(-1, folder, guessEngine(folder.filename().wstring()), true);
}

void App::showDriverMenu() {
    HMENU m = CreatePopupMenu();
    const auto drivers = AudioHost::asioDrivers();
    const bool curAsio = host_.deviceOpen() && host_.device().asio;
    if (!AudioHost::asioBuilt()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"ASIO: not included in this build");
    } else if (drivers.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"ASIO: no ASIO drivers installed");
    } else {
        const auto cur = host_.device().asioDriver.empty() ? drivers.front() : host_.device().asioDriver;
        for (std::size_t i = 0; i < drivers.size(); ++i)
            AppendMenuW(m, MF_STRING | (curAsio && drivers[i] == cur ? MF_CHECKED : 0), 1000 + i, (L"ASIO · " + widen(drivers[i])).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (!curAsio && host_.deviceOpen() && !host_.device().exclusive ? MF_CHECKED : 0), 2000, L"Windows audio (shared, plays with other apps)");
    AppendMenuW(m, MF_STRING | (!curAsio && host_.deviceOpen() && host_.device().exclusive ? MF_CHECKED : 0), 2001, L"Windows audio (exclusive, lower latency)");
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 0) return;
    DeviceChoice c = host_.device();
    if (cmd >= 1000 && cmd < 2000) {
        c.asio = true;
        c.asioDriver = drivers[static_cast<std::size_t>(cmd - 1000)];
    } else {
        c.asio = false;
        c.exclusive = cmd == 2001;
    }
    host_.allNotesOff();
    if (auto r = host_.openDevice(c); !r) setMessage(widen(host_.lastError()), true);
    else setMessage(L"Sound card: " + widen(host_.deviceName()));
    settings_.asio = c.asio;
    settings_.asioDriver = c.asioDriver;
    settings_.exclusive = c.exclusive;
    applyAll();
    saveSettings(settings_);
    invalidate(true);
}

void App::showBufferMenu() {
    HMENU m = CreatePopupMenu();
    const DeviceChoice& d = host_.device();
    const bool canSet = host_.deviceOpen() && (d.asio || d.exclusive);
    if (!canSet) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"Windows sets the buffer in shared mode");
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"(pick ASIO or exclusive for control)");
    } else {
        AppendMenuW(m, MF_STRING | (d.bufferFrames == 0 ? MF_CHECKED : 0), 3000, L"Driver default");
        for (int f : {64, 128, 256, 512, 1024, 2048}) {
            wchar_t b[64];
            std::swprintf(b, 64, L"%d samples  (%.1f ms)", f, f * 1000.0 / std::max(1.0, host_.sampleRate()));
            AppendMenuW(m, MF_STRING | (d.bufferFrames == f ? MF_CHECKED : 0), 3000 + f, b);
        }
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd < 3000) return;
    DeviceChoice c = d;
    c.bufferFrames = cmd - 3000;
    host_.allNotesOff();
    if (auto r = host_.openDevice(c); !r) setMessage(widen(host_.lastError()), true);
    settings_.bufferFrames = c.bufferFrames;
    applyAll();
    saveSettings(settings_);
    invalidate(true);
}

void App::showRateMenu() {
    HMENU m = CreatePopupMenu();
    const DeviceChoice& d = host_.device();
    const bool canSet = host_.deviceOpen() && d.asio;
    if (!canSet) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"Windows audio uses the rate set in");
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"Windows Sound settings");
    } else {
        AppendMenuW(m, MF_STRING | (d.sampleRate == 0 ? MF_CHECKED : 0), 4000, L"Keep the driver's rate");
        for (int r : {44100, 48000, 88200, 96000}) {
            wchar_t b[32];
            std::swprintf(b, 32, L"%.1f kHz", r / 1000.0);
            AppendMenuW(m, MF_STRING | (static_cast<int>(d.sampleRate) == r ? MF_CHECKED : 0), 4000 + r, b);
        }
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd < 4000) return;
    DeviceChoice c = d;
    c.sampleRate = cmd - 4000;
    host_.allNotesOff();
    if (auto r = host_.openDevice(c); !r) setMessage(widen(host_.lastError()), true);
    settings_.sampleRate = c.sampleRate;
    applyAll();
    saveSettings(settings_);
    invalidate(true);
}

void App::tick() {
    const ULONGLONG now = GetTickCount64();
    bool dyn = false;
    // Scheduled note-offs (key-map previews).
    std::erase_if(pendingOff_, [&](const auto& p) {
        if (p.first > now) return false;
        host_.noteOff(p.second);
        dyn = true;
        return true;
    });
    const auto live = host_.readLive();
    auto toDb = [](float lin) { return lin > 1e-5f ? 20.0f * std::log10(lin) : -90.0f; };
    const float l = toDb(live.peakL), r = toDb(live.peakR);
    const float fall = 1.6f;  // dB per tick (~48 dB/s)
    const float nl = std::max(l, meterL_ - fall), nr = std::max(r, meterR_ - fall);
    if (std::abs(nl - meterL_) > 0.05f || std::abs(nr - meterR_) > 0.05f) dyn = true;
    meterL_ = std::max(nl, -90.0f);
    meterR_ = std::max(nr, -90.0f);
    const float pk = std::max(l, r);
    if (pk >= peakHold_ || now > peakHoldUntil_) {
        if (pk > -89.0f || now > peakHoldUntil_) {
            if (pk != peakHold_) dyn = true;
            peakHold_ = pk;
            peakHoldUntil_ = now + 2000;
        }
    }
    if (now - lastStatsTick_ >= 500) {
        const double dt = static_cast<double>(now - lastStatsTick_) / 1000.0;
        if (lastStatsTick_ != 0 && live.diskBytes >= lastDiskBytes_)
            diskMBs_ = static_cast<float>(static_cast<double>(live.diskBytes - lastDiskBytes_) / 1048576.0 / dt);
        lastDiskBytes_ = live.diskBytes;
        lastStatsTick_ = now;
        cpuShown_ = live.cpu;
        voicesShown_ = live.voices;
        underrunsShown_ = live.underruns;
        dyn = true;
    }
    if (now - lastGcTick_ >= 1000) {
        host_.collectGarbage();
        lastGcTick_ = now;
    }
    if (!message_.empty() && now >= messageUntil_) {
        message_.clear();
        invalidate(true);
    }
    if (loading_ && (now - loadingSince_) % 1000 < 40) invalidate(true);
    if (dyn) invalidate(false);
}

// ===========================================================================
// window procedure
// ===========================================================================

LRESULT CALLBACK App::wndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* app = nullptr;
    if (m == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = h;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!app) return DefWindowProcW(h, m, w, l);
    return app->wndProc(m, w, l);
}

LRESULT App::wndProc(UINT m, WPARAM w, LPARAM l) {
    auto lx = [&](LPARAM lp) { return (static_cast<float>(GET_X_LPARAM(lp)) - ox_) / scale_; };
    auto ly = [&](LPARAM lp) { return (static_cast<float>(GET_Y_LPARAM(lp)) - oy_) / scale_; };
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            gfx_.resize(LOWORD(l), HIWORD(l));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize.x = 800;
            mm->ptMinTrackSize.y = 480;
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd_);
            mouseDown(lx(l), ly(l), m == WM_LBUTTONDBLCLK);
            return 0;
        case WM_MOUSEMOVE: mouseMove(lx(l), ly(l), (w & MK_SHIFT) != 0); return 0;
        case WM_LBUTTONUP: mouseUp(); return 0;
        case WM_CAPTURECHANGED:
            if (drag_ == Act::Keys) playMouseNote(-1);
            drag_ = Act::None;
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd_, &p);
            wheel((static_cast<float>(p.x) - ox_) / scale_, (static_cast<float>(p.y) - oy_) / scale_, GET_WHEEL_DELTA_WPARAM(w));
            return 0;
        }
        case WM_KEYDOWN: keyDown(w, l); return 0;
        case WM_KEYUP: keyUp(w); return 0;
        case WM_KILLFOCUS:
            for (int vk : std::set<int>(pcNotes_)) keyUp(static_cast<WPARAM>(vk));
            return 0;
        case WM_TIMER:
            if (w == kTimerUi) tick();
            return 0;
        case WM_APP_LOADED: onLoaded(std::unique_ptr<LoadResult>(reinterpret_cast<LoadResult*>(l))); return 0;
        case WM_APP_MIDI: InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(w);
            wchar_t path[MAX_PATH * 4] = {};
            if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0) importFolder(path);
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerUi);
            host_.closeMidi();
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Crisp on high-DPI laptop screens and over remote desktop.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto app = std::make_unique<App>();
    return app->run(inst, show);
}
