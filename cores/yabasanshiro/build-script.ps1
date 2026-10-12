# Build the (patched) YabaSanshiro libretro core for Android arm64 (Quest 3) and install it into the
# Unity project. YabaSanshiro is the HW-OpenGL Saturn core — full-speed on Quest where Beetle Saturn
# is CPU-bound (~88%). We build from source (not the buildbot binary) to carry our light-gun patch
# (the upstream libretro glue exposes only pad/3D-pad/wheel/mouse — see cores/yabasanshiro-patches/).
#
# SOURCE: github.com/libretro/yabause, branch **yabasanshiro** (NOT master — master is the old
# software core; the HW-GL core the buildbot ships as "yabasanshiro" lives on this branch).
#
# Usage:
#   pwsh cores/build-yabasanshiro-android.ps1 [-Src C:\ybs]
#
# HARD-WON GOTCHAS (why this script looks the way it does):
#   1. ndk-build (make-based) FAILS if the NDK path contains spaces ("Program Files"), so we junction
#      Unity's NDK to a space-free path (C:\ndk27).
#   2. The SOURCE tree must ALSO be at a space-free path (this repo lives under "My Apps") — ndk-build
#      chokes on spaces in the project path too. Default clone/build dir is C:\ybs.
#   3. Unity ships NDK r27; the buildbot built with r22. r27's `my-dir` resolves RELATIVE, and this
#      Makefile bakes $(LOCAL_PATH) into LOCAL_SRC_FILES, so ndk-build double-prepends the path
#      (`jni/jni/../../Counter.cpp: No rule to make target`). Passing ABSOLUTE NDK_PROJECT_PATH /
#      APP_BUILD_SCRIPT / NDK_APPLICATION_MK makes my-dir absolute and fixes it.
#   4. ndk-build must be run through `cmd /c` with its output redirected to a log INSIDE that cmd,
#      never invoked directly with its stream left in the PowerShell pipeline. The compile emits
#      warnings (deprecated non-prototype definitions in bundled zlib, etc.) on stderr, and PS 5.1
#      wraps native-command stderr as ERROR RECORDS — which $ErrorActionPreference = 'Stop' then makes
#      fatal, aborting a build that is actually succeeding. The build-yabasanshiro-windows.ps1 sibling
#      has always redirected inside bash for exactly this reason; this script did not, and aborted
#      partway on every run until fixed. `cmd /c` still propagates the real exit code, so a GENUINE
#      ndk-build failure is still caught by the $LASTEXITCODE check below.

param(
    [string]$Src = 'C:\ybs'   # space-free source/build dir (see gotcha #2)
)
$ErrorActionPreference = 'Stop'

$branch  = 'yabasanshiro'
$pin     = 'f448097b69a6037246a08e9dc09eabaa420d7893'  # pinned upstream commit (what the shipped core was built from)
$repo    = 'https://github.com/libretro/yabause.git'
$ndkReal = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK'
$ndkLink = 'C:\ndk27'
$projRoot = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu'
$androidDest = Join-Path $projRoot 'Assets\Plugins\Android\libs\arm64-v8a\libyabasanshiro_libretro_android.so'
$patchDir = Join-Path $PSScriptRoot 'yabasanshiro-patches'

# 0. Clone the correct branch if missing (space-free path).
if (-not (Test-Path $Src)) {
    Write-Host "Cloning $branch branch to $Src ..."
    git clone --depth 1 --branch $branch $repo $Src
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}
# 0a. Pin: make sure the clone is at the pinned commit (the branch tip moves; builds must not).
if ((git -C $Src rev-parse HEAD) -ne $pin) {
    git -C $Src fetch --depth 1 origin $pin
    if ($LASTEXITCODE -ne 0) { throw "git fetch $pin failed" }
    git -C $Src checkout --force $pin
    if ($LASTEXITCODE -ne 0) { throw "git checkout $pin failed" }
}
$core = Join-Path $Src 'yabause\src\libretro'
if (-not (Test-Path (Join-Path $core 'jni\Android.mk'))) { throw "libretro jni not found under $core" }

# 0b. Reset the clone to pristine upstream BEFORE copying patches, so a source file that was patched in
# a previous run (or a patch file since removed from cores/yabasanshiro-patches) can't linger in the
# clone and still get built (reproducibility). Matches the build-mesen-*.ps1 pattern; this is a shallow
# branch clone, so reset to the cloned HEAD.
git -C $Src reset --hard
if ($LASTEXITCODE -ne 0) { throw "git reset failed" }
git -C $Src clean -fd

# 1. Copy our patched sources over the clone (reproducible — no fragile inline patching).
if (Test-Path $patchDir) {
    Write-Host "Applying yabasanshiro-patches ..."
    Get-ChildItem -Recurse -File $patchDir | ForEach-Object {
        $rel = $_.FullName.Substring($patchDir.Length).TrimStart('\')
        $dst = Join-Path $Src $rel
        Copy-Item $_.FullName $dst -Force
        Write-Host "  patched $rel"
    }
}

# 2. Junction the NDK to a space-free path (gotcha #1).
if (-not (Test-Path $ndkLink)) {
    Write-Host "Junctioning NDK -> $ndkLink ..."
    New-Item -ItemType Junction -Path $ndkLink -Target $ndkReal | Out-Null
}

# 3. Build arm64-v8a with ABSOLUTE paths (gotcha #3), redirecting output to a log INSIDE cmd (gotcha #4).
Remove-Item -Recurse -Force (Join-Path $core 'obj'), (Join-Path $core 'libs') -ErrorAction SilentlyContinue
$coreU = $core -replace '\\','/'
$log = "$coreU/build_android.log"
# No quoting needed around any path here: the NDK junction (gotcha #1) and the source tree (gotcha #2)
# are both space-free by construction, and the log lives inside the source tree. Keep it that way — a
# path with a space would need cmd's awkward nested-quote escaping.
$cmd = "$ndkLink\ndk-build.cmd NDK_PROJECT_PATH=$coreU APP_BUILD_SCRIPT=$coreU/jni/Android.mk " +
       "NDK_APPLICATION_MK=$coreU/jni/Application.mk APP_ABI=arm64-v8a -j8 > $log 2>&1"
Write-Host "Building (ndk-build arm64-v8a) -> $log ..."
Push-Location $core
try {
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "ndk-build failed ($LASTEXITCODE) - see $log" }
} finally { Pop-Location }

# 4. Install into the Unity project (arm64 .so, lib-prefixed soname).
$built = Join-Path $core 'libs\arm64-v8a\libretro.so'
if (-not (Test-Path $built)) { throw "build produced no libretro.so" }
Copy-Item $built $androidDest -Force
Write-Host "`nInstalled -> $androidDest ($((Get-Item $androidDest).Length) bytes)"
Write-Host "Restart Unity or Build-And-Run to pick up the new core."
