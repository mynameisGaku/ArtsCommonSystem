# SPDX-License-Identifier: Apache-2.0
# ACS — Engineソリューション生成スクリプト
#
# 役割:
#   Engine moduleを中心とした名前付き .slnx を表層へ1つ生成する。
#   testsとtoolsは明示optionで追加し、生成物をsource treeから分離する。
#
#   .vcxproj / obj / .lib / FetchContent は以下に隔離し、hidden 属性 + .gitignore 済み:
#     Binaries/     … 実行ファイル + 配布 DLL
#     Intermediate/ … CMake cache / .vcxproj / obj / .lib / FetchContent の _deps
#     Saved/        … このスクリプトのログ
#
# ビルドに必須の外部libraryはconfigure中にFetchContentが解決してlinkする。
#
# -Openは生成後にsolutionを開き、-CleanはIntermediateを再生成前に除去する。
# -Tests/-Toolsは追加targetを登録し、backend switchは対応moduleを有効化する。
# -DistributionSmokeは物理DistributionRootのlink/run gateをsolutionへ追加する。
# generate.batは-Openを付けてこの処理へ委譲する。
[CmdletBinding()]
param(
    [switch]$Open,
    [switch]$Clean,
    [Alias("Name", "ProjectName", "GameName")]
    [string]$SolutionName = "ACSEngine",
    [string]$StartupProject = "",
    [switch]$Tests,
    [switch]$Tools,
    [switch]$DistributionSmoke,
    [string]$DistributionRoot = "",
    [string]$Generator = "Visual Studio 18 2026",
    [switch]$Diligent,
    [switch]$Scripting,
    [switch]$Steamworks,
    [switch]$Onnx,
    [switch]$OpenXr,
    [switch]$CrashReporter,
    [switch]$Telemetry,
    [switch]$Matchmaker,
    [switch]$AllBackends,
    [string[]]$CMakeArguments = @()
)

$ErrorActionPreference = "Stop"

# CMake/MSBuild の子プロセス向けに有効な Windows Path を維持する。一部 shell は
# PATH/Path の不整合を持ち込むため、machine + user scope から process の `Path` を
# 再構築し、呼び出し元の一時的な環境だけに依存せずツールを見つけられるようにする。
$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
$userPath    = [Environment]::GetEnvironmentVariable("Path", "User")
$processPath = [Environment]::GetEnvironmentVariable("Path", "Process")
$pathParts = @($machinePath, $userPath, $processPath) |
    Where-Object { $_ -and $_.Trim().Length -gt 0 }
if ($pathParts.Count -gt 0) {
    $env:Path = ($pathParts -join ";")
}

$proj  = $PSScriptRoot
$inter = Join-Path $proj "Intermediate\vs"
$saved = Join-Path $proj "Saved"
$bin   = Join-Path $proj "Binaries"

function Convert-ToSolutionFileName([string]$name) {
    if (-not $name -or $name.Trim().Length -eq 0) { $name = "ACSEngine" }
    $invalid = [System.IO.Path]::GetInvalidFileNameChars()
    $out = New-Object System.Text.StringBuilder
    foreach ($ch in $name.Trim().ToCharArray()) {
        if ($invalid -contains $ch) { [void]$out.Append("_") }
        else { [void]$out.Append($ch) }
    }
    $file = $out.ToString()
    if ($file.EndsWith(".slnx", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $file
    }
    if ($file.EndsWith(".sln", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $file
    }
    return "$file.slnx"
}

function Write-SurfaceSolution([string]$source, [string]$dest, [string]$buildRel) {
    $text = Get-Content -LiteralPath $source -Raw
    $prefix = $buildRel.Replace("\", "/")
    $pattern = '((?:Path|Project)=")([^":<>]+?\.vcxproj)"'
    $rewritten = [regex]::Replace($text, $pattern, {
        param($m)
        $p = $m.Groups[2].Value.Replace("\", "/")
        if ($p -match '^(?:[A-Za-z]:|/|\\\\)' -or $p.StartsWith("$prefix/")) {
            return $m.Value
        }
        return $m.Groups[1].Value + $prefix + "/" + $p + '"'
    })
    Set-Content -LiteralPath $dest -Value $rewritten -Encoding UTF8
}

function Repair-GeneratedAcl([string]$path) {
    if (-not (Test-Path $path)) { return }
    $icacls = Join-Path $env:SystemRoot "System32\icacls.exe"
    if (-not (Test-Path $icacls)) { return }
    $item = Get-Item -LiteralPath $path -Force
    $inheritArgs = @($path, "/inheritance:d")
    $removeArgs = @($path, "/remove:d", "Everyone")
    if ($item.PSIsContainer) {
        $inheritArgs += "/T"
        $removeArgs += "/T"
    }
    # 生成ツリーの継承 ACE を明示 ACE へ変換し、Everyone:(DENY)(DC) などの
    # 削除拒否 ACE を外す。MSBuild は一時ファイル置換で .tlog を書くため、
    # 削除拒否が残るとビルドを妨げる。
    & $icacls @inheritArgs | Out-Null
    & $icacls @removeArgs | Out-Null
}

function Repair-VisibleGeneratedAcl([string]$path) {
    if (-not (Test-Path $path)) { return }
    $icacls = Join-Path $env:SystemRoot "System32\icacls.exe"
    if (-not (Test-Path $icacls)) { return }
    # 表示用 .slnx は ACS の表層に置く。親 directory に子削除拒否 ACE が
    # 継承されていると生成済み solution を削除・改名できないため、この directory
    # だけを修復し、source tree 全体は再帰的に変更しない。
    & $icacls $path /inheritance:d | Out-Null
    & $icacls $path /remove:d Everyone | Out-Null
}

if ($Clean -and (Test-Path $inter)) {
    Write-Host "[generate] cleaning $inter" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $inter
}
New-Item -ItemType Directory -Force -Path $inter, $saved, $bin | Out-Null
Repair-VisibleGeneratedAcl $proj
Repair-GeneratedAcl (Join-Path $proj "Intermediate")
Repair-GeneratedAcl $saved
Repair-GeneratedAcl $bin

$cmakeArgs = @(
    "-S", (Join-Path $proj "engine"),
    "-B", $inter,
    "-G", $Generator,
    "-DACS_RENDER_DX12_RAW=ON",
    "-DACS_BUILD_TESTS=$(if ($Tests) { 'ON' } else { 'OFF' })",
    "-DACS_BUILD_TOOLS=$(if ($Tools) { 'ON' } else { 'OFF' })",
    "-DACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE=$(if ($DistributionSmoke) { 'ON' } else { 'OFF' })"
)
if ($StartupProject) { $cmakeArgs += "-DACS_STARTUP_PROJECT=$StartupProject" }
if ($DistributionRoot) {
    $cmakeArgs += "-DACS_DISTRIBUTION_CONSUMER_ROOT=$DistributionRoot"
}
if ($Diligent      -or $AllBackends) { $cmakeArgs += "-DACS_RENDER_DILIGENT=ON"; $cmakeArgs += "-DACS_Render_DILIGENT=ON" }
# ↑ Render モジュール feature (ACS_Render_DILIGENT → WITH_RENDER_DILIGENT 定義) も明示 ON にする。これが無いと
#   再 configure 時に stale cache の ACS_Render_DILIGENT=OFF が残り、editor の render lib が Diligent の
#   CreateRhiDevice をリンクせず raw-DX12 のまま (= engine IBL/SSGI が使えない) になる。
if ($Scripting     -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_SCRIPTING=ON" }
if ($Steamworks    -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_STEAMWORKS=ON" }
if ($Onnx          -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_ML_ONNX=ON" }
if ($OpenXr        -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_OPENXR=ON" }
if ($CrashReporter -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_CRASH_REPORTER=ON" }
if ($Telemetry     -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_TELEMETRY_FILE=ON" }
if ($Matchmaker    -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_LOCAL_MATCHMAKER=ON" }
if ($CMakeArguments) { $cmakeArgs += @($CMakeArguments) }

Write-Host "[generate] cmake $($cmakeArgs -join ' ')" -ForegroundColor Cyan
$log = Join-Path $saved "generate.log"
& cmake @cmakeArgs | Tee-Object -FilePath $log
$cmakeExitCode = $LASTEXITCODE
if ($cmakeExitCode -ne 0) {
    Write-Host "[generate] CMake configure failed (exit $cmakeExitCode). See $log" -ForegroundColor Red
    exit $cmakeExitCode
}

# 生成物フォルダを hidden に (エクスプローラの既定では隠れ、表示設定で見える)。
foreach ($d in @($bin, (Join-Path $proj "Intermediate"), $saved)) {
    if (Test-Path $d) { attrib +H $d }
}

# 既存のビルド/IDE ノイズ (Rider の cmake-build-*、.vs、.idea、screenshots 等) も
# 表層から隠す。中身は削除しない (表示設定で見える)。
$noiseExact = @(".vs", ".idea", "out", "build", "screenshots")
Get-ChildItem $proj -Directory -Force | Where-Object {
    $_.Name -like "cmake-build-*" -or ($noiseExact -contains $_.Name)
} | ForEach-Object { attrib +H $_.FullName }
# 表層に転がる生成ログ / IDE ユーザ設定ファイルも隠す。
Get-ChildItem $proj -File -Force | Where-Object {
    $_.Extension -in @(".log", ".user", ".suo")
} | ForEach-Object { attrib +H $_.FullName }

$sln = Get-ChildItem -Path $inter -Filter "ACS.sln*" -File | Select-Object -First 1
if (-not $sln) {
    Write-Error "[generate] solution file not found under $inter"
    exit 1
}
$surfaceName = Convert-ToSolutionFileName $SolutionName
$surfaceSln = Join-Path $proj $surfaceName
Write-SurfaceSolution $sln.FullName $surfaceSln "Intermediate/vs"
Repair-GeneratedAcl $surfaceSln
Write-Host "[generate] solution: $surfaceSln" -ForegroundColor Green
Write-Host "[generate] projects:  $inter" -ForegroundColor DarkGray
if ($Open) {
    Write-Host "[generate] opening in Visual Studio..." -ForegroundColor Green
    Start-Process $surfaceSln
}
