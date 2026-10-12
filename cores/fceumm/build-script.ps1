# Build the (patched) FCEUmm libretro core for Android arm64 (Quest 3) and install it into the
# Unity project. FCEUmm is the NES core (Zapper light gun). We build from source (not the buildbot
# binary) to carry our off-screen zapper patch: the stock core parks an off-screen shot at PIXEL
# (0,0), so "shoot away from screen" only works when a game happens to draw dark pixels at the
# top-left corner — PAL Barker Bill's Trick Shooting doesn't, so off-screen shots read as on-screen
# HITS there. Our patch (cores/fceumm-patches/) parks the aim outside the raster (0xFFFF,0xFFFF)
# so an off-screen pull can never sense light in any game/region.
#
# SOURCE: github.com/libretro/libretro-fceumm, PINNED to a known-good commit (reproducible builds,
# same pattern as SwanStation). Bump the pin deliberately when updating the core, then re-verify
# the patch file still matches upstream (it's a full-file snapshot, not a diff).
#
# Same NDK gotchas as build-yabasanshiro-android.ps1: space-free NDK junction + source path,
# absolute NDK_PROJECT_PATH/APP_BUILD_SCRIPT/NDK_APPLICATION_MK for NDK r27's relative my-dir.
# PLUS one of its own: fceumm links HUNDREDS of mapper/board objects, so the final link line blows
# Windows' 32K CreateProcess limit ("make (e=87): The parameter is incorrect") — APP_SHORT_COMMANDS
# makes ndk-build use linker list files instead. Required, not optional.
#
# Usage:
#   pwsh cores/build-fceumm-android.ps1 [-Src C:\fceumm]

param(
    [string]$Src = 'C:\fceumm'   # space-free source/build dir (shared with the Windows build)
)
$ErrorActionPreference = 'Stop'

$pin     = 'afe65ef1b4328c0bf2df05fb47a6b31a45bccf47'  # pinned upstream commit (2026-07 master)
$repo    = 'https://github.com/libretro/libretro-fceumm.git'
$ndkReal = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK'
$ndkLink = 'C:\ndk27'
$projRoot = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu'
$androidDest = Join-Path $projRoot 'Assets\Plugins\Android\libs\arm64-v8a\libfceumm_libretro_android.so'
$patchDir = Join-Path $PSScriptRoot 'fceumm-patches'

# 0. Clone at the pinned commit if missing (space-free path).
if (-not (Test-Path $Src)) {
    Write-Host "Cloning fceumm to $Src (pin $pin) ..."
    git clone $repo $Src
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    git -C $Src checkout $pin
    if ($LASTEXITCODE -ne 0) { throw "git checkout $pin failed" }
}
if (-not (Test-Path (Join-Path $Src 'jni\Android.mk'))) { throw "jni\Android.mk not found under $Src" }

# 1. Copy our patched sources over the clone (reproducible — no fragile inline patching).
if (Test-Path $patchDir) {
    Write-Host "Applying fceumm-patches ..."
    Get-ChildItem -Recurse -File $patchDir | ForEach-Object {
        $rel = $_.FullName.Substring($patchDir.Length).TrimStart('\')
        Copy-Item $_.FullName (Join-Path $Src $rel) -Force
        Write-Host "  patched $rel"
    }
}

# 2. Junction the NDK to a space-free path.
if (-not (Test-Path $ndkLink)) {
    Write-Host "Junctioning NDK -> $ndkLink ..."
    New-Item -ItemType Junction -Path $ndkLink -Target $ndkReal | Out-Null
}

# 3. Build arm64-v8a with ABSOLUTE paths.
Remove-Item -Recurse -Force (Join-Path $Src 'obj'), (Join-Path $Src 'libs') -ErrorAction SilentlyContinue
$srcU = $Src -replace '\\','/'
Push-Location $Src
try {
    & "$ndkLink\ndk-build.cmd" NDK_PROJECT_PATH="$srcU" APP_BUILD_SCRIPT="$srcU/jni/Android.mk" `
        NDK_APPLICATION_MK="$srcU/jni/Application.mk" APP_ABI=arm64-v8a APP_SHORT_COMMANDS=true -j8
    if ($LASTEXITCODE -ne 0) { throw "ndk-build failed ($LASTEXITCODE)" }
} finally { Pop-Location }

# 4. Install into the Unity project (arm64 .so, lib-prefixed soname).
$built = Join-Path $Src 'libs\arm64-v8a\libretro.so'
if (-not (Test-Path $built)) { throw "build produced no libretro.so" }
Copy-Item $built $androidDest -Force
Write-Host "`nInstalled -> $androidDest ($((Get-Item $androidDest).Length) bytes)"
Write-Host "Build-And-Run to pick up the new core."
