// Catalog — what each engine's dials are called and what they say in plain
// English, the list of sample packs the program knows, and the settings file.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "whe/instrument.hpp"

namespace whe::ui {

// One dial on the faceplate.
struct DialSpec {
    const char* param = nullptr;   // engine parameter name; "@release" = recorded release-noise level
    const wchar_t* label = L"";
    const wchar_t* help = L"";     // plain-English line under the dial
};

// A page fills the three pockets: 2 dials, 4 dials, 2 dials. Empty slots have param == nullptr.
struct DialPage {
    std::array<const wchar_t*, 3> titles{};
    std::array<const wchar_t*, 3> subtitles{};
    std::array<DialSpec, 8> dials{};
};

struct EngineUi {
    EngineKind kind;
    const wchar_t* title;        // "PIANO ENGINE · REAL HAMMER → MODELED STRINGS"
    std::vector<DialPage> pages;
};

[[nodiscard]] const EngineUi& engineUi(EngineKind k);
[[nodiscard]] const wchar_t* engineShortName(EngineKind k);  // "Piano", "Bowed strings", …
[[nodiscard]] EngineKind guessEngine(const std::wstring& folderName);

// Dial position (0..1) <-> engine value. Dials whose range crosses zero
// (strike point -1..1) rest at the middle; every other dial rests at 0.
[[nodiscard]] bool dialIsBipolar(const ParamInfo& p);
[[nodiscard]] float dialRest(const ParamInfo& p);
[[nodiscard]] float dialToValue(const ParamInfo& p, float pos);
[[nodiscard]] std::wstring formatValue(const ParamInfo& p, float value);

// ---- sample packs ----
struct Pack {
    std::filesystem::path folder;
    std::wstring name;
    EngineKind kind = EngineKind::Piano;
    int recordings = 0;      // audio files found in the folder
    bool builtIn = false;    // found next to the program (not removable)
};

[[nodiscard]] int countRecordings(const std::filesystem::path& folder);

// ---- saved state ----
struct SavedSound {
    std::map<std::string, float> dials;  // param name -> dial position 0..1
    float blend = 0.0f;
    bool physicsOn = true;
};

struct Settings {
    bool asio = true;
    std::string asioDriver;
    bool exclusive = false;
    int bufferFrames = 0;
    double sampleRate = 0;
    std::vector<std::pair<std::filesystem::path, EngineKind>> userPacks;
    std::filesystem::path lastPack;
    int baseOctaveNote = 48;   // lowest C shown on the keyboard
    int velocity = 100;
    std::map<std::wstring, SavedSound> sounds;  // keyed by pack folder (lower-case)
};

[[nodiscard]] std::filesystem::path settingsDir();  // %APPDATA%\HEM\World Heritage Engine
[[nodiscard]] Settings loadSettings();
bool saveSettings(const Settings& s);
[[nodiscard]] std::wstring packKey(const std::filesystem::path& folder);

// Appends one time-stamped line to studio.log in the program folder (or the settings folder)
// (start-up steps, sound-card results, load errors) so problems can be traced.
void logLine(const std::string& line);

[[nodiscard]] std::wstring widen(const std::string& s);
[[nodiscard]] std::string narrow(const std::wstring& s);
[[nodiscard]] std::wstring noteName(int midiNote);  // C4 = 60

}  // namespace whe::ui
