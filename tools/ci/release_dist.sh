#!/usr/bin/env bash
# tools/ci/release_dist.sh -- the ONE command behind the release workflow's macOS and Linux jobs, and the local
# reproduction of either (CI workflows are authored AND run locally through their scripts).
#
#   tools/ci/release_dist.sh [--build-dir DIR] [--dist-dir DIR] [--jobs N] [--system-deps] [--lto] [--no-fetch]
#                            [--skip-build] [--no-zip] [--iso IMAGE]
#
# What it does, in order (exit non-zero on the first failing gate):
#   1. the pinned wasm32 toolchain (tools/fetch_toolchain.sh; sha256 against tools/toolchain/PINS.sha256);
#   2. the RELEASE dependency shape, unless --system-deps: FFmpeg built statically from the engine's recipe
#      (engine/tools/ci/ffmpeg_static.sh: LGPL -- --disable-gpl --disable-nonfree --disable-version3, the mpeg1/mpeg2
#      decoders + swscale only) into <build>/third_party/ffmpeg, and SDL2 fetched and built statically from the
#      engine's pinned release (TE_DIST=ON -> engine/cmake/Sdl2Dependency.cmake on both the engine and the port);
#      the artifact then carries NO lib/ folder. --system-deps = the development shape (Homebrew / apt packages,
#      bundled into lib/ with their rpaths rewritten; REFUSED to ship when the bundled libavcodec is a GPL build);
#   3. configure + build the runner (cmake -G Ninja, Release; TE_LTO under --lto);
#   4. stage dist/<platform>/: twinsanityevolved, twinsanityevolved-modtool, BUILD-ID, README.md (the player half),
#      RELEASE-NOTES.txt (what the README's What's new links), LICENSE (MIT),
#      THIRD_PARTY_NOTICES.md + licenses/ (the engine's committed texts + the FFmpeg
#      build-configuration proof read from WHAT WAS LINKED), fixtures/padlogs/ (inputs only: no game data);
#   5. the asset scan over the staged folder (tools/ci/asset_scan.py ROOT=dist: zero findings);
#   6. a --dry-run of the binary when --iso is given (a local run only; CI has no disc);
#   7. dist/twinsanityevolved-<platform>.zip + .sha256.
#
# <platform> = macos-arm64 | macos-x86_64 | linux-x86_64 | linux-aarch64 (Windows: tools/ci/release_dist.ps1).
# Game data is never read except by the optional --iso dry run, and never written anywhere.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="$ROOT/build-release"; DIST_ROOT="$ROOT/dist"; JOBS=""; SYSTEM_DEPS=0; LTO=0; FETCH=1; SKIP_BUILD=0; ZIP=1; ISO=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD="$2"; shift 2 ;;
    --dist-dir) DIST_ROOT="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --system-deps) SYSTEM_DEPS=1; shift ;;
    --lto) LTO=1; shift ;;
    --no-fetch) FETCH=0; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --no-zip) ZIP=0; shift ;;
    --iso) ISO="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "release_dist: unknown argument $1" >&2; exit 2 ;;
  esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$DIST_ROOT" in /*) ;; *) DIST_ROOT="$ROOT/$DIST_ROOT" ;; esac

step() { echo; echo "=== $*"; }

uname_s="$(uname -s)"; uname_m="$(uname -m)"
case "$uname_s/$uname_m" in
  Darwin/arm64)  PLATFORM="macos-arm64" ;;
  Darwin/x86_64) PLATFORM="macos-x86_64" ;;
  Linux/x86_64)  PLATFORM="linux-x86_64" ;;
  Linux/aarch64) PLATFORM="linux-aarch64" ;;
  *) echo "release_dist: unsupported host $uname_s/$uname_m (Windows: tools/ci/release_dist.ps1)" >&2; exit 2 ;;
esac
DIST="$DIST_ROOT/$PLATFORM"
ENGINE="$ROOT/engine"
[ -f "$ENGINE/CMakeLists.txt" ] && [ -f "$ROOT/game/include/platform/pads.h" ] || { echo "release_dist: engine/ or game/ missing -- the clone is incomplete (both are tracked trees of this repository)" >&2; exit 2; }

step "host: $PLATFORM ($(nproc 2>/dev/null || sysctl -n hw.ncpu) cpus); build $BUILD; dist $DIST"
free_gb=$(df -g "$ROOT" 2>/dev/null | awk 'NR==2{print $4}' || df -BG "$ROOT" | awk 'NR==2{gsub("G","",$4); print $4}')
min_free_gb="${TE_MIN_FREE_GB:-15}"
echo "free: ${free_gb} GB (floor ${min_free_gb} GB)"
if [ "$free_gb" -lt "$min_free_gb" ]; then echo "release_dist: < ${min_free_gb} GB free -- prune first"; exit 3; fi

if [ "$SKIP_BUILD" = 0 ]; then
  if [ "$FETCH" = 1 ]; then
    step "toolchain (pinned, sha256)"
    tools/fetch_toolchain.sh
  fi
  CMAKE_ARGS=(-S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release)
  if [ "$SYSTEM_DEPS" = 0 ]; then
    step "ffmpeg (engine recipe, static LGPL -> $BUILD/third_party/ffmpeg)"
    mkdir -p "$BUILD/third_party"
    "$ENGINE/tools/ci/ffmpeg_static.sh" "$BUILD/third_party/ffmpeg"
    CMAKE_ARGS+=(-DTE_DIST=ON "-DCMAKE_PREFIX_PATH=$BUILD/third_party/ffmpeg")
  fi
  [ "$LTO" = 1 ] && CMAKE_ARGS+=(-DTE_LTO=ON)
  step "configure (${CMAKE_ARGS[*]})"
  cmake "${CMAKE_ARGS[@]}" > "$BUILD.configure.log" 2>&1 || { tail -40 "$BUILD.configure.log"; exit 1; }
  mkdir -p "$BUILD"; mv "$BUILD.configure.log" "$BUILD/configure.log"
  grep -E "SDL2|FFmpeg|LTO|TwinsanityEvolved:" "$BUILD/configure.log" | head -12 || true
  step "build (game wasm, wasm2c, engine libs, runner)"
  if ! cmake --build "$BUILD" ${JOBS:+-j "$JOBS"} --target twinsanityevolved twinsanityevolved-modtool > "$BUILD/build.log" 2>&1; then
    grep -n -A 30 "FAILED:" "$BUILD/build.log" | head -120; exit 1   # the failed step's own output (a linker's undefined references)
  fi
  echo "build OK -- warning: $(grep -c 'warning:' "$BUILD/build.log" || true)"
fi

BIN="$BUILD/twinsanityevolved"
[ -x "$BIN" ] || { echo "release_dist: no binary at $BIN (build first)" >&2; exit 1; }

step "stage ($DIST)"
rm -rf "$DIST"; mkdir -p "$DIST/lib" "$DIST/licenses" "$DIST/fixtures"
cp "$BIN" "$DIST/twinsanityevolved"
# the mod importer on the command line (the launcher's Mods page uses the same library): system libraries only
MODTOOL="$BUILD/twinsanityevolved-modtool"
[ -x "$MODTOOL" ] || { echo "release_dist: no mod tool at $MODTOOL (build first)" >&2; exit 1; }
cp "$MODTOOL" "$DIST/twinsanityevolved-modtool"
# BUILD-ID: the runner's build id (the save-state format key) + the three trees' commits.
{
  echo "twinsanityevolved $(git -C "$ROOT" rev-parse --short=12 HEAD) ($PLATFORM)"
  echo "port    $(git -C "$ROOT" rev-parse HEAD) $(git -C "$ROOT" describe --tags --always 2>/dev/null)"
  echo "engine  $(git -C "$ENGINE" rev-parse HEAD 2>/dev/null || echo '?')"
  echo "game    $(git -C "$ROOT/game" rev-parse HEAD 2>/dev/null || echo '?')"
  echo "built   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DIST/BUILD-ID"
cat "$DIST/BUILD-ID"
# The player half of README.md: from the top to the first "## Build" heading.
awk '/^## Build/ {exit} {print}' "$ROOT/README.md" > "$DIST/README.md"
cp "$ROOT/RELEASE-NOTES.txt" "$DIST/RELEASE-NOTES.txt"   # the README's What's new links it
cp "$ROOT/LICENSE" "$DIST/LICENSE"
# Pad logs are recorded inputs (no game data) -- the fixtures a user replays the recorded routes with.
cp -R "$ROOT/fixtures/padlogs" "$DIST/fixtures/padlogs"
# The repository's own mods (mods/<id>/mod.json: today the Arabic text-only language -- our translation and our glyphs, no disc
# data; the asset scan below covers them). The launcher places them into the game folder's mods/ when they are not there yet;
# a player without the launcher copies mods/<id> beside the game (the Vita: ux0:data/twinsanity/mods/<id>).
for m in "$ROOT"/mods/*/; do
  id=$(basename "$m")
  [ -f "$m/mod.json" ] || continue
  grep -Eq '"kind": *"builtin"|"api": *"c"' "$m/mod.json" && continue   # the C mods (level_select, speedrun) are compiled in
  mkdir -p "$DIST/mods/$id" && cp -R "$m"/. "$DIST/mods/$id/"
done

# --- bundled runtime libraries: the non-system dynamic closure of the binary (empty under TE_DIST) ---
is_system_lib() {
  case "$1" in
    /usr/lib/*|/System/Library/*|/lib/*|/lib64/*|/usr/lib64/*) return 0 ;;
    *) return 1 ;;
  esac
}
bundle_macos() {
  local queue=("$DIST/twinsanityevolved")
  while [ ${#queue[@]} -gt 0 ]; do
    local f="${queue[0]}"; queue=("${queue[@]:1}")
    local dep
    while read -r dep; do
      [ -n "$dep" ] || continue
      case "$dep" in @rpath/*|@executable_path/*|@loader_path/*) continue ;; esac
      is_system_lib "$dep" && continue
      local name; name="$(basename "$dep")"
      if [ ! -f "$DIST/lib/$name" ]; then
        [ -f "$dep" ] || { echo "release_dist: dependency $dep of $(basename "$f") not found" >&2; exit 1; }
        cp -L "$dep" "$DIST/lib/$name"; chmod u+w "$DIST/lib/$name"
        install_name_tool -id "@rpath/$name" "$DIST/lib/$name" 2>/dev/null
        queue+=("$DIST/lib/$name")
      fi
      install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null
    done < <(otool -L "$f" | awk 'NR>1 {print $1}')
  done
  while read -r rp; do
    [ -n "$rp" ] && install_name_tool -delete_rpath "$rp" "$DIST/twinsanityevolved" 2>/dev/null || true
  done < <(otool -l "$DIST/twinsanityevolved" | awk '/LC_RPATH/ {getline; getline; print $2}')
  install_name_tool -add_rpath "@executable_path/lib" "$DIST/twinsanityevolved"
  for f in "$DIST"/lib/* "$DIST/twinsanityevolved" "$DIST/twinsanityevolved-modtool"; do [ -e "$f" ] && codesign --force --sign - "$f" >/dev/null 2>&1 || true; done
}
bundle_linux() {
  local queue=("$DIST/twinsanityevolved")
  while [ ${#queue[@]} -gt 0 ]; do
    local f="${queue[0]}"; queue=("${queue[@]:1}")
    local dep
    while read -r dep; do
      [ -n "$dep" ] && [ -f "$dep" ] || continue
      is_system_lib "$dep" && continue
      local name; name="$(basename "$dep")"
      if [ ! -f "$DIST/lib/$name" ]; then
        cp -L "$dep" "$DIST/lib/$name"; chmod u+w "$DIST/lib/$name"
        queue+=("$DIST/lib/$name")
      fi
    done < <(ldd "$f" | awk '/=>/ {print $3}')
  done
  [ -n "$(ls -A "$DIST/lib" 2>/dev/null)" ] || return 0
  if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/lib' "$DIST/twinsanityevolved"
    for l in "$DIST"/lib/*; do patchelf --set-rpath '$ORIGIN' "$l" || true; done
  else
    echo "release_dist: patchelf not found -- run with LD_LIBRARY_PATH=$DIST/lib or install patchelf" >&2
  fi
}
case "$PLATFORM" in
  macos-*) bundle_macos ;;
  linux-*) bundle_linux ;;
esac
rmdir "$DIST/lib" 2>/dev/null || true
echo "lib/: $(ls -A "$DIST/lib" 2>/dev/null | tr '\n' ' ')"

# --- the FFmpeg that was LINKED must be an LGPL build (the engine's rule): read from the object, never the recipe ---
FFMPEG_GPL_RE='--enable-gpl|--enable-nonfree|--enable-libx26|--enable-version3'
ffmpeg_embedded_configuration() {
  local confs n
  confs="$(strings "$1" | grep -- '--prefix=' | sort -u || true)"
  n="$(printf '%s' "$confs" | grep -c . || true)"
  if [ "$n" -ne 1 ]; then
    echo "release_dist: REFUSING to ship -- $(basename "$1") embeds $n FFmpeg configuration strings (--prefix=), expected exactly 1" >&2
    printf '%s\n' "$confs" | sed 's/^/  /' >&2
    return 1
  fi
  printf '%s' "$confs"
}
FFMPEG_DYN=""
for l in "$DIST"/lib/libavcodec*; do [ -f "$l" ] && FFMPEG_DYN="$l"; done
if [ -n "$FFMPEG_DYN" ]; then FFMPEG_OBJ="$FFMPEG_DYN"; FFMPEG_FROM="$(basename "$FFMPEG_DYN") (bundled in lib/)"; FFMPEG_LINK=dynamic
else FFMPEG_OBJ="$DIST/twinsanityevolved"; FFMPEG_FROM="twinsanityevolved (linked statically)"; FFMPEG_LINK=static; fi
FFMPEG_CONF="$(ffmpeg_embedded_configuration "$FFMPEG_OBJ")" || { rm -rf "$DIST"; exit 1; }
if printf '%s' "$FFMPEG_CONF" | grep -qE -- "$FFMPEG_GPL_RE"; then
  echo "release_dist: REFUSING to ship -- the FFmpeg linked into $FFMPEG_FROM is a GPL/non-free/LGPLv3 build:" >&2
  echo "  $FFMPEG_CONF" >&2
  echo "  build without --system-deps (static FFmpeg from engine/tools/ci/ffmpeg_static.sh, --disable-gpl)" >&2
  rm -rf "$DIST"; exit 1
fi
if [ "$FFMPEG_LINK" = static ]; then
  LINKED_ARGS="$(printf '%s\n' "$FFMPEG_CONF" | tr ' ' '\n' | grep -v -e '^--prefix=' -e '^--disable-x86asm$' -e '^$')"
  RECIPE_ARGS="$("$ENGINE/tools/ci/ffmpeg_static.sh" --print-args)"
  if [ "$LINKED_ARGS" != "$RECIPE_ARGS" ]; then
    echo "release_dist: REFUSING to ship -- the linked FFmpeg was not configured with the engine's recipe:" >&2
    diff <(printf '%s\n' "$RECIPE_ARGS") <(printf '%s\n' "$LINKED_ARGS") | sed 's/^/  /' >&2 || true
    rm -rf "$DIST"; exit 1
  fi
fi
echo "ffmpeg: $FFMPEG_LINK, LGPL ($FFMPEG_FROM)"

# --- licence texts + notices (the engine's committed set; the port adds its own MIT) ---
LIC_SRC="$ENGINE/tools/dist/licenses"
for f in SDL2-LICENSE.txt FFmpeg-COPYING.LGPLv2.1.txt; do cp "$LIC_SRC/$f" "$DIST/licenses/$f"; done
cp "$ROOT/FFmpeg-LICENSE.md" "$DIST/licenses/FFmpeg-LICENSE.md"
case "$PLATFORM" in linux-*) for f in GCC-COPYING3.txt GCC-RUNTIME-LIBRARY-EXCEPTION.txt; do cp "$LIC_SRC/$f" "$DIST/licenses/$f"; done ;; esac
{
  echo "FFmpeg as linked into $FFMPEG_FROM."
  echo
  echo "libavcodec's configuration string embedded in $(basename "$FFMPEG_OBJ") (avcodec_configuration(); read with: strings <file> | grep -- --prefix=):"
  echo "$FFMPEG_CONF"
  if [ "$FFMPEG_LINK" = static ]; then echo; echo "It equals the engine's recipe (engine/tools/ci/ffmpeg_static.sh --print-configure):"; "$ENGINE/tools/ci/ffmpeg_static.sh" --print-configure; fi
} > "$DIST/licenses/FFmpeg-BUILD-CONFIGURATION.txt"
SDL2_TAG="$(grep -o 'release-[0-9.]*' "$ENGINE/cmake/Sdl2Dependency.cmake" | head -1)"
{
  echo "# TwinsanityEvolved -- third-party notices -- $(head -1 "$DIST/BUILD-ID")"
  echo
  echo "This folder ships the game program (the twinsanity-reversed decompilation compiled to wasm32 and lowered to"
  echo "native code) and the NativeEE engine. It"
  echo "contains NO game data: no textures, audio, models, movies or disc bytes -- you supply your own disc image."
  echo
  echo "| component | how it is linked | licence | text |"
  echo "|---|---|---|---|"
  echo "| TwinsanityEvolved (this port's code) | the binary | MIT | LICENSE |"
  echo "| twinsanity-reversed (the decompilation, game/) | compiled into the binary | twinsanity-reversed | https://github.com/Smartkin/twinsanity-reversed |"
  echo "| NativeEE (engine/) | static | MIT | the engine repository's LICENSE |"
  echo "| SDL2 ${SDL2_TAG:-} | $([ -n "$(ls "$DIST"/lib/libSDL2* 2>/dev/null)" ] && echo dynamic, lib/ || echo static) | zlib | licenses/SDL2-LICENSE.txt |"
  echo "| FFmpeg (libavcodec, libavutil, libswscale) | $FFMPEG_LINK | LGPL v2.1+ (no GPL / non-free component) | licenses/FFmpeg-LICENSE.md, licenses/FFmpeg-COPYING.LGPLv2.1.txt, licenses/FFmpeg-BUILD-CONFIGURATION.txt |"
  case "$PLATFORM" in linux-*) echo "| libstdc++ / libgcc | static | GPLv3 with the GCC Runtime Library Exception | licenses/GCC-COPYING3.txt, licenses/GCC-RUNTIME-LIBRARY-EXCEPTION.txt |" ;; esac
} > "$DIST/THIRD_PARTY_NOTICES.md"

step "asset scan over the staged folder (zero findings)"
python3 tools/ci/asset_scan.py "$DIST"

if [ -n "$ISO" ]; then
  step "dry run (the disc read at run time only)"
  "$DIST/twinsanityevolved" --iso "$ISO" --dry-run
fi

if [ "$ZIP" = 1 ]; then
  step "zip"
  ZIPNAME="twinsanityevolved-$PLATFORM"
  rm -f "$DIST_ROOT/$ZIPNAME.zip" "$DIST_ROOT/$ZIPNAME.zip.sha256"
  (cd "$DIST_ROOT" && rm -rf "$ZIPNAME" && cp -R "$PLATFORM" "$ZIPNAME" && zip -qr -y "$ZIPNAME.zip" "$ZIPNAME" && rm -rf "$ZIPNAME")
  if command -v shasum >/dev/null 2>&1; then (cd "$DIST_ROOT" && shasum -a 256 "$ZIPNAME.zip" > "$ZIPNAME.zip.sha256"); else (cd "$DIST_ROOT" && sha256sum "$ZIPNAME.zip" > "$ZIPNAME.zip.sha256"); fi
  cat "$DIST_ROOT/$ZIPNAME.zip.sha256"
fi
echo; echo "release_dist: OK -> $DIST"
