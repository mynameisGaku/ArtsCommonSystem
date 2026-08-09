# SPDX-License-Identifier: Apache-2.0
# Visual Studio solution generation compatibility wrapper.
# acs/generate.ps1 へ backend、test、tool の選択を転送する。

[CmdletBinding()]
param(
    [string]$Generator = "Visual Studio 18 2026",
    [string]$OutDir   = "",
    [string]$SolutionName = "",
    [switch]$EngineOnly,
    [switch]$Diligent,
    [switch]$NoDiligent,
    [switch]$Force,
    [switch]$Open,
    [switch]$AllBackends,
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
    Write-Warning "-EngineOnly is unnecessary. The generated solution already contains Engine targets."
}

$args = @("-Generator", $Generator)
if ($Force) { $args += "-Clean" }
if ($Open) { $args += "-Open" }
if ($SolutionName) { $args += @("-SolutionName", $SolutionName) }
if ($Tests) { $args += "-Tests" }
if ($Tools) { $args += "-Tools" }
if ($Diligent -and -not $NoDiligent) { $args += "-Diligent" }
if ($AllBackends) { $args += "-AllBackends" }

& powershell -NoProfile -ExecutionPolicy Bypass -File $Generate @args
exit $LASTEXITCODE
