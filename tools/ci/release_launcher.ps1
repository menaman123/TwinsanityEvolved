# tools/ci/release_launcher.ps1 -- the launcher's Windows release: the engine's launcher (engine/app/launcher, Qt 6
# Widgets) built from the vendored engine/ tree with the Windows Qt, staged as a portable folder (the .exe with the Qt
# DLLs beside it -- windeployqt, dynamic, LGPL -- the manifest under titles\, BUILD-ID, THIRD_PARTY_NOTICES.md,
# licenses\) and zipped. No installer: unzip and run TwinsanityEvolved.exe. The Windows mirror of
# tools/ci/release_launcher.sh.
#
#   powershell -ExecutionPolicy Bypass -File tools\ci\release_launcher.ps1 -Qt C:\Qt\6.8.0\msvc2022_64 [-BuildDir DIR]
#              [-DistDir DIR] [-Engine DIR] [-SkipBuild] [-ReleasesUrl URL]
param(
    [string]$Qt = $env:QT_PREFIX,
    [string]$BuildDir = "build-launcher-windows",
    [string]$DistDir = "dist",
    [string]$Engine = "engine",
    [switch]$SkipBuild,
    [string]$ReleasesUrl = ""
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
if (-not $Qt) { throw "release_launcher: -Qt PREFIX (the Qt 6 kit, e.g. C:\Qt\6.8.0\msvc2022_64)" }
$windeployqt = Join-Path $Qt "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "release_launcher: no $windeployqt" }
$Manifest = Join-Path $Root "launcher\title.json"
$m = Get-Content $Manifest -Raw | ConvertFrom-Json
$Product = $m.product
$Version = (git -C $Root describe --tags --always 2>$null); if (-not $Version) { $Version = (git -C $Root rev-parse --short HEAD) }
function Step($t) { Write-Host ""; Write-Host "=== $t" }

if (-not $SkipBuild) {
    Step "build the launcher ($Engine\app\launcher -> $BuildDir)"
    cmake -S (Join-Path $Root "$Engine\app\launcher") -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$Qt" "-DPS2R_LAUNCHER_VERSION=$Version"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    cmake --build $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Step "the launcher's tests"
    # the test executables load the Qt DLLs from the kit (windeployqt only runs on the staged copy)
    $env:PATH = "$(Join-Path $Qt 'bin');$env:PATH"
    $env:QT_QPA_PLATFORM = "offscreen"
    ctest --test-dir $BuildDir --output-on-failure
    $rc = $LASTEXITCODE
    Remove-Item Env:QT_QPA_PLATFORM
    if ($rc -ne 0) { throw "release_launcher: the launcher's tests failed" }
}
$Exe = Join-Path $BuildDir "ps2r_launcher.exe"
if (-not (Test-Path $Exe)) { throw "release_launcher: no $Exe" }

$Dist = Join-Path $DistDir $Product   # the folder inside the zip players see
Step "stage $Dist"
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force -Path (Join-Path $Dist "titles\discs"), (Join-Path $Dist "licenses") | Out-Null
Copy-Item $Exe (Join-Path $Dist "$Product.exe")
# the manifest (the launcher installs the newest release it names; -ReleasesUrl points it elsewhere) and the retail
# disc lists beside it (file names, sizes, sha256)
if ($ReleasesUrl) { $m.release.releases_url = $ReleasesUrl }
$m | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $Dist "titles\title.json") -Encoding UTF8
Copy-Item (Join-Path $Root "launcher\discs\*.json") (Join-Path $Dist "titles\discs")
$QtVersion = & (Join-Path $Qt "bin\qmake.exe") -query QT_VERSION
@(
    "$Product launcher $Version (windows-x64)",
    "port    $(git -C $Root rev-parse HEAD)",
    "engine  $(git -C (Join-Path $Root $Engine) rev-parse HEAD)",
    "qt      $QtVersion",
    "built   $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
) | Set-Content (Join-Path $Dist "BUILD-ID") -Encoding ASCII
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $Dist "LICENSE")
Copy-Item (Join-Path $Root "README.md") (Join-Path $Dist "README.md")
$lgpl = Get-ChildItem -Path $Qt -Recurse -Filter "LICENSE.LGPL3" -ErrorAction SilentlyContinue | Select-Object -First 1
# the kit's own copy, else the repository's (no network at release time)
if (-not $lgpl) { $lgpl = Get-Item (Join-Path $Root "$Engine\tools\dist\licenses\Qt-LICENSE.LGPL3.txt") }
Copy-Item $lgpl.FullName (Join-Path $Dist "licenses\Qt-LICENSE.LGPL3")
@"
# $Product launcher -- third-party notices -- $Product launcher $Version (windows-x64)

This application is the NativeEE engine's launcher (engine/app/launcher, MIT) carrying this title's manifest and its
retail disc lists (file names, sizes and sha256 -- no game data). It contains NO game data and no code derived from the
game: it recognises your own disc image, downloads the finished game from the project's releases (checked against their
SHA256SUMS) into the game folder you choose, and starts it.

| component | how it is linked | licence | text |
|---|---|---|---|
| TwinsanityEvolved (the manifest, this packaging) | titles\title.json | MIT | LICENSE |
| NativeEE launcher (engine/app/launcher) | the executable | MIT | the engine repository's LICENSE |
| Qt $QtVersion (Qt6Core, Qt6Gui, Qt6Widgets, Qt6Network, Qt6Concurrent, the platform plugin) | DYNAMIC: the DLLs beside the executable | LGPL v3 | licenses\Qt-LICENSE.LGPL3 |

Qt is used under the LGPL v3: it is linked dynamically and may be replaced by another build of the same major
version (section 4(d)). Qt's source is at https://download.qt.io/official_releases/qt/.
"@ | Set-Content (Join-Path $Dist "THIRD_PARTY_NOTICES.md") -Encoding UTF8

Step "windeployqt (the Qt DLLs and plugins beside the executable, dynamic)"
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $Dist "$Product.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

Step "asset scan over the staged folder"
& python (Join-Path $Root "tools\ci\asset_scan.py") $Dist
if ($LASTEXITCODE -ne 0) { throw "release_launcher: the asset scan found something in $Dist" }

Step "zip"
$ZipName = "$($Product.ToLower())-launcher-windows-x64.zip"
$Zip = Join-Path $DistDir $ZipName
if (Test-Path $Zip) { Remove-Item $Zip }
Compress-Archive -Path $Dist -DestinationPath $Zip
$sha = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
"$sha  $ZipName" | Set-Content "$Zip.sha256" -Encoding ASCII
Write-Host "zip: $Zip ($([math]::Round((Get-Item $Zip).Length / 1MB)) MB)  sha256 $sha"
Write-Host ""; Write-Host "release_launcher: OK -> $Dist"
