// Ingestion cache + resident sample store.
//
// Ingestion decodes each real recording (WAV/AIFF/FLAC) exactly once into a
// ".whs" cache file: a 4 KiB header followed by page-aligned interleaved
// float32 frames. Compressed FLAC cannot be randomly addressed, so the cache
// is what makes true direct-from-disk streaming possible.
//
// At load time every .whs is memory-mapped (MappedFile). Only a short head
// ("preload", default 4096 frames) is copied into a locked RAM arena so a
// note-on never touches the disk; everything after the head is streamed by
// the DiskStreamer from the mapping. Resident RAM per sample is therefore
// preloadFrames * channels * 4 bytes regardless of the recording's length.
#pragma once

#include <memory>
#include <optional>

#include "whe/audio_decode.hpp"
#include "whe/mapped_file.hpp"

namespace whe {

inline constexpr char kWhsMagic[8] = {'W', 'H', 'E', 'S', 'M', 'P', 'L', '1'};
inline constexpr u64 kWhsDataOffset = 4096;

struct WhsHeader {
    char magic[8];
    u32 version;
    u32 channels;
    u32 sampleRate;
    u32 sourceBits;
    u64 frames;
    i32 rootKey;  // -1 when the recording carries no root note
    float fineTuneCents;
    i32 lowKey, highKey, lowVel, highVel;  // -1 when absent
    u64 loopStart, loopEnd;                // loopEnd == 0: no loop
    u64 sourceSize;
    i64 sourceMtime;
    u64 dataOffset;
    float peak;
    u32 sourceFormat;
    u32 md5Verified;
    char sourceName[256];
};
static_assert(sizeof(WhsHeader) <= kWhsDataOffset);

struct IngestStats {
    std::size_t decoded = 0;      // files decoded this run
    std::size_t cacheHits = 0;    // files whose cache was already current
    std::size_t md5Verified = 0;  // FLAC files verified against their checksum
};

// Ensures an up-to-date cache exists for `source` in `cacheDir`; returns its path.
[[nodiscard]] Result<fs::path> ensureCached(const fs::path& source, const fs::path& cacheDir, IngestStats& stats);

// One real recording, mapped from disk.
struct StoredSample {
    fs::path source;
    WhsHeader header{};
    MappedFile map;
    const float* frames = nullptr;  // header.frames * channels floats inside the mapping
    const float* preload = nullptr; // first preloadFrames frames, resident RAM
    u32 preloadFrames = 0;

    [[nodiscard]] u32 channels() const noexcept { return header.channels; }
    [[nodiscard]] u64 frameCount() const noexcept { return header.frames; }
    [[nodiscard]] bool hasLoop() const noexcept { return header.loopEnd > header.loopStart; }
    [[nodiscard]] std::optional<int> rootKey() const noexcept {
        return header.rootKey >= 0 ? std::optional<int>(header.rootKey) : std::nullopt;
    }
};

class SampleStore {
public:
    explicit SampleStore(u32 preloadFrames = 4096) : preloadFrames_(preloadFrames) {}

    // Maps a cached .whs file. Not real-time safe (load thread only).
    [[nodiscard]] Result<u32> addCached(const fs::path& whsPath, const fs::path& originalSource);

    // Copies every sample's head into one contiguous RAM arena and (best effort)
    // locks it so the OS can never page it out. Call once after all addCached().
    void finalize(bool lockInRam = true);

    [[nodiscard]] const StoredSample& get(u32 id) const noexcept { return *samples_[id]; }
    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t residentBytes() const noexcept { return arena_.size() * sizeof(float); }
    [[nodiscard]] u64 mappedBytes() const noexcept;
    [[nodiscard]] bool ramLocked() const noexcept { return locked_; }

private:
    u32 preloadFrames_;
    std::vector<std::unique_ptr<StoredSample>> samples_;
    AlignedBuffer<float> arena_;
    bool locked_ = false;
};

}  // namespace whe
