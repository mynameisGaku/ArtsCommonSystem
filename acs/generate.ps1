# SPDX-License-Identifier: Apache-2.0
# ACS — ゲーム制作用ソリューション生成スクリプト
#
# 役割:
#   初めて ACS を触る人が迷わないよう、表層には名前付き .slnx を 1 つだけ置く。
#   既定では starter game (55_HelloScene2D) + Engine だけを生成し、
#   Solution Explorer の root filter は Engine / Game の 2 つだけにする。
#
#   .vcxproj / obj / .lib / FetchContent は以下に隔離し、hidden 属性 + .gitignore 済み:
#     Binaries/     … 実行ファイル + 配布 DLL
#     Intermediate/ … CMake cache / .vcxproj / obj / .lib / FetchContent の _deps
#     Saved/        … このスクリプトのログ
#
# ビルドに必須の外部ライブラリ (ImGui / stb / cgltf / ufbx / dr_libs 等) は
# 初回 configure 時に FetchContent が自動ダウンロード + リンクする。
#
# 使い方:
#   .\generate.ps1                 # 標準構成で生成 (DX12 raw + starter game)
#   .\generate.ps1 -Name MyGame    # MyGame.slnx を表層に生成
#   .\generate.ps1 -Open           # 生成して VS で開く
#   .\generate.ps1 -Clean          # Intermediate を消してから生成
#   .\generate.ps1 -AllSamples     # 全 sample を Game filter に追加
#   .\generate.ps1 -Sample 38_HelloFullGame -Name MyShooter
#   .\generate.ps1 -Tests -Tools   # tests/tools も Engine 配下に追加
#   .\generate.ps1 -AllBackends    # 任意 backend (Lua/Steamworks/ONNX/OpenXR/…) も全部 ON
#   .\generate.ps1 -Onnx -OpenXr   # 個別に backend を ON
#   .\generate.ps1 -Diligent       # Diligent RHI backend も追加
#
# ※ generate.bat をダブルクリックすると -Open 付きで本スクリプトを呼ぶ。
[CmdletBinding()]
param(
    [switch]$Open,
    [switch]$Clean,
    [Alias("Name", "ProjectName", "GameName")]
    [string]$SolutionName = "ACSGame",
    [string]$Sample = "55_HelloScene2D",
    [string]$StartupProject = "hello_scene2d",
    [switch]$AllSamples,
    [switch]$Tests,
    [switch]$Tools,
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

# Keep a valid Windows Path for CMake/MSBuild child processes. Some shells inject
# odd PATH/Path combinations; resetting the process `Path` from machine + user
# scope keeps tools discoverable without relying on the caller's transient env.
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
    if (-not $name -or $name.Trim().Length -eq 0) { $name = "ACSGame" }
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

function Get-SampleExecutableTarget([string]$sampleDir) {
    $cmake = Join-Path $proj "samples\$sampleDir\CMakeLists.txt"
    if (-not (Test-Path $cmake)) { return "" }
    $m = Select-String -LiteralPath $cmake -Pattern 'add_executable\(\s*([A-Za-z_][A-Za-z0-9_]*)' |
        Select-Object -First 1
    if (-not $m) { return "" }
    return $m.Matches[0].Groups[1].Value
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
    # Convert inherited ACEs to explicit ACEs for generated trees, then remove
    # inherited deny-delete ACEs such as Everyone:(DENY)(DC). MSBuild writes
    # .tlog files via temporary-file replacement; deny-delete breaks that.
    & $icacls @inheritArgs | Out-Null
    & $icacls @removeArgs | Out-Null
}

function Repair-VisibleGeneratedAcl([string]$path) {
    if (-not (Test-Path $path)) { return }
    $icacls = Join-Path $env:SystemRoot "System32\icacls.exe"
    if (-not (Test-Path $icacls)) { return }
    # The visible .slnx lives at the ACS surface. If the parent directory keeps
    # an inherited deny-delete-child ACE, users cannot remove/rename generated
    # solutions. Repair only this directory, not the source tree recursively.
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

if (-not $AllSamples -and $Sample -ne "55_HelloScene2D" -and $StartupProject -eq "hello_scene2d") {
    $detectedStartup = Get-SampleExecutableTarget $Sample
    if ($detectedStartup) { $StartupProject = $detectedStartup }
}

$cmakeArgs = @(
    "-S", $proj,
    "-B", $inter,
    "-G", $Generator,
    "-DACS_RENDER_DX12_RAW=ON",
    "-DACS_BUILD_SAMPLES=ON",
    "-DACS_BUILD_TESTS=$(if ($Tests) { 'ON' } else { 'OFF' })",
    "-DACS_BUILD_TOOLS=$(if ($Tools) { 'ON' } else { 'OFF' })",
    "-DACS_STARTUP_PROJECT=$StartupProject"
)
if ($AllSamples) {
    $cmakeArgs += "-DACS_ONLY_SAMPLE="
} else {
    $cmakeArgs += "-DACS_ONLY_SAMPLE=$Sample"
}
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
