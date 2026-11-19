# Builds the program and packs the release zip: dist\resurvivorvr-<version>.zip
# Usage (from the project folder):  powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version 1.0
# The zip holds only this project's files: never the game (no .bin / .cue / save states, no models made from it).
param([string]$Version = "dev")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# the American disc (NTSC, 60 Hz), VR only, no development tools, no weapon models built in
cmake -S . -B build_release -G "Visual Studio 17 2022" -A x64 -DPSX_PAL=OFF -DRESVR_RELEASE=ON -DPSXVR_DEV=OFF -DRESVR_EMBED_WEAPONS=OFF | Out-Null
cmake --build build_release --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$name = "resurvivorvr-$Version"
$out = Join-Path $root "dist\$name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\licenses" | Out-Null

Copy-Item build_release\Release\resurvivor.exe, build_release\Release\openxr_loader.dll, README.md, LICENSE, "Play in VR.bat" $out
Copy-Item third_party\openxr\share\doc\openxr\LICENSE "$out\licenses\OpenXR-loader-LICENSE.txt"

# safety: nothing of the game may end up in the zip
$bad = Get-ChildItem -Recurse $out | Where-Object { $_.Extension -in ".bin", ".cue", ".iso", ".st", ".mcd", ".img", ".chd" }
if ($bad) { throw "game files in the release: $($bad.Name -join ', ')" }

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Get-ChildItem -Recurse $out | Where-Object { -not $_.PSIsContainer } | ForEach-Object {
    "{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($out.Length + 1)
}
"-> $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
