# VS 2026 (Visual Studio 18) solution generator for ACS.
# ----------------------------------------------------------------------------
# Splits the build into one engine-only sln and one sln per sample.
# CMake produces one sln per configure, so we configure into multiple build
# dirs.
#
# Default output layout (under <repo>/sln/):
#   sln/engine/ACS.slnx                 (engine only, tests/samples OFF)
#   sln/01_HelloWindow/ACS.slnx         (engine + this one sample)
#   sln/02_HelloSprite/ACS.slnx
#   ...
#   sln/28_HelloGameFramework/ACS.slnx
#
# Usage:
#   .\scripts\gen-vs-solutions.ps1                                # all
#   .\scripts\gen-vs-solutions.ps1 -EngineOnly                    # engine only
#   .\scripts\gen-vs-solutions.ps1 -Sample 28_HelloGameFramework  # one sample
#   .\scripts\gen-vs-solutions.ps1 -NoDiligent                    # Diligent OFF
#   .\scripts\gen-vs-solutions.ps1 -OutDir D:\acs-sln              # custom out
#   .\scripts\gen-vs-solutions.ps1 -Generator "Visual Studio 17 2022"
#   .\scripts\gen-vs-solutions.ps1 -Force                         # wipe + redo

[CmdletBinding()]
param(
    [string]$Generator = "Visual Studio 18 2026",
    [string]$OutDir   = "",
    [string]$Sample   = "",
    [switch]$EngineOnly,
    [switch]$NoDiligent,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path "$ScriptDir\..").Path
$AcsRoot   = Join-Path $RepoRoot "acs"
if (-not (Test-Path "$AcsRoot\CMakeLists.txt")) {
    throw "ACS CMakeLists.txt not found at $AcsRoot"
}
if (-not $OutDir) {
    $OutDir = Join-Path $RepoRoot "sln"
}
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

$Diligent = if ($NoDiligent) { "OFF" } else { "ON" }

function Invoke-CmakeConfigure {
    param(
        [string]$BuildDir,
        [string[]]$ExtraArgs
    )
    $label = Split-Path -Leaf $BuildDir
    Write-Host ""
    Write-Host "================ $label ================" -ForegroundColor Cyan
    Write-Host "BuildDir: $BuildDir"

    if ($Force -and (Test-Path $BuildDir)) {
        Remove-Item -Recurse -Force $BuildDir
    }
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    } elseif (Test-Path "$BuildDir\CMakeCache.txt") {
        Remove-Item "$BuildDir\CMakeCache.txt" -Force
    }

    $cmakeArgs = @(
        "-S", $AcsRoot,
        "-B", $BuildDir,
        "-G", $Generator,
        "-DACS_RENDER_DX12_RAW=ON",
        "-DACS_RENDER_DILIGENT=$Diligent"
    ) + $ExtraArgs

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed for $label (exit $LASTEXITCODE)"
    }
}

function Get-SlnPath([string]$dir) {
    if (Test-Path (Join-Path $dir "ACS.slnx")) { return (Join-Path $dir "ACS.slnx") }
    if (Test-Path (Join-Path $dir "ACS.sln"))  { return (Join-Path $dir "ACS.sln") }
    return "(not found)"
}

# 1) Engine-only sln
$engineDir = Join-Path $OutDir "engine"
Invoke-CmakeConfigure -BuildDir $engineDir -ExtraArgs @(
    "-DACS_BUILD_SAMPLES=OFF",
    "-DACS_BUILD_TESTS=OFF"
)

if ($EngineOnly) {
    Write-Host ""
    Write-Host "Engine sln: $(Get-SlnPath $engineDir)" -ForegroundColor Green
    return
}

# 2) Per-sample sln
if ($Sample) {
    $samples = @($Sample)
} else {
    $samples = Get-ChildItem -Path (Join-Path $AcsRoot "samples") -Directory |
        Where-Object { $_.Name -match "^\d+_" } |
        Sort-Object Name |
        ForEach-Object { $_.Name }
}

foreach ($s in $samples) {
    $smpDir = Join-Path $OutDir $s
    Invoke-CmakeConfigure -BuildDir $smpDir -ExtraArgs @(
        "-DACS_BUILD_SAMPLES=ON",
        "-DACS_BUILD_TESTS=OFF",
        "-DACS_ONLY_SAMPLE=$s"
    )
}

Write-Host ""
Write-Host "All done. Solutions:" -ForegroundColor Green
Write-Host "  Engine: $(Get-SlnPath $engineDir)"
foreach ($s in $samples) {
    $smp = Join-Path $OutDir $s
    Write-Host "  Sample $s : $(Get-SlnPath $smp)"
}
