#!/usr/bin/env bash
# tools/ci/padlog_smoke.sh -- every level pad log under fixtures/padlogs/ replays to its END without a trap.
#
#   tools/ci/padlog_smoke.sh --iso <disc image> [--build-dir DIR] [--runner BIN] [--out DIR]
#
# The "end" of a pad log is the boundary its last sample's effect is judged at (the frozen judges' / the authors' stop
# boundaries, below); "without trap" is: the runner exits 0 at `--stop-at`, logs no trap / call_indirect / signal, and
# the GameController's last state is the one the log ends in. Prerequisites between logs are met in order: the hub log
# writes the card load_game reads and the 1925 state hub_quit starts from. Deterministic, --skip-movies, no draw
# (the content is the judges' business; this is the replay contract). The .oracle / .script variants are not port logs.
# Coverage (2026-10-17: the pad-log coverage work is stopped): every row runs by default (all ten replay in ~2 min
# headless); TE_CI_LONG_WALKS=0 skips the two LONG walks (end > 10000 boundaries: Jungle Bungle to the boss arena and the
# Mecha fight from its state) for a quick regression pass on the short logs. Exit non-zero on the first failing log;
# prints one quotable line per log.
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
    *) echo "padlog_smoke: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "padlog_smoke: --iso required"; exit 2; }
RUNNER="${RUNNER:-$BUILD/twinsanityevolved}"
[ -x "$RUNNER" ] || { echo "padlog_smoke: no runner at $RUNNER"; exit 2; }
OUT="${OUT:-$BUILD/padlog_smoke}"
rm -rf "$OUT"; mkdir -p "$OUT/mc_hub"
if pgrep -f '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; then
  echo "padlog_smoke: a game process is running (one at a time)"; exit 3
fi

# log : end boundary : last state (GameController name) : card (fresh = an empty --mc-dir, the pad logs' assumption;
# hub = the card hub_entry wrote; warp:<chunk> = that card with its save's chunk rewritten by tools/warp_save.py -- the
# authoring warp to a level the recorded progression has not reached) : a line the run must log (empty = none) : extra runner args
ROWS=(
  "hub_entry.padlog|4910|Playing|hub||--save-state $OUT/hub_2635.ps2st@2635"
  "load_game.padlog|3880|Paused|hub|game state @ boundary 2884: Playing|"
  "hub_quit.padlog|3780|Title|fresh|game state @ boundary 3704: Title|--load-state $OUT/hub_2635.ps2st"
  "jungle_bungle_walk.padlog|17150|Playing|fresh|@17027 chunk 0x00CECE40 'levels\\earth\\hub\\bossarea'|--trace-player 30 --save-state $OUT/bossarea_17051.ps2st@17051"
  "mecha_arena.padlog|20990|Playing|fresh|game state @ boundary 20193: Playing|--trace-player 30 --load-state $OUT/bossarea_17051.ps2st"
  "hub_walk_forward.padlog|3310|Playing|fresh|game state @ boundary 2629: Playing|--trace-player 30"
  "cavern_entry.padlog|3300|Playing|warp:levels\\earth\\cavern\\cavent|@2780 chunk 0x00C12C40 'levels\\earth\\cavern\\cavent'|--trace-player 30 --save-state $OUT/cavent_2840.ps2st@2840"
  "cavern_cryscave.padlog|6700|Playing|warp:levels\\earth\\cavern\\cavent|@6569 chunk 0x00C13240 'levels\\earth\\cavern\\tunnel02'|--trace-player 30 --save-state $OUT/cryscave_4460.ps2st@4460 --save-state $OUT/tunnel02_6600.ps2st@6600"
  "cavern_nitrocav.padlog|8300|Playing|warp:levels\\earth\\cavern\\cavent|@7815 chunk 0x00C12C40 'levels\\earth\\cavern\\nitrocav'|--trace-player 30 --save-state $OUT/nitrocav_7900.ps2st@7900"
  "totem_beach.padlog|3900|Playing|warp:levels\\earth\\totem\\l03beach|@2711 chunk 0x00C12C40 'levels\\earth\\totem\\l03beach'|--trace-player 30 --save-state $OUT/l03beach_2800.ps2st@2800"
)
status=0
LONG="${TE_CI_LONG_WALKS:-1}"
for row in "${ROWS[@]}"; do
  IFS='|' read -r log end state card must extra <<< "$row"
  padlog="fixtures/padlogs/$log"
  [ -f "$padlog" ] || { echo "padlog_smoke: $log: MISSING"; status=1; continue; }
  if [ "$end" -gt 10000 ] && [ "$LONG" != 1 ]; then echo "padlog_smoke: $log: end $end: SKIPPED (a long walk; TE_CI_LONG_WALKS=0)"; continue; fi
  last_sample=$(grep -v '^#' "$padlog" | awk 'NF{b=$1} END{print b}')
  run="$OUT/${log%.padlog}.log"
  mc="$OUT/mc_${card}"
  case "$card" in
    hub) ;;
    warp:*) mc="$OUT/mc_warp"; python3 tools/warp_save.py --card "$OUT/mc_hub" --chunk "${card#warp:}" --out "$mc" --place 0xFFFF > /dev/null ;;   # the hub card warped (assume-playable: a level entry the progression has not reached)
    *) rm -rf "$mc"; mkdir -p "$mc" ;;
  esac
  set +e
  # shellcheck disable=SC2086
  TE_GFX_HEADLESS=1 TE_GFX_NO_DRAW=1 "$RUNNER" --iso "$ISO" --deterministic --no-sdl-input --no-audio --skip-movies --trace-state \
    --play-pad "$padlog" --mc-dir "$mc" --stop-at "$end" --run-seconds 600 $extra > "$run" 2>&1
  rc=$?
  set -e
  reached=$(grep -c -- "--stop-at $end reached" "$run" || true)
  # the runner's trap lines ('wasm trap: ...', the call_indirect type-check failures, signals); not the word inside an instance
  # name the probe lists (cryscave's act_CAVERN_TRAPDOOR12)
  traps=$(grep -v "adapted at the call site; 0 traps" "$run" | grep -v "^\[probe\]" | grep -c -iE "wasm trap|trap:|trapped|Invalid call_indirect|SIGBUS|SIGSEGV|abort" || true)
  must_ok=1; [ -z "$must" ] || grep -qF -- "$must" "$run" || must_ok=0
  last_state=$(grep -oE "game state @ boundary [0-9]+: [A-Za-z]+ \([0-9]+\)" "$run" | tail -1)
  verdict=OK
  [ "$rc" = 0 ] && [ "$reached" -ge 1 ] && [ "$traps" = 0 ] && [ "$must_ok" = 1 ] && [[ "$last_state" == *": $state ("* ]] || verdict=FAIL
  echo "padlog_smoke: $log: last sample $last_sample, end $end: exit $rc, stop-at reached $reached, traps $traps, last ${last_state#game state @ boundary }${must:+, logged '$must' $must_ok} -> $verdict"
  [ "$verdict" = OK ] || { status=1; grep -iE "trap|call_indirect|SIG|abort|error" "$run" | head -5; }
done
# every level pad log has a row: the logs under fixtures/padlogs/ that are not a judge's own fixture (the title / menu logs
# and the one-click level-select logs the judges drive themselves), an oracle transposition or a plan. options_graphic
# and mods_page_toggle are the Options routes the mods judges (mods_overlay, mods_import, mods_page) drive with their own
# mods folders. hub_entry_ntsc is the hub log timed for the North American disc: ntsc_boot_judge replays it on that disc
# (with padlog_judge and fixtures/judges/hub_entry_ntsc.json); this script replays every log on --iso, the European disc.
# totem_hokum_user is the PS Vita's measurement route through Totem Hokum (level select, 50 Hz)
# and totem_hokum_user_desktop the same inputs one boundary later, the desktop's timing of that route.
JUDGE_DRIVEN="title_intro_idle title_main_menu language_table_rows language_rows language_pick_arabic decal_footprints landing_dust options_graphic mods_page_toggle hub_entry_ntsc totem_hokum_user totem_hokum_user_desktop"
for f in fixtures/padlogs/*.padlog; do
  b="$(basename "$f")"
  case "$b" in *.oracle.padlog|*.script.padlog|level_select_*.padlog) continue ;; esac
  case " $JUDGE_DRIVEN " in *" ${b%.padlog} "*) continue ;; esac
  printf '%s\n' "${ROWS[@]}" | grep -q "^$b|" || { echo "padlog_smoke: $b has no row in this script (add its end boundary)"; status=1; }
done
[ "$status" = 0 ] && echo "padlog_smoke: ALL PAD LOGS REPLAY TO THEIR END" || echo "padlog_smoke: FAILED"
exit $status
