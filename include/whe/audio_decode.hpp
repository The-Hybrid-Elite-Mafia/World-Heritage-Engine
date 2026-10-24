// From-scratch decoders for real recorded instrument samples:
//   WAV  (RIFF/RF64, PCM 8/16/24/32, float 32/64, WAVE_FORMAT_EXTENSIBLE, smpl + inst chunks)
//   AIFF / AIFC (NONE/twos/sowt/fl32/fl64, MARK + INST sustain loops)
//   FLAC (native .flac stream, all subframe types, all stereo decorrelation modes, MD5 verified)
// Decoding happens once at ingestion; playback always streams the decoded cache from disk.
#pragma once

#include <array>
#include <optional>

#include "whe/core.hpp"

namespace whe {

struct LoopInfo {
    u64 start = 0;  // first frame of loop
    u64 end = 0;    // one past last frame of loop
};

// Metadata embedded by the recording / pack author (when present).
struct EmbeddedMeta {
    std::optional<int> rootKey;  // MIDI note
    double fineTuneCents = 0.0;  // how far the recording's actual pitch sits above rootKey
    std::optional<int> lowKey, highKey, lowVel, highVel;
    std::optional<LoopInfo> loop;
};

enum class SourceFormat { Wav, Aiff, Flac };

struct DecodedAudio {
    SourceFormat format = SourceFormat::Wav;
    u32 sampleRate = 0;
    u32 channels = 0;
    u32 sourceBits = 0;
    u64 frames = 0;
    std::vector<float> interleaved;  // frames * channels, nominal range [-1, 1]
    EmbeddedMeta meta;
    bool md5Verified = false;  // FLAC only: decoded audio matched STREAMINFO MD5
};

[[nodiscard]] Result<DecodedAudio> decodeAudioFile(const fs::path& path);
[[nodiscard]] Result<DecodedAudio> decodeAudioMemory(std::span<const std::byte> bytes, const std::string& nameForErrors);

// Individual format entry points (used by decodeAudioMemory and the tests).
[[nodiscard]] Result<DecodedAudio> decodeWav(std::span<const u8> bytes);
[[nodiscard]] Result<DecodedAudio> decodeAiff(std::span<const u8> bytes);
[[nodiscard]] Result<DecodedAudio> decodeFlac(std::span<const u8> bytes);

[[nodiscard]] bool isSupportedAudioExtension(const fs::path& path);

// Writes 24-bit PCM WAV (used by offline render tools and tests).
[[nodiscard]] Result<void> writeWav24(const fs::path& path, u32 sampleRate, u32 channels, std::span<const float> interleaved);

// Minimal MD5 (RFC 1321) used to verify FLAC streams against STREAMINFO.
class Md5 {
public:
    Md5();
    void update(const u8* data, std::size_t len);
    std::array<u8, 16> finish();

private:
    void block(const u8* p);
    u32 h_[4];
    u8 buf_[64];
    u64 total_ = 0;
    std::size_t used_ = 0;
};

}  // namespace whe
