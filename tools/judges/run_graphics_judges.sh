#!/bin/sh
# tools/judges/run_graphics_judges.sh -- the graphics seam's judges: the host self-test's frame must be GREEN and its
# clear-only frame RED under title_frame_judge.py (the frozen-judge pattern: red on the broken binary, green on the fix),
# and, with --iso, one deterministic dump run of the real runner is judged twice: the LEGAL screen (frame 35,
# title_frame_judge.py with the state log: Title reached) and the TITLE'S WORLD (frame 1500, scene_frame_judge.py
# title_world: the opening scene's sky and sea drawn by the scene -- region luminance, clear-colour share, the frame's
# draws/triangles floor). TE_GFX_CULL_ALL_OUT=1 in the environment makes the world leg red on purpose (the frozen judge's
# "broken binary").
#   tools/judges/run_graphics_judges.sh [--build DIR] [--iso IMAGE [--seconds N]]
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$HERE/build"
ISO=""
SECONDS_RUN=60
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD="$2"; shift 2 ;;
        --iso) ISO="$2"; shift 2 ;;
        --seconds) SECONDS_RUN="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
OUT=$(mktemp -d "${TMPDIR:-/tmp}/te_judges.XXXXXX")
fail=0
echo "== graphics self-test (drawn): must be green"
mkdir -p "$OUT/drawn" && "$BUILD/te_graphics_selftest" "$OUT/drawn" > "$OUT/drawn.log" 2>&1
if python3 "$HERE/tools/judges/title_frame_judge.py" "$OUT/drawn" --clear 16,32,48 --summary-log "$OUT/drawn.log"; then echo "GREEN"; else echo "RED (unexpected)"; fail=1; fi
echo "== graphics self-test (clear only): must be red"
mkdir -p "$OUT/black" && "$BUILD/te_graphics_selftest" "$OUT/black" --black > "$OUT/black.log" 2>&1
if python3 "$HERE/tools/judges/title_frame_judge.py" "$OUT/black" --clear 16,32,48 --summary-log "$OUT/black.log"; then echo "GREEN (unexpected)"; fail=1; else echo "RED as required"; fi
if [ -n "$ISO" ]; then
    echo "== the runner: one deterministic dump run (every 5th frame, up to ${SECONDS_RUN}s or the run's end)"
    # (the process NAME only: a foreign runner's --game-files .../ps2recomp/... argument is not a game process)
    if pgrep -fl '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; then
        echo "another game process is running (one at a time); skipping the runner"; exit $fail
    fi
    mkdir -p "$OUT/run"
    # --skip-movies: the frozen frame numbers (35 legal, 1500 title world) are the drive model's boot timeline (the title appears at
    # boundary 1262 since the drive model; the world is GREEN from present ~1100 on, RED at 1050 -- re-anchored by the
    # re-anchored from 550); with the movie Player the boot movies occupy the first ~1123 frames (movie_judge.py judges those)
    TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$OUT/run" TE_GFX_DUMP_EVERY=5 TE_GFX_SUMMARY_EVERY=100 \
        "$BUILD/twinsanityevolved" --iso "$ISO" --deterministic --no-sdl-input --trace-state --skip-movies --stop-at 2000 --run-seconds "$SECONDS_RUN" > "$OUT/run.log" 2>&1 || true
    echo "== the legal screen (frame 35) and the Title state reached"
    if python3 "$HERE/tools/judges/title_frame_judge.py" "$OUT/run" --frame 35 --summary-log "$OUT/run.log" --state-log "$OUT/run.log"; then echo "LEGAL GREEN"; else echo "LEGAL RED"; fail=1; fi
    echo "== the title's world (frame 1500, the scene)"
    if python3 "$HERE/tools/judges/scene_frame_judge.py" "$OUT/run" --frame 1500 --log "$OUT/run.log" --preset title_world; then echo "WORLD GREEN"; else echo "WORLD RED"; fail=1; fi
fi
echo "frames and logs: $OUT"
exit $fail
