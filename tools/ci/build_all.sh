#!/usr/bin/env bash
# tools/ci/build_all.sh -- the local CI: toolchain present, configure + build, module contract, the generated
# snapshots the tree commits (tools/compile_report.md, tools/seam_imports.txt, platform/nativeee/ps2e/ps2e_seam.h)
# identical to what the build produced, the dry run against a disc image when one is given, asset scan, the no-codenames
# lint, vita lane.
#
#   tools/ci/build_all.sh [--iso <disc image>] [--no-vita] [--update-snapshots] [--build-dir DIR]
#   env: TE_MIN_FREE_GB (disk floor), TE_CI_COVERAGE=1 (level_entries + make_states --verify: opt-in -- play-testing
#        covers the levels), TE_CI_LONG_WALKS=0 (skip padlog_smoke's jungle / mecha walks for a quick pass; default on).
#
# Exit non-zero on the first failing gate. Game data is read at run time only; nothing is written from it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ISO=""; VITA=1; UPDATE=0; BUILD="$ROOT/build"
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --no-vita) VITA=0; shift ;;
    --update-snapshots) UPDATE=1; shift ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    *) echo "build_all: unknown argument $1"; exit 2 ;;
  esac
done

step() { echo; echo "=== $*"; }

step "disk"
free_gb=$(df -g "$ROOT" | awk 'NR==2{print $4}')
# the default floor is 15 GB; TE_MIN_FREE_GB lowers it for a run on a disk whose remaining space is not ours to prune
# (records the value used)
min_free_gb="${TE_MIN_FREE_GB:-15}"
echo "free: ${free_gb} GB (floor ${min_free_gb} GB)"
if [ "$free_gb" -lt "$min_free_gb" ]; then echo "build_all: < ${min_free_gb} GB free -- prune reproducible build dirs first"; exit 3; fi

step "one game process at a time"
if pgrep -f '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; then
  pgrep -fl '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)'
  echo "build_all: a game process is running; refusing to start another (one at a time)"; exit 3
fi
echo "none running"


step "toolchain (pinned, sha256)"
tools/fetch_toolchain.sh

step "configure"
cmake -S "$ROOT" -B "$BUILD" -G Ninja > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
mv "$BUILD.configure.log" "$BUILD/configure.log"

step "build (game wasm, wasm2c, engine libs, runner)"
cmake --build "$BUILD" > "$BUILD/build.log" 2>&1 || { grep -n "error\|FAILED" "$BUILD/build.log" | head -40; exit 1; }
# (an up-to-date build prints neither line: re-run the contract check and the report explicitly)
"$(command -v python3)" tools/check_module.py --wasm-objdump tools/toolchain/wabt/bin/wasm-objdump --wasm "$BUILD/wasm/game.wasm" \
    --imports "$BUILD/gen/seam/seam_imports.txt" --image-start 2989456 --image-end 4043264 --report "$BUILD/wasm/module_report.json"
grep "compile_report:" "$BUILD/build.log" | tail -1 || echo "compile_report: up to date ($BUILD/compile_report.md)"

step "snapshots committed in the tree == generated"
check_snapshot() { # generated committed
  if ! cmp -s "$1" "$2"; then
    if [ "$UPDATE" = 1 ]; then cp "$1" "$2"; echo "updated $2"; else echo "build_all: $2 differs from the build's $1 (run with --update-snapshots)"; diff "$2" "$1" | head -20; return 1; fi
  else echo "ok $2"; fi
}
check_snapshot "$BUILD/gen/seam/seam_imports.txt" tools/seam_imports.txt
check_snapshot "$BUILD/gen/seam/ps2e_seam.h" platform/nativeee/ps2e/ps2e_seam.h
# the decomp's vtable slot signatures (libclang over every TU, ~50 s) must match the committed snapshot the thunks
# and the patcher's slot table are generated from
cmake --build "$BUILD" --target te_vtable_slots > "$BUILD/vtable_slots.log" 2>&1 || { tail -20 "$BUILD/vtable_slots.log"; exit 1; }
grep "gen_vtable_slots:" "$BUILD/vtable_slots.log" | tail -1 || true
check_snapshot "$BUILD/vtable_slots.tsv" tools/vtable_slots.tsv
grep "w2c_sigfix:" "$BUILD/build.log" | tail -1 || echo "w2c_sigfix: up to date ($BUILD/w2c/sigfix.json)"

step "platform/nativeee unit tests (one ctest per header)"
ctest --test-dir "$BUILD" -L nativeee --output-on-failure

step "oracle diff tool: alignment self-test; the perceptual descriptor pinned to the harness's C++ (test_harness)"
python3 tools/oracle/diff.py --selftest

if [ -n "$ISO" ]; then
  step "dry run (retail image from the disc at run time, code pointers patched)"
  "$BUILD/twinsanityevolved" --iso "$ISO" --dry-run
  step "boot (deterministic, 20 s watchdog): the services come up, the frame loop paces, the title is reached -- exit 0 or a known (non-call_indirect) trap"
  set +e
  rm -rf "$BUILD/boot_frames"; mkdir -p "$BUILD/boot_frames"
  TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$BUILD/boot_frames" TE_GFX_DUMP_AT=400 \
    "$BUILD/twinsanityevolved" --iso "$ISO" --deterministic --no-sdl-input --trace-state --run-seconds 20 > "$BUILD/boot.log" 2>&1
  rc=$?
  set -e
  grep -E "system: disc|services started|clock:|trap|elapsed" "$BUILD/boot.log" | grep -v "LoadDriver" || true
  boundaries=$(grep -o "clock: [0-9]* boundaries" "$BUILD/boot.log" | grep -o "[0-9]*" | head -1)
  if [ "${boundaries:-0}" -lt 60 ]; then echo "build_all: the boot reached only ${boundaries:-0} boundaries (< 60: the services did not carry the game to its frame loop)"; exit 1; fi
  if [ "$rc" != 0 ] && [ "$rc" != 70 ]; then echo "build_all: boot exit $rc"; exit 1; fi
  # Gate: no call_indirect trap (the slot thunks + the adapters), the game reaches the Title state
  if grep -q "Invalid call_indirect" "$BUILD/boot.log"; then echo "build_all: a call_indirect trap "; exit 1; fi
  grep -E "vtable thunks:|call_indirect adapted:" "$BUILD/boot.log" || true
  if ! grep -q "game state @ boundary [0-9]*: Title (7)" "$BUILD/boot.log"; then echo "build_all: the boot did not reach GameController StateTitle"; exit 1; fi
  grep "game state @ boundary [0-9]*: Title (7)" "$BUILD/boot.log" | head -1
  # Save-state gate + the hub pad log: title -> new game (the save file created) -> the hub, a state saved in the hub and loaded
  # in a new process continues identically (state trace, frames, census)
  step "save states + fixtures/padlogs/hub_entry.padlog: save at 2380, load, compare to 2600"
  tools/ci/savestate_roundtrip.sh --iso "$ISO" --build-dir "$BUILD"
  # Movie gate: save states taken INSIDE a movie (the skipped intro FMV on the hub pad log; the TT ident FMV
  # playing) load and continue identically -- the movie player's section, no host pointer across the checkpoint
  step "save states inside a movie: tools/ci/savestate_movie_roundtrip.sh"
  tools/ci/savestate_movie_roundtrip.sh --iso "$ISO" --build-dir "$BUILD"
  # Hub gate (the frozen judge fixtures/judges/hub_entry_pal.json): the hub pad log replays to its end (4200
  # boundaries, Playing held, no trap) and the game's small pages are not exhausted (the frame-memory leak)
  step "pad log to its end: fixtures/padlogs/hub_entry.padlog to 4200, heap census, padlog_judge"
  rm -rf "$BUILD/padlog_mc"; mkdir -p "$BUILD/padlog_mc"
  set +e
  TE_GFX_HEADLESS=1 "$BUILD/twinsanityevolved" --iso "$ISO" --deterministic --no-sdl-input --no-audio --trace-state --skip-movies \
    --play-pad fixtures/padlogs/hub_entry.padlog --mc-dir "$BUILD/padlog_mc" --stop-at 4200 --run-seconds 240 > "$BUILD/padlog.log" 2>&1
  echo "padlog run exit $?"
  set -e
  grep -E "clock:|heap:" "$BUILD/padlog.log" || true
  python3 tools/judges/padlog_judge.py "$BUILD/padlog.log" --fixture fixtures/judges/hub_entry_pal.json
  # Replay gate: EVERY level pad log under fixtures/padlogs/ replays to its end without a trap
  # (the hub, the load-game flow on the hub's card, the quit flow from the 1925 state, Jungle Bungle into hubb)
  # (TE_CI_LONG_WALKS=0 skips the jungle / mecha walks for a quick pass; the short logs always; all ten by default)
  step "every pad log to its end: tools/ci/padlog_smoke.sh (TE_CI_LONG_WALKS=${TE_CI_LONG_WALKS:-1})"
  tools/ci/padlog_smoke.sh --iso "$ISO" --build-dir "$BUILD"
  # Coverage (2026-10-17: the pad-log coverage work is stopped; play-testing reports the bugs): the COVERAGE gates --
  # every level's first chunk on the warp (level_entries, 21 runs) and the level-entry states regenerated + verified from the
  # long logs (make_states --verify, regenerated from metadata) -- are opt-in with TE_CI_COVERAGE=1. The fixtures stay in the tree; a
  # reported level bug re-runs its row by hand.
  if [ "${TE_CI_COVERAGE:-0}" = 1 ]; then
    step "every level loads (fixtures/padlogs/entries/*.padlog on the warp): tools/ci/level_entries.sh"
    tools/ci/level_entries.sh --iso "$ISO" --build-dir "$BUILD"

    step "level-entry save states (fixtures/states/*.meta): tools/make_states.sh --verify"
    tools/make_states.sh --iso "$ISO" --build-dir "$BUILD" --verify
  else
    step "level_entries + make_states --verify: SKIPPED (coverage gates are opt-in: TE_CI_COVERAGE=1)"
  fi
  # Movie gate (the frozen judge fixtures/judges/movie_boot_pal.json): the boot movies play through the Player --
  # decoded = queued = uploaded = presented, drawn, the sound track popped with no silence; frame 400 is the Vivendi logo
  step "movies (deterministic boot): the boot movies play through the game's Player"
  python3 tools/judges/movie_judge.py "$BUILD/boot.log" --fixture fixtures/judges/movie_boot_pal.json --dir "$BUILD/boot_frames"
  # audio health of the same boot: one tick per boundary, nothing dropped (the device is open in the deterministic boot)
  step "audio (deterministic boot): ticks == boundaries, dropped=0 underruns=0"
  grep -E "audio: device=" "$BUILD/boot.log"
  if ! grep -qE "audio: device=1 hz=60 ticks=${boundaries} .* dropped=0 underruns=0" "$BUILD/boot.log"; then echo "build_all: the audio census is not ticks=boundaries dropped=0 underruns=0"; exit 1; fi
fi

step "asset scan (zero findings)"
python3 tools/ci/asset_scan.py

step "no developer vocabulary where a player reads (tools/ci/no_codenames_lint.py: the launcher's strings, title.json, the README's user sections)"
python3 tools/ci/no_codenames_lint.py

if [ "$VITA" = 1 ]; then
  step "armv7 vitasdk syntax lane"
  tools/ci/vita_syntax.sh -j8
fi

echo; echo "build_all: ALL GATES PASSED"
