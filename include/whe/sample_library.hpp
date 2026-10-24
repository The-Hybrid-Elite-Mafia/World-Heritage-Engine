// Real-instrument sample-pack ingestion and automatic key-zone / velocity /
// round-robin mapping from file naming conventions.
//
// A SampleLibrary is the mandatory foundation of every World Heritage Engine
// instrument: engines cannot be constructed without one, and a library cannot
// be built from anything except recorded audio files.
#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "whe/sample_store.hpp"

namespace whe {

struct NamingOptions {
    int middleCOctave = 4;  // 4: C4 = MIDI 60 (scientific). 3: C3 = MIDI 60 (Yamaha/Cubase convention)
};

// Everything the parser understood from one file name.
struct ParsedName {
    std::vector<std::string> words;   // lower-case descriptive tokens ("cello", "sustain", "center")
    std::optional<int> note;          // MIDI note from a note name (C3, F#2, Db4, A-1) or key tag (k60, key60)
    std::optional<int> velocity;      // explicit MIDI velocity (vel100; v100 once resolved as velocity)
    std::optional<int> ambiguousV;    // "v<number>": velocity OR layer index — resolved per instrument group
    std::optional<int> layerIndex;    // vl3, layer3, dyn3
    std::optional<int> dynamicRank;   // ppp=1 … fff=8, soft/medium/hard words
    std::optional<int> roundRobin;    // rr3, rnd2, seq4, take2, alt1
    std::vector<int> bareNumbers;     // unlabeled numbers (kept for diagnostics)
    bool release = false;             // release-trigger sample (rel, release)
};

[[nodiscard]] ParsedName parseSampleName(std::string_view fileStem, const NamingOptions& opts = {});
[[nodiscard]] std::optional<int> parseNoteName(std::string_view token, int middleCOctave);

struct Zone {
    u32 index = 0;  // library-wide index (engines keep per-zone round-robin counters)
    int loKey = 0, hiKey = 127;
    int loVel = 1, hiVel = 127;
    int rootKey = 60;
    bool pitched = true;
    float tuneCents = 0.0f;
    int layer = 0;                    // 0 = softest velocity layer for this key
    int layerCount = 1;
    std::vector<u32> roundRobins;     // sample ids in the SampleStore, cycled per hit
};

struct Articulation {
    std::string name;
    bool releaseTrigger = false;
    bool pitched = true;
    std::vector<Zone> zones;
    std::vector<i32> lookup;  // 128 keys * 128 velocities -> zone position, -1 = unmapped

    [[nodiscard]] const Zone* find(int key, int velocity) const noexcept {
        if (key < 0 || key > 127) return nullptr;
        velocity = std::clamp(velocity, 1, 127);
        const i32 z = lookup[static_cast<std::size_t>(key) * 128 + static_cast<std::size_t>(velocity)];
        return z < 0 ? nullptr : &zones[static_cast<std::size_t>(z)];
    }
    // Every recorded layer covering `key`, softest first (used for layer morphing).
    [[nodiscard]] std::vector<const Zone*> layersAt(int key) const;
};

struct ImportOptions {
    fs::path cacheDir;  // default: <folder>/.whe_cache
    NamingOptions naming;
    u32 preloadFrames = 4096;
    bool lockPreloadInRam = true;
    unsigned threads = 0;  // decode threads, 0 = hardware concurrency
};

class SampleLibrary {
public:
    // Scans `folder` recursively for .wav/.wave/.flac/.aif/.aiff/.aifc, decodes
    // each once into the DFD cache, maps everything, and returns the library.
    // Fails if the folder holds no decodable recordings.
    [[nodiscard]] static Result<std::shared_ptr<SampleLibrary>> import(const fs::path& folder, const ImportOptions& opts = {});

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<Articulation>& articulations() const noexcept { return articulations_; }
    [[nodiscard]] const Articulation* articulation(std::string_view name) const noexcept;
    [[nodiscard]] const Articulation* primary() const noexcept;           // first non-release articulation
    [[nodiscard]] const Articulation* releaseArticulation() const noexcept;
    [[nodiscard]] const SampleStore& store() const noexcept { return store_; }
    [[nodiscard]] std::size_t zoneCount() const noexcept { return zoneCount_; }
    [[nodiscard]] const std::string& report() const noexcept { return report_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] const IngestStats& ingestStats() const noexcept { return stats_; }

private:
    std::string name_;
    std::vector<Articulation> articulations_;
    SampleStore store_;
    std::size_t zoneCount_ = 0;
    std::string report_;
    std::vector<std::string> warnings_;
    IngestStats stats_;
};

// General MIDI percussion key for an unpitched recording, from its words.
[[nodiscard]] std::optional<int> gmPercussionKey(const std::vector<std::string>& words);

}  // namespace whe
