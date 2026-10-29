#!/usr/bin/env bash
# tools/vita/ffmpeg_vita.sh -- the Vita lane's FFmpeg: the engine's static LGPL recipe (engine/tools/ci/ffmpeg_static.sh:
# the mpeg1/mpeg2 video decoders, the mpegvideo parser and swscale, nothing else -- what the movies' decoder
# engine/engine/ipu/hle/ffmpeg_video_decoder.cpp calls) cross-compiled with the VitaSDK's arm-vita-eabi GCC.
# The VitaSDK's own FFmpeg package is configured for audio / MPEG-4 players (--enable-decoder=aac,...,mpeg4,...): it has
# no MPEG-2 decoder and no mpegvideo parser, so on the Vita every movie opened "0x0 @ 0.000 fps" and ended at once.
# CMakeLists.txt runs this under if(TE_VITA) (the te_ffmpeg_vita target, before the engine's configure) into
# <build>/third_party/ffmpeg and links those archives instead of the package's.
#
#   tools/vita/ffmpeg_vita.sh <prefix>        build into <prefix>; CACHED only when <prefix>/lib/{libavcodec,libavutil,
#                                             libswscale}.a exist AND <prefix>/PS2R_FFMPEG_CONFIGURE (the version, the
#                                             sha256 and every configure argument) equals what this script would run
#   tools/vita/ffmpeg_vita.sh --print-args    the configure arguments one per line (no --prefix), nothing else
#   FFMPEG_SOURCE=<file>                      a local copy of the pinned release tarball (checked against the sha256)
#                                             instead of the download (offline builds)
#   FFMPEG_JOBS=<n>                           make jobs (default: the host's CPU count)
#   VITASDK                                   the SDK root (default /usr/local/vitasdk)
#
# The source: the engine recipe's release (its tag n6.1.2 at git.ffmpeg.org) as the release tarball, pinned by sha256
# (ffmpeg-6.1.2.tar.xz = the tag's tree + a VERSION file). The script refuses to run when the engine recipe builds
# another tag: the two move together.
#
# The configure arguments: the recipe's --print-args list verbatim except --enable-pic (the eboot is one static non-PIC
# image), then the cross-compilation arguments the VitaSDK's own package is built with (vitasdk/packages ffmpeg/VITABUILD):
# the cross prefix, armv7-a / cortex-a9 (NEON and VFPv3; Thumb-2, the compiler's default), no OS, no run-time CPU
# detection, the armv5te / armv6t2 code off, and its --extra-cflags without -ffast-math and the optimisation flags
# (FFmpeg's own -O3 applies, as in the recipe).
#
# Licence: as the recipe -- LGPL v2.1 or later, no GPL / non-free / version-3 component (--disable-gpl --disable-nonfree
# --disable-version3). After the build the archive's embedded configuration string (avcodec_configuration(), the one
# tools/ci/release_dist.sh reads from a linked binary: strings <file> | grep -- --prefix=) is checked: exactly one, none of
# --enable-gpl / --enable-nonfree / --enable-version3, and the MPEG-2 decoder and the mpegvideo parser are in the archive.
# No path with spaces: FFmpeg's configure refuses an out-of-tree build from one.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RECIPE="$ROOT/engine/tools/ci/ffmpeg_static.sh"
VERSION=6.1.2
TAG=n6.1.2
URL="https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
SHA256=3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38
VITASDK="${VITASDK:-/usr/local/vitasdk}"

RECIPE_TAG="$(env -u FFMPEG_TAG bash "$RECIPE" --print-configure | awk 'NR == 1 {print $2}')"
if [ "$RECIPE_TAG" != "$TAG" ]; then
  echo "ffmpeg_vita: the engine recipe ($RECIPE) builds FFmpeg $RECIPE_TAG; this script pins $TAG ($URL," \
       "sha256 $SHA256): update VERSION / TAG / SHA256 here to the recipe's release" >&2
  exit 1
fi

# The ONE list of configure arguments: the recipe's, then the Vita's.
ARGS=()
while IFS= read -r a; do
  [ "$a" = "--enable-pic" ] && continue
  ARGS+=("$a")
done < <(env -u FFMPEG_TAG bash "$RECIPE" --print-args)
ARGS+=(
  --enable-cross-compile "--cross-prefix=$VITASDK/bin/arm-vita-eabi-" --arch=armv7-a --cpu=cortex-a9 --target-os=none
  --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2
  "--extra-cflags=-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wl,-q -D_BSD_SOURCE"
)

if [ "${1:-}" = "--print-args" ]; then
  printf '%s\n' "${ARGS[@]}"
  exit 0
fi
PREFIX="${1:?usage: ffmpeg_vita.sh <prefix> | --print-args}"
case "$PREFIX" in /*) ;; *) PREFIX="$(pwd)/$PREFIX" ;; esac
case "$PREFIX" in *" "*) echo "ffmpeg_vita: the prefix '$PREFIX' contains a space (FFmpeg's configure cannot build there)" >&2; exit 1 ;; esac
[ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || { echo "ffmpeg_vita: $VITASDK/bin/arm-vita-eabi-gcc not found (set VITASDK)" >&2; exit 1; }
if [ -n "${FFMPEG_JOBS:-}" ]; then JOBS="$FFMPEG_JOBS"
elif command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then JOBS="$(sysctl -n hw.ncpu)"
else JOBS=4; fi

RECORD="$({ echo "version=$VERSION"; echo "sha256=$SHA256"; printf '%s\n' "${ARGS[@]}"; })"
if [ -f "$PREFIX/lib/libavcodec.a" ] && [ -f "$PREFIX/lib/libswscale.a" ] && [ -f "$PREFIX/lib/libavutil.a" ] &&
   [ -f "$PREFIX/PS2R_FFMPEG_CONFIGURE" ] && [ "$(cat "$PREFIX/PS2R_FFMPEG_CONFIGURE")" = "$RECORD" ]; then
  echo "ffmpeg_vita: cached at $PREFIX (FFmpeg $VERSION; PS2R_FFMPEG_CONFIGURE == the recipe)"
  touch "$PREFIX/PS2R_FFMPEG_CONFIGURE"   # the build's stamp (CMakeLists.txt: the custom command's output)
  exit 0
fi
if [ -f "$PREFIX/PS2R_FFMPEG_CONFIGURE" ]; then
  echo "ffmpeg_vita: STALE prefix $PREFIX -- its PS2R_FFMPEG_CONFIGURE differs from the recipe; rebuilding:"
  diff <(cat "$PREFIX/PS2R_FFMPEG_CONFIGURE") <(printf '%s\n' "$RECORD") | sed 's/^/  /' || true
fi

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}
DOWNLOADS="$PREFIX.download"
if [ -n "${FFMPEG_SOURCE:-}" ]; then
  TARBALL="$FFMPEG_SOURCE"
  [ -f "$TARBALL" ] || { echo "ffmpeg_vita: FFMPEG_SOURCE=$TARBALL does not exist" >&2; exit 1; }
  got="$(sha256_of "$TARBALL")"
  [ "$got" = "$SHA256" ] || { echo "ffmpeg_vita: $TARBALL has sha256 $got, not the pinned $SHA256 (FFmpeg $VERSION)" >&2; exit 1; }
else
  TARBALL="$DOWNLOADS/ffmpeg-$VERSION.tar.xz"
  if [ ! -f "$TARBALL" ] || [ "$(sha256_of "$TARBALL")" != "$SHA256" ]; then
    mkdir -p "$DOWNLOADS"
    rm -f "$TARBALL" "$TARBALL.part"
    echo "ffmpeg_vita: downloading $URL"
    curl -fsSL --retry 3 -o "$TARBALL.part" "$URL"
    got="$(sha256_of "$TARBALL.part")"
    if [ "$got" != "$SHA256" ]; then
      rm -f "$TARBALL.part"
      echo "ffmpeg_vita: $URL has sha256 $got, not the pinned $SHA256" >&2
      exit 1
    fi
    mv "$TARBALL.part" "$TARBALL"
  fi
fi

SRC="$PREFIX.src"
BLD="$PREFIX.build"
rm -rf "$SRC" "$BLD" "$PREFIX"
mkdir -p "$SRC" "$BLD"
tar -xJf "$TARBALL" -C "$SRC" --strip-components=1
start=$(date +%s)
echo "ffmpeg_vita: configure (FFmpeg $VERSION: mpeg1/mpeg2 video decoders + mpegvideo parser + swscale, static, no GPL; arm-vita-eabi)"
(cd "$BLD" && "$SRC/configure" --prefix="$PREFIX" "${ARGS[@]}") > "$BLD/configure.log" 2>&1 ||
  { tail -n 30 "$BLD/configure.log" >&2; echo "ffmpeg_vita: configure FAILED (log $BLD/configure.log)" >&2; exit 1; }
echo "ffmpeg_vita: make -j$JOBS"
make -C "$BLD" -j"$JOBS" > "$BLD/make.log" 2>&1 ||
  { tail -n 40 "$BLD/make.log" >&2; echo "ffmpeg_vita: make FAILED (log $BLD/make.log)" >&2; exit 1; }
make -C "$BLD" install > "$BLD/install.log" 2>&1 ||
  { tail -n 20 "$BLD/install.log" >&2; echo "ffmpeg_vita: make install FAILED" >&2; exit 1; }

# The archive as installed: the licence and the components the movies' decoder needs, read from the object.
CONF="$(strings "$PREFIX/lib/libavcodec.a" | grep -- '--prefix=' | sort -u || true)"
n="$(grep -c . <<< "$CONF" || true)"
if [ "$n" -ne 1 ]; then
  echo "ffmpeg_vita: libavcodec.a embeds $n configuration strings (--prefix=), expected exactly 1" >&2; exit 1
fi
if grep -qE -- '--enable-gpl|--enable-nonfree|--enable-version3' <<< "$CONF"; then
  echo "ffmpeg_vita: the build is not LGPL v2.1: $CONF" >&2; exit 1
fi
SYMS="$("$VITASDK/bin/arm-vita-eabi-nm" -g --defined-only "$PREFIX/lib/libavcodec.a" 2>/dev/null || true)"
for sym in ff_mpeg2video_decoder ff_mpeg1video_decoder ff_mpegvideo_parser; do
  grep -q " $sym\$" <<< "$SYMS" || { echo "ffmpeg_vita: $sym is not in libavcodec.a" >&2; exit 1; }
done

echo "$TAG" > "$PREFIX/PS2R_FFMPEG_TAG"
mkdir -p "$PREFIX/share/licenses"
cp "$SRC/COPYING.LGPLv2.1" "$PREFIX/share/licenses/FFmpeg-COPYING.LGPLv2.1.txt"
cp "$SRC/LICENSE.md" "$PREFIX/share/licenses/FFmpeg-LICENSE.md"
printf '%s\n' "$CONF" > "$PREFIX/share/licenses/FFmpeg-BUILD-CONFIGURATION.txt"
rm -rf "$SRC" "$BLD"
printf '%s\n' "$RECORD" > "$PREFIX/PS2R_FFMPEG_CONFIGURE"   # last: a prefix without it is rebuilt
echo "ffmpeg_vita: installed FFmpeg $VERSION -> $PREFIX in $(( $(date +%s) - start )) s" \
     "($(cd "$PREFIX/lib" && ls *.a | tr '\n' ' '); LGPL v2.1+)"
