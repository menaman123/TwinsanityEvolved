#!/usr/bin/env bash
# tools/ci/release_launcher.sh -- the launcher's macOS release: the engine's launcher (engine/app/launcher, Qt 6
# Widgets) built from the vendored engine/ tree, wrapped in a bundle carrying this title's name and manifest, Qt
# bundled DYNAMICALLY (LGPL) with the notices, signed (ad hoc unless CODESIGN_IDENTITY names a certificate), and put
# in a .dmg, and the same bundle zipped for the launcher's own update. Extends tools/ci/release_dist.sh (the game's
# release) with the application a player downloads.
#
#   tools/ci/release_launcher.sh [--qt PREFIX] [--build-dir DIR] [--dist-dir DIR] [--engine DIR] [--skip-build]
#                                [--releases-url URL] [--version V] [--no-dmg] [--manifest FILE]
#
# What it does, in order:
#   1. configure + build engine/app/launcher alone (cmake -S engine/app/launcher; PS2R_LAUNCHER_VERSION = this tree's
#      describe), its unit tests (LauncherModels, LauncherScreens, LauncherOneClass);
#   2. stage dist/launcher/<Product>.app from ps2r_launcher.app: Info.plist with the product's name and bundle id,
#      Contents/Resources/titles/title.json (launcher/title.json, with --releases-url's URL when given) and its retail
#      disc lists (titles/discs/*.json: names, sizes, sha256), BUILD-ID, THIRD_PARTY_NOTICES.md (the engine's + Qt), licenses/;
#   3. macdeployqt: the Qt frameworks and plugins into the bundle (dynamic -- LGPL section 6 is met by relinking);
#   4. codesign (ad hoc by default) and verify;
#   5. the asset scan over the staged folder (zero findings);
#   6. dist/<product>-launcher-macos-<arch>.dmg + .sha256, the size reported;
#   7. dist/<product>-launcher-<version>-macos-<arch>.zip + .sha256: the signed bundle zipped by ditto (modes and links
#      kept, no resource forks, extended attributes, ACLs or quarantine) -- the file a running launcher downloads to
#      replace itself (title.json release.launcher_assets); unzipped again and checked with
#      codesign --verify --deep --strict before it is kept.
# --version names the launcher (default: this tree's git describe; the self-update test makes two from one build).
# --manifest packages another title manifest than launcher/title.json (the update judge's copy names its own launcher
# data folder, so that a launcher it opens can never read a player's).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
QT="${QT_PREFIX:-/opt/homebrew/opt/qt}"; BUILD="$ROOT/build-launcher"; DIST_ROOT="$ROOT/dist"; ENGINE="$ROOT/engine"; SKIP_BUILD=0; RELEASES_URL=""; DMG=1; VERSION=""
MANIFEST="$ROOT/launcher/title.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --qt) QT="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --dist-dir) DIST_ROOT="$2"; shift 2 ;;
    --engine) ENGINE="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --releases-url) RELEASES_URL="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --no-dmg) DMG=0; shift ;;
    --manifest) MANIFEST="$2"; shift 2 ;;
    -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
    *) echo "release_launcher: unknown argument $1" >&2; exit 2 ;;
  esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$DIST_ROOT" in /*) ;; *) DIST_ROOT="$ROOT/$DIST_ROOT" ;; esac
case "$MANIFEST" in /*) ;; *) MANIFEST="$ROOT/$MANIFEST" ;; esac
step() { echo; echo "=== $*"; }
[ "$(uname -s)" = Darwin ] || { echo "release_launcher: macOS only (Windows: tools/ci/release_launcher.ps1)" >&2; exit 2; }
[ -f "$ENGINE/app/launcher/CMakeLists.txt" ] || { echo "release_launcher: the engine at $ENGINE has no app/launcher (the engine/ tree carries it)" >&2; exit 2; }
[ -x "$QT/bin/macdeployqt" ] || { echo "release_launcher: no macdeployqt under $QT/bin (brew install qt, or --qt PREFIX)" >&2; exit 2; }
[ -f "$MANIFEST" ] || { echo "release_launcher: no title manifest at $MANIFEST" >&2; exit 2; }
PRODUCT="$(python3 -c "import json;print(json.load(open('$MANIFEST'))['product'])")"
TITLE_ID="$(python3 -c "import json;print(json.load(open('$MANIFEST'))['id'])")"
ARCH="$(uname -m)"
[ -n "$VERSION" ] || VERSION="$(git -C "$ROOT" describe --tags --always 2>/dev/null || git -C "$ROOT" rev-parse --short HEAD)"
free_gb=$(df -g "$ROOT" | awk 'NR==2{print $4}'); min_free_gb="${TE_MIN_FREE_GB:-15}"
echo "free: ${free_gb} GB (floor ${min_free_gb} GB)"
[ "$free_gb" -ge "$min_free_gb" ] || { echo "release_launcher: < ${min_free_gb} GB free -- prune first"; exit 3; }

if [ "$SKIP_BUILD" = 0 ]; then
  step "build the launcher ($ENGINE/app/launcher -> $BUILD)"
  cmake -S "$ENGINE/app/launcher" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QT" \
        "-DPS2R_LAUNCHER_VERSION=$VERSION" > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
  mkdir -p "$BUILD"; mv "$BUILD.configure.log" "$BUILD/configure.log"
  cmake --build "$BUILD" > "$BUILD/build.log" 2>&1 || { grep -n "error\|FAILED" "$BUILD/build.log" | head -20; exit 1; }
  step "the launcher's tests"
  (cd "$BUILD" && QT_QPA_PLATFORM=offscreen ctest --output-on-failure 2>&1 | tail -8)
fi
APP_SRC="$BUILD/ps2r_launcher.app"
[ -d "$APP_SRC" ] || { echo "release_launcher: no $APP_SRC" >&2; exit 1; }

DIST="$DIST_ROOT/launcher-macos-$ARCH"
APP="$DIST/$PRODUCT.app"
step "stage $APP"
rm -rf "$DIST"; mkdir -p "$DIST"
cp -R "$APP_SRC" "$APP"
PLIST="$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleName $PRODUCT" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string $PRODUCT" "$PLIST" 2>/dev/null || /usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName $PRODUCT" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier dev.nativeee.launcher.$(echo "$TITLE_ID" | tr 'A-Z' 'a-z')" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST" 2>/dev/null || /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string $VERSION" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(git -C "$ROOT" rev-list --count HEAD)" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Delete :CFBundleIconFile" "$PLIST" 2>/dev/null || true
RES="$APP/Contents/Resources"
mkdir -p "$RES/titles/discs" "$RES/licenses"
# the manifest (the launcher installs the newest release it names; --releases-url points it elsewhere) and the retail
# disc lists beside it
python3 - "$MANIFEST" "$RES/titles/title.json" "$RELEASES_URL" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
if sys.argv[3]:
    m["release"]["releases_url"] = sys.argv[3]
json.dump(m, open(sys.argv[2], "w"), indent=2)
EOF
cp "$ROOT"/launcher/discs/*.json "$RES/titles/discs/"
{
  echo "$PRODUCT launcher $VERSION ($ARCH)"
  echo "port    $(git -C "$ROOT" rev-parse HEAD)"
  echo "engine  $(git -C "$ENGINE" rev-parse HEAD)"
  echo "qt      $("$QT/bin/qmake" -query QT_VERSION)"
  echo "built   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RES/BUILD-ID"
cat "$RES/BUILD-ID"
cp "$ROOT/LICENSE" "$RES/LICENSE"
cp "$ROOT/README.md" "$RES/README.md"
QT_VERSION="$("$QT/bin/qmake" -query QT_VERSION)"
QT_LIC_SRC="$QT/share/qt/licenses"
[ -d "$QT_LIC_SRC" ] || QT_LIC_SRC="$(ls -d "$QT"/../qt*/*/share/qt/licenses 2>/dev/null | head -1 || true)"
for f in LICENSE.LGPL3 LICENSE.GPL3-EXCEPT LICENSE.GPL3 LICENSE.FDL; do [ -f "$QT_LIC_SRC/$f" ] && cp "$QT_LIC_SRC/$f" "$RES/licenses/Qt-$f" || true; done
[ -f "$RES/licenses/Qt-LICENSE.LGPL3" ] || cp "$ENGINE/tools/dist/licenses/Qt-LICENSE.LGPL3.txt" "$RES/licenses/Qt-LICENSE.LGPL3"   # the repository's copy: no network at release time
{
  echo "# $PRODUCT launcher -- third-party notices -- $(head -1 "$RES/BUILD-ID")"
  echo
  echo "This application is the NativeEE engine's launcher (engine/app/launcher, MIT) carrying this title's manifest and"
  echo "its retail disc lists (file names, sizes and sha256 -- no game data). It contains NO game data and no code derived"
  echo "from the game: it recognises your own disc image, downloads the finished game from the project's releases (checked"
  echo "against their SHA256SUMS) into the game folder you choose, and starts it."
  echo
  echo "| component | how it is linked | licence | text |"
  echo "|---|---|---|---|"
  echo "| TwinsanityEvolved (the manifest, this packaging) | Resources/titles/title.json | MIT | LICENSE |"
  echo "| NativeEE launcher (engine/app/launcher) | the executable | MIT | the engine repository's LICENSE |"
  echo "| Qt $QT_VERSION (QtCore, QtGui, QtWidgets, QtNetwork, QtConcurrent, QtDBus, the platform plugin) | DYNAMIC: Contents/Frameworks, Contents/PlugIns | LGPL v3 | licenses/Qt-LICENSE.LGPL3 |"
  echo
  echo "Qt is used under the LGPL v3: it is linked dynamically and may be replaced by another build of the same major"
  echo "version (section 4(d)): the frameworks sit in Contents/Frameworks and are loaded by name. Qt's source is at"
  echo "https://download.qt.io/official_releases/qt/ and the launcher's at the engine repository. The game this launcher"
  echo "installs carries its own THIRD_PARTY_NOTICES.md (tools/ci/release_dist.sh)."
} > "$RES/THIRD_PARTY_NOTICES.md"

step "macdeployqt (Qt frameworks and plugins into the bundle, dynamic)"
"$QT/bin/macdeployqt" "$APP" -no-strip > "$DIST/macdeployqt.log" 2>&1 || { tail -20 "$DIST/macdeployqt.log"; exit 1; }
echo "frameworks: $(ls "$APP/Contents/Frameworks" | tr '\n' ' ')"
# the LGPL notices' claim holds only if Qt is dynamic: the executable must reference the frameworks
otool -L "$APP/Contents/MacOS/ps2r_launcher" | grep -q "QtWidgets" || { echo "release_launcher: the executable does not link QtWidgets dynamically" >&2; exit 1; }

step "codesign (${CODESIGN_IDENTITY:-ad hoc})"
# Every framework, dylib and plugin first (Homebrew's copies carry their own ad-hoc identity; a process may load only
# libraries of its own identity), then the bundle. No hardened runtime: an ad-hoc identity cannot carry it.
while IFS= read -r -d '' lib; do codesign --force --sign "${CODESIGN_IDENTITY:--}" "$lib" 2>/dev/null; done < <(find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f \( -name "*.dylib" -o -perm -u+x \) -print0 2>/dev/null)
while IFS= read -r -d '' fw; do codesign --force --sign "${CODESIGN_IDENTITY:--}" "$fw" 2>/dev/null; done < <(find "$APP/Contents/Frameworks" -name "*.framework" -maxdepth 1 -print0)
codesign --force --sign "${CODESIGN_IDENTITY:--}" "$APP"
codesign --verify --deep --strict "$APP" && echo "codesign: verified"
# the proof that the bundle runs on its own: the executable loads its frameworks and answers --help
"$APP/Contents/MacOS/ps2r_launcher" --help > /dev/null 2>&1 || { echo "release_launcher: the signed bundle does not start" >&2; exit 1; }
echo "bundle starts: ok"

step "asset scan over the staged folder (zero findings)"
python3 tools/ci/asset_scan.py "$DIST"

APP_BYTES=$(du -sk "$APP" | awk '{print $1*1024}')
echo "app: $APP ($(( APP_BYTES / 1048576 )) MB)"
if [ "$DMG" = 1 ]; then
  step "dmg"
  DMGNAME="$(echo "$PRODUCT" | tr 'A-Z' 'a-z')-launcher-macos-$ARCH"
  rm -f "$DIST_ROOT/$DMGNAME.dmg" "$DIST_ROOT/$DMGNAME.dmg.sha256"
  STAGE="$(mktemp -d)"; cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
  cp "$RES/README.md" "$STAGE/README.md"
  hdiutil create -quiet -volname "$PRODUCT" -srcfolder "$STAGE" -ov -format UDZO "$DIST_ROOT/$DMGNAME.dmg"
  rm -rf "$STAGE"
  (cd "$DIST_ROOT" && shasum -a 256 "$DMGNAME.dmg" > "$DMGNAME.dmg.sha256" && cat "$DMGNAME.dmg.sha256")
  echo "dmg: $DIST_ROOT/$DMGNAME.dmg ($(( $(stat -f %z "$DIST_ROOT/$DMGNAME.dmg") / 1048576 )) MB)"
fi

step "the launcher's own update: the bundle zipped"
LOWER="$(echo "$PRODUCT" | tr '[:upper:]' '[:lower:]')"
ZIPNAME="$LOWER-launcher-$VERSION-macos-$ARCH.zip"
rm -f "$DIST_ROOT/$LOWER-launcher-"*"-macos-$ARCH.zip" "$DIST_ROOT/$LOWER-launcher-"*"-macos-$ARCH.zip.sha256"
ditto -c -k --norsrc --noextattr --noacl --noqtn --keepParent "$APP" "$DIST_ROOT/$ZIPNAME"
# the round trip a launcher makes: unzipped, the bundle's signature must still check
CHECK="$(mktemp -d)"
ditto -x -k "$DIST_ROOT/$ZIPNAME" "$CHECK"
codesign --verify --deep --strict "$CHECK/$PRODUCT.app" || { echo "release_launcher: the zipped bundle does not verify once unzipped" >&2; rm -rf "$CHECK"; exit 1; }
rm -rf "$CHECK"
(cd "$DIST_ROOT" && shasum -a 256 "$ZIPNAME" > "$ZIPNAME.sha256" && cat "$ZIPNAME.sha256")
echo "zip: $DIST_ROOT/$ZIPNAME ($(( $(stat -f %z "$DIST_ROOT/$ZIPNAME") / 1048576 )) MB; unzipped and verified)"
echo; echo "release_launcher: OK -> $APP"
