# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = "Stop"

# 条件違反を自己テスト失敗として報告する。
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

# 文字列内に期待値が含まれることを確認する。
function Assert-Contains([string]$Actual, [string]$Expected, [string]$Context) {
    if (-not $Actual.Contains($Expected)) {
        throw "$Context`nexpected: $Expected`nactual:`n$Actual"
    }
}

# 指定pathが存在しないことを確認する。
function Assert-PathMissing([string]$Path, [string]$Context) {
    if (Test-Path -LiteralPath $Path) {
        throw "$Context`nunexpected path: $Path"
    }
}

# shortcutを別processで実行し、表示と終了codeを返す。
function Invoke-Shortcut([string]$Shortcut, [string[]]$Arguments, [string]$WorkingDirectory) {
    $previousErrorAction = $ErrorActionPreference
    Push-Location $WorkingDirectory
    try {
        $ErrorActionPreference = "Continue"
        if ([System.IO.Path]::GetExtension($Shortcut) -eq ".ps1") {
            $powerShell = Join-Path $PSHOME "powershell.exe"
            $output = & $powerShell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $Shortcut @Arguments 2>&1
        } else {
            $output = & $Shortcut @Arguments 2>&1
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
        Pop-Location
    }
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        Output = (@($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }
}

# PowerShell対話入力と同じcommand parserを通してscriptを実行する。
function Invoke-PowerShellLine([string]$CommandLine, [string]$WorkingDirectory) {
    $previousErrorAction = $ErrorActionPreference
    Push-Location $WorkingDirectory
    try {
        $ErrorActionPreference = "Continue"
        $powerShell = Join-Path $PSHOME "powershell.exe"
        $output = & $powerShell -NoLogo -NoProfile -ExecutionPolicy Bypass -Command $CommandLine 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
        Pop-Location
    }
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        Output = (@($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }
}

# PowerShell単一引用符へ安全に埋め込めるliteralへ変換する。
function Convert-ToPowerShellSingleQuotedLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

# cmd.exeのcommand lineからshortcutを実行し、表示と終了codeを返す。
function Invoke-CmdLine([string]$CommandLine, [string]$WorkingDirectory) {
    $previousErrorAction = $ErrorActionPreference
    Push-Location $WorkingDirectory
    try {
        $ErrorActionPreference = "Continue"
        $output = & $env:ComSpec /d /v:off /c $CommandLine 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
        Pop-Location
    }
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        Output = (@($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }
}

# 実行結果の終了codeを確認する。
function Assert-ExitCode($Result, [int]$Expected, [string]$Context) {
    if ($Result.ExitCode -ne $Expected) {
        throw "$Context`nexpected exit: $Expected`nactual exit: $($Result.ExitCode)`noutput:`n$($Result.Output)"
    }
}

# fake native toolの受領argumentを行単位で読む。
function Read-ToolArguments([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        throw "tool logが生成されませんでした: $LogPath"
    }
    return @(Get-Content -LiteralPath $LogPath | Where-Object { $_ -like "ARG=*" })
}

# 一時rootだけを削除対象として許可する。
function Assert-SafeTemporaryRoot([string]$Path) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
    if (-not $resolvedPath.StartsWith($temporaryRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "一時directory外は削除できません: $resolvedPath"
    }
    if ([System.IO.Path]::GetFileName($resolvedPath) -notlike "acs-project-operations-self-test-*") {
        throw "自己テスト専用directoryではありません: $resolvedPath"
    }
}

$sourceAcsRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$sourceRepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $sourceAcsRoot ".."))
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("acs-project-operations-self-test-{0}" -f [guid]::NewGuid().ToString("N"))
$repositoryRoot = Join-Path $temporaryRoot "checkout with spaces"
$acsRoot = Join-Path $repositoryRoot "acs"
$scriptsRoot = Join-Path $acsRoot "scripts"
$toolsRoot = Join-Path $temporaryRoot "fake tools"
$workingDirectory = Join-Path $temporaryRoot "unrelated working directory"
$buildDirectory = Join-Path $acsRoot "Intermediate\vs"
$shortcut = Join-Path $repositoryRoot "acs.cmd"
$powerShellShortcut = Join-Path $repositoryRoot "acs.ps1"
$toolLog = Join-Path $temporaryRoot "native tool.log"
$scriptLog = Join-Path $temporaryRoot "project script.log"
$oldPath = $env:Path
$oldToolLog = $env:ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG
$oldScriptLog = $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG
$oldExitCode = $env:ACS_PROJECT_OPERATIONS_TEST_EXIT

try {
    New-Item -ItemType Directory -Force -Path $scriptsRoot, $toolsRoot, $workingDirectory, $buildDirectory, (Join-Path $acsRoot "engine") | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRepositoryRoot "acs.cmd") -Destination $shortcut
    Copy-Item -LiteralPath (Join-Path $sourceRepositoryRoot "acs.ps1") -Destination $powerShellShortcut
    Copy-Item -LiteralPath (Join-Path $sourceAcsRoot "scripts\project_operations.ps1") -Destination (Join-Path $scriptsRoot "project_operations.ps1")
    Set-Content -LiteralPath (Join-Path $buildDirectory "CMakeCache.txt") -Value "# isolated self-test cache" -Encoding ASCII

    # native toolはargumentを記録し、指定codeをそのまま返す。
$nativeTool = @'
@echo off
setlocal EnableExtensions DisableDelayedExpansion
echo fake-native-stdout:%~n0
> "%ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG%" echo TOOL=%~n0
:record_argument
if "%~1"=="" goto finished
>> "%ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG%" echo ARG=["%~1"]
shift
goto record_argument
:finished
if defined ACS_PROJECT_OPERATIONS_TEST_EXIT exit /b %ACS_PROJECT_OPERATIONS_TEST_EXIT%
exit /b 0
'@
    foreach ($toolName in @("cmake.cmd", "python.cmd")) {
        Set-Content -LiteralPath (Join-Path $toolsRoot $toolName) -Value $nativeTool -Encoding ASCII
    }
    $ctestStub = @'
Write-Output "fake-native-stdout:ctest"
Set-Content -LiteralPath $env:ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG -Value "TOOL=ctest" -Encoding UTF8
foreach ($argument in $args) {
    Add-Content -LiteralPath $env:ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG -Value "ARG=[`"$argument`"]" -Encoding UTF8
}
if ($env:ACS_PROJECT_OPERATIONS_TEST_EXIT) {
    $global:LASTEXITCODE = [int]$env:ACS_PROJECT_OPERATIONS_TEST_EXIT
}
'@
    Set-Content -LiteralPath (Join-Path $toolsRoot "ctest.ps1") -Value $ctestStub -Encoding UTF8

    # generate stubは受領した公開optionとCMake追加argumentだけを記録する。
    $generateStub = @'
param(
    [switch]$Open,
    [switch]$Clean,
    [switch]$Tests,
    [switch]$Tools,
    [switch]$DistributionSmoke,
    [string]$DistributionRoot = "",
    [string]$Generator = "",
    [string]$SolutionName = "",
    [string]$StartupProject = "",
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
$lines = @(
    "SCRIPT=generate",
    "CWD=$((Get-Location).Path)",
    "Open=$Open",
    "Tests=$Tests",
    "Scripting=$Scripting"
)
foreach ($argument in $CMakeArguments) {
    $lines += "CMAKE=[$argument]"
}
Set-Content -LiteralPath $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG -Value $lines -Encoding UTF8
Write-Output "fake-generate-stdout"
Write-Host "fake-generate-host"
if ($env:ACS_PROJECT_OPERATIONS_TEST_EXIT) {
    exit [int]$env:ACS_PROJECT_OPERATIONS_TEST_EXIT
}
'@
    Set-Content -LiteralPath (Join-Path $acsRoot "generate.ps1") -Value $generateStub -Encoding UTF8

    # 配布stubは構成と空白を含む配布先を記録する。
    $distributionStub = @'
param(
    [string[]]$Configs = @(),
    [string]$Deploy = "",
    [switch]$SelfTest
)
$lines = @(
    "SCRIPT=dist",
    "CONFIGS=$($Configs -join ',')",
    "DEPLOY=$Deploy",
    "SELF_TEST=$SelfTest"
)
Set-Content -LiteralPath $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG -Value $lines -Encoding UTF8
Write-Output "fake-dist-stdout"
'@
    Set-Content -LiteralPath (Join-Path $scriptsRoot "build_single_header.ps1") -Value $distributionStub -Encoding UTF8

    # clean stubは既存clean-upへの委譲と確認省略だけを記録する。
    $cleanStub = @'
param([switch]$Yes)
Set-Content -LiteralPath $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG -Value @("SCRIPT=clean", "YES=$Yes") -Encoding UTF8
Write-Output "fake-clean-stdout"
'@
    Set-Content -LiteralPath (Join-Path $repositoryRoot "clean-up.ps1") -Value $cleanStub -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $scriptsRoot "run_foundation_end_to_end.py") -Value "# isolated self-test stub" -Encoding ASCII

    $presetDocument = @{
        version = 6
        configurePresets = @(@{
            name = "debug-preset"
            generator = "Ninja"
            binaryDir = '${sourceParentDir}/Intermediate/preset with spaces'
            cacheVariables = @{ CMAKE_BUILD_TYPE = "Debug" }
        })
        buildPresets = @(@{
            name = "debug-preset"
            configurePreset = "debug-preset"
        })
    } | ConvertTo-Json -Depth 8
    Set-Content -LiteralPath (Join-Path $acsRoot "engine\CMakePresets.json") -Value $presetDocument -Encoding UTF8

    $env:Path = "$toolsRoot;$oldPath"
    $env:ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG = $toolLog
    $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG = $scriptLog
    Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_EXIT -ErrorAction SilentlyContinue

    $result = Invoke-Shortcut $powerShellShortcut @("HeLp") $workingDirectory
    Assert-ExitCode $result 0 "helpは別cwdと大小文字混在でも成功する必要があります"
    Assert-Contains $result.Output "configure -> build -> test" "help本文が不足しています"
    $cmdHelpLine = '"{0}" HeLp' -f $shortcut
    $result = Invoke-CmdLine $cmdHelpLine $workingDirectory
    Assert-ExitCode $result 0 "cmd.exe launcherのhelpは別cwdでも成功する必要があります"

    $result = Invoke-Shortcut $powerShellShortcut @("unknown-command") $workingDirectory
    Assert-ExitCode $result 2 "未知commandは使用方法errorにする必要があります"
    Assert-Contains $result.Output "未知のcommand" "未知command診断が不足しています"

    $result = Invoke-Shortcut $powerShellShortcut @("build", "--config", "profile") $workingDirectory
    Assert-ExitCode $result 2 "未知構成は使用方法errorにする必要があります"
    Assert-Contains $result.Output "Debug または Release" "構成診断が不足しています"

    Remove-Item -LiteralPath $toolLog, $scriptLog -Force -ErrorAction SilentlyContinue
    $result = Invoke-Shortcut $powerShellShortcut @("configure", "--name", "--dry-run") $workingDirectory
    Assert-ExitCode $result 2 "不足した--name値がdry-runを消費してはいけません"
    Assert-PathMissing $toolLog "不足値のconfigureがnative toolを実行しています"
    Assert-PathMissing $scriptLog "不足値のconfigureがproject scriptを実行しています"
    $result = Invoke-Shortcut $powerShellShortcut @("build", "--target", "--dry-run") $workingDirectory
    Assert-ExitCode $result 2 "不足した--target値がdry-runを消費してはいけません"
    Assert-PathMissing $toolLog "不足値のbuildがnative toolを実行しています"
    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--deploy", "--dry-run") $workingDirectory
    Assert-ExitCode $result 2 "不足した--deploy値がdry-runを消費してはいけません"
    Assert-PathMissing $scriptLog "不足値のdistが配布scriptを実行しています"
    $result = Invoke-Shortcut $powerShellShortcut @("build", "--preset", "") $workingDirectory
    Assert-ExitCode $result 2 "空のpreset名は使用方法errorにする必要があります"
    $result = Invoke-Shortcut $powerShellShortcut @("configure", "--config", "Debug", "--dry-run") $workingDirectory
    Assert-ExitCode $result 2 "既定configureで無効な構成指定を黙って無視できません"
    Assert-PathMissing $scriptLog "無効な構成指定でconfigureを実行しています"

    $defaultCache = Join-Path $buildDirectory "CMakeCache.txt"
    $savedDefaultCache = "$defaultCache.saved"
    Move-Item -LiteralPath $defaultCache -Destination $savedDefaultCache
    $result = Invoke-Shortcut $powerShellShortcut @("build") $workingDirectory
    Assert-ExitCode $result 3 "既定tree不足は環境errorにする必要があります"
    Assert-Contains $result.Output ".\acs.ps1 configure" "PowerShell用configure案内が不足しています"
    $cmdBuildLine = '"{0}" build' -f $shortcut
    $result = Invoke-CmdLine $cmdBuildLine $workingDirectory
    Assert-ExitCode $result 3 "cmd.exeの既定tree不足は環境errorにする必要があります"
    Assert-Contains $result.Output "acs.cmd configure" "cmd.exe用configure案内が不足しています"
    Move-Item -LiteralPath $savedDefaultCache -Destination $defaultCache
    $missingBuildDirectory = Join-Path $temporaryRoot "missing build tree"
    $result = Invoke-Shortcut $powerShellShortcut @("build", "--build-dir", $missingBuildDirectory) $workingDirectory
    Assert-ExitCode $result 3 "未configure treeは環境errorにする必要があります"
    Assert-Contains $result.Output "cmake -S" "custom build treeのconfigure案内が不足しています"

    Remove-Item -LiteralPath $toolLog, $scriptLog -Force -ErrorAction SilentlyContinue
    $result = Invoke-Shortcut $powerShellShortcut @("all", "--config", "dEbUg", "--target", "target with spaces", "--filter", "filter with spaces", "--dry-run", "--", "-DVALUE=two words") $workingDirectory
    Assert-ExitCode $result 0 "all dry-runは成功する必要があります"
    Assert-Contains $result.Output "--config Debug" "構成の大小文字正規化が不足しています"
    Assert-Contains $result.Output '"target with spaces"' "targetの引用表示が不足しています"
    Assert-Contains $result.Output '"filter with spaces"' "filterの引用表示が不足しています"
    Assert-Contains $result.Output '"-DVALUE=two words"' "--以後の引用表示が不足しています"
    Assert-PathMissing $toolLog "dry-runでnative toolを実行しています"
    Assert-PathMissing $scriptLog "dry-runでproject scriptを実行しています"

    $env:ACS_PROJECT_OPERATIONS_TEST_EXIT = "37"
    $result = Invoke-Shortcut $powerShellShortcut @("build", "--config", "rElEaSe", "--target", "target with spaces", "--", "--parallel", "3") $workingDirectory
    Assert-ExitCode $result 37 "native childの非zero終了codeを保持する必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    Assert-True ($toolArguments -contains 'ARG=["--config"]') "build構成optionが欠落しています"
    Assert-True ($toolArguments -contains 'ARG=["Release"]') "Release正規化が欠落しています"
    Assert-True ($toolArguments -contains 'ARG=["target with spaces"]') "空白を含むtarget境界が壊れています"
    Assert-True ($toolArguments -contains 'ARG=["--parallel"]') "--以後のbuild argumentが欠落しています"
    Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_EXIT

    $result = Invoke-Shortcut $powerShellShortcut @("test", "--config", "release", "--filter", "ACS.Space Filter", "--", "--repeat", "until-pass:2") $workingDirectory
    Assert-ExitCode $result 0 "test argument転送は成功する必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    $toolOutput = Get-Content -Raw -LiteralPath $toolLog
    Assert-Contains $toolOutput "TOOL=ctest" "ctestへ委譲されていません"
    Assert-True ($toolArguments -contains 'ARG=["ACS.Space Filter"]') "空白を含むCTest filter境界が壊れています"
    Assert-True ($toolArguments -contains 'ARG=["until-pass:2"]') "--以後のCTest argumentが欠落しています"

    $powerShellFilter = "A^B|C&D%Z!E"
    $powerShellTail = "tail^one|two&three%Q!five"
    $result = Invoke-Shortcut $powerShellShortcut @("test", "--filter", $powerShellFilter, "--", "--label", $powerShellTail) $workingDirectory
    Assert-ExitCode $result 0 "PowerShellからのmeta文字argument転送は成功する必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    Assert-True ($toolArguments -contains "ARG=[`"$powerShellFilter`"]") "PowerShell filterの^ | & % !境界が壊れています"
    Assert-True ($toolArguments -contains "ARG=[`"$powerShellTail`"]") "PowerShell --後の^ | & % !境界が壊れています"

    $powerShellCommandLine = "& {0} test --filter {1} '--' '--label' {2}" -f (Convert-ToPowerShellSingleQuotedLiteral $powerShellShortcut), (Convert-ToPowerShellSingleQuotedLiteral $powerShellFilter), (Convert-ToPowerShellSingleQuotedLiteral $powerShellTail)
    $result = Invoke-PowerShellLine $powerShellCommandLine $workingDirectory
    Assert-ExitCode $result 0 "PowerShell対話相当の引用済みseparatorは成功する必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    Assert-True ($toolArguments -contains "ARG=[`"$powerShellFilter`"]") "PowerShell対話相当のfilter境界が壊れています"
    Assert-True ($toolArguments -contains "ARG=[`"$powerShellTail`"]") "PowerShell対話相当の'--'後の境界が壊れています"

    $cmdTailOne = "cmd^value|pipe&ampersand%percent!bang"
    $cmdTailTwo = "tail^value|pipe&ampersand%percent!bang"
    $cmdCommandLine = '"{0}" configure --tests -- "{1}" "{2}"' -f $shortcut, $cmdTailOne, $cmdTailTwo
    $result = Invoke-CmdLine $cmdCommandLine $workingDirectory
    Assert-ExitCode $result 0 "cmd.exeからのmeta文字argument転送は成功する必要があります"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "CMAKE=[$cmdTailOne]" "cmd.exe第一argumentの^ | & % !境界が壊れています"
    Assert-Contains $scriptOutput "CMAKE=[$cmdTailTwo]" "cmd.exe --後の^ | & % !境界が壊れています"

    $cmdFilter = "filter^value|pipe&ampersand%percent!bang"
    $cmdTestTail = "label^value|pipe&ampersand%percent!bang"
    $cmdTestLine = '"{0}" test --filter "{1}" -- "--label" "{2}"' -f $shortcut, $cmdFilter, $cmdTestTail
    $result = Invoke-CmdLine $cmdTestLine $workingDirectory
    Assert-ExitCode $result 0 "cmd.exeからのfilter転送は成功する必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    Assert-True ($toolArguments -contains "ARG=[`"$cmdFilter`"]") "cmd.exe filterの^ | & % !境界が壊れています"
    Assert-True ($toolArguments -contains "ARG=[`"$cmdTestTail`"]") "cmd.exe test --後の^ | & % !境界が壊れています"

    $env:ACS_PROJECT_OPERATIONS_LITERAL = "expanded-value-must-not-arrive"
    $cmdLiteral = "%ACS_PROJECT_OPERATIONS_LITERAL%"
    $cmdEscapedLiteral = "^%ACS_PROJECT_OPERATIONS_LITERAL^%"
    $cmdLiteralLine = '"{0}" test --filter {1} -- "--label" {1}' -f $shortcut, $cmdEscapedLiteral
    $result = Invoke-CmdLine $cmdLiteralLine $workingDirectory
    Remove-Item Env:\ACS_PROJECT_OPERATIONS_LITERAL
    Assert-ExitCode $result 0 "cmd.exeからliteral環境変数形式を転送できる必要があります"
    $toolArguments = Read-ToolArguments $toolLog
    Assert-True ($toolArguments -contains "ARG=[`"$cmdLiteral`"]") "cmd.exe filterのliteral %NAME%境界が壊れています"
    Assert-True (@($toolArguments | Where-Object { $_ -eq "ARG=[`"$cmdLiteral`"]" }).Count -eq 2) "cmd.exe filterと--後のliteral %NAME%を両方保持する必要があります"
    Assert-True (-not ($toolArguments -contains 'ARG=["expanded-value-must-not-arrive"]')) "cmd.exeがliteral %NAME%を環境変数展開しています"

    Remove-Item -LiteralPath $scriptLog -Force -ErrorAction SilentlyContinue
    $result = Invoke-Shortcut $powerShellShortcut @("GeNeRaTe", "--tests", "--scripting", "--", "-DCUSTOM_PATH=C:\Path With Spaces", "-DPAIR=two words") $workingDirectory
    Assert-ExitCode $result 0 "generate aliasはconfigureへ委譲する必要があります"
    Assert-Contains $result.Output "fake-generate-stdout" "project scriptの標準出力が表示されていません"
    Assert-Contains $result.Output "fake-generate-host" "project scriptの情報出力が表示されていません"
    Assert-True (-not $result.Output.Contains("#< CLIXML")) "project scriptの標準出力がCLIXML化されています"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "SCRIPT=generate" "generate.ps1へ委譲されていません"
    Assert-Contains $scriptOutput "Tests=True" "--testsが欠落しています"
    Assert-Contains $scriptOutput "Scripting=True" "--scriptingが欠落しています"
    Assert-Contains $scriptOutput "CMAKE=[-DCUSTOM_PATH=C:\Path With Spaces]" "CMake追加argumentの空白境界が壊れています"
    Assert-Contains $scriptOutput "CMAKE=[-DPAIR=two words]" "二つ目のCMake追加argumentが欠落しています"
    Assert-Contains $scriptOutput "CWD=$repositoryRoot" "project scriptの基準cwdが不正です"

    $env:ACS_PROJECT_OPERATIONS_TEST_EXIT = "29"
    $result = Invoke-Shortcut $powerShellShortcut @("configure", "--tests") $workingDirectory
    Assert-ExitCode $result 29 "PowerShell childの非zero終了codeを保持する必要があります"
    Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_EXIT

    $result = Invoke-Shortcut $powerShellShortcut @("dist") $workingDirectory
    Assert-ExitCode $result 0 "構成省略時のdistは成功する必要があります"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "CONFIGS=Debug,Release" "dist既定構成のarray境界が壊れています"
    Assert-Contains $scriptOutput "DEPLOY=" "dist構成を配布先へ誤bindしています"

    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--config", "rElEaSe", "--", "-SelfTest") $workingDirectory
    Assert-ExitCode $result 0 "distは既存配布scriptへ委譲する必要があります"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "CONFIGS=Release" "dist構成の正規化が欠落しています"
    Assert-Contains $scriptOutput "DEPLOY=" "単一構成distを配布先へ誤bindしています"
    Assert-Contains $scriptOutput "SELF_TEST=True" "distの追加argumentが欠落しています"

    $deployDirectory = Join-Path $temporaryRoot "deploy with spaces"
    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--config", "Release", "--deploy", $deployDirectory) $workingDirectory
    Assert-ExitCode $result 2 "単一構成distのdeployを許可してはいけません"
    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--deploy", $deployDirectory, "--", "-SelfTest") $workingDirectory
    Assert-ExitCode $result 0 "両構成distは空白を含む配布先へ委譲できる必要があります"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "CONFIGS=Debug,Release" "deploy時の両構成指定が欠落しています"
    Assert-Contains $scriptOutput "DEPLOY=$deployDirectory" "空白を含む配布先境界が壊れています"

    Remove-Item -LiteralPath $scriptLog -Force -ErrorAction SilentlyContinue
    $result = Invoke-Shortcut $powerShellShortcut @("clean", "--yes", "--dry-run") $workingDirectory
    Assert-ExitCode $result 0 "clean dry-runは成功する必要があります"
    Assert-PathMissing $scriptLog "clean dry-runでclean-up.ps1を実行しています"
    $result = Invoke-Shortcut $powerShellShortcut @("clean", "--yes") $workingDirectory
    Assert-ExitCode $result 0 "cleanは既存clean-up.ps1へ委譲する必要があります"
    $scriptOutput = Get-Content -Raw -LiteralPath $scriptLog
    Assert-Contains $scriptOutput "YES=True" "clean確認省略optionが欠落しています"

    $result = Invoke-Shortcut $powerShellShortcut @("configure", "--build-dir", "custom tree") $workingDirectory
    Assert-ExitCode $result 2 "非対応のconfigure --build-dirは黙って無視できません"
    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--preset", "debug-preset") $workingDirectory
    Assert-ExitCode $result 2 "非対応のdist --presetは黙って無視できません"
    $result = Invoke-Shortcut $powerShellShortcut @("test", "--foundation", "--filter", "ignored") $workingDirectory
    Assert-ExitCode $result 2 "基盤E2EでCTest filterを黙って無視できません"

    $result = Invoke-Shortcut $powerShellShortcut @("build", "--preset", "debug-preset", "--config", "dEbUg", "--dry-run") $workingDirectory
    Assert-ExitCode $result 3 "未configure presetは環境errorにする必要があります"
    Assert-Contains $result.Output ".\acs.ps1 configure --preset debug-preset" "未configure presetの案内が不足しています"
    $presetBuildDirectory = Join-Path $acsRoot "Intermediate\preset with spaces"
    New-Item -ItemType Directory -Force -Path $presetBuildDirectory | Out-Null
    Set-Content -LiteralPath (Join-Path $presetBuildDirectory "CMakeCache.txt") -Value "# preset self-test cache" -Encoding ASCII
    $result = Invoke-Shortcut $powerShellShortcut @("build", "--preset", "debug-preset", "--config", "dEbUg", "--dry-run") $workingDirectory
    Assert-ExitCode $result 0 "CMake presetの大小文字構成整合は成功する必要があります"
    Assert-Contains $result.Output "--preset debug-preset" "build presetが使用されていません"

    # OS標準commandだけのPATHで不足toolごとの診断を確認する。
    $windowsPowerShellRoot = [System.IO.Path]::GetDirectoryName((Get-Command powershell.exe).Source)
    $systemRoot = Join-Path $env:SystemRoot "System32"
    $env:Path = "$systemRoot;$windowsPowerShellRoot"
    $result = Invoke-Shortcut $powerShellShortcut @("configure", "--dry-run") $workingDirectory
    Assert-ExitCode $result 3 "cmake不足は環境errorにする必要があります"
    Assert-Contains $result.Output "cmake" "cmake不足診断が欠落しています"
    $result = Invoke-Shortcut $powerShellShortcut @("test", "--dry-run") $workingDirectory
    Assert-ExitCode $result 3 "ctest不足は環境errorにする必要があります"
    Assert-Contains $result.Output "ctest" "ctest不足診断が欠落しています"
    $result = Invoke-Shortcut $powerShellShortcut @("dist", "--dry-run") $workingDirectory
    Assert-ExitCode $result 3 "python不足は環境errorにする必要があります"
    Assert-Contains $result.Output "python" "python不足診断が欠落しています"

    $originalLauncher = Get-Content -Raw -Encoding ASCII -LiteralPath $shortcut
    $missingPowerShell = Join-Path $temporaryRoot "missing Windows PowerShell.exe"
    $missingLauncher = $originalLauncher.Replace('%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe', $missingPowerShell)
    Set-Content -LiteralPath $shortcut -Value $missingLauncher -Encoding ASCII
    $result = Invoke-Shortcut $shortcut @("help") $workingDirectory
    Assert-ExitCode $result 3 "PowerShell不足はlauncher環境errorにする必要があります"
    Assert-Contains $result.Output "PowerShell" "PowerShell不足診断が欠落しています"
    Set-Content -LiteralPath $shortcut -Value $originalLauncher -Encoding ASCII

    Write-Host "project_operations_self_test: PASS" -ForegroundColor Green
} finally {
    $env:Path = $oldPath
    $env:ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG = $oldToolLog
    $env:ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG = $oldScriptLog
    $env:ACS_PROJECT_OPERATIONS_TEST_EXIT = $oldExitCode
    if ($null -eq $oldToolLog) { Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_TOOL_LOG -ErrorAction SilentlyContinue }
    if ($null -eq $oldScriptLog) { Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_SCRIPT_LOG -ErrorAction SilentlyContinue }
    if ($null -eq $oldExitCode) { Remove-Item Env:\ACS_PROJECT_OPERATIONS_TEST_EXIT -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $temporaryRoot) {
        Assert-SafeTemporaryRoot $temporaryRoot
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
