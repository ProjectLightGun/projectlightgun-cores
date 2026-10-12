# Build a patched SwanStation libretro core for Android arm64 (Quest 3).
#
# SwanStation upstream only implements NamcoGunCon and reads light-gun aim through the desktop
# window/letterbox path. Our patches add:
#   - the Konami Justifier device (ported from DuckStation's pre-relicense GPLv2 commit d0949782),
#   - a Timer0 hblank gate so Konami games can read horizontal gun position,
#   - a direct normalised-aim -> active-display-area beam mapping (GunCon + Justifier), so aim is
#     resolution/region/aspect independent (no per-game calibration for GunCon), and
#   - the Justifier's baked systematic readback correction + edge-reachable (blanking-extended) mapping.
# Die Hard Trilogy, Lethal Enforcers 1 & 2, Area 51 etc. need the Justifier; they do not recognise
# GunCon and fall back to gamepad without it.
#
# HOW THE PATCHES ARE APPLIED (changed 2026-07): the full set of patched/new source files is snapshot
# in cores/swanstation-patches/ (real .cpp/.h files, version controlled). This script clones the
# upstream repo PINNED to the exact commit those files were based on, then COPIES the snapshot over the
# clone. This is robust (no fragile inline heredocs/regex) and reproduces every patch exactly. If you
# edit the tree in $Src directly during development, re-snapshot with:
#     cores/snapshot-swanstation-patches.ps1
#
# Usage:
#   powershell -File cores/build-swanstation-android.ps1 [-Src <swanstation clone dir>]
#
# If -Src is omitted it defaults to ..\..\swanstation-build relative to the repo root. An EXISTING
# $Src is treated as your working tree and left as-is (only built); a fresh run clones + copies.

# Output: cores/out/release/ (shipping core, no gun-diagnostics logger) and cores/out/dev/ (logger compiled in).
# Unity's Tools > Project Light Gun > Build Mode switch copies the matching one into Assets/Plugins.
param(
    [string]$Src = (Join-Path $PSScriptRoot '..\..\swanstation-build'),
    [ValidateSet('both','release','dev')][string]$Variant = 'both'   # 'dev' compiles the gun-diagnostics logger in (-DPLG_DEV)
)
$ErrorActionPreference = 'Stop'

# Upstream commit the snapshot in cores/swanstation-patches was based on. Pinned so the copied files
# always match the surrounding upstream source.
$PinnedCommit = '89309ccf3c5c97150777730778a820745669fc45'

$ap    = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer'
$ndk   = Join-Path $ap 'NDK'
$cmk   = Join-Path $ap 'SDK\cmake\3.22.1\bin\cmake.exe'
$ninja = Join-Path $ap 'SDK\cmake\3.22.1\bin\ninja.exe'
$tool  = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
$clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$sys   = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot'

# ---------------------------------------------------------------------------
# 1 + 2. Fresh clone (pinned) + copy the patched sources. Skipped if $Src already exists (your dev
#        tree is the source of truth then; re-snapshot to update cores/swanstation-patches).
# ---------------------------------------------------------------------------
$patchDir = Join-Path $PSScriptRoot 'swanstation-patches'
if (-not (Test-Path $Src)) {
    Write-Host "Cloning SwanStation (pinned to $PinnedCommit) ..."
    & git clone --recurse-submodules 'https://github.com/libretro/swanstation.git' $Src
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    & git -C $Src checkout $PinnedCommit
    if ($LASTEXITCODE -ne 0) { throw "git checkout $PinnedCommit failed (upstream history changed?)" }
    & git -C $Src submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "submodule update failed" }

    Write-Host "Applying source patches (copy from cores/swanstation-patches) ..."
    if (-not (Test-Path $patchDir)) { throw "patch snapshot not found: $patchDir" }
    Get-ChildItem -Path $patchDir -Recurse -File | ForEach-Object {
        $rel    = $_.FullName.Substring($patchDir.Length).TrimStart('\', '/')
        $target = Join-Path $Src $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        Write-Host "  patched $rel"
    }
} else {
    Write-Host "Using existing $Src (working tree; assumed already patched)."
}

# ---------------------------------------------------------------------------
# 3. Configure + build
# ---------------------------------------------------------------------------
# CMake/Ninja/clang write progress to stderr; under Windows PowerShell 5.1 + 'Stop' that is treated as
# a terminating error. Use 'Continue' here -- real failures are caught by the explicit $LASTEXITCODE /
# Test-Path checks below.
$ErrorActionPreference = 'Continue'
$variants = if ($Variant -eq 'both') { @('release','dev') } else { @($Variant) }
foreach ($variantName in $variants) {
    $isDev = ($variantName -eq 'dev')
    Write-Host "`n=== SwanStation Android: $variantName ==="
    $bld = Join-Path $Src $(if ($isDev) { 'build-android-dev' } else { 'build-android' })
    $devFlag = if ($isDev) { '-DPLG_DEV' } else { '' }  # X9: release cores carry no gun-diagnostics logger
    # Android: the NDK toolchain APPENDS to -DCMAKE_CXX_FLAGS (it ignores the CXXFLAGS env var), so pass it there. CMake only
    # reads it on a fresh cache, so drop the cache to make a Dev <-> release switch take effect.
        Remove-Item (Join-Path $bld 'CMakeCache.txt') -ErrorAction SilentlyContinue
    Write-Host "`nConfiguring SwanStation..."
    & $cmk -B $bld -S $Src -G Ninja `
      "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tool" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-29" "-DANDROID_NDK=$ndk" `
      "-DANDROID_STL=c++_static" `
        "-DCMAKE_CXX_FLAGS=$devFlag" `
      "-DCMAKE_BUILD_TYPE=Release"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }

    Write-Host "Building SwanStation (compile only -- link is done manually below)..."
    # Run ninja to compile all objects + archives; the ninja final link will fail because CMake's
    # generated link command omits libc++_static.a. That's expected -- we relink below.
    & $cmk --build $bld --config Release -j
    $soOut = Join-Path $bld 'swanstation_libretro_android.so'

    if (-not (Test-Path $soOut)) {
        # NOTE (2026-09-16): used to pass libc++_static.a/libc++abi.a as raw archive paths here. That
        # stopped resolving basic runtime symbols (operator new/delete, exceptions, even std::string
        # methods) even though both archives genuinely contain them (verified with llvm-nm) -- a minimal
        # standalone "hello world" linked the SAME way, by hand-listing those two archives, failed
        # identically, while letting clang's own driver resolve the C++ runtime via
        # -static-libstdc++ -stdlib=libc++ (no raw archive paths at all) succeeded immediately, for both
        # a plain executable and a -shared library. So this was never a SwanStation-specific problem or a
        # source-code regression (confirmed separately by rebuilding yesterday's exact unmodified code,
        # which failed the same way) -- manually listing those two specific archives by full path bypasses
        # some internal grouping/ordering the driver normally sets up for them. Fix: drop the raw paths,
        # let the driver flags below pull in the C++ runtime itself.
        Write-Host "Relinking manually (driver resolves the C++ runtime; see note above) ..."
        $objs = @(
            'src/libretro/CMakeFiles/swanstation_libretro.dir/libretro_audio_stream.cpp.o',
            'src/libretro/CMakeFiles/swanstation_libretro.dir/libretro_game_settings.cpp.o',
            'src/libretro/CMakeFiles/swanstation_libretro.dir/libretro_host_display.cpp.o',
            'src/libretro/CMakeFiles/swanstation_libretro.dir/libretro_host_interface.cpp.o',
            'src/libretro/CMakeFiles/swanstation_libretro.dir/libretro_settings_interface.cpp.o'
        )
        $libs = @(
            'src/core/libcore.a', 'src/common/libcommon.a',
            'dep/glad/libglad.a', 'dep/vulkan-loader/libvulkan-loader.a',
            'dep/libretro-common/liblibretro-common.a',
            'dep/libchdr/libchdr-static.a',
            'dep/libchdr/deps/lzma-25.01/libchdr-lzma.a',
            'dep/libchdr/deps/zstd-1.5.7/libzstd.a',
            'dep/libchdr/deps/miniz-3.1.1/libminiz.a',
            'dep/stb/libstb.a', 'dep/xxhash/libxxhash.a', 'dep/vixl/libvixl.a'
        )
        Push-Location $bld
        try {
            # This exact link command has been observed to fail intermittently (different missing symbol
            # each time, same inputs) and succeed on an immediate retry with nothing changed -- looks like
            # transient interference (e.g. AV scanning the freshly-written .a files) rather than anything
            # deterministic. Retry a few times before giving up for real.
            $attempt = 0
            do {
                $attempt++
                & $clang "--target=aarch64-none-linux-android29" "--sysroot=$sys" -fPIC `
                  -static-libstdc++ -stdlib=libc++ `
                  "-Wl,--build-id=sha1" "-Wl,--no-rosegment" "-Wl,--no-undefined-version" `
                  "-Wl,--no-undefined" -Qunused-arguments "-Wl,--gc-sections" -s `
                  -shared "-Wl,-soname,swanstation_libretro_android.so" `
                  -o 'swanstation_libretro_android.so' `
                  @objs @libs `
                  '-llog' '-ldl' '-latomic' '-lm'
                if ($LASTEXITCODE -ne 0 -and $attempt -lt 3) {
                    Write-Host "Link attempt $attempt failed (likely transient) -- retrying ..."
                    Start-Sleep -Seconds 2
                }
            } while ($LASTEXITCODE -ne 0 -and $attempt -lt 3)
            if ($LASTEXITCODE -ne 0) { throw "manual link failed after $attempt attempts" }
        } finally { Pop-Location }
    }
    if (-not (Test-Path $soOut)) { throw "no output .so produced at $soOut" }

    New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot "out\$variantName") | Out-Null
    Copy-Item $soOut (Join-Path $PSScriptRoot "out\$variantName\libswanstation_libretro_android.so") -Force
    Write-Host "Built $variantName core -> cores\out\$variantName"
}

# ---------------------------------------------------------------------------
# 4. Install: the release core becomes the default in Plugins; Unity's Build Mode switch swaps it.
# ---------------------------------------------------------------------------
$dest = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu\Assets\Plugins\Android\libs\arm64-v8a\libswanstation_libretro_android.so'
$rel0 = Join-Path $PSScriptRoot 'out\release\libswanstation_libretro_android.so'
if (Test-Path $rel0) { Copy-Item $rel0 $dest -Force; Write-Host "`nInstalled RELEASE core -> $dest (Build Mode > Dev in Unity swaps in the dev one)" }
