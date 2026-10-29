#!/usr/bin/env bash
# tools/ci/vita_syntax.sh -- the game code (game/src minus src/platform/ps2) parses for armv7 with the vitasdk GCC
# (a CI gate: nothing done now may make another 32-bit target harder). Syntax-only: GCC 15 accepts the decomp's
# C++26 as written (consteval asm thunks included); the PS2SDK headers come from platform/nativeee/sdk_shim.
#
#   tools/ci/vita_syntax.sh [VITASDK=/usr/local/vitasdk] [-jN]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VITASDK="${VITASDK:-/usr/local/vitasdk}"
JOBS=4
for a in "$@"; do case "$a" in -j*) JOBS="${a#-j}" ;; *) VITASDK="$a" ;; esac; done
GXX="$VITASDK/bin/arm-vita-eabi-g++"
[ -x "$GXX" ] || { echo "vita_syntax: $GXX not found (install vitasdk or set VITASDK)"; exit 2; }
cd "$ROOT"
FLAGS=(-fsyntax-only -std=gnu++26 -D_EE -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-strict-aliasing
       -Wall -Wno-invalid-offsetof -Wno-psabi -Igame/include -Iplatform/nativeee/sdk_shim -Iplatform/nativeee/ps2e)
FILES=( game/src/game/*.cpp game/src/*.cpp game/src/platform/ps2/libc.cpp game/src/platform/ps2/libm.cpp game/src/platform/ps2/malloc.cpp )   # bash 3 (macOS): no mapfile
LOG="$(mktemp -t vita_syntax.XXXXXX)"
printf '%s\n' "${FILES[@]}" | xargs -P "$JOBS" -I{} sh -c '"$0" "$@" "{}" > "{}.vita.log" 2>&1 || echo "FAIL {}"' "$GXX" "${FLAGS[@]}" > "$LOG" || true
fails=$(grep -c '^FAIL' "$LOG" || true)
for f in "${FILES[@]}"; do
  if grep -q "error" "$f.vita.log" 2>/dev/null; then echo "== $f"; grep -m3 "error" "$f.vita.log"; fi
  rm -f "$f.vita.log"
done
rm -f "$LOG"
echo "vita_syntax: ${#FILES[@]} TUs, $fails failed (armv7 vitasdk, $("$GXX" -dumpversion))"
[ "$fails" -eq 0 ]
