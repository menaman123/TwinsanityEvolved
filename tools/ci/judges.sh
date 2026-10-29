#!/usr/bin/env bash
# tools/ci/judges.sh -- the regression judges on one binary: every presented-frame / census judge under tools/judges with its
# frozen thresholds under fixtures/judges. Each judge was RED on the binary before its fix and GREEN after; here they are run
# GREEN-only on a candidate, one game process at a time, in three sets:
#
#   tools/ci/judges.sh --iso <disc image> [--bin BIN] [--build-dir DIR] [--out DIR] [--launcher-build DIR]
#                      [--set standing|features|title|launcher|discs|all] [--ntsc-iso <the North American disc>]
#
#   standing   the hub's skinned characters, shadow volumes, the shadow kept off the caster, landing dust, particles, the
#              cryscave flicker, Ice Climb's cave passes against the exact GS depth test, Ice Climb's floating ice against the
#              recompiled reference's heights, the frozen heap layout
#   features   the speedrun HUD, level select, the language table (text and speech, two lists) and the pairs chosen apart, the fps counter, the cavern crystals, the title
#              orb, mod saves namespaced, the mods folder's disc overlay (a text bank, a level chunk, the order, nothing on =
#              identical), the mod importer (images, xdelta / PPF / BPS, Mod Crates, executables, the base check), speedrun mode, the language setting, decals, display settings, the CT16 present,
#              the intro ocean seams (the fixture's frozen maximum; the oracle comparison needs the oracle dump), voice pitch,
#              the Arabic language pack (picked in the options and drawn, length, the language judges with it present), the game's Mods page with a mods
#              folder (the built-ins only: no folder mod's row or note; the overlays still apply)
#   title      tools/judges/run_graphics_judges.sh: the self-test frames and the title's legal screen + world
#   launcher   the launcher (--launcher-build, default build-launcher): the cold run of the prebuilt-install flow against
#              a local release server whose asset is BIN zipped (launcher_cold_judge), every Settings row through ps2r.ini
#              (launcher_settings_judge), the Mods page against a mods folder (launcher_mods_judge), the launcher
#              replacing itself on macOS (launcher_update_judge: two packaged launchers, LaunchServices; skipped
#              elsewhere), and no developer
#              vocabulary where a player reads (tools/ci/no_codenames_lint.py); in `all` only when the launcher is built
#   discs      what a disc IS (disc_identity_judge: the modified PAL image used for development, generated plausible / refused / NTSC-U
#              images, manifest.py, the launcher's card; with the North American disc its retail identity and file list),
#              the region-aware loader (region_map_judge: the identity and a synthetic +0x10000 relocation, the game run
#              from the moved executable, the mod tool's maps the same files; with the North American disc the committed map
#              made again from the two discs), and the North American disc played (ntsc_boot_judge: the title, the hub pad
#              log at 60, the movies). The North American disc: --ntsc-iso FILE or NTSC_ISO=FILE in the environment; without
#              it those cases are SKIPPED -- not RED
#
# Prints one line per judge (rc, seconds, the verdict line); exit non-zero when any judge is RED. The hub card the voice
# pitch judge warps from is padlog_smoke's (build_all writes it); without it that judge is skipped.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
ISO=""; BUILD="$ROOT/build"; BIN=""; OUT=""; SET=all; LAUNCHER_BUILD="$ROOT/build-launcher"; NTSC_ISO="${NTSC_ISO:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --set) SET="$2"; shift 2 ;;
    --launcher-build) LAUNCHER_BUILD="$2"; shift 2 ;;
    --ntsc-iso) NTSC_ISO="$2"; shift 2 ;;
    *) echo "judges: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "judges: --iso required"; exit 2; }
BIN="${BIN:-$BUILD/twinsanityevolved}"
[ -x "$BIN" ] || { echo "judges: no runner at $BIN"; exit 2; }
OUT="${OUT:-$BUILD/judges}"
mkdir -p "$OUT"
status=0
# a game process is one whose EXECUTABLE (argv[0]) is the runner -- not a shell or a judge whose arguments name the binary
# (a wrapper `sh -c '... --bin build/twinsanityevolved ...'` matched the old pattern and made this wait forever)
waitidle() { while pgrep -f '^([^ ]*/)?(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; do sleep 5; done; }
j() { # name judge [args...]  -- python3 tools/judges/<judge>.py --bin BIN --iso ISO --out DIR args
  local name=$1 judge=$2; shift 2
  local dir="$OUT/$name.d"; rm -rf "$dir"; mkdir -p "$dir"
  waitidle
  local t0; t0=$(date +%s)
  python3 "tools/judges/$judge.py" --bin "$BIN" --iso "$ISO" --out "$dir" "$@" > "$OUT/$name.log" 2>&1
  local rc=$?
  echo "judges: $name rc=$rc $(( $(date +%s) - t0 ))s $(grep -oE '(verdict )?(RED|GREEN).*' "$OUT/$name.log" | tail -1 | cut -c1-200)"
  [ "$rc" = 0 ] || status=1
  rm -rf "$dir"
}
standing() {
  j skin skin_judge --fixture fixtures/judges/hub_skin_pal.json
  j shadow shadow_judge --fixture fixtures/judges/hub_shadow_pal.json
  j shadow_caster shadow_caster_judge --fixture fixtures/judges/hub_shadow_caster_pal.json
  j landing_dust dust_judge --fixture fixtures/judges/landing_dust_pal.json
  j particles particle_judge --fixture fixtures/judges/hub_particles_pal.json
  j cryscave_flicker flicker_judge --fixture fixtures/judges/cavern_cryscave_flicker_pal.json
  j iceclimb_depth icetex_judge --fixture fixtures/judges/icetex_pal.json
  j iceclimb_bergs iceclimb_bergs_judge --fixture fixtures/judges/iceclimb_bergs_pal.json
  j heap_layout heap_layout_judge
}
features() {
  j speedrun_hud speedrun_hud_judge
  j level_select level_select_judge
  j language_table language_table_judge
  j language_tuple language_tuple_judge
  j fps_counter fps_counter_judge
  j cavern_crystals crystal_judge
  j title_orb orb_judge
  j mods_save mods_save_judge
  j mods_overlay mods_overlay_judge
  # the game's Options > MODS page with a mods folder: the built-ins only (no folder mod's row or note), the overlays applied
  j mods_page mods_page_judge
  # the mod importer (twinsanityevolved-modtool beside BIN): a patched image, xdelta3 / PPF / BPS patches, a Mod Crate, a
  # modified executable, the base check (xdelta3 from PATH or $XDELTA3; without it the judge writes its own VCDIFF), a loose
  # archive entry by its name, and with --ntsc-iso a USA crate made from the North American disc (SKIPPED without it)
  if [ -n "$NTSC_ISO" ]; then j mods_import mods_import_judge --ntsc-iso "$NTSC_ISO"; else j mods_import mods_import_judge; fi
  j speedrun speedrun_judge
  j language language_judge
  # the Arabic language pack (mods/arabic, not a mod): picked in GAME OPTIONS > TEXT LANGUAGE and drawn right (forms,
  # direction, boxes), every text fitting, and the disc's languages unchanged with it present
  j arabic_render arabic_render_judge
  j arabic_length arabic_length_judge
  j language_arabic language_judge --fixture fixtures/judges/language_arabic_pal.json
  j language_table_arabic language_table_judge --fixture fixtures/judges/language_table_arabic_pal.json
  j decals decal_judge
  j display display_judge
  j present_ct16 present_ct16_judge
  j intro_ocean ocean_seam_judge --no-oracle
  if [ -d "$BUILD/padlog_smoke/mc_hub" ]; then
    local dir="$OUT/voice_pitch.d"; rm -rf "$dir"; mkdir -p "$dir"; waitidle
    local t0; t0=$(date +%s)
    python3 tools/judges/voice_pitch_judge.py --binary "$BIN" --iso "$ISO" --out "$dir" --hub-card "$BUILD/padlog_smoke/mc_hub" > "$OUT/voice_pitch.log" 2>&1
    local rc=$?
    echo "judges: voice_pitch rc=$rc $(( $(date +%s) - t0 ))s $(grep -E 'voice_pitch_judge: (RED|GREEN)' "$OUT/voice_pitch.log" | tail -1 | cut -c1-200)"
    [ "$rc" = 0 ] || status=1
    rm -rf "$dir"
  else
    echo "judges: voice_pitch SKIPPED (no hub card at $BUILD/padlog_smoke/mc_hub: run build_all or padlog_smoke first)"
  fi
}
title() {
  waitidle
  local t0; t0=$(date +%s)
  tools/judges/run_graphics_judges.sh --build "$(dirname "$BIN")" --iso "$ISO" > "$OUT/title.log" 2>&1
  local rc=$?
  echo "judges: title rc=$rc $(( $(date +%s) - t0 ))s $(grep -E '^(GREEN|RED|LEGAL|WORLD)' "$OUT/title.log" | tr '\n' ' ')"
  [ "$rc" = 0 ] || status=1
}
launcher() {
  if [ ! -x "$LAUNCHER_BUILD/test_launcher_cold" ]; then
    echo "judges: launcher SKIPPED (no launcher build at $LAUNCHER_BUILD: cmake -S engine/app/launcher -B build-launcher)"
    return
  fi
  local name rc t0
  for name in launcher_cold launcher_settings launcher_mods; do
    local dir="$OUT/$name.d"; rm -rf "$dir"; mkdir -p "$dir"; waitidle
    t0=$(date +%s)
    if [ "$name" = launcher_cold ]; then
      python3 tools/judges/launcher_cold_judge.py --launcher-build "$LAUNCHER_BUILD" --game "$BIN" --iso "$ISO" --out "$dir" > "$OUT/$name.log" 2>&1
    else
      python3 "tools/judges/${name}_judge.py" --launcher-build "$LAUNCHER_BUILD" --iso "$ISO" --out "$dir" > "$OUT/$name.log" 2>&1
    fi
    rc=$?
    echo "judges: $name rc=$rc $(( $(date +%s) - t0 ))s $(grep -oE '(RED|GREEN).*' "$OUT/$name.log" | tail -1 | cut -c1-200)"
    [ "$rc" = 0 ] || status=1
  done
  # the launcher replacing itself: two launchers packaged by release_launcher.sh (its own build beside the launcher
  # build, reused), the old one opened by LaunchServices in a scratch home; macOS only -- the Windows and Linux
  # replacements are the unit test's (LauncherSelfUpdate)
  if [ "$(uname -s)" = Darwin ]; then
    local udir="$OUT/launcher_update.d"; rm -rf "$udir"; mkdir -p "$udir"; waitidle
    t0=$(date +%s)
    python3 tools/judges/launcher_update_judge.py --out "$udir" --work "$LAUNCHER_BUILD-update" > "$OUT/launcher_update.log" 2>&1
    rc=$?
    echo "judges: launcher_update rc=$rc $(( $(date +%s) - t0 ))s $(grep -oE '(RED|GREEN).*' "$OUT/launcher_update.log" | tail -1 | cut -c1-200)"
    [ "$rc" = 0 ] || status=1
  else
    echo "judges: launcher_update SKIPPED (macOS only: the Windows and Linux replacements are LauncherSelfUpdate's)"
  fi
  python3 tools/ci/no_codenames_lint.py > "$OUT/no_codenames.log" 2>&1
  rc=$?
  echo "judges: no_codenames rc=$rc $(tail -1 "$OUT/no_codenames.log")"
  [ "$rc" = 0 ] || status=1
}
discs() {
  local ntsc=(); [ -n "$NTSC_ISO" ] && ntsc=(--ntsc-iso "$NTSC_ISO")
  j disc_identity disc_identity_judge --launcher-build "$LAUNCHER_BUILD" ${ntsc[@]+"${ntsc[@]}"}
  j region_map region_map_judge ${ntsc[@]+"${ntsc[@]}"}
  # the North American disc: exit 2 = SKIPPED (no image), not RED
  local dir="$OUT/ntsc_boot.d"; rm -rf "$dir"; mkdir -p "$dir"; waitidle
  local t0 rc; t0=$(date +%s)
  if [ -n "$NTSC_ISO" ]; then
    python3 tools/judges/ntsc_boot_judge.py --bin "$BIN" --iso "$ISO" --ntsc-iso "$NTSC_ISO" --out "$dir" > "$OUT/ntsc_boot.log" 2>&1
  else
    python3 tools/judges/ntsc_boot_judge.py --bin "$BIN" --iso "$ISO" --out "$dir" > "$OUT/ntsc_boot.log" 2>&1
  fi
  rc=$?
  echo "judges: ntsc_boot rc=$rc $(( $(date +%s) - t0 ))s $(grep -oE '(SKIPPED|RED|GREEN).*' "$OUT/ntsc_boot.log" | tail -1 | cut -c1-200)"
  [ "$rc" = 0 ] || [ "$rc" = 2 ] || status=1
  rm -rf "$dir"
}
case "$SET" in
  standing) standing ;;
  features) features ;;
  title) title ;;
  launcher) launcher ;;
  discs) discs ;;
  all) standing; features; title; launcher; discs ;;
  *) echo "judges: --set standing|features|title|launcher|discs|all"; exit 2 ;;
esac
[ "$status" = 0 ] && echo "judges: ALL GREEN" || echo "judges: RED"
exit $status
