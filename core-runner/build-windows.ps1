# Build the core runner for Windows (x86_64) -> core-runner/build/core-runner.exe
# Used for testing on the PC (and, from S5, by the Unity Editor). Same MinGW-w64 toolchain as the
# FCEUmm/YabaSanshiro Windows builds (C:\mingw64, winlibs UCRT posix).
#
# Usage:  powershell -File core-runner/build-windows.ps1
$ErrorActionPreference = 'Stop'
$gcc = 'C:\mingw64\bin\gcc.exe'
if (-not (Test-Path $gcc)) { throw "MinGW-w64 not found at C:\mingw64 (see cores/build-yabasanshiro-windows.ps1 for the install steps)." }
$src = Join-Path $PSScriptRoot 'src'
$out = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'core-runner.exe'
$log = Join-Path $out 'build-windows.log'
# Compiler output goes to a log inside cmd, so warnings on stderr can't fail the script (PS 5.1).
cmd /c "`"$gcc`" -O2 -Wall -Wextra -std=c11 -static -o `"$exe`" `"$src\runner.c`" `"$src\input.c`" `"$src\dump.c`" `"$src\gpu_test.c`" `"$src\hw_render.c`" -lws2_32 > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $log; throw "build failed ($LASTEXITCODE)" }
Write-Host "Built $exe ($((Get-Item $exe).Length) bytes)"
