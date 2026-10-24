# v0.3.3 installed instrument creator

Startup is always stopped. Saved Steinberg ASIO preference is separate from Connect; no sound until you explicitly connect and play. Save instrument names the preset and defaults to library/User Instruments, visible in the browser. Unsaved changes are guarded before New, Load, Library replacement and Close. Original recordings remain unchanged. Save records sample paths, not audio copies; retain your original recordings or package them alongside the preset.

# Version 0.3.1 incremental expansion

See ../EXPANSION-README.md and ../ADDITIONS-COVERAGE.json for mixed-license additions and current validation. The acoustic collection description below documents the preserved baseline only.

# World Heritage Instrument Creator

Standalone Windows x64 instrument creator, C++23 and native Win32. Every playable instrument starts with real recorded acoustic samples. No synthesized substitute library, song generator, DAW or fixed tracks.

## Build and run

Run build.ps1 -Compiler <path-to-x86_64-w64-mingw32-clang++.exe> on Windows 10/11 x64, or use CMake. Tested compiler: LLVM MinGW 20261020, Clang 23.1.4. Media Foundation decodes FLAC. No JUCE, Qt, paid API or application framework. Python is only for developer tests and optional library setup.

Run bin/WorldHeritageInstrumentCreator.exe. Put the prepared library directory beside the executable. Library... opens a searchable native preset browser. Load an instrument, review provenance and confirm its acoustic source before auditioning. Import WAV/FLAC/AIFF folders, correct key/velocity/round-robin maps and save portable .whic presets. Source audio is never modified.

WASAPI shared opens the Windows default output. Audio output... offers per-app device selection, explicit exclusive mode and stop/reconnect. Unsupported/busy exclusive requests fail clearly without fallback. Windows defaults never change. There is no automatic demo playback. Normal startup is stopped; Connect is explicit.

Keyboard, on-screen keys and MIDI audition samples. Resonance, brightness/tension, strike and friction-like processing shape those recordings, not invented oscillator sources or exact reconstructed bow pressure. No sample means silence.

## Acoustic collection

CC0 recordings from VSCO 2 Community Edition, VCSL and selected FreePats packs. Original notices, pinned source revisions, hashes and exact mapping adaptations are in the collection manifests. Unique instrument types are counted separately from presets, articulations, models and files, with cross-library deduplication. Electronic TX81Z, uncertain-origin material and restricted redistribution packs are excluded.

Large samples are not committed to git. See LIBRARY_SETUP.md and acquisition scripts for official sources and verification. A source checkout without assets is honestly import-first. Allow about 10 GB for downloads and additional room for backups.

Converters are audited for these named collections, not general SFZ interpreters. Preset v4 preserves source offset, tuning, gain, attack/release, key/velocity/RR and DSP. Older versions migrate with unchanged defaults. Release/CC triggers and keyswitch programs are omitted. Velocity crossfades become documented hard switches. Loops are not implemented: held notes end with the recording. Other envelope/velocity-response differences remain explicit in manifests. An unpitched keyboard pad is not a claimed measured musical pitch.

## Runtime and tests

32 voices, 4096 zones, quietest-voice stealing. Worker MMAP: one view up to 1 MiB and two 8192-frame stereo pages per decoder. Only workers access mapped pages. Background FLAC preparation: 256 MiB per sample and 1 GiB process disk cache limits, originals unchanged. Normal final release removes temporary caches; interrupted processes may leave their own cache directories.

Output transfer uses a prefaulted locked 128 KiB ring, fixed arithmetic and acquire/release atomics. No transfer-time allocation, file I/O or mutex acquisition. Decoding/DSP occur on workers. Setup may block on Windows APIs. Residency failure refuses playback. These limits do not eliminate Bluetooth, disk, scheduler or hardware latency.

library_test.exe <library-folder> <report-prefix> decodes every referenced sample in bounded blocks and offline-renders every preset, checking finite/nonzero PCM, frames and complete zone/RR coverage. Reports are JSON/TSV. Additional silent tests cover formats, MMAP, presets, exact source-parameter references, DSP stability/no-source silence, lock-free handoff and WASAPI policy. Native UI smoke modes suppress audio output.

Unrun: audible quality tests, physical MIDI, hardware latency, long soak, hotplug certification and ASIO hardware. Supported mono/stereo WAV PCM8/16/24/32/float32, AIFF/AIFC NONE/twos/sowt/fl32 and Windows-decoded FLAC at 8–384 kHz. RF64, compressed WAV, multichannel, full SFZ and looping remain gaps. This is not production certification or a claim that every instrument exists in the collection.

## License

Application source: GPL version 3 (full text in LICENSE). Samples retain original CC0 licenses and notices, not GPL relabeling. Optional official Steinberg ASIO SDK follows its GPLv3 alternative; WASAPI remains primary. No proprietary agreement or driver installation. See THIRD_PARTY_NOTICES.md.

No automatic GitHub Actions workflows, credentials, signing keys, caches or personal machine paths are supplied. Close and remove the portable application folder to uninstall; separately stored packs/presets remain intact.
