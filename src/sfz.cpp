#include "whe/sfz.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "whe/sample_library.hpp"

namespace whe {

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Expands #include and #define in reading order (a later #define changes the
// text that follows it, which Salamander and many other packs rely on) and drops comments.
// #include paths are relative to the main .sfz file's folder (SFZ spec), not to the including file.
void preprocess(const fs::path& file, const fs::path& root, std::map<std::string, std::string>& defines, std::string& out, int depth,
                std::vector<std::string>& notes) {
    if (depth > 16) {
        notes.push_back("#include nested too deep at " + file.filename().string());
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        notes.push_back("#include file not found: " + file.string());
        return;
    }
    const std::string text = readFile(file);
    const fs::path& base = root;
    std::size_t i = 0;
    auto expand = [&](std::string_view s) {
        // $NAME substitution, longest defined name wins.
        std::string r;
        for (std::size_t k = 0; k < s.size();) {
            if (s[k] == '$') {
                std::size_t e = k + 1;
                while (e < s.size() && identChar(s[e])) ++e;
                bool done = false;
                for (std::size_t len = e - k; len > 1; --len) {
                    auto it = defines.find(std::string(s.substr(k, len)));
                    if (it != defines.end()) {
                        r += it->second;
                        k += len;
                        done = true;
                        break;
                    }
                }
                if (done) continue;
            }
            r += s[k++];
        }
        return r;
    };
    std::string chunk;
    auto flush = [&] {
        out += expand(chunk);
        chunk.clear();
    };
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const auto e = text.find("*/", i + 2);
            i = e == std::string::npos ? text.size() : e + 2;
            chunk += ' ';
            continue;
        }
        if (c == '#' && text.compare(i, 8, "#include") == 0) {
            flush();
            i += 8;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (i < text.size() && text[i] == '"') {
                const auto e = text.find('"', i + 1);
                if (e == std::string::npos) break;
                std::string rel = expand(std::string_view(text).substr(i + 1, e - i - 1));
                std::ranges::replace(rel, '\\', '/');
                out += '\n';
                preprocess(base / fs::path(rel), root, defines, out, depth + 1, notes);
                out += '\n';
                i = e + 1;
            }
            continue;
        }
        if (c == '#' && text.compare(i, 7, "#define") == 0) {
            flush();
            i += 7;
            auto skipWs = [&] {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            };
            skipWs();
            std::size_t s = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            const std::string name = text.substr(s, i - s);
            skipWs();
            s = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (!name.empty() && name[0] == '$') defines[name] = expand(std::string_view(text).substr(s, i - s));
            continue;
        }
        chunk += c;
        ++i;
    }
    flush();
}

std::optional<int> keyValue(const std::string& v, int noteOffset) {
    if (v.empty()) return std::nullopt;
    bool numeric = true;
    for (std::size_t k = 0; k < v.size(); ++k) numeric &= std::isdigit(static_cast<unsigned char>(v[k])) || (k == 0 && v[k] == '-');
    if (numeric) return std::stoi(v) + noteOffset;
    std::string t = v;
    if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    if (auto n = parseNoteName(t, 4)) return *n + noteOffset;
    return std::nullopt;
}

struct Level {
    std::map<std::string, std::string> op;
    void clear() { op.clear(); }
};

}  // namespace

Result<SfzMapping> parseSfz(const fs::path& file) {
    SfzMapping m;
    m.file = file;
    std::map<std::string, std::string> defines;
    std::string text;
    preprocess(file, file.parent_path(), defines, text, 0, m.notes);

    // ---- headers and opcodes ----
    // A value runs to the end of the line, or up to the next "<header>" or " name=" (sample paths may contain spaces).
    struct Item {
        bool header;
        std::string a, b;
    };
    std::vector<Item> items;
    std::size_t i = 0;
    auto isOpStart = [&](std::size_t k) {
        // k points at a non-space char: is it "identifier=" ?
        std::size_t e = k;
        while (e < text.size() && (identChar(text[e]) || text[e] == '$')) ++e;
        return e > k && e < text.size() && text[e] == '=';
    };
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '<') {
            const auto e = text.find('>', i);
            if (e == std::string::npos) break;
            items.push_back({true, text.substr(i + 1, e - i - 1), {}});
            i = e + 1;
            continue;
        }
        if (isOpStart(i)) {
            std::size_t e = i;
            while (text[e] != '=') ++e;
            const std::string name = text.substr(i, e - i);
            i = e + 1;
            std::size_t vEnd = i;
            const bool spaced = name == "sample" || name == "default_path" || name.ends_with("label");
            if (spaced) {
                while (vEnd < text.size() && text[vEnd] != '\n' && text[vEnd] != '\r' && text[vEnd] != '<') {
                    if (std::isspace(static_cast<unsigned char>(text[vEnd]))) {
                        std::size_t k = vEnd;
                        while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
                        if (k < text.size() && (text[k] == '<' || isOpStart(k))) break;
                    }
                    ++vEnd;
                }
            } else {
                while (vEnd < text.size() && !std::isspace(static_cast<unsigned char>(text[vEnd])) && text[vEnd] != '<') ++vEnd;
            }
            std::string value = text.substr(i, vEnd - i);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            items.push_back({false, name, value});
            i = vEnd;
            continue;
        }
        ++i;  // stray text
    }

    // ---- evaluate ----
    Level control, global, master, group, region;
    enum class Cur { None, Control, Global, Master, Group, Region, Other } cur = Cur::None;
    std::map<int, float> ccDefaults;  // CC number -> 0..127
    std::string defaultPath;
    int noteOffset = 0;
    std::optional<int> swDefault;
    int skippedCc = 0, skippedSw = 0, missing = 0;
    std::vector<SfzRegion> out;

    auto finishRegion = [&] {
        if (cur != Cur::Region) return;
        std::map<std::string, std::string> op;
        for (const Level* l : {&global, &master, &group, &region})
            for (const auto& [k, v] : l->op) op[k] = v;
        auto get = [&](const char* k) -> const std::string* {
            auto it = op.find(k);
            return it == op.end() ? nullptr : &it->second;
        };
        auto num = [&](const char* k, float def) {
            const std::string* v = get(k);
            if (!v) return def;
            try {
                return std::stof(*v);
            } catch (...) {
                return def;
            }
        };
        const std::string* sample = get("sample");
        if (!sample || sample->empty() || (*sample)[0] == '*') return;  // "*sine" etc. are generators: not recordings
        // CC-triggered regions (pedal noises…) never play from a key.
        for (const auto& [k, v] : op)
            if (k.rfind("on_locc", 0) == 0 || k.rfind("on_hicc", 0) == 0) {
                ++skippedCc;
                return;
            }
        // CC conditions against the pack's default controller values.
        for (const auto& [k, v] : op) {
            const bool lo = k.rfind("locc", 0) == 0, hi = k.rfind("hicc", 0) == 0;
            if (!lo && !hi) continue;
            try {
                const int cc = std::stoi(k.substr(4));
                const float val = ccDefaults.contains(cc) ? ccDefaults[cc] : 0.0f;
                const float lim = std::stof(v);
                if ((lo && val < lim) || (hi && val > lim)) {
                    ++skippedCc;
                    return;
                }
            } catch (...) {
            }
        }
        if (const std::string* sw = get("sw_last")) {
            const auto k = keyValue(*sw, noteOffset);
            if (swDefault && k && *k != *swDefault) {
                ++skippedSw;
                return;
            }
        }
        SfzRegion r;
        std::string rel = defaultPath + *sample;
        std::ranges::replace(rel, '\\', '/');
        r.sample = (file.parent_path() / fs::path(rel)).lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(r.sample, ec)) {
            ++missing;
            if (missing <= 5) m.notes.push_back("sample not found: " + r.sample.string());
            return;
        }
        if (const std::string* k = get("key")) {
            if (auto v = keyValue(*k, noteOffset)) r.loKey = r.hiKey = r.keyCenter = *v;
        }
        if (const std::string* k = get("lokey")) {
            if (auto v = keyValue(*k, noteOffset)) r.loKey = *v;
        }
        if (const std::string* k = get("hikey")) {
            if (auto v = keyValue(*k, noteOffset)) r.hiKey = *v;
        }
        if (const std::string* k = get("pitch_keycenter")) {
            if (auto v = keyValue(*k, noteOffset)) r.keyCenter = *v;
        } else if (!get("key")) {
            r.keyCenter = get("lokey") ? r.loKey : 60;
        }
        if (r.loKey < 0 || r.hiKey < 0 || r.loKey > 127) {
            ++skippedCc;  // lokey=-1: not playable from the keyboard
            return;
        }
        r.hiKey = std::min(r.hiKey, 127);
        r.loVel = std::clamp(static_cast<int>(num("lovel", 1)), 1, 127);
        r.hiVel = std::clamp(static_cast<int>(num("hivel", 127)), r.loVel, 127);
        r.seqLength = std::max(1, static_cast<int>(num("seq_length", 1)));
        r.seqPosition = std::max(1, static_cast<int>(num("seq_position", 1)));
        r.loRand = num("lorand", 0.0f);
        r.hiRand = num("hirand", 1.0f);
        if (const std::string* t = get("trigger")) r.release = *t == "release" || *t == "release_key";
        r.pitched = num("pitch_keytrack", 100.0f) != 0.0f;
        r.tuneCents = num("tune", 0.0f) + 100.0f * num("transpose", 0.0f);
        r.volumeDb = num("volume", 0.0f);
        if (const std::string* l = get("group_label")) r.label = *l;
        out.push_back(std::move(r));
    };

    for (const Item& it : items) {
        if (it.header) {
            finishRegion();
            const std::string& h = it.a;
            if (h == "control") {
                cur = Cur::Control;
            } else if (h == "global") {
                cur = Cur::Global;
                global.clear();
                master.clear();
                group.clear();
            } else if (h == "master") {
                cur = Cur::Master;
                master.clear();
                group.clear();
            } else if (h == "group") {
                cur = Cur::Group;
                group.clear();
            } else if (h == "region") {
                cur = Cur::Region;
                region.clear();
            } else {
                cur = Cur::Other;  // <curve>, <effect>, <midi>…
            }
            continue;
        }
        const std::string& k = it.a;
        const std::string& v = it.b;
        switch (cur) {
            case Cur::Control:
                if (k == "default_path") {
                    defaultPath = v;
                    std::ranges::replace(defaultPath, '\\', '/');
                    if (!defaultPath.empty() && defaultPath.back() != '/') defaultPath += '/';
                } else if (k == "note_offset") {
                    noteOffset += std::atoi(v.c_str());
                } else if (k == "octave_offset") {
                    noteOffset += 12 * std::atoi(v.c_str());
                } else if (k.rfind("set_hdcc", 0) == 0) {
                    ccDefaults[std::atoi(k.c_str() + 8)] = std::strtof(v.c_str(), nullptr) * 127.0f;
                } else if (k.rfind("set_cc", 0) == 0) {
                    ccDefaults[std::atoi(k.c_str() + 6)] = std::strtof(v.c_str(), nullptr);
                }
                break;
            case Cur::Global:
                global.op[k] = v;
                break;
            case Cur::Master:
                master.op[k] = v;
                break;
            case Cur::Group:
                group.op[k] = v;
                break;
            case Cur::Region:
                region.op[k] = v;
                break;
            default: break;
        }
        if (k == "sw_default" && cur != Cur::Region) swDefault = keyValue(v, noteOffset);
    }
    finishRegion();

    if (skippedCc) m.notes.push_back(std::to_string(skippedCc) + " controller-triggered region(s) (pedal noises etc.) not mapped to keys");
    if (skippedSw) m.notes.push_back(std::to_string(skippedSw) + " region(s) belong to a non-default keyswitch and were left out");
    if (missing) m.notes.push_back(std::to_string(missing) + " region(s) point to sample files that are not in the folder");
    m.regions = std::move(out);
    if (m.regions.empty()) return fail("no playable regions in " + file.filename().string());
    return m;
}

std::vector<std::pair<int, int>> parseMappingChart(const fs::path& file) {
    std::vector<std::pair<int, int>> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string a = line.substr(0, eq), b = line.substr(eq + 1);
        if (a.empty() || !std::ranges::all_of(a, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) continue;
        try {
            const int key = std::stoi(b);
            if (key >= 0 && key <= 127) out.emplace_back(std::stoi(a), key);
        } catch (...) {
        }
    }
    return out;
}

}  // namespace whe
