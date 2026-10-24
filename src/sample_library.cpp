#include "whe/sample_library.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

namespace whe {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isSeparator(char c) {
    return c == '_' || c == '-' || c == ' ' || c == '.' || c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '+' || c == '~' || c == '=' || c == '&' || c == '\'';
}
bool isNoteLetter(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return c >= 'A' && c <= 'G';
}

// Splits a file stem into tokens. Separators break tokens, except a '-' that
// forms a negative octave ("C-1", "F#-1"). Tokens are then split at
// digit->letter boundaries so "C4v12" becomes "C4", "v12".
std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> raw;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '-' && !cur.empty() && i + 1 < s.size() && s[i + 1] == '1' && (i + 2 == s.size() || !isDigit(s[i + 2]))) {
            const std::size_t L = cur.size();
            // Only a bare note ("C", "F#", "Bb") directly before "-1" is a negative octave.
            const bool noteBefore = (L == 1 && isNoteLetter(cur[0])) ||
                                    (L == 2 && isNoteLetter(cur[0]) && (cur[1] == '#' || cur[1] == 'b' || cur[1] == 's'));
            if (noteBefore) {
                cur.push_back('-');
                continue;
            }
        }
        if (isSeparator(c)) {
            if (!cur.empty()) raw.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) raw.push_back(std::move(cur));

    std::vector<std::string> out;
    for (const auto& t : raw) {
        std::string piece;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (i > 0 && isDigit(t[i - 1]) && isAlpha(t[i]) && !piece.empty()) {
                out.push_back(std::move(piece));
                piece.clear();
            }
            piece.push_back(t[i]);
        }
        if (!piece.empty()) out.push_back(std::move(piece));
    }
    return out;
}

bool splitPrefixNumber(const std::string& lowerTok, std::string& prefix, int& value) {
    std::size_t i = 0;
    while (i < lowerTok.size() && isAlpha(lowerTok[i])) ++i;
    if (i == 0 || i == lowerTok.size()) return false;
    for (std::size_t k = i; k < lowerTok.size(); ++k)
        if (!isDigit(lowerTok[k])) return false;
    if (lowerTok.size() - i > 4) return false;
    prefix = lowerTok.substr(0, i);
    value = std::stoi(lowerTok.substr(i));
    return true;
}

template <std::size_t N>
bool oneOf(const std::string& s, const char* const (&list)[N]) {
    for (const char* w : list)
        if (s == w) return true;
    return false;
}

std::optional<int> dynamicWordRank(const std::string& w) {
    static const std::pair<const char*, int> table[] = {
        {"ppp", 1},  {"pp", 2},    {"p", 3},     {"mp", 4},       {"mf", 5},     {"f", 6},      {"ff", 7},      {"fff", 8},
        {"soft", 2}, {"quiet", 2}, {"light", 2}, {"gentle", 2},   {"medium", 5}, {"med", 5},    {"moderate", 5}, {"normal", 5},
        {"hard", 7}, {"loud", 7},  {"strong", 7}, {"heavy", 7},   {"accent", 8}, {"accented", 8}};
    for (const auto& [name, rank] : table)
        if (w == name) return rank;
    return std::nullopt;
}

bool isReleaseWord(const std::string& w) { return w == "rel" || w == "release" || w == "releases" || w == "rls"; }

}  // namespace

std::optional<int> parseNoteName(std::string_view t, int middleCOctave) {
    if (t.size() < 2 || t.size() > 4) return std::nullopt;
    static constexpr int pcOf[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    const char L = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    if (L < 'A' || L > 'G') return std::nullopt;
    int pc = pcOf[L - 'A'];
    std::size_t i = 1;
    if (i < t.size() && (t[i] == '#' || t[i] == 'b' || t[i] == 's') && i + 1 < t.size() && (isDigit(t[i + 1]) || t[i + 1] == '-')) {
        pc += (t[i] == 'b') ? -1 : 1;
        ++i;
    }
    bool neg = false;
    if (i < t.size() && t[i] == '-') {
        neg = true;
        ++i;
    }
    if (i + 1 != t.size() || !isDigit(t[i])) return std::nullopt;
    const int octave = (neg ? -1 : 1) * (t[i] - '0');
    const int midi = 12 * (octave - middleCOctave + 5) + pc;
    if (midi < 0 || midi > 127) return std::nullopt;
    return midi;
}

ParsedName parseSampleName(std::string_view stem, const NamingOptions& opts) {
    ParsedName p;
    static const char* const kVelPrefix[] = {"v", "vel", "velo", "velocity"};
    static const char* const kLayerPrefix[] = {"vl", "layer", "lyr", "lay", "dyn", "dl"};
    static const char* const kRrPrefix[] = {"rr", "r", "rnd", "robin", "seq", "take", "alt", "var", "rrb"};
    static const char* const kKeyPrefix[] = {"k", "key", "midi", "note", "n", "m"};

    for (const std::string& tok : tokenize(stem)) {
        const std::string lt = lower(tok);
        if (auto n = parseNoteName(tok, opts.middleCOctave)) {
            p.note = n;
            continue;
        }
        std::string prefix;
        int value = 0;
        if (splitPrefixNumber(lt, prefix, value)) {
            if (oneOf(prefix, kVelPrefix)) {
                p.ambiguousV = value;
            } else if (oneOf(prefix, kLayerPrefix)) {
                p.layerIndex = value;
            } else if (oneOf(prefix, kRrPrefix)) {
                p.roundRobin = value;
            } else if (oneOf(prefix, kKeyPrefix) && value <= 127) {
                p.note = value;
            } else if (isReleaseWord(prefix)) {
                p.release = true;
                p.words.push_back("release");
            } else {
                // Embedded note after a word: "harmLA0" -> word "harml", note A0. The
                // note letter must be upper-case so words like "Hard3" are not misread.
                bool embedded = false;
                for (std::size_t len = 2; len <= 4 && len < tok.size(); ++len) {
                    const std::string_view tail(tok.data() + tok.size() - len, len);
                    if (!std::isupper(static_cast<unsigned char>(tail[0]))) continue;
                    if (auto n = parseNoteName(tail, opts.middleCOctave)) {
                        bool alphaPrefix = true;
                        for (std::size_t k = 0; k + len < tok.size(); ++k) alphaPrefix &= isAlpha(tok[k]);
                        if (!alphaPrefix) continue;
                        p.note = n;
                        const std::string w = lower(std::string_view(tok.data(), tok.size() - len));
                        if (isReleaseWord(w)) {
                            p.release = true;
                            p.words.push_back("release");
                        } else {
                            p.words.push_back(w);
                        }
                        embedded = true;
                        break;
                    }
                }
                if (!embedded) p.words.push_back(lt);
            }
            continue;
        }
        bool allDigits = !lt.empty();
        for (char c : lt) allDigits &= isDigit(c);
        if (allDigits) {
            if (lt.size() <= 6) p.bareNumbers.push_back(std::stoi(lt));
            continue;
        }
        if (auto r = dynamicWordRank(lt)) {
            p.dynamicRank = r;
            continue;
        }
        if (isReleaseWord(lt)) {
            p.release = true;
            p.words.push_back("release");
            continue;
        }
        p.words.push_back(lt);
    }
    return p;
}

std::optional<int> gmPercussionKey(const std::vector<std::string>& words) {
    auto has = [&](const char* w) { return std::ranges::find(words, std::string(w)) != words.end(); };
    auto contains = [&](const char* w) {
        return std::ranges::any_of(words, [&](const std::string& x) { return x.find(w) != std::string::npos; });
    };
    auto any = [&](std::initializer_list<const char*> ws) {
        for (const char* w : ws)
            if (has(w)) return true;
        return false;
    };
    // Exact-word pass first so a snare recorded on a "kickmic" stays a snare.
    for (int pass = 0; pass < 2; ++pass) {
        auto m = [&](std::initializer_list<const char*> ws) {
            for (const char* w : ws)
                if (pass == 0 ? has(w) : contains(w)) return true;
            return false;
        };
        if (m({"crossstick", "xstick", "sidestick", "rimclick", "stick"}) && m({"snare", "rim", "cross"})) return 37;
        if (m({"snare", "sd"})) return any({"rimshot", "rim"}) ? 40 : 38;
        if (m({"kick", "bassdrum", "bd", "kik"})) return 36;
        if (m({"clap", "handclap"})) return 39;
        if (m({"hihat", "hh", "hat", "hats"})) {
            if (contains("open")) return 46;
            if (contains("pedal") || contains("foot")) return 44;
            return 42;
        }
        if (m({"tom", "toms"})) {
            if (contains("floor")) return 41;
            if (contains("low")) return 45;
            if (contains("mid")) return 47;
            if (contains("high") || has("hi")) return 50;
            return 45;
        }
        if (m({"crash"})) return 49;
        if (m({"ride"})) return contains("bell") ? 53 : 51;
        if (m({"china"})) return 52;
        if (m({"splash"})) return 55;
        if (m({"cowbell"})) return 56;
        if (m({"tambourine", "tamb"})) return 54;
        if (m({"vibraslap"})) return 58;
        if (m({"bongo", "bongos"})) return contains("low") ? 61 : 60;
        if (m({"tumba", "tumbadora"})) return 64;
        if (m({"conga", "congas", "quinto"})) return contains("low") ? 64 : (contains("mute") ? 62 : 63);
        if (m({"timbale", "timbales"})) return contains("low") ? 66 : 65;
        if (m({"agogo"})) return contains("low") ? 68 : 67;
        if (m({"cabasa"})) return 69;
        if (m({"maracas", "shaker", "shakers"})) return 70;
        if (m({"guiro"})) return 73;
        if (m({"claves", "clave"})) return 75;
        if (m({"woodblock", "block"})) return 76;
        if (m({"cuica"})) return 78;
        if (m({"triangle"})) return contains("mute") ? 80 : 81;
    }
    return std::nullopt;
}

std::vector<const Zone*> Articulation::layersAt(int key) const {
    std::vector<const Zone*> out;
    if (key < 0 || key > 127) return out;
    for (const Zone& z : zones)
        if (key >= z.loKey && key <= z.hiKey) out.push_back(&z);
    std::ranges::sort(out, [](const Zone* a, const Zone* b) { return a->loVel < b->loVel; });
    return out;
}

const Articulation* SampleLibrary::articulation(std::string_view n) const noexcept {
    for (const auto& a : articulations_)
        if (a.name == n) return &a;
    return nullptr;
}
const Articulation* SampleLibrary::primary() const noexcept {
    for (const auto& a : articulations_)
        if (!a.releaseTrigger) return &a;
    return nullptr;
}
const Articulation* SampleLibrary::releaseArticulation() const noexcept {
    for (const auto& a : articulations_)
        if (a.releaseTrigger) return &a;
    return nullptr;
}

// ----------------------------------------------------------------------------
// Import + mapping
// ----------------------------------------------------------------------------
namespace {

struct Candidate {
    fs::path path;
    ParsedName name;
    fs::path cached;
    std::string error;
    u32 sampleId = 0;
    const WhsHeader* header = nullptr;
};

enum class DynKind { None = 0, Rank = 1, Index = 2, Velocity = 3 };
struct DynKey {
    DynKind kind = DynKind::None;
    int value = 0;
    auto operator<=>(const DynKey&) const = default;
};

}  // namespace

Result<std::shared_ptr<SampleLibrary>> SampleLibrary::import(const fs::path& folder, const ImportOptions& opts) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return fail("sample pack folder not found: " + folder.string());
    const fs::path cacheDir = opts.cacheDir.empty() ? folder / ".whe_cache" : opts.cacheDir;

    std::vector<Candidate> files;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (it->is_directory(ec) && p.filename() == ".whe_cache") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isSupportedAudioExtension(p)) files.push_back(Candidate{p, {}, {}, {}, 0, nullptr});
    }
    if (files.empty())
        return fail("no recorded audio (.wav/.flac/.aif/.aiff) found in " + folder.string() +
                    " — every World Heritage Engine instrument must be built from real recorded samples");
    std::ranges::sort(files, [](const Candidate& a, const Candidate& b) { return a.path < b.path; });

    auto lib = std::make_shared<SampleLibrary>();
    lib->name_ = folder.filename().string();
    if (lib->name_.empty()) lib->name_ = folder.parent_path().filename().string();
    lib->store_ = SampleStore(opts.preloadFrames);

    // ---- Decode once into the DFD cache (parallel) ----
    unsigned threads = opts.threads ? opts.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned>(threads, static_cast<unsigned>(files.size()));
    std::atomic<std::size_t> next{0};
    std::vector<IngestStats> perThread(threads);
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                for (std::size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
                    auto r = ensureCached(files[i].path, cacheDir, perThread[t]);
                    if (r) files[i].cached = *r;
                    else files[i].error = r.error().message;
                }
            });
        }
    }
    for (const auto& s : perThread) {
        lib->stats_.decoded += s.decoded;
        lib->stats_.cacheHits += s.cacheHits;
        lib->stats_.md5Verified += s.md5Verified;
    }

    std::vector<Candidate*> good;
    for (auto& c : files) {
        if (!c.error.empty()) {
            lib->warnings_.push_back("skipped " + c.path.filename().string() + ": " + c.error);
            continue;
        }
        auto id = lib->store_.addCached(c.cached, c.path);
        if (!id) {
            lib->warnings_.push_back("skipped " + c.path.filename().string() + ": " + id.error().message);
            continue;
        }
        c.sampleId = *id;
        c.name = parseSampleName(c.path.stem().string(), opts.naming);
        good.push_back(&c);
    }
    if (good.empty()) return fail("none of the audio files in " + folder.string() + " could be decoded");
    lib->store_.finalize(opts.lockPreloadInRam);
    for (auto* c : good) c->header = &lib->store_.get(c->sampleId).header;

    // ---- Articulation signature: words not shared by every file ----
    std::map<std::string, std::size_t> wordFreq;
    for (auto* c : good) {
        std::set<std::string> uniq(c->name.words.begin(), c->name.words.end());
        for (const auto& w : uniq) ++wordFreq[w];
    }
    std::map<std::string, std::vector<Candidate*>> groups;
    std::map<std::string, std::vector<std::string>> groupAllWords;
    for (auto* c : good) {
        std::string sig;
        for (const auto& w : c->name.words) {
            if (good.size() > 1 && wordFreq[w] == good.size()) continue;
            if (w == "release") continue;
            if (!sig.empty()) sig += ' ';
            sig += w;
        }
        if (sig.empty()) sig = "default";
        if (c->name.release) sig = (sig == "default") ? "release" : sig + " release";
        groups[sig].push_back(c);
        auto& all = groupAllWords[sig];
        for (const auto& w : c->name.words) all.push_back(w);
        all.push_back(lower(lib->name_));
    }

    std::set<int> usedPercussionKeys;
    u32 zoneIndex = 0;
    std::string rep;
    char line[512];
    std::snprintf(line, sizeof line, "Instrument '%s': %zu recordings, %zu articulation group(s)\n", lib->name_.c_str(), good.size(), groups.size());
    rep += line;

    for (auto& [sig, members] : groups) {
        Articulation art;
        art.name = sig;
        art.releaseTrigger = std::ranges::any_of(members, [](Candidate* c) { return c->name.release; });

        // Pitch: filename note, else embedded root key.
        auto rootOf = [](const Candidate* c) -> std::optional<int> {
            if (c->name.note) return c->name.note;
            if (c->header->rootKey >= 0) return c->header->rootKey;
            return std::nullopt;
        };
        const std::size_t pitchedCount = static_cast<std::size_t>(std::ranges::count_if(members, [&](Candidate* c) { return rootOf(c).has_value(); }));
        art.pitched = pitchedCount * 2 > members.size();

        // Resolve "v<n>": a contiguous run 1..N (or 0..N-1) with N <= 64 is a layer index; otherwise MIDI velocity.
        std::set<int> vValues;
        for (auto* c : members)
            if (c->name.ambiguousV) vValues.insert(*c->name.ambiguousV);
        bool vIsIndex = false;
        if (!vValues.empty()) {
            const int lo = *vValues.begin(), hi = *vValues.rbegin();
            vIsIndex = (lo == 0 || lo == 1) && hi <= 64 && static_cast<int>(vValues.size()) == hi - lo + 1;
        }
        auto dynOf = [&](const Candidate* c) {
            const ParsedName& n = c->name;
            if (n.velocity) return DynKey{DynKind::Velocity, *n.velocity};
            if (n.ambiguousV) return vIsIndex ? DynKey{DynKind::Index, *n.ambiguousV} : DynKey{DynKind::Velocity, std::clamp(*n.ambiguousV, 1, 127)};
            if (n.layerIndex) return DynKey{DynKind::Index, *n.layerIndex};
            if (n.dynamicRank) return DynKey{DynKind::Rank, *n.dynamicRank};
            return DynKey{};
        };

        // key -> dyn -> samples (round robins)
        std::map<int, std::map<DynKey, std::vector<Candidate*>>> byKey;
        int unpitchedKey = -1;
        if (!art.pitched && !art.releaseTrigger) {
            auto gm = gmPercussionKey(groupAllWords[sig]);
            int k = gm.value_or(60);
            while (usedPercussionKeys.contains(k) && k < 127) ++k;
            unpitchedKey = k;
            usedPercussionKeys.insert(k);
        }
        for (auto* c : members) {
            int key;
            if (art.pitched) {
                auto r = rootOf(c);
                if (!r) {
                    lib->warnings_.push_back("no pitch in name or metadata, skipped: " + c->path.filename().string());
                    continue;
                }
                key = *r;
            } else {
                key = art.releaseTrigger ? 60 : unpitchedKey;
            }
            byKey[key][dynOf(c)].push_back(c);
        }
        if (byKey.empty()) continue;

        std::vector<int> roots;
        for (auto& [k, _] : byKey) roots.push_back(k);
        for (std::size_t ri = 0; ri < roots.size(); ++ri) {
            const int root = roots[ri];
            int loKey = art.pitched ? (ri == 0 ? 0 : (roots[ri - 1] + root) / 2 + 1) : root;
            int hiKey = art.pitched ? (ri + 1 == roots.size() ? 127 : (root + roots[ri + 1]) / 2) : root;
            if (!art.pitched && art.releaseTrigger) {  // unpitched release noise (key-off thump) serves the whole keyboard
                loKey = 0;
                hiKey = 127;
            }
            auto& layers = byKey[root];
            const int layerCount = static_cast<int>(layers.size());
            const bool allVelocity = std::ranges::all_of(layers, [](auto& kv) { return kv.first.kind == DynKind::Velocity; });
            int li = 0, prevHi = 0;
            for (auto& [dyn, rrs] : layers) {
                Zone z;
                z.index = zoneIndex++;
                z.rootKey = root;
                z.pitched = art.pitched;
                z.layer = li;
                z.layerCount = layerCount;
                z.loKey = loKey;
                z.hiKey = hiKey;
                if (allVelocity) {
                    z.loVel = prevHi + 1;
                    z.hiVel = (li + 1 == layerCount) ? 127 : std::max(dyn.value, z.loVel);
                } else {
                    z.loVel = prevHi + 1;
                    z.hiVel = (li + 1 == layerCount) ? 127 : std::max(z.loVel, (127 * (li + 1) + layerCount / 2) / layerCount);
                }
                prevHi = z.hiVel;
                std::ranges::sort(rrs, [](Candidate* a, Candidate* b) {
                    const int ra = a->name.roundRobin.value_or(0), rb = b->name.roundRobin.value_or(0);
                    return ra != rb ? ra < rb : a->path < b->path;
                });
                const WhsHeader& h0 = *rrs.front()->header;
                // Metadata embedded by the pack author overrides the inferred ranges.
                if (std::ranges::all_of(rrs, [](Candidate* c) { return c->header->lowKey >= 0; }) && art.pitched) {
                    z.loKey = h0.lowKey;
                    z.hiKey = h0.highKey;
                }
                if (std::ranges::all_of(rrs, [](Candidate* c) { return c->header->lowVel >= 1; })) {
                    z.loVel = h0.lowVel;
                    z.hiVel = h0.highVel;
                }
                z.tuneCents = rrs.front()->name.note ? 0.0f : h0.fineTuneCents;
                for (auto* c : rrs) z.roundRobins.push_back(c->sampleId);
                art.zones.push_back(std::move(z));
                ++li;
            }
        }

        art.lookup.assign(128 * 128, -1);
        for (std::size_t zi = 0; zi < art.zones.size(); ++zi) {
            const Zone& z = art.zones[zi];
            for (int k = std::max(0, z.loKey); k <= std::min(127, z.hiKey); ++k)
                for (int v = std::max(1, z.loVel); v <= std::min(127, z.hiVel); ++v)
                    if (art.lookup[static_cast<std::size_t>(k) * 128 + static_cast<std::size_t>(v)] < 0)
                        art.lookup[static_cast<std::size_t>(k) * 128 + static_cast<std::size_t>(v)] = static_cast<i32>(zi);
        }

        int maxLayers = 0, maxRr = 0;
        for (auto& z : art.zones) {
            maxLayers = std::max(maxLayers, z.layerCount);
            maxRr = std::max(maxRr, static_cast<int>(z.roundRobins.size()));
        }
        const int keyLo = art.zones.empty() ? 0 : std::ranges::min(art.zones, {}, &Zone::loKey).loKey;
        const int keyHi = art.zones.empty() ? 0 : std::ranges::max(art.zones, {}, &Zone::hiKey).hiKey;
        std::snprintf(line, sizeof line,
                      "  [%s]%s %s: %zu recordings -> %zu zones, %zu root notes, keys %d-%d, up to %d velocity layer(s)%s, up to %d round robin(s)\n",
                      art.name.c_str(), art.releaseTrigger ? " (release trigger)" : "", art.pitched ? "pitched" : "unpitched",
                      members.size(), art.zones.size(), roots.size(), keyLo, keyHi, maxLayers,
                      vValues.empty() ? "" : (vIsIndex ? " ['v' read as layer index]" : " ['v' read as MIDI velocity]"), maxRr);
        rep += line;
        lib->articulations_.push_back(std::move(art));
    }
    // Primary (non-release) articulations first.
    std::ranges::stable_partition(lib->articulations_, [](const Articulation& a) { return !a.releaseTrigger; });
    lib->zoneCount_ = zoneIndex;
    if (!lib->primary()) return fail("only release-trigger samples were found in " + folder.string());

    std::snprintf(line, sizeof line, "DFD: %zu samples mapped (%.1f MB on disk), %.1f MB resident preload%s; decoded %zu, cache hits %zu, FLAC MD5 verified %zu\n",
                  lib->store_.size(), lib->store_.mappedBytes() / 1048576.0, lib->store_.residentBytes() / 1048576.0,
                  lib->store_.ramLocked() ? " (locked)" : "", lib->stats_.decoded, lib->stats_.cacheHits, lib->stats_.md5Verified);
    rep += line;
    for (const auto& w : lib->warnings_) rep += "  warning: " + w + "\n";
    lib->report_ = std::move(rep);
    return lib;
}

}  // namespace whe
