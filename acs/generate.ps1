# SPDX-License-Identifier: Apache-2.0
# ACS — 単一ソリューション生成スクリプト
#
# 役割:
#   プロジェクト全体 (engine + 全 sample + tools + tests) を含む ACS.sln を
#   1 つだけ Intermediate/vs に生成する。ソース/アセットだけが表層に見えるよう、
#   生成物は以下に隔離し、いずれも hidden 属性 + .gitignore 済み:
#     Binaries/     … 実行ファイル + 配布 DLL
#     Intermediate/ … CMake cache / .vcxproj / obj / .lib / FetchContent の _deps
#     Saved/        … このスクリプトのログ
#
# ビルドに必須の外部ライブラリ (ImGui / stb / cgltf / ufbx / dr_libs 等) は
# 初回 configure 時に FetchContent が自動ダウンロード + リンクする。
#
# 使い方:
#   .\generate.ps1                 # 標準構成で生成 (DX12 raw + samples + tools + tests)
#   .\generate.ps1 -Open           # 生成して VS で開く
#   .\generate.ps1 -Clean          # Intermediate を消してから生成
#   .\generate.ps1 -AllBackends    # 任意 backend (Lua/Steamworks/ONNX/OpenXR/…) も全部 ON
#   .\generate.ps1 -Onnx -OpenXr   # 個別に backend を ON
#   .\generate.ps1 -Diligent       # Diligent RHI backend も追加
#
# ※ generate.bat をダブルクリックすると -Open 付きで本スクリプトを呼ぶ。
[CmdletBinding()]
param(
    [switch]$Open,
    [switch]$Clean,
    [string]$Generator = "Visual Studio 18 2026",
    [switch]$Diligent,
    [switch]$Scripting,
    [switch]$Steamworks,
    [switch]$Onnx,
    [switch]$OpenXr,
    [switch]$CrashReporter,
    [switch]$Telemetry,
    [switch]$Matchmaker,
    [switch]$AllBackends
)

$ErrorActionPreference = "Stop"
$proj  = $PSScriptRoot
$inter = Join-Path $proj "Intermediate\vs"
$saved = Join-Path $proj "Saved"
$bin   = Join-Path $proj "Binaries"

if ($Clean -and (Test-Path $inter)) {
    Write-Host "[generate] cleaning $inter" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $inter
}
New-Item -ItemType Directory -Force -Path $inter, $saved, $bin | Out-Null

$cmakeArgs = @(
    "-S", $proj,
    "-B", $inter,
    "-G", $Generator,
    "-DACS_RENDER_DX12_RAW=ON",
    "-DACS_BUILD_SAMPLES=ON",
    "-DACS_BUILD_TESTS=ON",
    "-DACS_BUILD_TOOLS=ON"
)
if ($Diligent      -or $AllBackends) { $cmakeArgs += "-DACS_RENDER_DILIGENT=ON" }
if ($Scripting     -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_SCRIPTING=ON" }
if ($Steamworks    -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_STEAMWORKS=ON" }
if ($Onnx          -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_ML_ONNX=ON" }
if ($OpenXr        -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_OPENXR=ON" }
if ($CrashReporter -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_CRASH_REPORTER=ON" }
if ($Telemetry     -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_TELEMETRY_FILE=ON" }
if ($Matchmaker    -or $AllBackends) { $cmakeArgs += "-DACS_BUILD_LOCAL_MATCHMAKER=ON" }

Write-Host "[generate] cmake $($cmakeArgs -join ' ')" -ForegroundColor Cyan
$log = Join-Path $saved "generate.log"
& cmake @cmakeArgs | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) {
    Write-Error "[generate] CMake configure failed (exit $LASTEXITCODE). See $log"
    exit $LASTEXITCODE
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
Write-Host "[generate] solution: $($sln.FullName)" -ForegroundColor Green
if ($Open) {
    Write-Host "[generate] opening in Visual Studio..." -ForegroundColor Green
    Start-Process $sln.FullName
}
