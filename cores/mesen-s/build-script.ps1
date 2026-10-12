# Build the (patched) Mesen-S libretro core for Android arm64 (Quest 3) and install it into the
# Unity project. Mesen-S is the SNES core (Super Scope light gun; GPLv3, ships in the paid build —
# it replaced non-commercial snes9x). We build from source (not the buildbot binary) to carry our
# Super Scope aim patch: the stock core latches the beam position at pos.X + 10, but games read that
# back as the raw PPU horizontal dot counter (OPHCT) whose origin is the active-display start
# (~+40 dots) — so every shot landed ~0.23 (of half-width) LEFT of aim and the rightmost ~12% of the
# screen was unhittable (our pointer can't exceed the frame edge, so no host-side calibration could
# recover it). Our patch (cores/mesen-patches/) raises that offset to +40. See SuperScope.h.
#
# SOURCE: github.com/libretro/Mesen-S, PINNED to a known-good commit (reproducible builds, same
# pattern as SwanStation/FCEUmm). Bump the pin deliberately when updating the core, then re-verify
# the patch files still match upstream (they are full-file snapshots, not diffs).
#
# Same NDK gotchas as build-fceumm-android.ps1: space-free NDK junction + source path, absolute
# NDK_PROJECT_PATH/APP_BUILD_SCRIPT/NDK_APPLICATION_MK for NDK r27's relative my-dir. Mesen's makefiles
# live under Libretro/jni/ (NOT the repo root like fceumm). APP_SHORT_COMMANDS=true is kept as a
# precaution: Mesen links ~200 objects, which can blow Windows' 32K CreateProcess limit at link time
# ("make (e=87)") the same way fceumm does.
#
# Usage:
#   pwsh cores/build-mesen-android.ps1 [-Src C:\mesen-s]

param(
    [string]$Src = 'C:\mesen-s'   # space-free source/build dir (shared with the Windows build)
)
$ErrorActionPreference = 'Stop'

$pin     = '1d475abd174d16ecb1fb030961ff26076ab51ee6'  # pinned upstream commit (libretro/Mesen-S master, 2026-07)
$repo    = 'https://github.com/libretro/Mesen-S.git'
$ndkReal = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK'
$ndkLink = 'C:\ndk27'
$projRoot = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu'
$androidDest = Join-Path $projRoot 'Assets\Plugins\Android\libs\arm64-v8a\libmesen-s_libretro_android.so'
$patchDir = Join-Path $PSScriptRoot 'mesen-patches'

# 0. Clone at the pinned commit if missing (space-free path).
if (-not (Test-Path $Src)) {
    Write-Host "Cloning Mesen-S to $Src (pin $pin) ..."
    git clone $repo $Src
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    git -C $Src checkout $pin
    if ($LASTEXITCODE -ne 0) { throw "git checkout $pin failed" }
}
if (-not (Test-Path (Join-Path $Src 'Libretro\jni\Android.mk'))) { throw "Libretro\jni\Android.mk not found under $Src" }

# 0b. Reset the clone to pristine pinned upstream BEFORE copying patches. Otherwise a patch file that
# was removed from cores/mesen-patches (e.g. an abandoned approach) lingers in the clone and still gets
# built — reproducibility requires the tree match ONLY the current patch set.
git -C $Src reset --hard $pin
if ($LASTEXITCODE -ne 0) { throw "git reset failed" }
git -C $Src clean -fd

# 1. Copy our patched sources over the clone (reproducible — no fragile inline patching).
if (Test-Path $patchDir) {
    Write-Host "Applying mesen-patches ..."
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

# 3. Build arm64-v8a with ABSOLUTE paths (Mesen's Android.mk is under Libretro/jni/).
Remove-Item -Recurse -Force (Join-Path $Src 'obj'), (Join-Path $Src 'libs') -ErrorAction SilentlyContinue
$srcU = $Src -replace '\\','/'
Push-Location $Src
try {
    & "$ndkLink\ndk-build.cmd" NDK_PROJECT_PATH="$srcU" APP_BUILD_SCRIPT="$srcU/Libretro/jni/Android.mk" `
        NDK_APPLICATION_MK="$srcU/Libretro/jni/Application.mk" APP_ABI=arm64-v8a APP_SHORT_COMMANDS=true -j8
    if ($LASTEXITCODE -ne 0) { throw "ndk-build failed ($LASTEXITCODE)" }
} finally { Pop-Location }

# 4. Install into the Unity project (arm64 .so, lib-prefixed soname).
$built = Join-Path $Src 'libs\arm64-v8a\libretro.so'
if (-not (Test-Path $built)) { throw "build produced no libretro.so" }
Copy-Item $built $androidDest -Force
Write-Host "`nInstalled -> $androidDest ($((Get-Item $androidDest).Length) bytes)"
Write-Host "Build-And-Run to pick up the new core."
