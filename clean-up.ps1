# SPDX-License-Identifier: Apache-2.0
# ACS clean-up — scan the tree for regenerable junk, show what was found,
# ask Y/N, then delete the found set. Nothing is deleted before you confirm.
#
# Everything targeted here is gitignored and recreated by a build or by
# acs\generate.bat, so source / assets / tracked files are never touched.
#
# Run via clean-up.bat (double-click) or directly:
#   .\clean-up.ps1        scan, list findings, ask Y/N, delete
#   .\clean-up.ps1 -y     delete the found set immediately (no prompt)
[CmdletBinding()]
param([Alias('y')][switch]$Yes)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$acs  = Join-Path $root 'acs'

# --- what counts as junk ----------------------------------------------------
$dirNames     = @('Binaries','Intermediate','Saved','x64','.vs','out','build')  # matched anywhere
$dirGlobs     = @('cmake-build-*')                                              # matched anywhere
$rootOnlyDirs = @('b38','sln','sln-test')                                       # repo root only
$looseRootExt = @('.log','.pdb','.tmp','.ilk')                                  # top level of root
$looseAcsExt  = @('.log','.pdb','.obj','.tmp','.ilk')                           # top level of acs
$namedFiles   = @(
    (Join-Path $root 'acs_memdump.bmp'), (Join-Path $root 'acs_memdump.svg'),
    (Join-Path $root 'configure.log'),   (Join-Path $root 'build-out.log'),
    (Join-Path $root 'vc140.pdb'),
    (Join-Path $root 'persist_progress.sav'), (Join-Path $root 'persist_settings.ini'),
    (Join-Path $acs  'build-out.log'),   (Join-Path $acs  'configure.log'),
    (Join-Path $acs  'src\persist_progress.sav'), (Join-Path $acs 'src\persist_settings.ini')
)

$found = New-Object System.Collections.Generic.List[object]
$seen  = New-Object System.Collections.Generic.HashSet[string]

function Add-Hit([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    $item = Get-Item -LiteralPath $path -Force
    $full = $item.FullName
    if (-not $seen.Add($full.ToLower())) { return }
    if ($item.PSIsContainer) {
        $size = (Get-ChildItem -LiteralPath $full -Recurse -File -Force -EA SilentlyContinue |
                 Measure-Object Length -Sum).Sum
    } else { $size = $item.Length }
    if (-not $size) { $size = 0 }
    $found.Add([pscustomobject]@{ Path = $full; Size = [int64]$size; IsDir = [bool]$item.PSIsContainer })
}

# Recurse to find build/IDE dirs at every level. Prune at a match (don't walk
# a doomed tree) and never descend into .git.
function Scan-Dir([string]$dir) {
    foreach ($c in (Get-ChildItem -LiteralPath $dir -Directory -Force -EA SilentlyContinue)) {
        $name = $c.Name
        $hit  = $dirNames -contains $name
        if (-not $hit) { foreach ($g in $dirGlobs) { if ($name -like $g) { $hit = $true; break } } }
        if ($hit)            { Add-Hit $c.FullName; continue }
        if ($name -eq '.git') { continue }
        Scan-Dir $c.FullName
    }
}

Write-Host ""
Write-Host "=========================================================================="
Write-Host " ACS clean-up — scanning for regenerable junk"
Write-Host " root: $root"
Write-Host "=========================================================================="
Write-Host " scanning..."

Scan-Dir $root
foreach ($d in $rootOnlyDirs) { Add-Hit (Join-Path $root $d) }
foreach ($f in $namedFiles)   { Add-Hit $f }
# Match loose files by exact .Extension, NOT -Filter: on Windows -Filter '*.tmp'
# also matches 8.3 short names (e.g. a 'foo.tmpl' template -> short name FOO~1.TMP),
# which would wrongly flag real files. Extension comparison is exact.
Get-ChildItem -LiteralPath $root -File -Force -EA SilentlyContinue |
    Where-Object { $looseRootExt -contains $_.Extension.ToLower() } | ForEach-Object { Add-Hit $_.FullName }
if (Test-Path $acs) {
    Get-ChildItem -LiteralPath $acs -File -Force -EA SilentlyContinue |
        Where-Object { $looseAcsExt -contains $_.Extension.ToLower() } | ForEach-Object { Add-Hit $_.FullName }
}
# NOTE: -Include is silently IGNORED when combined with -LiteralPath (it would
# then return every file). Filter on .Name with Where-Object instead.
Get-ChildItem -LiteralPath $root -Recurse -File -Force -EA SilentlyContinue |
    Where-Object { ($_.Name -eq 'Thumbs.db' -or $_.Name -eq 'ehthumbs.db') -and $_.FullName -notmatch '\\\.git\\' } |
    ForEach-Object { Add-Hit $_.FullName }

function Format-Size([int64]$b) {
    if ($b -ge 1GB) { return ('{0,8:N1} GB' -f ($b / 1GB)) }
    if ($b -ge 1MB) { return ('{0,8:N1} MB' -f ($b / 1MB)) }
    if ($b -ge 1KB) { return ('{0,8:N1} KB' -f ($b / 1KB)) }
    return ('{0,9} B' -f $b)
}

if ($found.Count -eq 0) {
    Write-Host ""
    Write-Host " Nothing to clean — the tree is already tidy."
    Write-Host ""
    return
}

$total = ($found | Measure-Object Size -Sum).Sum
Write-Host ""
Write-Host (" Found {0} item(s), {1} total:" -f $found.Count, (Format-Size $total).Trim())
Write-Host ""
foreach ($h in ($found | Sort-Object @{ e = { -not $_.IsDir } }, Path)) {
    $tag = if ($h.IsDir) { '[DIR ]' } else { '[file]' }
    $rel = $h.Path
    if ($rel.ToLower().StartsWith($root.ToLower())) { $rel = '.' + $rel.Substring($root.Length) }
    Write-Host ("   {0} {1}  {2}" -f $tag, (Format-Size $h.Size), $rel)
}
Write-Host ""

if (-not $Yes) {
    $ans = Read-Host (" Delete these {0} item(s)? (Y/N)" -f $found.Count)
    if ($ans -notmatch '^[Yy]') { Write-Host " Aborted. Nothing was deleted."; Write-Host ""; return }
}

Write-Host ""
Write-Host " deleting..."
$del = 0; $freed = [int64]0; $failed = 0
foreach ($h in $found) {
    if (-not (Test-Path -LiteralPath $h.Path)) { continue }   # parent already removed
    try {
        Remove-Item -LiteralPath $h.Path -Recurse -Force -ErrorAction Stop
        $del++; $freed += $h.Size
    } catch {
        Write-Host ("   [FAILED] {0}`n            {1}" -f $h.Path, $_.Exception.Message)
        $failed++
    }
}
Write-Host ""
Write-Host (" Deleted {0} item(s), freed {1}." -f $del, (Format-Size $freed).Trim())
if ($failed) { Write-Host (" {0} item(s) could not be removed (locked / in use)." -f $failed) }
Write-Host " Run  acs\generate.bat  to recreate the solution."
Write-Host ""
