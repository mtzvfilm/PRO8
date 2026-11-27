# SPDX-License-Identifier: GPL-3.0-or-later
# Builds plugin\build\PRO8-<version>-windows-x64.zip from a finished Release build (see README.md):
#   PRO8.vst3\ (bundle folder), PRO8.clap, licenses, install notes.
#   powershell -ExecutionPolicy Bypass -File plugin\make_zip_win.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$V = "1.0.3"
$build = Join-Path $here "build"
$out = Join-Path $build "out"

function Find-One($pattern) {
    $hit = Get-ChildItem -Path $out -Recurse -Filter $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit) { $hit = Get-ChildItem -Path $build -Recurse -Filter $pattern -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch "\\_deps\\|\\CMakeFiles\\" } | Select-Object -First 1 }
    if (-not $hit) { throw "not found: $pattern (build the Release configuration first)" }
    return $hit
}
$vst3 = Find-One "PRO8.vst3"   # the bundle folder (Contents\x86_64-win\PRO8.vst3 inside)
$clap = Find-One "PRO8.clap"
if ($vst3.PSIsContainer -eq $false) { throw "PRO8.vst3 should be a bundle folder, found a file: $($vst3.FullName)" }

$stage = Join-Path $build "zip\PRO8-$V-windows-x64"
if (Test-Path (Join-Path $build "zip")) { Remove-Item -Recurse -Force (Join-Path $build "zip") }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Recurse $vst3.FullName (Join-Path $stage "PRO8.vst3")
Copy-Item $clap.FullName (Join-Path $stage "PRO8.clap")
Copy-Item (Join-Path $here "..\LICENSE") (Join-Path $stage "LICENSE.txt")

# third-party licenses + the WebView2 SDK loader (statically linked on Windows)
$tpl = Get-Content -Raw (Join-Path $here "resources\THIRD_PARTY_LICENSES.txt")
$wv = Get-ChildItem -Path (Join-Path $build "_deps") -Recurse -Filter "LICENSE.txt" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "webview2" } | Select-Object -First 1
if ($tpl -match "WebView2") { }   # already in resources\THIRD_PARTY_LICENSES.txt
elseif ($wv) {
    $sep = "=" * 80
    $tpl += "`r`n`r`n$sep`r`nMicrosoft Edge WebView2 SDK (WebView2LoaderStatic, Windows build only)`r`n$sep`r`n`r`n" + (Get-Content -Raw $wv.FullName)
} else { Write-Warning "WebView2 SDK LICENSE.txt not found under build\_deps: add it to THIRD_PARTY_LICENSES.txt by hand" }
Set-Content -Path (Join-Path $stage "THIRD_PARTY_LICENSES.txt") -Value $tpl -Encoding UTF8

@"
PRO8 $V for Windows (x64) - 8-voice analog polysynth, VST3 + CLAP
DSP by mtzvfilm - ig: mtzv_film - https://github.com/mtzvfilm/PRO8

Install
  VST3: copy the folder PRO8.vst3 to  C:\Program Files\Common Files\VST3\
  CLAP: copy PRO8.clap to             C:\Program Files\Common Files\CLAP\
  then rescan plugins in your DAW.

The panel uses the Microsoft Edge WebView2 Runtime (built into Windows 10 / 11).
If the panel stays empty, install it: https://developer.microsoft.com/microsoft-edge/webview2/

Presets live in %APPDATA%\PRO8\Banks (one .syx per bank A..Z); settings in %APPDATA%\PRO8\settings.json.

PRO8 is free software under the GNU GPL v3.0 (LICENSE.txt); third-party licenses in THIRD_PARTY_LICENSES.txt.
Not affiliated with Behringer / Music Tribe; Behringer and PRO-800 are trademarks of their respective owners.
"@ | Set-Content -Path (Join-Path $stage "README.txt") -Encoding UTF8

$zip = Join-Path $build "PRO8-$V-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# zip with forward slashes (Compress-Archive in Windows PowerShell 5 writes backslashes, which other unzip tools turn
# into file names instead of folders): Windows 10+ tar (bsdtar), else .NET with explicit entry names
$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if ($tar) { Push-Location (Split-Path -Parent $stage); & $tar.Source -a -c -f $zip (Split-Path -Leaf $stage); Pop-Location }
else {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $root = Split-Path -Parent $stage
    $za = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
    Get-ChildItem -Path $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $rel) | Out-Null
    }
    $za.Dispose()
}
Write-Host "built $zip"
