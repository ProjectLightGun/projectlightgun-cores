# Build the PATCHED Flycast libretro core for Android arm64 (Quest 3) and install it into the
# Unity project. The libretro buildbot Android core crashes on Android 14 — see ./README.md
# and flycast-android14-libandroid.patch (the one-line posix_vmem mmap fix this build applies).
#
# Usage:
#   pwsh cores/build-flycast-android.ps1 [-Src <flycast clone dir>]
#
# If -Src is omitted it defaults to ..\..\flycast-build relative to the repo. The script:
#   1. (re)applies the mmap patch if not already present,
#   2. configures + builds with Unity's bundled NDK r27 + CMake/Ninja,
#   3. works around the flaky Windows ninja final-link by re-running the link command manually,
#   4. copies the result to Assets/Plugins/Android/libs/arm64-v8a/libflycast_libretro_android.so.

param(
    [string]$Src = (Join-Path $PSScriptRoot '..\..\flycast-build')
)
$ErrorActionPreference = 'Stop'

$ap    = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer'
$ndk   = Join-Path $ap 'NDK'
$cmk   = Join-Path $ap 'SDK\cmake\3.22.1\bin\cmake.exe'
$ninja = Join-Path $ap 'SDK\cmake\3.22.1\bin\ninja.exe'
$tool  = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
$clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$sys   = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot'

if (-not (Test-Path $Src)) {
    throw "Flycast source not found at $Src. Clone it first:`n" +
          "  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/flyinghead/flycast.git `"$Src`""
}

# 0. Pin: the shipped core was built from this upstream commit. Refuse to build from anything else so
#    the published source always matches the binary (submodules must match too: git submodule update --init --recursive).
$pin = 'c0f2cf4319d6c77d577599906ca0a90627a3afc8'
$head = (git -C $Src rev-parse HEAD)
if ($head -ne $pin) { throw "Flycast clone is at $head but the pinned commit is $pin. Run: git -C `"$Src`" fetch origin $pin; git -C `"$Src`" checkout $pin; git -C `"$Src`" submodule update --init --recursive" }

# 1. Ensure the Android fix is present (libandroid link in the libretro CMake target).
$marker = Join-Path $Src 'CMakeLists.txt'
if (-not (Select-String -Path $marker -Pattern 'libandroid resolves ASharedMemory_create' -Quiet)) {
    Write-Host "Applying flycast-android14-libandroid.patch ..."
    Push-Location $Src
    & git apply (Join-Path $PSScriptRoot 'flycast-android14-libandroid.patch')
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "git apply failed" }
    Pop-Location
} else {
    Write-Host "mmap fix already present."
}

# 1b. Copy the patched sources over the clone (full-file snapshots, same reproducible pattern as
#     SwanStation/Mesen/FCEUmm/yabasanshiro — see cores/README.md). Currently: the light-gun aim
#     origin fix in core/hw/pvr/spg.cpp (anchors the gun to the real picture start VO_STARTY instead
#     of a game-programmable interrupt line, which drifted per game and per region).
#     Re-capture dev edits with cores/snapshot-flycast-patches.ps1.
$patchRoot = Join-Path $PSScriptRoot 'flycast-patches'
if (Test-Path $patchRoot) {
    Get-ChildItem -Path $patchRoot -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($patchRoot.Length).TrimStart('\', '/')
        $dst = Join-Path $Src $rel
        if (-not (Test-Path $dst)) { throw "Patch target missing in clone: $rel" }
        Write-Host "  patch -> $rel"
        Copy-Item $_.FullName $dst -Force
    }
} else {
    Write-Host "No flycast-patches folder; building stock sources."
}

# 2. Configure + build.
$bld = Join-Path $Src 'build-android'
& $cmk -B $bld -S $Src -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tool" `
  "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-29" "-DANDROID_NDK=$ndk" `
  "-DLIBRETRO=ON" "-DCMAKE_BUILD_TYPE=Release"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

# Build everything (compiles all objects + archives). If ninja fails only at the final link — the
# flaky bit on Windows — that's fine; we relink manually below. So don't throw on its exit code.
& $cmk --build $bld --config Release -j
$soOut = Join-Path $bld 'flycast_libretro.so'

# 3. Final link, manually, from the rsp ninja generated. This is deterministic, unlike the ninja
#    link step which intermittently emits bogus "undefined symbol" errors on Windows.
$rsp = Join-Path $bld 'CMakeFiles\flycast_libretro.rsp'
if (Test-Path $rsp) {
    Write-Host "Relinking manually (deterministic) ..."
    # The rsp lists object/library paths relative to the build dir, so run the link from there.
    Push-Location $bld
    try {
        & $clang "--target=aarch64-none-linux-android29" "--sysroot=$sys" -fPIC `
          -static-libstdc++ "-Wl,--build-id=sha1" "-Wl,--no-rosegment" "-Wl,--no-undefined-version" `
          "-Wl,--fatal-warnings" "-Wl,--no-undefined" -Qunused-arguments "-Wl,--gc-sections" -s `
          -shared "-Wl,-soname,flycast_libretro.so" -o 'flycast_libretro.so' '@CMakeFiles/flycast_libretro.rsp'
        if ($LASTEXITCODE -ne 0) { throw "manual link failed" }
    } finally { Pop-Location }
}
if (-not (Test-Path $soOut)) { throw "no output .so produced at $soOut" }

# 4. Install into the Unity project (Android .so must be lib-prefixed).
$dest = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu\Assets\Plugins\Android\libs\arm64-v8a\libflycast_libretro_android.so'
Copy-Item $soOut $dest -Force
Write-Host "`nInstalled patched Flycast core -> $dest"
Get-Item $dest | Select-Object FullName, Length
