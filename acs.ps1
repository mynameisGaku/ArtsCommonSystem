# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = "Stop"

$operations = Join-Path $PSScriptRoot "acs\scripts\project_operations.ps1"
if (-not (Test-Path -LiteralPath $operations -PathType Leaf)) {
    Write-Host "[acs] operation script was not found: $operations" -ForegroundColor Red
    exit 3
}

& $operations @args
$exitCode = $LASTEXITCODE
if ($null -eq $exitCode) {
    $exitCode = if ($?) { 0 } else { 1 }
}
exit ([int]$exitCode)
