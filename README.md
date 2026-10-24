# The World Heritage Engine — Sample-Based Hybrid Engine (C++23)

A standalone instrument creator. Not a DAW, not a mixer: it builds, shapes and plays
instruments. **Every instrument starts from real recorded acoustic samples.** The physical
models exist only to articulate and reshape those recordings.

- Pure C++23, zero third-party code (no JUCE, no Qt, no libFLAC, no libsndfile).
- Windows x64 target: WASAPI output, MMCSS, Win32 memory mapping, runtime AVX2/AVX-512 dispatch.
- Also builds on Linux (used for the test suite and offline renders).

---

## 0. The instrument window — `whe_studio.exe`

The program you play. One window drawn with Direct2D over a faceplate rendered in
Blender (`art/plate.py`, `art/parts.py`; coordinates shared through `art/layout.py`
→ `app/layout_gen.hpp`). Only Windows' own APIs: Direct2D, WIC, DirectWrite, Win32.

| Area | What it does |
|---|---|
| Status bar | Sound card (tap to pick ASIO driver or Windows audio), sample rate, buffer + latency (tap to change), CPU, disk stream, voices, underruns |
| Instruments | Sample packs found in `Samples\` next to the program plus any you import. **+ Import sample pack**, drag a folder onto the window, or start `whe_studio.exe <folder>` |
| Engine glass | What is loaded; **FILE MAP** shows every file and how its name was read (root note, keys, velocity layer, round robin, articulation) and lets you choose the engine |
| Key map | Which recording plays where; tap a zone to hear it; lit = sounding |
| Dials | The physics that reshape the recording, a plain-English line under each. Every dial starts at zero (strike-point dials start at the middle); double-click a dial to put it back |
| Blend | Left = full recording, right = physics only; **ON** button bypasses the physics; L/R meters |
| Keyboard | Mouse, PC keys (A W S E D F T G Y H U J K …, Z/X octave, Space sustain) or any MIDI keyboard (all MIDI inputs are opened) |

Faceplate rules: a lit light is ON, an unlit light is OFF. Art is decoded once and
re-sampled on the CPU to the exact pixel size it occupies in the window before it
reaches the GPU — the 4K renders are never uploaded at full size. Static parts are
cached in one layer so meters animate cheaply (remote desktop friendly).

Settings and saved instrument sounds: `%APPDATA%\HEM\World Heritage Engine\settings.txt`.
Fonts: Montserrat (SIL Open Font License, `app/res/fonts/OFL.txt`).

---

## 1. The sample-first rule, enforced in code

| Rule | Where it is enforced |
|---|---|
| An instrument can only be built from recordings | `createInstrument()` takes a `SampleLibrary`; it fails on `nullptr`, empty, or (for pitched engines) unpitched libraries. `SampleLibrary::import()` fails if a folder holds no decodable `.wav/.flac/.aif`. |
| The only sound source is a recording | `SampleVoice` (streamed recording playback) is the only class that produces audio. There is no oscillator or noise-generator class anywhere in the code base. |
| Physics only reshapes the recording | Every hybrid hook is `process(input) -> output`. With zero input from a reset state each one returns **exact zeros**; every feedback loop is passive (gain ≤ 1), so the response dies away once the recording stops feeding it. |
| Verified, not assumed | `whe_tests` checks, for every hook at extreme settings: silence in → bit-exact silence out, finite output, decay to < −80 dB of peak. It also renders every engine for 1 s with no notes and requires every output sample to be exactly `0.0f`. |

---

## 2. Pipeline

```
 sample pack folder (.wav .flac .aif .aiff .aifc, any sub-folders)
        │  SampleLibrary::import  (parallel decode threads)
        ▼
 decoders (from scratch): WAV/RF64/EXTENSIBLE · AIFF/AIFC · FLAC (MD5-verified)
        │  ensureCached: decode ONCE → .whs cache (4 KiB header + page-aligned float32)
        ▼
 file-name parser ──► auto mapper ──► Articulations → Zones (key range × velocity layer) → round robins
        │
        ▼
 SampleStore: every .whs memory-mapped; only the first 4096 frames per sample are copied
 into one locked RAM arena (VirtualLock / mlock)
        │
        ▼
 DiskStreamer thread ──(lock-free MPMC requests)──► per-voice double buffer (BlockPool)
        │
        ▼
 SampleVoice (Hermite pitch, velocity, release)  ← the ONLY sound source
        │ recording
        ▼
 bespoke engine hooks:  processVoice (per-voice physics)  →  postProcess (instrument-level physics)
        │
        ▼
 InstrumentManager: thousands of engines rendered in parallel → summed to the device
        │
        ▼
 WasapiOutput (event-driven shared / exclusive, MMCSS "Pro Audio")
```

---

## 3. Ingestion and automatic mapping

### Decoders (`src/audio_decode.cpp`, `src/flac.cpp`)
- **WAV**: RIFF and RF64, PCM 8/16/24/32-bit, float 32/64, `WAVE_FORMAT_EXTENSIBLE`; reads `smpl`
  (root key, pitch fraction, sustain loop) and `inst` (root, fine tune, key/velocity range).
- **AIFF/AIFC**: `NONE`, `twos`, `sowt`, `fl32`, `fl64`; `INST` + `MARK` sustain loops.
- **FLAC**: every subframe type (constant, verbatim, fixed 0–4, LPC 1–32), wasted bits, Rice/Rice2
  with escape partitions, all stereo decorrelation modes; decoded audio is checked against the
  encoder's STREAMINFO MD5.
- Multichannel files (e.g. several mic positions in one file) are folded to stereo at ingestion.

### Why a cache
FLAC is compressed and cannot be read at an arbitrary frame, so true direct-from-disk streaming
needs a raw image. Each recording is decoded once into `<pack>/.whe_cache/<hash>.whs`. The cache is
re-used until the source file's size or modification time changes.

### File-name conventions understood (`parseSampleName`)
| Token | Meaning | Examples |
|---|---|---|
| Note name | root key (C4 = 60 by default, `--middle-c 3` for C3 = 60) | `C3`, `F#2`, `Db4`, `A-1`, `Cs3` |
| Note glued to a word | root key + word (note letter must be upper-case) | `harmLA0` |
| `k/key/midi/note` + number | explicit MIDI key | `key55` |
| `v/vel/velo/velocity` + number | MIDI velocity **or** layer index (resolved per group) | `v127`, `v12` |
| `vl/layer/dyn` + number | velocity layer index | `vl3` |
| `rr/rnd/seq/take/alt/var` + number | round robin | `RR3`, `rr2` |
| dynamic words | layer order | `ppp pp p mp mf f ff fff`, `soft med hard loud` |
| `rel/release` | release-trigger sample | `Piano_C4_rel` |
| other words | articulation (words shared by every file are dropped) | `Sustain`, `Pizz`, `center` |

**`v` ambiguity:** if the `v` numbers in a group form a contiguous run starting at 0/1 (e.g. 1…16),
they are layer indices and are spread evenly over velocities 1–127; otherwise they are MIDI velocities
and layer *i* covers `(v[i-1], v[i]]`.

### Mapping
- **Key zones**: sorted root notes; each zone extends half-way to its neighbours; the outer zones
  stretch to keys 0 and 127. Ranges embedded in the file (`inst`/`INST`) override this.
- **Velocity layers**: per root note, from the rule above.
- **Round robins**: files sharing articulation + note + layer; ordered by RR number, then file name.
- **Unpitched recordings** (no note anywhere) are placed on their General MIDI percussion key from
  their words (`snare` → 38, `kick` → 36, `hihat open` → 46, `ride bell` → 53, …; collisions move to
  the next free key). Unpitched release samples serve the whole keyboard.
- `whe_ingest <folder> --zones` prints the complete map.

---

## 4. Direct-from-disk streaming (`src/dfd_stream.cpp`)

- **Memory-mapped**: every `.whs` is mapped once (`CreateFileMapping/MapViewOfFile`, `mmap`). Mapped
  pages are file-backed and evictable — they never count as committed RAM.
- **Resident RAM per sample** = 4096 frames × channels × 4 bytes (32 KiB stereo), copied into one
  locked arena, so a note-on never touches the disk. (Real test: an 85-file grand piano subset is
  431.7 MB on disk and 2.7 MB resident.)
- **Double buffering**: each voice owns two 16384-frame region buffers from a lock-free `BlockPool`.
  While the voice plays region *k*, the disk thread fills region *k+1*.
- **Lock-free requests**: a Vyukov bounded MPMC queue carries fill requests; the disk thread sleeps on
  `std::atomic::wait` (futex / `WaitOnAddress`). Page faults happen only on the disk thread, which also
  issues read-ahead (`PrefetchVirtualMemory` / `madvise(WILLNEED)`).
- **Voice stealing safety**: each buffer carries a `(generation, region)` ticket; a fill that finishes
  after its voice was restarted is discarded instead of playing the old note's audio.
- **Loops** embedded in the recording are streamed as a virtual timeline that wraps inside the loop.
- **Underruns** zero-fill and are counted (`DiskStreamer::stats()`); they are never silent failures.

---

## 5. Hybrid DSP hooks (`include/whe/hybrid.hpp`)

| Hook | Physics | Fed by |
|---|---|---|
| `TransientExtractor` | onset window + envelope-flux gate | isolates the real hammer strike / pluck / stick hit / tongue from the recording |
| `StringResonator` | extended Karplus–Strong waveguide: exact phase-delay tuning, T60 loop gain, loop low-pass, 4-stage allpass stiffness dispersion, excitation-position comb | real strike transient |
| `SympatheticBank` | bank of open strings | summed recordings |
| `BowFrictionShaper` | bidirectional waveguide with a pressure-dependent stick/slip bow-table junction (scattering matrix norm ≤ 1) | the real bowed recording, used as the bow-velocity signal |
| `AirColumnShaper` | bore waveguide (open–open or closed–open), bell diffraction split at ka = 1 (fc = c / 2πa), register loop for overblowing, passive tanh pressure saturation | the real breath/tone, its own breath noise (turbulence) and attack (tonguing) |
| `MembraneMesh` | FDTD of the damped 2-D wave equation (finite-difference twin of a rectilinear waveguide mesh), circular head mask, frequency-dependent loss, velocity pickup; stability bound λ² + 2κ ≤ ½ enforced | the real hit transient injected at the strike point through a mallet-hardness low-pass |
| `BodyRevoicer` | two modal banks: `out = in + amount·(Body(f·√(E/ρ)) − Body(f))` | the summed recordings; modes are **estimated from the recordings themselves** (geometric mean of normalised long-term spectra across notes). At reference wood settings the output is bit-identical to the input. |

The mode estimate is an approximation, not a laboratory modal analysis.

---

## 6. Bespoke engines (`include/whe/engines.hpp`)

All parameters are lock-free (`SmoothedParam`: any thread writes, the render thread glides per block).

**PianoEngine** — real hammer strike → 1/2/3 unison modeled strings (by register) layered on the recording; sustain pedal opens 25 sympathetic strings.
`HammerHardness, StrikeWindowMs, StringResonance, RecordingLevel, UnisonDetune, Stiffness, StringDecay, SympatheticResonance`.
`RecordingLevel = 0` leaves only modeled strings, driven entirely by the real recorded hammer.

**BowedStringEngine** — real bowed recording → friction waveguide; `Dynamics` equal-power crossfades the two *recorded* dynamic layers that bracket it (map to CC1); body re-voicing.
`BowPressure, BowPosition, Rosin, FrictionAmount, RecordingLevel, Dynamics, UseDynamics, WoodDensity, WoodStiffness, BodyAmount, StringDecay`.

**PluckedStringEngine** — real pluck transient → Karplus–Strong string; open-string sympathetics (`setOpenStrings()`, default guitar EADGBE); body re-voicing.
`PluckPosition, StringDecay, Brightness, ResonatorAmount, RecordingLevel, PickWindowMs, Sympathetic, WoodDensity, WoodStiffness, BodyAmount`.

**AerophoneEngine** — real breath/tone → bore waveguide with live bore length.
`BoreLengthScale, BoreType, Overblow, Turbulence, Tonguing, BellRadius, ColumnAmount, RecordingLevel, ColumnDecay, Nonlinearity`.

**MembraneEngine** — every real hit excites ONE shared drumhead (one physical head per drum).
`Tension, Damping, HighDamping, StrikeX, StrikeY, MalletHardness, HeadAmount, RecordingLevel, ExciteWindowMs`.

Adding an instrument family = subclass `InstrumentEngine`, override `processVoice` / `postProcess`.
The base class guarantees the input is always a real recording.

---

## 7. Parallel instrument manager (`InstrumentManager`)

- Fixed slots with atomic engine pointers; add/remove never blocks audio; removal is deferred two blocks.
- Per block, the audio thread publishes a `(sequence << 32 | index)` job counter; workers *and* the audio
  thread claim instruments by CAS, render into per-slot buffers, and the audio thread sums them.
- Workers sleep on `std::atomic::wait`; on Windows they join MMCSS "Pro Audio".

---

## 8. SIMD (`src/simd/`)

Scalar, AVX2+FMA and AVX-512F kernels live in separate translation units compiled with their own ISA
flags; `detectSimdLevel()` checks CPUID **and** OS register support (XGETBV) and picks the best at
start-up. `WHE_SIMD=scalar|avx2|avx512` forces a level. Vectorised: membrane mesh step, modal banks
(8/16 modes per register, whole block in registers), output accumulation.

---

## 9. Build

Windows (Visual Studio 2022 17.8+ or any CMake ≥ 3.25 toolchain):
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
MinGW-w64 (GCC 13) also works; Linux: `cmake -S . -B build && cmake --build build -j`.

Tools:
- `whe_ingest <pack> [--zones] [--middle-c 3|4]` — import, cache, print the map.
- `whe_render <pack> <piano|bowed|plucked|aerophone|membrane> out.wav [--set Name=value] [--notes 60,64,67]` — offline render.
- `whe_play <pack> <engine> [--asio [driver]] [--list-asio] [--buffer N] [--rate Hz] [--exclusive] [--period N] [--midi N] [--set Name=value]` — Windows real-time host:
  WASAPI or ASIO out, MIDI in (notes, CC64 sustain, pitch bend, CC1 → Dynamics, CC20–29 → parameters 0–9),
  PC-keyboard fallback.
- `whe_tests [real-pack ...]` — test suite.

---

## 10. What has been verified — and what has not

Verified (October 2026):
- Linux GCC 13 build: **402/402 checks pass**, including real-pack tests on 85 FLAC files from the
  Salamander Grand Piano and 72 FLAC files from Virtuosity Drums (snare).
- Windows x64 build (MinGW-w64 GCC 13, cross-compiled): **248/248 checks pass** running under Wine,
  exercising the Win32 memory-mapping, `VirtualLock`, prefetch and atomic-wait paths.
- FLAC/WAV/AIFF decoding is bit-exact against FFmpeg across 24 format variants and on the real files.
- `whe_play.exe` ran under Wine against a null audio device in WASAPI **shared** mode: audio thread,
  note input and streaming worked (0 underruns).
- Throughput (one core, AVX-512, 48 kHz, 256-frame blocks, 32 notes): piano 5.3× real time,
  bowed 9.8×, plucked 15×, aerophone 7.1×; membrane (16 hits) 39× (9.8× scalar).

Not verified:
- **No real Windows machine or sound card** — WASAPI on real hardware is untested.
- **WASAPI exclusive mode** could not be exercised (Wine's null device refuses it).
- **MSVC** has not compiled this code; only GCC (Linux and MinGW). Expect possible minor fixes.
- **ASIO** (`src/platform/asio_output.cpp`) is written against the Steinberg ASIO SDK 2.3.4 but has not
  been run against a real ASIO driver. The SDK is never committed: `tools/fetch-asio-sdk.ps1` downloads it
  from Steinberg and checks its SHA-512, and the GitHub Actions workflow builds a WASAPI-only and an
  ASIO-enabled flavor. Because the ASIO SDK is used under its GPLv3 option, this whole engine is
  licensed GPLv3 (see `LICENSE`).
- The bowed, plucked and aerophone engines have only been run on piano recordings (no real cello,
  guitar or flute pack was available in the build environment), so their *sound* on their own
  instrument families has not been judged — only their correctness and stability.
- Default parameter levels were set by measurement, not by ear.

Known limits: Ogg-encapsulated FLAC and compressed WAV (ADPCM etc.) are rejected; recordings with more
than two channels are folded to stereo; the naming parser covers common conventions but not every
vendor's scheme (check `whe_ingest --zones`); unpitched release noises are spread over the whole keyboard.

---

## Licence
GNU General Public License v3.0 — see `LICENSE`. The Steinberg ASIO SDK is used under its GPLv3 option and is downloaded at build time, never stored here.

## Sample credits (test packs, not included)
- Salamander Grand Piano V3 — Alexander Holm, CC BY 3.0 (github.com/sfzinstruments/SalamanderGrandPiano)
- Virtuosity Drums — performed by Austin McMahon, recorded at Virtuosity Musical Instruments (Boston), CC0 (github.com/sfzinstruments/virtuosity_drums)
