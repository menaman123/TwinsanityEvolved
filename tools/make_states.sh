#!/usr/bin/env bash
# tools/make_states.sh -- regenerate the level-entry save states from their METADATA (fixtures/states/*.meta).
#
#   tools/make_states.sh --iso <disc image> [--build-dir DIR] [--out DIR] [--only NAME] [--verify]
#
# For every fixtures/states/NAME.meta: the pad log it names is replayed (--play-pad, --deterministic, --skip-movies, an empty
# card) with --save-state OUT/NAME.ps2st@BOUNDARY and --stop-at BOUNDARY+60; the run must log the state's chunk entered at
# chunk_entered_at (the probe) and the save. The states are bound to the build that wrote them and are never committed;
# OUT defaults to <build dir>/states. --verify loads each written state with the same pad log to BOUNDARY+60 and requires the
# GameController trace and the probe's character positions after BOUNDARY to be identical to the straight run's (a
# loaded game IS the saved one). Exit non-zero on the first state that cannot be written or verified.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ISO=""; BUILD="$ROOT/build"; OUT=""; ONLY=""; VERIFY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    --verify) VERIFY=1; shift ;;
    *) echo "make_states: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "make_states: --iso required"; exit 2; }
BIN="$BUILD/twinsanityevolved"
[ -x "$BIN" ] || { echo "make_states: no runner at $BIN"; exit 2; }
OUT="${OUT:-$BUILD/states}"
mkdir -p "$OUT"
if pgrep -f "build/twinsanityevolved --iso|ps2recomp_release|ps2recomp_dist|app/host/ps2recomp" >/dev/null; then
  echo "make_states: another game process is running (one at a time)"; exit 3
fi
meta_value() { sed -n "s/^$2=//p" "$1" | head -1; }
status=0
for meta in fixtures/states/*.meta; do
  name="$(meta_value "$meta" name)"
  [ -z "$ONLY" ] || [ "$ONLY" = "$name" ] || continue
  padlog="$(meta_value "$meta" padlog)"; boundary="$(meta_value "$meta" boundary)"
  chunk="$(meta_value "$meta" chunk)"; entered="$(meta_value "$meta" chunk_entered_at)"
  [ -f "$padlog" ] || { echo "make_states: $name: pad log $padlog missing"; status=1; continue; }
  mc="$(mktemp -d "${TMPDIR:-/tmp}/make_states_mc.XXXXXX")"
  card="$(meta_value "$meta" card)"
  # card=warp:<chunk>: the hub card (written by hub_entry.padlog) with its save's chunk rewritten (tools/warp_save.py)
  if [ "${card#warp:}" != "$card" ]; then
    hub="$OUT/mc_hub_for_warp"
    if [ ! -f "$hub/mc0/BESLES-52568/Bank0.bin" ]; then
      rm -rf "$hub"; mkdir -p "$hub"
      TE_GFX_NO_DRAW=1 "$BIN" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state \
        --play-pad fixtures/padlogs/hub_entry.padlog --mc-dir "$hub" --stop-at 2700 > "$OUT/mc_hub_for_warp.log" 2>&1 || { echo "make_states: the hub card for the warp failed"; status=1; continue; }
    fi
    python3 tools/warp_save.py --card "$hub" --chunk "${card#warp:}" --out "$mc" --place 0xFFFF > /dev/null
  fi
  log="$OUT/$name.log"
  echo "make_states: $name <- $padlog @ $boundary${card:+ (card $card)}"
  if ! TE_GFX_NO_DRAW=1 "$BIN" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state --trace-player 0 \
      --play-pad "$padlog" --mc-dir "$mc" --save-state "$OUT/$name.ps2st@$boundary" --stop-at "$((boundary + 60))" > "$log" 2>&1; then
    echo "make_states: $name: the run failed (exit $?), see $log"; status=1; rm -rf "$mc"; continue
  fi
  rm -rf "$mc"
  if ! grep -q "savestate: saved .*/$name.ps2st at boundary $boundary " "$log"; then
    echo "make_states: $name: no save at boundary $boundary (see $log)"; status=1; continue
  fi
  if [ -n "$entered" ] && ! grep -qF "@$entered chunk" "$log"; then
    echo "make_states: $name: the chunk was not entered at $entered (see $log)"; status=1; continue
  fi
  if [ -n "$chunk" ] && ! grep -F "@$entered chunk" "$log" | grep -qF "'$chunk'"; then
    echo "make_states: $name: the chunk entered at $entered is not $chunk (see $log)"; status=1; continue
  fi
  ls -la "$OUT/$name.ps2st" | awk '{print "make_states: written", $NF, $5, "bytes"}'
  if [ "$VERIFY" = 1 ]; then
    mc="$(mktemp -d "${TMPDIR:-/tmp}/make_states_mc.XXXXXX")"
    [ "${card#warp:}" = "$card" ] || python3 tools/warp_save.py --card "$OUT/mc_hub_for_warp" --chunk "${card#warp:}" --out "$mc" --place 0xFFFF > /dev/null
    vlog="$OUT/$name.verify.log"
    if ! TE_GFX_NO_DRAW=1 "$BIN" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state --trace-player 1 \
        --play-pad "$padlog" --mc-dir "$mc" --load-state "$OUT/$name.ps2st" --stop-at "$((boundary + 60))" > "$vlog" 2>&1; then
      echo "make_states: $name: the loaded run failed (see $vlog)"; status=1; rm -rf "$mc"; continue
    fi
    rm -rf "$mc"
    # +1: the loaded run logs its current state once at the load boundary (as savestate_roundtrip.sh)
    after() { awk -v b="$boundary" '/game state @ boundary/ { n = $0; sub(/.*@ boundary /, "", n); sub(/:.*/, "", n); if (n + 0 > b + 1) print }' "$1"; }
    # the straight run was traced every 0 boundaries (chunk changes only); re-trace it for the positions
    slog="$OUT/$name.straight.log"
    mc="$(mktemp -d "${TMPDIR:-/tmp}/make_states_mc.XXXXXX")"
    [ "${card#warp:}" = "$card" ] || python3 tools/warp_save.py --card "$OUT/mc_hub_for_warp" --chunk "${card#warp:}" --out "$mc" --place 0xFFFF > /dev/null
    TE_GFX_NO_DRAW=1 "$BIN" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state --trace-player 1 \
        --play-pad "$padlog" --mc-dir "$mc" --stop-at "$((boundary + 60))" > "$slog" 2>&1 || true
    rm -rf "$mc"
    # the probe samples every 10 boundaries from its first sample, so the two runs' phases may differ: compare the boundaries both logged
    pos() { awk -v b="$boundary" '/\[probe\] @[0-9]+ pos/ { n = $2; sub(/@/, "", n); if (n + 0 > b) print $2, $4, $5, $6 }' "$1" | sort; }
    if ! diff <(after "$slog") <(after "$vlog") >/dev/null; then echo "make_states: $name: VERIFY FAILED, the state trace differs after $boundary"; status=1; continue; fi
    common="$(join <(pos "$slog") <(pos "$vlog") | awk '$2 != $5 || $3 != $6 || $4 != $7 { bad++ } END { print NR, bad + 0 }')"
    if [ "${common#* }" != 0 ] || [ "${common% *}" = 0 ]; then echo "make_states: $name: VERIFY FAILED, the character's positions differ after $boundary ($common)"; status=1; continue; fi
    echo "make_states: $name: verified (${common% *} positions, $(after "$vlog" | wc -l | tr -d ' ') state transitions identical after $boundary)"
  fi
done
exit $status
