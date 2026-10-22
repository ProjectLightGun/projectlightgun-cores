# Build the core runner for the headset (Android arm64) -> core-runner/build/android/libcorerunner.so
#
# It is a normal Android EXECUTABLE (a PIE program, not a library). It is named lib*.so only so
# that, once it is packaged with the app (S4), Android unpacks it into the app's native library
# folder with execute permission - the same trick as libchdman.so (cores/build-chdman-android.ps1).
#
# Usage:  powershell -File core-runner/build-android.ps1
$ErrorActionPreference = 'Stop'
$ndkReal = 'C:\Program Files\Unity\Hub\Editor\6000.4.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK'
$ndkLink = 'C:\ndk27'   # space-free junction, shared with the core build scripts
if (-not (Test-Path $ndkLink)) { New-Item -ItemType Junction -Path $ndkLink -Target $ndkReal | Out-Null }
$clang = "$ndkLink\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
if (-not (Test-Path $clang)) { throw "NDK clang not found at $clang" }
$src = Join-Path $PSScriptRoot 'src'
$out = Join-Path $PSScriptRoot 'build\android'
New-Item -ItemType Directory -Force $out | Out-Null
$bin = Join-Path $out 'libcorerunner.so'
$log = Join-Path $out 'build-android.log'
cmd /c "`"$clang`" --target=aarch64-linux-android32 -O2 -Wall -Wextra -std=c11 -fPIE -pie -o `"$bin`" `"$src\runner.c`" `"$src\input.c`" `"$src\dump.c`" `"$src\gpu_test.c`" `"$src\hw_render.c`" -ldl -lEGL -lGLESv3 > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $log; throw "build failed ($LASTEXITCODE)" }
Write-Host "Built $bin ($((Get-Item $bin).Length) bytes)"

# Install into the Unity project so it is packaged with the app (next to libchdman.so). The .so is
# gitignored; its .meta is committed.
$dest = Join-Path $PSScriptRoot '..\unity\QuestLightgunEmu\Assets\Plugins\Android\libs\arm64-v8a\libcorerunner.so'
Copy-Item $bin $dest -Force
if (-not (Test-Path "$dest.meta")) {
    $guid = [guid]::NewGuid().ToString('N')
    Set-Content -Path "$dest.meta" -Value "fileFormatVersion: 2`nguid: $guid" -NoNewline -Encoding ascii
}
Write-Host "Installed -> $dest"
