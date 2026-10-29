# tools/ci/release_dist.ps1 -- the Windows arm of the release workflow and its local reproduction (PowerShell 5.1+ / 7).
#
#   .\tools\ci\release_dist.ps1 [-Preset windows-clang-cl-release] [-BuildDir DIR] [-DistDir DIR] [-Jobs N] [-SkipBuild] [-NoZip] [-Lto]
#
# What it does, in order (the Windows counterpart of tools/ci/release_dist.sh):
#   1. the pinned wasm32 toolchain (tools/fetch_toolchain.ps1: wasi-sdk, wabt, libclang for x86_64 Windows; sha256
#      against tools/toolchain/PINS.sha256);
#   2. configure + build through the Windows CMake PRESET (CMakePresets.json `windows-clang-cl-release`, or -Preset;
#      -Lto = windows-clang-cl-release-lto): clang-cl + Ninja through the ENGINE's clang-cl shim
#      (cmake/windows-clang-cl.toolchain.cmake; engine/tools/build/clang_cl_shim.py spells the trees' GNU-style
#      per-target flags for the cl front end), SDL2 and FFmpeg from the vcpkg manifest (vcpkg.json: sdl2 +
#      ffmpeg[avcodec,swscale] -- LGPL features only) through the vcpkg toolchain file chain-loaded by ours; the same
#      toolchain file is forwarded to the engine's ExternalProject (CMakeLists.txt). -BuildDir defaults to the preset's
#      binaryDir (build-windows-release[-lto]);
#   3. stage dist\windows-x64\: twinsanityevolved.exe, twinsanityevolved-modtool.exe + every DLL vcpkg's applocal deployment placed beside the built exe,
#      BUILD-ID, README.md (the player half), RELEASE-NOTES.txt (what the README's What's new links), LICENSE,
#      THIRD_PARTY_NOTICES.md +
#      licenses\ (the engine's committed texts + vcpkg's share\<port>\copyright), fixtures\padlogs\;
#   4. the asset scan over the staged folder (tools/ci/asset_scan.py: zero findings);
#   5. dist\twinsanityevolved-windows-x64.zip + .sha256.
#
# Prerequisites: Visual Studio 2022 Build Tools with "C++ Clang tools for Windows" (clang-cl,
# Ninja) -- run from a Developer PowerShell or let the script import VsDevCmd.bat; vcpkg with VCPKG_ROOT; CMake 3.24+;
# Python 3.8+ as `python`; git (the BUILD-ID's commits -- the launcher's MinGit is enough, no bash needed).
#
# STATUS: the runner's Win32 arm exists (runner/te_fiber.c
# VirtualAlloc, te_fiber_win64.c, te_savestate.cpp with /DYNAMICBASE:NO and the adopted /GS cookie) and the whole lane was
# cross-checked for x86_64-pc-windows-msvc on the dev host (tools/ci/win_cross_smoke.sh), not yet RUN on Windows: this
# script runs the unit tests (nativeee.te_fiber among them) right after the build; the first Windows run then does a
# `--save-state`/`--load-state` round trip on the title. The release workflow marks the Windows job continue-on-error
# until a Windows machine has passed both (lists what to send back).
[CmdletBinding()]
param(
    [string]$Preset = "",
    [string]$BuildDir = "",
    [string]$DistDir = "",
    [int]$Jobs = 0,
    [switch]$SkipBuild,
    [switch]$NoZip,
    [switch]$Lto,
    [switch]$Help
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\..")).Path
Set-Location $Root
if ($Help) {
    Get-Content $MyInvocation.MyCommand.Path | Select-Object -Skip 0 -First 30 | ForEach-Object { $_ -replace '^# ?', '' }
    exit 0
}
function Step($msg) { Write-Host ""; Write-Host "=== $msg" -ForegroundColor Cyan }
# A native command whose stderr is merged terminates PowerShell 5.1 under Stop on its first stderr line: run such
# commands with the preference relaxed and judge them by $LASTEXITCODE (the engine's build.ps1 does the same).
function Invoke-Native([scriptblock]$Block) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = 0
    try { & $Block | Out-Host } finally { $ErrorActionPreference = $saved }
    return $global:LASTEXITCODE
}

if (-not $Preset) { $Preset = if ($Lto) { "windows-clang-cl-release-lto" } else { "windows-clang-cl-release" } }
if (-not $BuildDir) {
    # the preset's binaryDir (CMakePresets.json): build-windows-release / build-windows-release-lto / build-windows-dev
    $presets = Get-Content (Join-Path $Root "CMakePresets.json") -Raw | ConvertFrom-Json
    $p = $presets.configurePresets | Where-Object { $_.name -eq $Preset } | Select-Object -First 1
    if (-not $p) { throw "release_dist.ps1: no configure preset '$Preset' in CMakePresets.json" }
    $bin = $p.binaryDir
    while (-not $bin -and $p.inherits) { $p = $presets.configurePresets | Where-Object { $_.name -eq $p.inherits } | Select-Object -First 1; $bin = $p.binaryDir }
    if (-not $bin) { throw "release_dist.ps1: preset '$Preset' has no binaryDir" }
    $BuildDir = $bin.Replace('${sourceDir}', $Root)
}
if (-not $DistDir) { $DistDir = Join-Path $Root "dist" }
$Platform = "windows-x64"
$Dist = Join-Path $DistDir $Platform
$Engine = Join-Path $Root "engine"
if (-not (Test-Path (Join-Path $Engine "CMakeLists.txt")) -or -not (Test-Path (Join-Path $Root "game\include\platform\pads.h"))) {
    throw "release_dist.ps1: engine\ or game\ missing -- the clone is incomplete (both are tracked trees of this repository)"
}

# --- the MSVC developer environment (clang-cl, Ninja, the Windows SDK on PATH) ---------------------------------------
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $devcmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
        if (Test-Path $devcmd) {
            Step "importing the VS developer environment ($devcmd)"
            cmd /c "`"$devcmd`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
                if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process") }
            }
        }
        $llvm = Join-Path $vs "VC\Tools\Llvm\x64\bin"
        if ($vs -and (Test-Path (Join-Path $llvm "clang-cl.exe")) -and -not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
            $env:PATH = "$llvm;$env:PATH"
        }
    }
}
foreach ($tool in @("cmake", "ninja", "clang-cl", "python", "git")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "release_dist.ps1: '$tool' not on PATH (prerequisites at the top of this script)" }
}
if (-not $SkipBuild) {
    if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
        throw "release_dist.ps1: VCPKG_ROOT must point at a vcpkg checkout (git clone https://github.com/microsoft/vcpkg; .\bootstrap-vcpkg.bat)"
    }

    Step "toolchain (pinned, sha256; tools/fetch_toolchain.ps1)"
    & (Join-Path $Root "tools\fetch_toolchain.ps1")
    if ($LASTEXITCODE -ne 0) { throw "release_dist.ps1: fetch_toolchain.ps1 failed ($LASTEXITCODE)" }

    # The preset carries the whole configuration (cmake/windows-clang-cl.toolchain.cmake: clang-cl, the shim, vcpkg); a
    # -BuildDir other than the preset's binaryDir is passed as -B (CMake 3.24+ allows -B with --preset).
    $args = @("--preset", $Preset)
    if ($BuildDir) { $args += @("-B", $BuildDir) }
    Step "configure (cmake $($args -join ' '))"
    $rc = Invoke-Native { cmake @args }
    if ($rc -ne 0) { throw "release_dist.ps1: configure failed ($rc)" }

    Step "build (game wasm, wasm2c, engine libs, runner, tests)"
    $buildArgs = @("--build", $BuildDir, "--target", "twinsanityevolved", "twinsanityevolved-modtool", "te_tests", "te_fiber_test")
    if ($Jobs -gt 0) { $buildArgs += @("-j", "$Jobs") }
    $rc = Invoke-Native { cmake @buildArgs }
    if ($rc -ne 0) { throw "release_dist.ps1: build failed ($rc)" }

    Step "unit tests (ctest -L nativeee: the seam tests + nativeee.te_fiber, the Win64 fiber arm's run)"
    $rc = Invoke-Native { ctest --test-dir $BuildDir -L nativeee --output-on-failure }
    if ($rc -ne 0) { throw "release_dist.ps1: ctest failed ($rc)" }
}

$Bin = Join-Path $BuildDir "twinsanityevolved.exe"
if (-not (Test-Path $Bin)) { throw "release_dist.ps1: no binary at $Bin (build first)" }

Step "stage ($Dist)"
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force -Path $Dist, (Join-Path $Dist "licenses"), (Join-Path $Dist "fixtures") | Out-Null
Copy-Item $Bin (Join-Path $Dist "twinsanityevolved.exe")
# the mod importer on the command line (the launcher's Mods page uses the same library)
$ModTool = Join-Path $BuildDir "twinsanityevolved-modtool.exe"
if (-not (Test-Path $ModTool)) { throw "release_dist: no mod tool at $ModTool (build first)" }
Copy-Item $ModTool (Join-Path $Dist "twinsanityevolved-modtool.exe")
# Every DLL vcpkg's applocal deployment placed beside the built exe (SDL2, avcodec, avutil, swscale and their closure).
Get-ChildItem -Path $BuildDir -Filter "*.dll" -File | ForEach-Object { Copy-Item $_.FullName (Join-Path $Dist $_.Name) }
$pdb = Join-Path $BuildDir "twinsanityevolved.pdb"
if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $Dist "twinsanityevolved.pdb") }
$portRev = (git -C $Root rev-parse HEAD).Trim()
$engineRev = (git -C $Engine rev-parse HEAD).Trim()
$gameRev = (git -C (Join-Path $Root "game") rev-parse HEAD).Trim()
@(
    "twinsanityevolved $($portRev.Substring(0, 12)) ($Platform)",
    "port    $portRev $((git -C $Root describe --tags --always).Trim())",
    "engine  $engineRev",
    "game    $gameRev",
    "built   $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
) | Set-Content -Encoding ASCII (Join-Path $Dist "BUILD-ID")
Get-Content (Join-Path $Dist "BUILD-ID")
# The player half of README.md: from the top to the first "## Build" heading.
$readme = Get-Content (Join-Path $Root "README.md")
$cut = [Array]::IndexOf(($readme | ForEach-Object { $_ -match '^## Build' }), $true)
if ($cut -lt 0) { $cut = $readme.Count }
$readme[0..([Math]::Max($cut - 1, 0))] | Set-Content -Encoding UTF8 (Join-Path $Dist "README.md")
Copy-Item (Join-Path $Root "RELEASE-NOTES.txt") (Join-Path $Dist "RELEASE-NOTES.txt")   # the README's What's new links it
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $Dist "LICENSE")
Copy-Item -Recurse (Join-Path $Root "fixtures\padlogs") (Join-Path $Dist "fixtures\padlogs")
# The repository's own mods (mods\<id>\mod.json; the compiled-in C mods excepted): the Arabic text-only language today
Get-ChildItem (Join-Path $Root "mods") -Directory | ForEach-Object {
    $mj = Join-Path $_.FullName "mod.json"
    if ((Test-Path $mj) -and -not ((Get-Content $mj -Raw) -match '"kind":\s*"builtin"|"api":\s*"c"')) {
        Copy-Item -Recurse $_.FullName (Join-Path (Join-Path $Dist "mods") $_.Name)
    }
}
# Licence texts: the engine's committed set + vcpkg's copyright files for the two ports.
$licSrc = Join-Path $Engine "tools\dist\licenses"
foreach ($f in @("SDL2-LICENSE.txt", "FFmpeg-COPYING.LGPLv2.1.txt")) { Copy-Item (Join-Path $licSrc $f) (Join-Path $Dist "licenses\$f") }
Copy-Item (Join-Path $Root "FFmpeg-LICENSE.md") (Join-Path $Dist "licenses\FFmpeg-LICENSE.md")
foreach ($port in @("sdl2", "ffmpeg")) {
    $cr = Join-Path $BuildDir "vcpkg_installed\x64-windows\share\$port\copyright"
    if (Test-Path $cr) { Copy-Item $cr (Join-Path $Dist "licenses\vcpkg-$port-copyright.txt") }
}
# The FFmpeg DLL must be an LGPL build: the configuration string libavcodec embeds, read from the shipped DLL.
$avcodec = Get-ChildItem -Path $Dist -Filter "avcodec*.dll" -File | Select-Object -First 1
if (-not $avcodec) { throw "release_dist.ps1: no avcodec DLL beside the exe (vcpkg applocal deployment missing?)" }
$bytes = [System.IO.File]::ReadAllBytes($avcodec.FullName)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
$conf = [regex]::Matches($text, '--prefix=[\x20-\x7e]{0,4000}') | ForEach-Object { $_.Value } | Sort-Object -Unique
if ($conf.Count -ne 1) { Remove-Item -Recurse -Force $Dist; throw "release_dist.ps1: REFUSING to ship -- $($avcodec.Name) embeds $($conf.Count) FFmpeg configuration strings, expected 1" }
if ($conf[0] -match '--enable-gpl|--enable-nonfree|--enable-libx26|--enable-version3') {
    Remove-Item -Recurse -Force $Dist
    throw "release_dist.ps1: REFUSING to ship -- the FFmpeg in $($avcodec.Name) is a GPL/non-free/LGPLv3 build: $($conf[0])"
}
@("FFmpeg as linked (dynamic, $($avcodec.Name)).", "", "libavcodec's configuration string embedded in the shipped DLL (avcodec_configuration()):", $conf[0]) |
    Set-Content -Encoding UTF8 (Join-Path $Dist "licenses\FFmpeg-BUILD-CONFIGURATION.txt")
@(
    "# TwinsanityEvolved -- third-party notices -- $((Get-Content (Join-Path $Dist 'BUILD-ID'))[0])",
    "",
    "This folder ships the game program (the twinsanity-reversed decompilation compiled to wasm32 and lowered to native",
    "code) and the NativeEE engine. It contains NO game",
    "data: no textures, audio, models, movies or disc bytes -- you supply your own disc image.",
    "",
    "| component | how it is linked | licence | text |",
    "|---|---|---|---|",
    "| TwinsanityEvolved (this port's code) | the binary | MIT | LICENSE |",
    "| twinsanity-reversed (the decompilation, game/) | compiled into the binary | twinsanity-reversed | https://github.com/Smartkin/twinsanity-reversed |",
    "| NativeEE (engine/) | static | MIT | the engine repository's LICENSE |",
    "| SDL2 (vcpkg) | dynamic, SDL2.dll | zlib | licenses/SDL2-LICENSE.txt, licenses/vcpkg-sdl2-copyright.txt |",
    "| FFmpeg (libavcodec, libavutil, libswscale; vcpkg, features avcodec + swscale) | dynamic DLLs | LGPL v2.1+ (no GPL / non-free component) | licenses/FFmpeg-LICENSE.md, licenses/FFmpeg-COPYING.LGPLv2.1.txt, licenses/FFmpeg-BUILD-CONFIGURATION.txt |"
) | Set-Content -Encoding UTF8 (Join-Path $Dist "THIRD_PARTY_NOTICES.md")

Step "asset scan over the staged folder (zero findings)"
$rc = Invoke-Native { & python (Join-Path $Root "tools\ci\asset_scan.py") $Dist }
if ($rc -ne 0) { throw "release_dist.ps1: asset scan found game data in the staged folder ($rc)" }

if (-not $NoZip) {
    Step "zip"
    $zipName = "twinsanityevolved-$Platform"
    $zip = Join-Path $DistDir "$zipName.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    $staging = Join-Path $DistDir $zipName
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Copy-Item -Recurse $Dist $staging
    Compress-Archive -Path $staging -DestinationPath $zip
    Remove-Item -Recurse -Force $staging
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    "$hash  $zipName.zip" | Set-Content -Encoding ASCII "$zip.sha256"
    Get-Content "$zip.sha256"
}
Write-Host ""
Write-Host "release_dist.ps1: OK -> $Dist"
