<#
.SYNOPSIS
    Configure + build GoBot.

.PARAMETER Config
    Debug or Release (default Release).

.PARAMETER Clean
    Wipe the build directory first.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'

if ($Clean -and (Test-Path $build)) {
    Write-Host "Removing $build" -ForegroundColor Yellow
    Remove-Item $build -Recurse -Force
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $candidate = "$env:ProgramFiles\CMake\bin"
    if (Test-Path "$candidate\cmake.exe") {
        $env:PATH = "$candidate;$env:PATH"
    } else {
        throw "cmake not found. Run .\bootstrap.ps1 first."
    }
}

Write-Host "==> Configuring ($Config)" -ForegroundColor Cyan
cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "==> Building" -ForegroundColor Cyan
cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$out = Join-Path $build "bin\$Config"
Write-Host "`n==> Built to $out" -ForegroundColor Green
Get-ChildItem $out -Filter *.exe | ForEach-Object { Write-Host "    $($_.Name)" }
Write-Host @"

    Run as Administrator (Npcap requires it):
      $out\gobot_sniff.exe --list
      $out\gobot_cli.exe --help
"@ -ForegroundColor Green
