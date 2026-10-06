<#
.SYNOPSIS
    One-shot toolchain setup for GoBot.

.DESCRIPTION
    This machine has Npcap installed but no C++ toolchain. This script installs
    what is missing and leaves the tree ready for build.ps1:

      * Visual Studio 2022 Build Tools (MSVC v143 + Windows SDK)  ~3 GB
      * CMake
      * Npcap SDK  (headers + wpcap.lib)  -> third_party/npcap-sdk   ~1 MB

    Already-present components are skipped. Safe to re-run.

.PARAMETER SkipBuildTools
    Don't touch Visual Studio. Use when you already have a C++ toolchain that
    vswhere can't see, or you intend to build with clang-cl / MinGW.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuildTools
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn2{ param($m) Write-Host "    $m" -ForegroundColor Yellow }

# --------------------------------------------------------------------- winget
Write-Step "Checking prerequisites"
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from the Microsoft Store, then re-run."
}
Write-Ok "winget present"

$freeGB = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
Write-Ok "C: has $freeGB GB free"
if ($freeGB -lt 8 -and -not $SkipBuildTools) {
    Write-Warn2 "Build Tools needs roughly 4-6 GB. This may be tight."
}

# ------------------------------------------------------- VS 2022 Build Tools
function Test-MsvcPresent {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $found = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    return [bool]$found
}

if ($SkipBuildTools) {
    Write-Step "Visual Studio Build Tools -- skipped by request"
} elseif (Test-MsvcPresent) {
    Write-Step "Visual Studio Build Tools"
    Write-Ok "MSVC C++ toolset already installed"
} else {
    Write-Step "Installing Visual Studio 2022 Build Tools (this takes a while)"
    $vsArgs = @(
        '--add', 'Microsoft.VisualStudio.Workload.VCTools',
        '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
        '--includeRecommended',
        '--quiet', '--wait', '--norestart'
    ) -join ' '

    winget install --id Microsoft.VisualStudio.2022.BuildTools `
        --accept-source-agreements --accept-package-agreements `
        --override $vsArgs

    if (-not (Test-MsvcPresent)) {
        throw "Build Tools install finished but vswhere still reports no C++ toolset. Open the Visual Studio Installer and confirm the 'Desktop development with C++' workload is checked."
    }
    Write-Ok "MSVC installed"
}

# ------------------------------------------------------------------ CMake
Write-Step "CMake"
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    Write-Ok "already on PATH: $((Get-Command cmake).Source)"
} else {
    winget install --id Kitware.CMake --accept-source-agreements --accept-package-agreements
    $cmakeBin = "$env:ProgramFiles\CMake\bin"
    if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
    Write-Ok "installed (open a new shell to pick up PATH)"
}

# -------------------------------------------------------------- Npcap SDK
Write-Step "Npcap SDK"
$sdkDir = Join-Path $root 'third_party\npcap-sdk'
if (Test-Path (Join-Path $sdkDir 'Include\pcap.h')) {
    Write-Ok "already unpacked at third_party\npcap-sdk"
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $sdkDir) | Out-Null
    $zip = Join-Path $env:TEMP 'npcap-sdk.zip'
    $url = 'https://npcap.com/dist/npcap-sdk-1.13.zip'
    Write-Ok "downloading $url"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $sdkDir -Force
        Remove-Item $zip -Force
        Write-Ok "unpacked to third_party\npcap-sdk"
    } catch {
        Write-Warn2 "Automatic download failed: $($_.Exception.Message)"
        Write-Warn2 "Download the SDK zip from https://npcap.com/#download and unpack"
        Write-Warn2 "it so that this path exists: $sdkDir\Include\pcap.h"
    }
}

# ------------------------------------------------------- Npcap runtime check
Write-Step "Npcap runtime"
$svc = Get-Service npcap -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Ok "npcap service running"
} elseif ($svc) {
    Write-Warn2 "npcap service present but $($svc.Status). Start it with: Start-Service npcap"
} else {
    Write-Warn2 "Npcap runtime not installed. Get it from https://npcap.com/#download"
    Write-Warn2 "Tick 'Install Npcap in WinPcap API-compatible Mode' during setup."
}

Write-Step "Done"
Write-Host @"
    Next:
      1. Open a NEW terminal (so PATH changes apply)
      2. .\build.ps1
      3. Run the capture as Administrator - Npcap needs it.
"@ -ForegroundColor Green
