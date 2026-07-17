# SPDX-License-Identifier: Apache-2.0
# Regenerate the single-header ACS distribution into dist/:
#   - dist/acs.h               (via amalgamate.py)
#   - dist/lib/x64/Debug/acs.lib   and   dist/lib/x64/Release/acs.lib
#
# Prereqs: the engine must already be built for the configs you want, e.g.
#   cmake --build acs/Intermediate/vs --config Debug   -j
#   cmake --build acs/Intermediate/vs --config Release -j
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1
#   ... -Configs Debug          # only one config
#   ... -Deploy C:\acs          # also mirror dist/ to a consumer location
param([string[]]$Configs = @('Debug','Release'), [string]$Deploy = '')
$ErrorActionPreference = 'Stop'

$repo  = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $repo 'acs\Intermediate\vs'
$dist  = Join-Path $repo 'dist'

# locate lib.exe via vswhere
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$tv = (Get-Content (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt')).Trim()
$libexe = Join-Path $vs "VC\Tools\MSVC\$tv\bin\Hostx64\x64\lib.exe"
if (-not (Test-Path $libexe)) { throw "lib.exe not found: $libexe" }

# 1) amalgamate header
Write-Host "==> generating dist/acs.h"
python (Join-Path $PSScriptRoot 'amalgamate.py') --write

# 2) merge libs per config
foreach ($cfg in $Configs) {
    $libdir = Join-Path $build $cfg
    if (-not (Test-Path $libdir)) { Write-Warning "skip $cfg (not built: $libdir)"; continue }
    $libs = Get-ChildItem (Join-Path $libdir 'acs_*.lib') | Where-Object { $_.Name -ne 'acs_test.lib' } | ForEach-Object { $_.FullName }
    $imgui = Join-Path $libdir 'imgui.lib'
    if (Test-Path $imgui) { $libs += $imgui }
    # mimalloc-static は _deps 配下に独自名 (mimalloc-debug.lib 等) で出力される。
    # acs_memory が mi_* を参照するため、acs.lib へ統合して consumer 側の
    # リンク入力を増やさずに解決させる。
    $mimalloc = Get-ChildItem (Join-Path $build "_deps\acs_mimalloc-build\$cfg") -Filter 'mimalloc*.lib' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch 'redirect' } | Select-Object -First 1
    if ($mimalloc) { $libs += $mimalloc.FullName } else { Write-Warning "  mimalloc lib not found for ${cfg}" }
    $outdir = Join-Path $dist "lib\x64\$cfg"
    New-Item -ItemType Directory -Force -Path $outdir | Out-Null
    $outlib = Join-Path $outdir 'acs.lib'
    $rsp = Join-Path $env:TEMP "acs_merge_$cfg.rsp"
    Set-Content $rsp -Value (@('/NOLOGO', "/OUT:$outlib") + $libs) -Encoding ascii
    Write-Host "==> merging $($libs.Count) libs -> $outlib"
    & $libexe "@$rsp" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed for $cfg" }
    Write-Host ("    {0:N1} MB" -f ((Get-Item $outlib).Length/1MB))

    # bundle Diligent backend + xxhash static libs next to acs.lib so the
    # consumer's #pragma comment(lib,...) (see amalgamate.py banner) auto-links
    # them. Required because Sky/Atmosphere call CreateRhiComputePipeline and the
    # device factory GetEngineFactoryD3D12, both implemented only in Diligent.
    $diligentNames = @(
        'Diligent-Archiver-static','Diligent-BasicPlatform','Diligent-Common',
        'Diligent-GraphicsAccessories','Diligent-GraphicsEngine','Diligent-GraphicsEngineD3D12-static',
        'Diligent-GraphicsEngineD3DBase','Diligent-GraphicsEngineNextGenBase','Diligent-GraphicsTools',
        'Diligent-Primitives','Diligent-ShaderTools','Diligent-Win32Platform','xxhash')
    $depsRoot = Join-Path $build '_deps'
    $copied = 0
    foreach ($n in $diligentNames) {
        $src = Get-ChildItem $depsRoot -Recurse -File -Filter "$n.lib" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\$cfg\\" } | Select-Object -First 1
        if ($src) { Copy-Item $src.FullName (Join-Path $outdir "$n.lib") -Force; $copied++ }
        else { Write-Warning "  Diligent dep not found for ${cfg}: $n.lib" }
    }
    Write-Host "    bundled $copied/$($diligentNames.Count) Diligent/xxhash libs"
}

# 3) optional: mirror dist/ to a consumer location (e.g. C:\acs)
if ($Deploy) {
    Write-Host "==> deploying dist/ -> $Deploy"
    & robocopy $dist $Deploy /MIR /NJH /NJS /NDL /NFL /NP | Out-Null
    # robocopy exit codes 0..7 are success; >=8 is a real error
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Write-Host "    deployed."
}
Write-Host "done."
