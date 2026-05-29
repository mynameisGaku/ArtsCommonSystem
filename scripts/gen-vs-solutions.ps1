# SPDX-License-Identifier: Apache-2.0
# Compatibility wrapper.
#
# The old version of this script generated one engine solution plus one solution
# per sample under <repo>/sln/. The beginner-facing path now lives in
# acs/generate.ps1 and writes a named solution at the ACS surface:
#
#   acs/<SolutionName>.slnx
#
# Keep this wrapper so old shortcuts still work, but route everything through the
# single-solution generator.

[CmdletBinding()]
param(
    [string]$Generator = "Visual Studio 18 2026",
    [string]$OutDir   = "",
    [string]$Sample   = "",
    [string]$SolutionName = "",
    [switch]$EngineOnly,
    [switch]$Diligent,
    [switch]$NoDiligent,
    [switch]$Force,
    [switch]$Open,
    [switch]$AllBackends,
    [switch]$AllSamples,
    [switch]$Tests,
    [switch]$Tools
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path "$ScriptDir\..").Path
$Generate  = Join-Path $RepoRoot "acs\generate.ps1"

if ($OutDir) {
    Write-Warning "-OutDir is ignored. The visible solution is generated under acs/."
}
if ($EngineOnly) {
    Write-Warning "-EngineOnly is ignored. The beginner solution always contains Engine + Game."
}

$args = @("-Generator", $Generator)
if ($Force) { $args += "-Clean" }
if ($Open) { $args += "-Open" }
if ($SolutionName) { $args += @("-SolutionName", $SolutionName) }
elseif ($Sample) { $args += @("-SolutionName", $Sample) }
if ($Sample) { $args += @("-Sample", $Sample) }
if ($AllSamples) { $args += "-AllSamples" }
if ($Tests) { $args += "-Tests" }
if ($Tools) { $args += "-Tools" }
if ($Diligent -and -not $NoDiligent) { $args += "-Diligent" }
if ($AllBackends) { $args += "-AllBackends" }

& powershell -NoProfile -ExecutionPolicy Bypass -File $Generate @args
exit $LASTEXITCODE
