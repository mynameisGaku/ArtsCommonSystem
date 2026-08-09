# SPDX-License-Identifier: Apache-2.0
# ACS pack-assets — assets/ 配下を 1 個の .acpak (ACS アセットアーカイブ) に変換する。
#
# acs_assetpack CLI (acs/tools/acs_assetpack) を driver にして、assets/ を再帰
# スキャンし game.acpak にまとめる。CLI が未ビルドなら自動でビルドする。
#
# 使い方 (pack-assets.bat 経由 or 直接):
#   .\pack-assets.ps1                       assets/ -> game.acpak (LZ4 圧縮)
#   .\pack-assets.ps1 -In art -Out art.acpak
#   .\pack-assets.ps1 -Encrypt              AES-256-GCM 暗号化 (鍵は assets.key、無ければ生成)
#   .\pack-assets.ps1 -Encrypt -KeyFile my.key
#   .\pack-assets.ps1 -NoCompress           圧縮なしで生格納
#   .\pack-assets.ps1 -NoVerify             pack 後の verify を省略
[CmdletBinding()]
param(
    [string]$In        = 'assets',
    [string]$Out       = 'game.acpak',
    [switch]$Encrypt,
    [string]$KeyFile   = 'assets.key',
    [switch]$NoCompress,
    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$acs  = Join-Path $root 'acs'

function Fail([string]$msg) { Write-Host "[pack-assets] ERROR: $msg" -ForegroundColor Red; exit 1 }

# --- input dir -------------------------------------------------------------
$inDir = if ([System.IO.Path]::IsPathRooted($In)) { $In } else { Join-Path $root $In }
if (-not (Test-Path -LiteralPath $inDir -PathType Container)) {
    Fail "input folder not found: $inDir`n  (assets/ を作ってアセットを入れてから実行してください)"
}
$fileCount = @(Get-ChildItem -LiteralPath $inDir -Recurse -File -Force -EA SilentlyContinue).Count
if ($fileCount -eq 0) { Fail "input folder is empty: $inDir" }

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }

Write-Host ""
Write-Host "=========================================================================="
Write-Host " ACS pack-assets"
Write-Host "   input : $inDir  ($fileCount files)"
Write-Host "   output: $outPath"
Write-Host "   mode  : $(if($NoCompress){'store (no compress)'}else{'LZ4 compress'})$(if($Encrypt){' + AES-256-GCM encrypt'}else{''})"
Write-Host "=========================================================================="

# --- locate or build the acs_assetpack CLI ---------------------------------
function Find-Exe {
    $cands = @()
    $cands += Get-ChildItem -LiteralPath $acs -Recurse -File -Filter 'acs_assetpack.exe' -Force -EA SilentlyContinue |
              Where-Object { $_.FullName -notmatch '\\_deps\\' }
    if ($cands.Count -eq 0) { return $null }
    # newest build wins
    ($cands | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

$exe = Find-Exe
if (-not $exe) {
    Write-Host ""
    Write-Host "[pack-assets] acs_assetpack.exe が見つからないのでビルドします..." -ForegroundColor Yellow
    $bld = Join-Path $acs 'Intermediate\packtool'
    $gen = 'Visual Studio 18 2026'
    & cmake -S (Join-Path $acs 'engine') -B $bld -G $gen -A x64 `
        -DACS_BUILD_TESTS=OFF -DACS_BUILD_TOOLS=ON `
        -DACS_RENDER_DX12_RAW=ON -DACS_RENDER_DILIGENT=OFF
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed (exit $LASTEXITCODE)" }
    # NOTE: the exe target is acs_assetpack_cli (OUTPUT_NAME acs_assetpack.exe);
    # the bare name acs_assetpack is the library/module target, which would NOT
    # produce the executable.
    & cmake --build $bld --config Release --target acs_assetpack_cli
    if ($LASTEXITCODE -ne 0) { Fail "build failed (exit $LASTEXITCODE)" }
    $exe = Find-Exe
    if (-not $exe) { Fail "build succeeded but acs_assetpack.exe was not found" }
}
Write-Host "[pack-assets] tool: $exe"

# --- key handling (only when -Encrypt) -------------------------------------
$keyPath = $null
if ($Encrypt) {
    $keyPath = if ([System.IO.Path]::IsPathRooted($KeyFile)) { $KeyFile } else { Join-Path $root $KeyFile }
    if (-not (Test-Path -LiteralPath $keyPath)) {
        Write-Host "[pack-assets] 鍵ファイルが無いので 256-bit 鍵を生成します: $keyPath" -ForegroundColor Yellow
        $bytes = New-Object byte[] 32
        [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $hex = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
        # write WITHOUT BOM/newline so the CLI reads exactly 64 hex chars
        [System.IO.File]::WriteAllText($keyPath, $hex, (New-Object System.Text.ASCIIEncoding))
        Write-Host "[pack-assets] 鍵を生成しました。**この鍵は復号に必須・再生成不可・コミット禁止** です。" -ForegroundColor Yellow
    } else {
        Write-Host "[pack-assets] 既存の鍵ファイルを使用: $keyPath"
    }
}

# --- build argument list ---------------------------------------------------
$packArgs = @('pack', $inDir, $outPath)
if (-not $NoCompress) { $packArgs += '--compress' }
if ($Encrypt)         { $packArgs += @('--encrypt', '--key-file', $keyPath) }

Write-Host ""
Write-Host "[pack-assets] packing..."
& $exe @packArgs
if ($LASTEXITCODE -ne 0) { Fail "pack failed (exit $LASTEXITCODE)" }

# --- verify (round-trip integrity) -----------------------------------------
if (-not $NoVerify) {
    Write-Host ""
    Write-Host "[pack-assets] verifying..."
    $verifyArgs = @('verify', $outPath)
    if ($Encrypt) { $verifyArgs += @('--key-file', $keyPath) }
    & $exe @verifyArgs
    if ($LASTEXITCODE -ne 0) { Fail "verify failed (exit $LASTEXITCODE) — archive may be corrupt" }
}

Write-Host ""
Write-Host "[pack-assets] DONE -> $outPath" -ForegroundColor Green
if ($Encrypt) {
    Write-Host "[pack-assets] 復号には鍵ファイルが必要: $keyPath  (紛失すると復号不能)" -ForegroundColor Yellow
}
Write-Host ""
