# TwinsanityEvolved — third-party notices

TwinsanityEvolved's own code is MIT licensed (`LICENSE`). This file names every third-party component in the
repository and in the shipped artifact, its licence, and how it is used. A release folder (`dist/<platform>/`, staged
by `tools/ci/release_dist.sh` / `tools/ci/release_dist.ps1`) carries its own generated `THIRD_PARTY_NOTICES.md` — the
subset below that is actually linked into that binary, with the exact versions — and the licence texts in `licenses/`
(from `engine/tools/dist/licenses/` and `FFmpeg-LICENSE.md`, pinned by `engine/tests/lint/lint_artifact_licenses.py`).

The repository contains **no game data**: no textures, audio, models, movies or disc bytes. Crash Twinsanity is the
property of its rights holders; you must own the game and supply your own disc image (`README.md`).

## The two vendored trees

| Tree | Licence | |
|---|---|---|
| `game/` — the twinsanity-reversed decompilation | [twinsanity-reversed](https://github.com/Smartkin/twinsanity-reversed) | compiled to wasm32 and into the binary |
| `engine/` — NativeEE | MIT — `engine/LICENSE` | the runtime the game runs on |

## Compiled into the shipped binary

| Component | Version | Licence | Where / how |
|---|---|---|---|
| SDL2 | `release-2.30.9` (`engine/cmake/Sdl2Dependency.cmake`) | zlib — `engine/tools/dist/licenses/SDL2-LICENSE.txt` | built from source and linked statically by the `*-dist` presets; the system package (Homebrew / distribution / vcpkg DLL) in the other presets |
| FFmpeg — libavcodec, libavutil, libswscale | `n6.1.2` (`engine/tools/ci/ffmpeg_static.sh`) | LGPL v2.1 or later — `engine/tools/dist/licenses/FFmpeg-COPYING.LGPLv2.1.txt`, per-component terms in `FFmpeg-LICENSE.md` | the MPEG-1/2 movie decoder. Built with `--disable-gpl --disable-nonfree --disable-version3` and only the mpeg1/mpeg2 decoders, the mpegvideo parser and colour conversion; linked statically on macOS / Linux and in the PlayStation Vita's eboot (the same release and recipe cross-compiled by `tools/vita/ffmpeg_vita.sh`), bundled as DLLs on Windows. LGPL §6: this repository is the complete source of the program and the recipe that rebuilds the library |
| Dear ImGui | `v1.91.9b` | MIT — `engine/app/gui/third_party/imgui/LICENSE.txt` | the engine's developer overlay. Its `imstb_*.h` headers carry their own public-domain / MIT notices |
| gl3w-derived OpenGL loader (`imgui_impl_opengl3_loader.h`) | part of the ImGui copy | public domain (Unlicense-style) — `engine/tools/dist/licenses/imgl3w-UNLICENSE.txt` | used by the ImGui overlay only |
| GCC libstdc++ / libgcc (Linux artifact only) | the toolchain's | GPL v3 with the GCC Runtime Library Exception 3.1 — `engine/tools/dist/licenses/GCC-COPYING3.txt`, `GCC-RUNTIME-LIBRARY-EXCEPTION.txt` | linked statically so the folder runs without matching distribution packages |

The launcher (`engine/app/launcher`) links Qt 6 dynamically (LGPL v3); its release folder carries Qt's notices.

## Build-time tools (nothing of them is in the shipped binary)

| Component | Licence | Where / how |
|---|---|---|
| wasi-sdk, wabt (wasm2c) | Apache 2.0 with LLVM exception / Apache 2.0 | the pinned toolchain `tools/fetch_toolchain.sh` downloads; wabt's wasm-rt runtime sources are compiled into the binary from that download |
| rabbitizer (MIPS disassembler) | MIT — `engine/third_party/rabbitizer/LICENSE` | the engine's recompilers decode instructions with it at build time |
| ELFIO | MIT — `engine/third_party/elfio/LICENSE.txt` | the engine's recompilers read the ELF / IRX with it at build time |
| JSON for Modern C++ (nlohmann/json, `engine/tools/recompiler_v2/include/json.hpp`) | MIT — the notice at the top of the header | the recompiler's symbol / stub inputs |

## Not bundled

- The operating system's own libraries (OpenGL, the C/C++ runtime, the audio and window-system back ends SDL2 loads)
  are the user's own.
