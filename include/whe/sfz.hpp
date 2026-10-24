// SFZ reader — the mapping files most free sample packs ship with (VSCO,
// VCSL, FreePats, Salamander, sfzinstruments…). Only the mapping is read:
// which recording plays on which keys and velocities, round robins, release
// triggers, tuning and level. Sound-shaping opcodes (envelopes, filters, CC
// modulation) are ignored on purpose: the engine's physics does the shaping.
//
// Supported: <control> <global> <master> <group> <region> inheritance,
// #define / #include (in order, as the SFZ spec requires), default_path,
// note_offset / octave_offset, sample, key / lokey / hikey / pitch_keycenter
// (numbers or note names, C4 = 60), lovel / hivel, seq_length / seq_position,
// lorand / hirand, trigger (attack / release / first / legato), tune,
// transpose, volume, pitch_keytrack, sw_last / sw_default (keyswitch: the
// default switch's regions are used), locc / hicc against set_cc / set_hdcc
// defaults, and on_locc / on_hicc (CC-triggered regions are skipped).
#pragma once

#include <string>
#include <vector>

#include "whe/core.hpp"

namespace whe {

struct SfzRegion {
    fs::path sample;          // absolute
    int loKey = 0, hiKey = 127, keyCenter = 60;
    int loVel = 1, hiVel = 127;
    int seqLength = 1, seqPosition = 1;
    float loRand = 0.0f, hiRand = 1.0f;
    bool release = false;
    bool pitched = true;      // pitch_keytrack != 0
    float tuneCents = 0.0f;   // + = play sharper
    float volumeDb = 0.0f;
    std::string label;        // group_label / region_label, for display
};

struct SfzMapping {
    fs::path file;
    std::vector<SfzRegion> regions;     // usable regions only
    std::vector<std::string> notes;     // what was skipped and why (CC-triggered, keyswitch, missing files…)
};

[[nodiscard]] Result<SfzMapping> parseSfz(const fs::path& file);

// MappingChart.txt (VSCO style): lines "000=21" map a number in the file name to a MIDI key.
[[nodiscard]] std::vector<std::pair<int, int>> parseMappingChart(const fs::path& file);

}  // namespace whe
