#!/usr/bin/env bash
# tools/ci/release_launcher_linux.sh -- the launcher's Linux release: the engine's launcher (engine/app/launcher, Qt 6
# Widgets) built from the vendored engine/ tree, staged as an AppDir carrying this title's name and manifest, Qt bundled
# DYNAMICALLY (LGPL) by linuxdeploy and its Qt plugin at pinned releases (sha256 below), checked by the asset scan and
# packed as ONE AppImage: download, mark executable, run. The Linux mirror of tools/ci/release_launcher.sh (macOS .dmg)
# and tools/ci/release_launcher.ps1 (Windows zip); x86_64 (the release workflow's ubuntu-24.04 runner).
#
#   tools/ci/release_launcher_linux.sh [--qt PREFIX] [--build-dir DIR] [--dist-dir DIR] [--engine DIR] [--skip-build]
#                                      [--releases-url URL] [--tools-dir DIR]
#
# What it does, in order:
#   1. configure + build engine/app/launcher alone (PS2R_LAUNCHER_VERSION = this tree's describe) and its unit tests
#      (offscreen);
#   2. stage dist/launcher-linux-x86_64/AppDir: usr/bin/<Product> with titles/title.json and the disc tables
#      titles/discs/*.json beside it (where the launcher looks: beside its executable, as on Windows), the notices
#      (BUILD-ID, LICENSE, README.md, THIRD_PARTY_NOTICES.md, licenses/) under usr/share/doc/<product>-launcher/, the
#      .desktop entry and an icon drawn here (the product's initials on a plain square: no picture of the game);
#   3. linuxdeploy + linuxdeploy-plugin-qt (both AppImages, fetched once into --tools-dir and checked against the pins;
#      run without FUSE): the Qt libraries and plugins the launcher links (the xcb platform, the TLS backends) into the
#      AppDir, dynamic;
#   4. the asset scan over the AppDir (zero findings);
#   5. dist/<product>-launcher-linux-x86_64.AppImage + .sha256, the size reported.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
QT="${QT_PREFIX:-${QT_ROOT_DIR:-}}"; BUILD="$ROOT/build-launcher-linux"; DIST_ROOT="$ROOT/dist"; ENGINE="$ROOT/engine"
SKIP_BUILD=0; RELEASES_URL=""; TOOLS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --qt) QT="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --dist-dir) DIST_ROOT="$2"; shift 2 ;;
    --engine) ENGINE="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --releases-url) RELEASES_URL="$2"; shift 2 ;;
    --tools-dir) TOOLS="$2"; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "release_launcher_linux: unknown argument $1" >&2; exit 2 ;;
  esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$DIST_ROOT" in /*) ;; *) DIST_ROOT="$ROOT/$DIST_ROOT" ;; esac
TOOLS="${TOOLS:-$BUILD/appimage-tools}"
step() { echo; echo "=== $*"; }

# linuxdeploy and its Qt plugin: dated releases (not `continuous`), the AppImages' sha256 recorded when they were pinned
LINUXDEPLOY_TAG="1-alpha-20251107-1"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
PLUGIN_QT_TAG="1-alpha-20250213-1"
PLUGIN_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

[ "$(uname -s)/$(uname -m)" = Linux/x86_64 ] || { echo "release_launcher_linux: Linux x86_64 only (macOS: tools/ci/release_launcher.sh, Windows: tools/ci/release_launcher.ps1)" >&2; exit 2; }
[ -n "$QT" ] && [ -x "$QT/bin/qmake" ] || { echo "release_launcher_linux: no Qt 6 kit (--qt PREFIX, or QT_ROOT_DIR from install-qt-action)" >&2; exit 2; }
[ -f "$ENGINE/app/launcher/CMakeLists.txt" ] || { echo "release_launcher_linux: the engine at $ENGINE has no app/launcher" >&2; exit 2; }
MANIFEST="$ROOT/launcher/title.json"
PRODUCT="$(python3 -c "import json;print(json.load(open('$MANIFEST'))['product'])")"
LOWER="$(echo "$PRODUCT" | tr 'A-Z' 'a-z')"
VERSION="$(git -C "$ROOT" describe --tags --always 2>/dev/null || git -C "$ROOT" rev-parse --short HEAD)"
QT_VERSION="$("$QT/bin/qmake" -query QT_VERSION)"
free_gb=$(df -BG "$ROOT" | awk 'NR==2{gsub("G","",$4); print $4}'); min_free_gb="${TE_MIN_FREE_GB:-15}"
echo "free: ${free_gb} GB (floor ${min_free_gb} GB)"
[ "$free_gb" -ge "$min_free_gb" ] || { echo "release_launcher_linux: < ${min_free_gb} GB free -- prune first"; exit 3; }

if [ "$SKIP_BUILD" = 0 ]; then
  step "build the launcher ($ENGINE/app/launcher -> $BUILD)"
  cmake -S "$ENGINE/app/launcher" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QT" \
        "-DPS2R_LAUNCHER_VERSION=$VERSION" > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
  mkdir -p "$BUILD"; mv "$BUILD.configure.log" "$BUILD/configure.log"
  cmake --build "$BUILD" > "$BUILD/build.log" 2>&1 || { grep -n "error\|FAILED" "$BUILD/build.log" | head -20; exit 1; }
  step "the launcher's tests"
  (cd "$BUILD" && QT_QPA_PLATFORM=offscreen ctest --output-on-failure 2>&1 | tail -8)
fi
EXE_SRC="$BUILD/ps2r_launcher"
[ -x "$EXE_SRC" ] || { echo "release_launcher_linux: no $EXE_SRC" >&2; exit 1; }

DIST="$DIST_ROOT/launcher-linux-x86_64"
APPDIR="$DIST/AppDir"
DOC="$APPDIR/usr/share/doc/$LOWER-launcher"
step "stage $APPDIR"
rm -rf "$DIST"; mkdir -p "$APPDIR/usr/bin/titles/discs" "$DOC/licenses"
cp "$EXE_SRC" "$APPDIR/usr/bin/$PRODUCT"
# the manifest (the launcher installs the newest release it names; --releases-url points it elsewhere) and the disc
# tables beside it (names, sizes, hashes: no game data)
python3 - "$MANIFEST" "$APPDIR/usr/bin/titles/title.json" "$RELEASES_URL" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
if sys.argv[3]:
    m["release"]["releases_url"] = sys.argv[3]
json.dump(m, open(sys.argv[2], "w"), indent=2)
EOF
cp "$ROOT"/launcher/discs/*.json "$APPDIR/usr/bin/titles/discs/"
{
  echo "$PRODUCT launcher $VERSION (linux-x86_64)"
  echo "port    $(git -C "$ROOT" rev-parse HEAD)"
  echo "engine  $(git -C "$ENGINE" rev-parse HEAD)"
  echo "qt      $QT_VERSION"
  echo "built   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DOC/BUILD-ID"
cat "$DOC/BUILD-ID"
cp "$ROOT/LICENSE" "$DOC/LICENSE"
cp "$ROOT/README.md" "$DOC/README.md"
QT_LGPL="$(find "$QT" -name LICENSE.LGPL3 -print -quit 2>/dev/null || true)"
if [ -n "$QT_LGPL" ]; then cp "$QT_LGPL" "$DOC/licenses/Qt-LICENSE.LGPL3"; else cp "$ENGINE/tools/dist/licenses/Qt-LICENSE.LGPL3.txt" "$DOC/licenses/Qt-LICENSE.LGPL3"; fi
cat > "$DOC/THIRD_PARTY_NOTICES.md" <<EOF
# $PRODUCT launcher -- third-party notices -- $PRODUCT launcher $VERSION (linux-x86_64)

This application is the NativeEE engine's launcher (engine/app/launcher, MIT) carrying this title's manifest and its
disc tables (file names, sizes and hashes -- no game data). It contains NO game data and no code derived from the game:
it recognises your own disc image, downloads the finished game from the project's releases (checked against their
SHA256SUMS) into the game folder you choose, and starts it.

| component | how it is linked | licence | text |
|---|---|---|---|
| $PRODUCT (the manifest, this packaging) | usr/bin/titles/title.json | MIT | LICENSE |
| NativeEE launcher (engine/app/launcher) | the executable | MIT | the engine repository's LICENSE |
| Qt $QT_VERSION (Qt6Core, Qt6Gui, Qt6Widgets, Qt6Network, Qt6Concurrent, Qt6DBus, the xcb platform plugin, the TLS backends) | DYNAMIC: usr/lib, usr/plugins inside the AppImage | LGPL v3 | licenses/Qt-LICENSE.LGPL3 |

Qt is used under the LGPL v3: it is linked dynamically and may be replaced by another build of the same major version
(section 4(d)): \`./$PRODUCT-launcher-linux-x86_64.AppImage --appimage-extract\` unpacks the libraries, which the
executable loads by name from usr/lib. Qt's source is at https://download.qt.io/official_releases/qt/ and the
launcher's at the engine repository. The game this launcher installs carries its own THIRD_PARTY_NOTICES.md.
EOF
DESKTOP="$DIST/$LOWER-launcher.desktop"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=$PRODUCT
Comment=Play Crash Twinsanity from your own disc image
Exec=$PRODUCT
Icon=$LOWER-launcher
Categories=Game;
Terminal=false
EOF
# the icon: the product's initials, drawn here (no artwork of the game)
ICON="$DIST/$LOWER-launcher.svg"
INITIALS="$(echo "$PRODUCT" | sed 's/[^A-Z]//g' | cut -c1-2)"
cat > "$ICON" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect x="8" y="8" width="240" height="240" rx="48" fill="#e8742a"/>
  <text x="128" y="160" font-family="sans-serif" font-size="104" font-weight="bold" fill="#ffffff" text-anchor="middle">${INITIALS:-TE}</text>
</svg>
EOF

step "linuxdeploy $LINUXDEPLOY_TAG + linuxdeploy-plugin-qt $PLUGIN_QT_TAG (pinned, sha256)"
mkdir -p "$TOOLS"
fetch_tool() { # url file sha256
  if [ ! -f "$TOOLS/$2" ]; then curl -fsSL -o "$TOOLS/$2.part" "$1"; mv "$TOOLS/$2.part" "$TOOLS/$2"; fi
  local got; got="$(sha256sum "$TOOLS/$2" | awk '{print $1}')"
  [ "$got" = "$3" ] || { echo "release_launcher_linux: sha256 MISMATCH for $2: want $3 got $got" >&2; rm -f "$TOOLS/$2"; exit 4; }
  chmod +x "$TOOLS/$2"; echo "verified $2"
}
fetch_tool "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage" \
           linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_SHA256"
fetch_tool "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$PLUGIN_QT_TAG/linuxdeploy-plugin-qt-x86_64.AppImage" \
           linuxdeploy-plugin-qt-x86_64.AppImage "$PLUGIN_QT_SHA256"

step "linuxdeploy (the Qt libraries and plugins into the AppDir, dynamic) -> the AppImage"
NAME="$LOWER-launcher-linux-x86_64.AppImage"
rm -f "$DIST_ROOT/$NAME" "$DIST_ROOT/$NAME.sha256"
# APPIMAGE_EXTRACT_AND_RUN: the tools are AppImages themselves; extracting instead of mounting needs no FUSE on the host.
# QMAKE: the kit the launcher was built with (its plugins and libraries are the ones deployed). LDAI_OUTPUT / OUTPUT:
# the AppImage's file name (the appimage output plugin's current and older names for it).
( export PATH="$TOOLS:$PATH" APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$QT/bin/qmake" LD_LIBRARY_PATH="$QT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
         LDAI_OUTPUT="$DIST_ROOT/$NAME" OUTPUT="$DIST_ROOT/$NAME" LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
  cd "$DIST"
  linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --executable "$APPDIR/usr/bin/$PRODUCT" \
      --desktop-file "$DESKTOP" --icon-file "$ICON" --plugin qt --output appimage )
[ -f "$DIST_ROOT/$NAME" ] || { echo "release_launcher_linux: linuxdeploy wrote no $DIST_ROOT/$NAME" >&2; exit 1; }

step "asset scan over the AppDir (zero findings)"
python3 tools/ci/asset_scan.py "$APPDIR"

step "the AppImage"
chmod +x "$DIST_ROOT/$NAME"
(cd "$DIST_ROOT" && sha256sum "$NAME" > "$NAME.sha256" && cat "$NAME.sha256")
echo "AppImage: $DIST_ROOT/$NAME ($(( $(stat -c %s "$DIST_ROOT/$NAME") / 1048576 )) MB)"
echo; echo "release_launcher_linux: OK -> $DIST_ROOT/$NAME"
