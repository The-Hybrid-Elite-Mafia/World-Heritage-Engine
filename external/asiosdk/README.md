# Steinberg ASIO SDK — drop zone (never committed)

The ASIO SDK is Steinberg's code, dual-licensed GPLv3 or Steinberg proprietary.
It is kept out of this repository; `.gitignore` blocks everything here except this file.

Install it (PowerShell, from the repo folder):
```
powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
```
This downloads ASIO SDK 2.3.4 from download.steinberg.net, checks its SHA-512,
applies the `asiolist.cpp` delete[] fix, and installs it to `external/asiosdk/sdk/`.
Re-run CMake and `whe_play` is built with `--asio` support.

Licence note: an ASIO-enabled build uses the SDK under GPLv3 unless you hold
Steinberg's proprietary licence. Under GPLv3, if you distribute that build you must
also release this engine's source under the GPL. Keeping it private is unaffected.
