#!/bin/bash
# tools/ci/level_entries.sh -- every level LOADS (assume-playable coverage, S-UV): for each entry pad log in
# fixtures/padlogs/entries/<area>_<level>_<chunk>.padlog the hub card is warped to that chunk (tools/warp_save.py: the save's
# chunk rewritten, LOAD GAME starts there) and the log replays to 4200: exit 0, no trap, the chunk entered, Playing logged, no
# GameOver. The per-level pad logs are the same LOAD GAME presses recorded by the autopilot (the auto-save dialog's dismissal
# lands where that level's load ends). A level whose load traps is a STAGE, listed in LEVEL_ENTRY_BLOCKED
# below so the gate stays green while the block is open.
#   tools/ci/level_entries.sh --iso <disc image> [--build-dir DIR] [--runner BIN] [--out DIR]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ISO=""; BUILD="$ROOT/build"; RUNNER=""; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --runner) RUNNER="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "level_entries: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "level_entries: --iso required"; exit 2; }
RUNNER="${RUNNER:-$BUILD/twinsanityevolved}"
[ -x "$RUNNER" ] || { echo "level_entries: no runner at $RUNNER"; exit 2; }
OUT="${OUT:-$BUILD/level_entries}"
rm -rf "$OUT"; mkdir -p "$OUT/mc_hub"
if pgrep -f '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; then
  echo "level_entries: a game process is running (one at a time)"; exit 3
fi
# Known load blocks (a STAGE each): the gate reports them as BLOCKED and does not fail on them
LEVEL_ENTRY_BLOCKED=""
# the hub card the warp derives from (the hub_entry pad log's new game, as padlog_smoke does)
TE_GFX_HEADLESS=1 TE_GFX_NO_DRAW=1 "$RUNNER" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state \
  --play-pad fixtures/padlogs/hub_entry.padlog --mc-dir "$OUT/mc_hub" --stop-at 2700 > "$OUT/mc_hub.log" 2>&1 || { echo "level_entries: the hub card failed"; exit 1; }
status=0
for padlog in fixtures/padlogs/entries/*.padlog; do
  name="$(basename "$padlog" .padlog)"
  # <area>_<level>_<chunk> -> levels\<area>\<level>\<chunk> (an area/level/chunk never carries an underscore except sch_hub / boiler_N / altdoc_b...: the pad log's header names the chunk)
  chunk="$(sed -n 's/^# chunk=//p' "$padlog" | head -1)"
  [ -n "$chunk" ] || { echo "level_entries: $name: no '# chunk=' header"; status=1; continue; }
  # '# character=crash': the save's summary names the character played (a chunk whose start script does not set the
  # player mode -- escape, entered only from cortthro -- has no player at StartingPlay with the hub autosave's NoCharacter)
  character="$(sed -n 's/^# character=//p' "$padlog" | head -1)"
  mc="$OUT/mc_$name"
  python3 tools/warp_save.py --card "$OUT/mc_hub" --chunk "$chunk" --out "$mc" --place 0xFFFF ${character:+--character "$character"} > /dev/null
  run="$OUT/$name.log"
  set +e
  TE_GFX_HEADLESS=1 TE_GFX_NO_DRAW=1 "$RUNNER" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state --trace-player 0 \
    --play-pad "$padlog" --mc-dir "$mc" --stop-at 4200 --run-seconds 600 > "$run" 2>&1
  rc=$?
  set -e
  rm -rf "$mc"
  traps=$(grep -v "adapted at the call site; 0 traps" "$run" | grep -v "^\[probe\]" | grep -c -iE "wasm trap|trap:|Invalid call_indirect|SIGBUS|SIGSEGV|abort" || true)
  # entered: the player's chunk is this one, or (a chunk with no character of its own: escape) it is LOADED as a link of the
  # chunk the player stands in (the probe's "link N -> <chunk> (flags ..., linked 0x<non-zero>)")
  entered=$(grep -cF -- "'$chunk'" "$run" || true)
  if [ "$entered" = 0 ]; then entered=$(grep -F -- "-> $chunk (flags" "$run" | grep -c -E "linked 0x0*[1-9A-Fa-f]" || true); fi
  playing=$(grep -c "Playing (12) <-" "$run" || true)
  over=$(grep -c "GameOver" "$run" || true)
  last=$(grep -oE "game state @ boundary [0-9]+: [A-Za-z]+ \([0-9]+\)" "$run" | tail -1)
  verdict=OK
  [ "$rc" = 0 ] && [ "$traps" = 0 ] && [ "$entered" != 0 ] && [ "$playing" != 0 ] && [ "$over" = 0 ] || verdict=FAIL
  if [ "$verdict" = FAIL ] && [[ " $LEVEL_ENTRY_BLOCKED " == *" $name "* ]]; then verdict="BLOCKED (a filed stage)"; fi
  echo "level_entries: $name ($chunk): exit $rc, traps $traps, entered $entered, Playing $playing, GameOver $over, last [$last] -> $verdict"
  [ "$verdict" = FAIL ] && status=1
done
[ "$status" = 0 ] && echo "level_entries: EVERY LEVEL LOADS (blocked ones listed)" || echo "level_entries: FAILED"
exit $status
