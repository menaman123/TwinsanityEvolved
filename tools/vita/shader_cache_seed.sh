#!/usr/bin/env bash
# tools/vita/shader_cache_seed.sh -- the SHIPPED SHADER CACHE: every GPU program the Vita build compiles, compiled once in
# Vita3K and packed into the .vpk as app0:shaders/<key>.tegxp, so the device's FIRST launch compiles nothing.
#
# Why: on the PS Vita the GLSL -> GXP compiles run Sony's libshacccg at start-up (a first launch measured on a console: 40
# programs, 13.7 s before the legal screen). Its output for given sources is the same bytes in Vita3K (the same module,
# the GXP stats of 48 of the 49 programs identical between the device's first launch and Vita3K's cold compile; the 49th
# was a stale file on the device), and the app keys every program by an FNV-1a hash of what it compiles (the pair's GLSL,
# the GLSL -> Cg prelude and the cache format: gl_shader_library.cpp BuildProgram). The app reads
# ux0:data/twinsanity/shaders/<key>.tegxp first (its own cache, written at a compile), then app0:shaders/<key>.tegxp (this
# set); a program whose sources changed since the set was made has another key -- not in the set, compiled and stored on
# the memory card as before. So a stale set costs compile time, never a wrong picture. Re-run this after any change to a
# shader source, the translator (gl_shader_library.cpp VitaGlsl / kVitaPrelude) or vitaGL.
#
# How: the package is run once in Vita3K with an EMPTY cache directory, TE_GL_SHADER_SEED=0 (the set already inside it is
# not read) and TE_GL_SHADER_SEED_BUILD=1 (every variant built at start-up, the ones otherwise compiled on first use
# mid-game included); the app exits by itself at boundary 150 (--stop-at), after the renderer built everything. The cache
# files it stored are the set. Optionally route runs (--routes: pad logs of ux0:data/twinsanity/padlogs/) add anything a
# scene builds on its own (nothing does today), and --verify runs the packed result with an empty cache and requires
# "0 compiled".
#
#   tools/vita/shader_cache_seed.sh --vpk IN.vpk [--out OUT.vpk] [--install DIR] [--routes "title_intro_idle hub_walk_forward"]
#                                   [--route-args "ARGS"]... [--verify] [--seconds N] [--runner CMD] [--keep]
#   --vpk      the built package (build-vita-release/twinsanityevolved.vpk)
#   --out      IN.vpk with app0:shaders/ (re)placed by the set (zip; a .vpk is a zip archive)
#   --install  the set copied into DIR (e.g. tools/vita/shader_seed: CMake packs that directory into the next build)
#   --routes   pad logs (names without .padlog) replayed after the build-all run, each to its --stop-at 3300
#   --route-args  one more route as the runner's own arguments (repeatable): a scene that needs a memory card of its own
#              (an arena checkpoint, a cavern) -- e.g. "--mc-dir ux0:data/twinsanity/cards/arena --play-pad
#              ux0:data/twinsanity/padlogs/mecha_fight.padlog --stop-at 8400" -- run after the build-all run into the same
#              cache directory (the disc, --skip-movies --no-kbm --config none and the cache tokens are added)
#   --verify   OUT.vpk run once with an empty cache: its log must say "0 compiled"
#   --runner   how Vita3K is run: unset = directly (VITA3K = the binary, default /Applications/Vita3K.app/Contents/MacOS/
#              Vita3K; VITA3K_PREF = its data directory, default ~/Library/Application Support/Vita3K/Vita3K; the app's
#              args.txt is written there and put back afterwards); or a script taking
#              `--vpk P --tag T --seconds N --args "A"` that prints "vita_run: <run dir>" with <run dir>/app.log (the
#              a wrapper of your own around Vita3K: then VITA3K_PREF must name the data directory it uses)
#   --keep     leave the emulated cache / card directories behind (default: removed)
# The emulated memory card needs the disc image as ux0:data/twinsanity/game.iso (as for playing in Vita3K).
set -euo pipefail
VPK=""; OUT=""; INSTALL=""; ROUTES=""; VERIFY=0; SECS=400; RUNNER=""; KEEP=0
ROUTE_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --vpk) VPK="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --install) INSTALL="$2"; shift 2 ;;
    --routes) ROUTES="$2"; shift 2 ;;
    --route-args) ROUTE_ARGS+=("$2"); shift 2 ;;
    --verify) VERIFY=1; shift ;;
    --seconds) SECS="$2"; shift 2 ;;
    --runner) RUNNER="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    *) echo "shader_cache_seed: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$VPK" ] && [ -f "$VPK" ] || { echo "shader_cache_seed: --vpk PATH (a built .vpk) is required"; exit 2; }
[ "$VERIFY" = 0 ] || [ -n "$OUT" ] || { echo "shader_cache_seed: --verify needs --out"; exit 2; }
command -v zip > /dev/null || { echo "shader_cache_seed: zip not found"; exit 2; }
VPK="$(cd "$(dirname "$VPK")" && pwd)/$(basename "$VPK")"
[ -z "$OUT" ] || OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
VITA3K="${VITA3K:-/Applications/Vita3K.app/Contents/MacOS/Vita3K}"
PREF="${VITA3K_PREF:-$HOME/Library/Application Support/Vita3K/Vita3K}"
DATA="$PREF/fs/ux0/data/twinsanity"
[ -d "$DATA" ] || { echo "shader_cache_seed: $DATA not found (VITA3K_PREF: Vita3K's data directory)"; exit 2; }
[ -n "$RUNNER" ] || [ -x "$VITA3K" ] || { echo "shader_cache_seed: $VITA3K not found (VITA3K)"; exit 2; }
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
CACHE="shaders-seed-$STAMP"; CARDS="cards-seed-$STAMP"; VCACHE="shaders-verify-$STAMP"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/te-shader-seed.XXXXXX")"
mkdir -p "$WORK/shaders" "$DATA/$CARDS/mc0" "$DATA/$CARDS/mc1"
cleanup() {
  rm -rf "$WORK"
  if [ "$KEEP" = 0 ]; then rm -rf "${DATA:?}/${CACHE:?}" "${DATA:?}/${CARDS:?}" "${DATA:?}/${VCACHE:?}"; fi
}
trap cleanup EXIT

# one run of package $1 (tag $2) with args.txt = $3; the app's log copied to $WORK/$2.log
run() {
  local vpk="$1" tag="$2" args="$3" log="$WORK/$2.log"
  echo "shader_cache_seed: run $tag: $args"
  if [ -n "$RUNNER" ]; then
    local out run_dir
    out="$("$RUNNER" --vpk "$vpk" --tag "$tag" --seconds "$SECS" --args "$args" 2>&1)" || true
    run_dir="$(echo "$out" | grep '^vita_run: /' | head -1 | sed 's/^vita_run: //')"
    [ -n "$run_dir" ] && [ -f "$run_dir/app.log" ] && cp "$run_dir/app.log" "$log"
    echo "shader_cache_seed: $tag -> ${run_dir:-?}"
  else
    local keep_args="" pid start
    [ -f "$DATA/args.txt" ] && keep_args="$(cat "$DATA/args.txt")"
    printf '# shader_cache_seed.sh %s\n%s\n' "$tag" "$args" > "$DATA/args.txt"
    rm -f "$DATA/twinsanityevolved.log"
    "$VITA3K" --frontend sdl -z --log-level 3 "$vpk" > "$WORK/$tag.vita3k.out" 2>&1 &
    pid=$!; start=$(date +%s)
    while kill -0 "$pid" 2>/dev/null; do
      if [ $(( $(date +%s) - start )) -ge "$SECS" ]; then kill "$pid" 2>/dev/null; sleep 2; kill -9 "$pid" 2>/dev/null; break; fi
      sleep 1
    done
    wait "$pid" 2>/dev/null || true
    [ -f "$DATA/twinsanityevolved.log" ] && cp "$DATA/twinsanityevolved.log" "$log"
    if [ -n "$keep_args" ]; then printf '%s\n' "$keep_args" > "$DATA/args.txt"; else rm -f "$DATA/args.txt"; fi
  fi
  [ -f "$log" ] || { echo "shader_cache_seed: $tag left no app log"; return 1; }
  grep -h "\[gl\] shader cache (" "$log" | sed 's/^\[[0-9.]*\] //' || true
}

BASE="--iso ux0:data/twinsanity/game.iso --skip-movies --no-kbm --config none --mc-dir ux0:data/twinsanity/$CARDS"
# 1. every program compiled into an empty cache directory
run "$VPK" seed-build "$BASE --stop-at 150 TE_LOG_STAMP=1 TE_GL_SHADER_CACHE=ux0:data/twinsanity/$CACHE TE_GL_SHADER_SEED=0 TE_GL_SHADER_SEED_BUILD=1"
# 2. the routes, into the same directory (whatever a scene builds on its own)
for route in $ROUTES; do
  run "$VPK" "seed-$route" "$BASE --play-pad ux0:data/twinsanity/padlogs/$route.padlog --stop-at 3300 TE_LOG_STAMP=1 TE_GL_SHADER_CACHE=ux0:data/twinsanity/$CACHE TE_GL_SHADER_SEED=0"
done
i=0
for ra in ${ROUTE_ARGS[@]+"${ROUTE_ARGS[@]}"}; do
  i=$((i + 1))
  run "$VPK" "seed-route$i" "--iso ux0:data/twinsanity/game.iso --skip-movies --no-kbm --config none $ra TE_LOG_STAMP=1 TE_GL_SHADER_CACHE=ux0:data/twinsanity/$CACHE TE_GL_SHADER_SEED=0"
done
n=$( (ls "$DATA/$CACHE"/*.tegxp 2>/dev/null || true) | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "shader_cache_seed: no .tegxp stored in ux0:data/twinsanity/$CACHE"; exit 1; }
cp "$DATA/$CACHE"/*.tegxp "$WORK/shaders/"
{
  echo "# the shipped shader cache: $n programs (tools/vita/shader_cache_seed.sh, $(date -u +%Y-%m-%dT%H:%M:%SZ))"
  echo "# made from $(basename "$VPK") md5 $(md5 -q "$VPK" 2>/dev/null || md5sum "$VPK" | cut -d' ' -f1)"
  (cd "$WORK/shaders" && for f in *.tegxp; do echo "$f $(wc -c < "$f" | tr -d ' ')"; done)
} > "$WORK/shaders/SEED.txt"
echo "shader_cache_seed: $n programs, $(du -sk "$WORK/shaders" | cut -f1) KB"
if [ -n "$INSTALL" ]; then
  mkdir -p "$INSTALL"
  rm -f "$INSTALL"/*.tegxp
  cp "$WORK/shaders/"*.tegxp "$WORK/shaders/SEED.txt" "$INSTALL/"
  echo "shader_cache_seed: installed into $INSTALL"
fi
if [ -n "$OUT" ]; then
  [ "$OUT" = "$VPK" ] || cp "$VPK" "$OUT"
  zip -q -d "$OUT" 'shaders/*' > /dev/null 2>&1 || true   # a set packed before: replaced
  (cd "$WORK" && zip -q -r "$OUT" shaders)
  echo "shader_cache_seed: packed into $OUT ($(unzip -l "$OUT" | grep -c 'shaders/.*\.tegxp') programs in app0:shaders/)"
fi
if [ "$VERIFY" = 1 ]; then
  # 3. the packed package with an empty cache: everything from app0:shaders, nothing compiled
  run "$OUT" seed-verify "$BASE --stop-at 150 TE_LOG_STAMP=1 TE_GL_SHADER_CACHE=ux0:data/twinsanity/$VCACHE"
  line="$(grep -h "\[gl\] shader cache (" "$WORK/seed-verify.log" | tail -1)"
  case "$line" in
    *", 0 compiled"*) echo "shader_cache_seed: VERIFIED -- a first launch of $(basename "$OUT") compiles nothing" ;;
    *) echo "shader_cache_seed: NOT verified: $line"; exit 1 ;;
  esac
fi
