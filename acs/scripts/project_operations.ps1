# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = "Stop"

# 呼び出し位置に依存しない正規path。
$AcsRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $AcsRoot ".."))
$DefaultBuildDirectory = Join-Path $AcsRoot "Intermediate\vs"
$EngineDirectory = Join-Path $AcsRoot "engine"
$CmdEnvironmentMarker = "__ACS_CMD_ENVIRONMENT_ARGUMENTS__"
$LauncherCommand = ".\acs.ps1"
$PassThroughSeparator = "'--'"

function Throw-UsageError([string]$Message) {
    throw [System.ArgumentException]::new($Message)
}

function Throw-EnvironmentError([string]$Message) {
    throw [System.InvalidOperationException]::new($Message)
}

# cmd launcherがprocess環境へ格納したargument境界を復元する。
function Get-CmdLauncherArguments {
    $countText = [Environment]::GetEnvironmentVariable("ACS_PROJECT_OPERATIONS_ARGUMENT_COUNT", "Process")
    $count = 0
    if (-not [int]::TryParse($countText, [ref]$count) -or $count -lt 0 -or $count -gt 1024) {
        Throw-EnvironmentError "cmd launcherのargument countが不正です"
    }
    $tokens = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $count; ++$index) {
        $name = "ACS_PROJECT_OPERATIONS_ARGUMENT_$index"
        $value = [Environment]::GetEnvironmentVariable($name, "Process")
        if ($null -eq $value) {
            Throw-EnvironmentError "cmd launcherのargumentが欠落しています: $index"
        }
        $tokens.Add($value)
    }
    return $tokens.ToArray()
}

function Get-RequiredArgument([string[]]$Tokens, [ref]$Index, [string]$OptionName) {
    if ($Index.Value + 1 -ge $Tokens.Count) {
        Throw-UsageError "$OptionName には値が必要です"
    }
    $candidate = [string]$Tokens[$Index.Value + 1]
    if ([string]::IsNullOrWhiteSpace($candidate) -or $candidate.StartsWith("-")) {
        Throw-UsageError "$OptionName には空でない値が必要です"
    }
    $Index.Value++
    return $candidate
}

function Convert-ToConfiguration([string]$Value) {
    if ([string]::Equals($Value, "Debug", [System.StringComparison]::OrdinalIgnoreCase)) {
        return "Debug"
    }
    if ([string]::Equals($Value, "Release", [System.StringComparison]::OrdinalIgnoreCase)) {
        return "Release"
    }
    Throw-UsageError "未対応の構成です: $Value。Debug または Release を指定してください"
}

function Resolve-RepositoryPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        Throw-UsageError "空のpathは指定できません"
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepositoryRoot $Path))
}

function New-OperationOptions {
    return [pscustomobject]@{
        Command = "help"
        Configuration = "Debug"
        ConfigurationSpecified = $false
        Target = ""
        Filter = ""
        Deploy = ""
        Preset = ""
        BuildDirectory = $DefaultBuildDirectory
        BuildDirectorySpecified = $false
        DryRun = $false
        Foundation = $false
        Yes = $false
        Open = $false
        Clean = $false
        Tests = $false
        Tools = $false
        DistributionSmoke = $false
        DistributionRoot = ""
        Generator = ""
        SolutionName = ""
        Sample = ""
        StartupProject = ""
        AllSamples = $false
        Diligent = $false
        Scripting = $false
        Steamworks = $false
        Onnx = $false
        OpenXr = $false
        CrashReporter = $false
        Telemetry = $false
        Matchmaker = $false
        AllBackends = $false
        PassThrough = [System.Collections.Generic.List[string]]::new()
    }
}

function Convert-ToCommandName([string]$Value) {
    $normalized = $Value.Trim().ToLowerInvariant()
    switch ($normalized) {
        "configure" { return "configure" }
        "generate" { return "configure" }
        "open" { return "open" }
        "build" { return "build" }
        "test" { return "test" }
        "all" { return "all" }
        "dist" { return "dist" }
        "clean" { return "clean" }
        "help" { return "help" }
        "-h" { return "help" }
        "--help" { return "help" }
        "/?" { return "help" }
        default { Throw-UsageError "未知のcommandです: $Value" }
    }
}

function Convert-ArgumentsToOptions([string[]]$Tokens) {
    $options = New-OperationOptions
    if ($Tokens.Count -eq 0) {
        return $options
    }

    # ide generate/open は既存IDE生成操作との互換入口。
    $first = [string]$Tokens[0]
    $optionStart = 1
    if ([string]::Equals($first, "ide", [System.StringComparison]::OrdinalIgnoreCase)) {
        if ($Tokens.Count -lt 2) {
            Throw-UsageError "ide には generate または open が必要です"
        }
        $ideCommand = [string]$Tokens[1]
        if ([string]::Equals($ideCommand, "generate", [System.StringComparison]::OrdinalIgnoreCase)) {
            $options.Command = "configure"
        } elseif ([string]::Equals($ideCommand, "open", [System.StringComparison]::OrdinalIgnoreCase)) {
            $options.Command = "configure"
            $options.Open = $true
        } else {
            Throw-UsageError "未知のIDE操作です: $ideCommand"
        }
        $optionStart = 2
    } else {
        $options.Command = Convert-ToCommandName $first
        if ($options.Command -eq "open") {
            $options.Command = "configure"
            $options.Open = $true
        }
    }

    for ($index = $optionStart; $index -lt $Tokens.Count; ++$index) {
        $token = [string]$Tokens[$index]
        if ($token -eq "--") {
            for ($tailIndex = $index + 1; $tailIndex -lt $Tokens.Count; ++$tailIndex) {
                $options.PassThrough.Add([string]$Tokens[$tailIndex])
            }
            break
        }

        switch ($token.ToLowerInvariant()) {
            "--config" {
                $options.Configuration = Convert-ToConfiguration (Get-RequiredArgument $Tokens ([ref]$index) $token)
                $options.ConfigurationSpecified = $true
            }
            "-c" {
                $options.Configuration = Convert-ToConfiguration (Get-RequiredArgument $Tokens ([ref]$index) $token)
                $options.ConfigurationSpecified = $true
            }
            "--target" { $options.Target = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "-t" { $options.Target = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--filter" { $options.Filter = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "-r" { $options.Filter = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--deploy" { $options.Deploy = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--preset" { $options.Preset = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--build-dir" {
                $options.BuildDirectory = Get-RequiredArgument $Tokens ([ref]$index) $token
                $options.BuildDirectorySpecified = $true
            }
            "--dry-run" { $options.DryRun = $true }
            "-n" { $options.DryRun = $true }
            "--foundation" { $options.Foundation = $true }
            "--yes" { $options.Yes = $true }
            "-y" { $options.Yes = $true }
            "--open" { $options.Open = $true }
            "--clean" { $options.Clean = $true }
            "--tests" { $options.Tests = $true }
            "--tools" { $options.Tools = $true }
            "--distribution-smoke" { $options.DistributionSmoke = $true }
            "--distribution-root" { $options.DistributionRoot = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--generator" { $options.Generator = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--name" { $options.SolutionName = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--sample" { $options.Sample = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--startup-project" { $options.StartupProject = Get-RequiredArgument $Tokens ([ref]$index) $token }
            "--all-samples" { $options.AllSamples = $true }
            "--diligent" { $options.Diligent = $true }
            "--scripting" { $options.Scripting = $true }
            "--steamworks" { $options.Steamworks = $true }
            "--onnx" { $options.Onnx = $true }
            "--openxr" { $options.OpenXr = $true }
            "--crash-reporter" { $options.CrashReporter = $true }
            "--telemetry" { $options.Telemetry = $true }
            "--matchmaker" { $options.Matchmaker = $true }
            "--all-backends" { $options.AllBackends = $true }
            default { Throw-UsageError "未知のoptionです: $token。実体へ渡す引数は -- の後へ置いてください" }
        }
    }

    if ($options.BuildDirectorySpecified) {
        $options.BuildDirectory = Resolve-RepositoryPath $options.BuildDirectory
    }
    if ($options.Deploy) {
        $options.Deploy = Resolve-RepositoryPath $options.Deploy
    }
    if ($options.DistributionRoot) {
        $options.DistributionRoot = Resolve-RepositoryPath $options.DistributionRoot
    }
    return $options
}

function Test-GenerateOptionSpecified($Options) {
    return ($Options.Open -or $Options.Clean -or $Options.Tests -or $Options.Tools -or $Options.DistributionSmoke -or $Options.DistributionRoot -or $Options.Generator -or $Options.SolutionName -or $Options.Sample -or $Options.StartupProject -or $Options.AllSamples -or $Options.Diligent -or $Options.Scripting -or $Options.Steamworks -or $Options.Onnx -or $Options.OpenXr -or $Options.CrashReporter -or $Options.Telemetry -or $Options.Matchmaker -or $Options.AllBackends)
}

function Assert-OptionBoundary($Options) {
    if ($Options.Command -eq "help") {
        return
    }
    if ($Options.Preset -and $Options.BuildDirectorySpecified) {
        Throw-UsageError "--preset と --build-dir は同時に指定できません"
    }
    if ($Options.ConfigurationSpecified -and $Options.Command -eq "configure" -and -not $Options.Preset) {
        Throw-UsageError "既定configureはVisual Studioの複数構成treeを生成します。--config は build、test、all、dist、または --preset と使用してください"
    }
    if ($Options.ConfigurationSpecified -and $Options.Command -notin @("configure", "build", "test", "all", "dist")) {
        Throw-UsageError "--config は build、test、all、dist、または --preset付きconfigureで使用してください"
    }
    if ($Options.Preset -and $Options.Command -notin @("configure", "build", "test", "all")) {
        Throw-UsageError "--preset は configure、build、test、all で使用してください"
    }
    if ($Options.BuildDirectorySpecified -and $Options.Command -notin @("build", "test")) {
        Throw-UsageError "--build-dir は build または test で使用してください。configure/all は既定treeか --preset を使用します"
    }
    if ($Options.Command -notin @("configure", "all") -and (Test-GenerateOptionSpecified $Options)) {
        Throw-UsageError "生成optionは configure、generate、open、all だけで使用できます"
    }
    if ($Options.Target -and $Options.Command -notin @("build", "all")) {
        Throw-UsageError "--target は build または all で使用してください"
    }
    if ($Options.Filter -and $Options.Command -notin @("test", "all")) {
        Throw-UsageError "--filter は test または all で使用してください"
    }
    if ($Options.Deploy -and $Options.Command -ne "dist") {
        Throw-UsageError "--deploy は dist で使用してください"
    }
    if ($Options.Command -eq "dist" -and $Options.Deploy -and $Options.ConfigurationSpecified) {
        Throw-UsageError "単一構成のdistはlocal staging専用です。--deployにはDebug/Releaseの両構成生成を使用してください"
    }
    if ($Options.Foundation -and $Options.Command -ne "test") {
        Throw-UsageError "--foundation は test で使用してください"
    }
    if ($Options.Foundation -and $Options.Filter) {
        Throw-UsageError "--filter はCTest専用です。基盤E2Eの追加argumentは -- の後へ置いてください"
    }
    if ($Options.Yes -and $Options.Command -ne "clean") {
        Throw-UsageError "--yes は clean で使用してください"
    }
    if ($Options.Preset -and $Options.Open) {
        Throw-UsageError "--preset はVisual Studio solutionを生成しないため --open と併用できません"
    }
    if ($Options.Command -eq "all" -and $Options.Foundation) {
        Throw-UsageError "all は configure→build→ctest 固定です。基盤E2Eは test --foundation を使用してください"
    }
}

function Get-RequiredTool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        Throw-EnvironmentError "必要なcommandが見つかりません: $Name。PATHとインストール状態を確認してください"
    }
    if ($command.Path) {
        return $command.Path
    }
    return $command.Source
}

function Format-DisplayArgument([string]$Value) {
    if ($Value -notmatch '[\s"&|<>^%!()]') {
        return $Value
    }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Write-CommandPreview([string]$Executable, [string[]]$Arguments) {
    $displayArguments = @($Arguments | ForEach-Object { Format-DisplayArgument ([string]$_) })
    Write-Host ("[acs] {0} {1}" -f (Format-DisplayArgument $Executable), ($displayArguments -join " ")) -ForegroundColor Cyan
}

function Invoke-NativeCommand([string]$ToolName, [string[]]$Arguments, [string]$WorkingDirectory, [bool]$DryRun, [bool]$ShowPreview = $true) {
    $executable = Get-RequiredTool $ToolName
    if ($ShowPreview) {
        Write-CommandPreview $executable $Arguments
    }
    if ($DryRun) {
        return 0
    }

    Push-Location $WorkingDirectory
    try {
        & $executable @Arguments | Out-Host
        $exitCode = $LASTEXITCODE
        if ($null -eq $exitCode) {
            $exitCode = if ($?) { 0 } else { 1 }
        }
        return [int]$exitCode
    } finally {
        Pop-Location
    }
}

function Format-ScriptParameters([hashtable]$Parameters) {
    $arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @($Parameters.Keys | Sort-Object)) {
        $value = $Parameters[$name]
        if ($value -is [bool]) {
            if ($value) {
                $arguments.Add("-$name")
            }
            continue
        }
        $arguments.Add("-$name")
        foreach ($entry in @($value)) {
            $arguments.Add([string]$entry)
        }
    }
    return $arguments.ToArray()
}

# PowerShell単一引用符内で安全な文字列へ変換する。
function Convert-ToPowerShellLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

# named arrayと追加optionを保ったPowerShell script呼び出し式を構築する。
function New-ProjectScriptInvocation([string]$Path, [hashtable]$Parameters, [string[]]$PassThrough) {
    $fragments = [System.Collections.Generic.List[string]]::new()
    $fragments.Add("&")
    $fragments.Add((Convert-ToPowerShellLiteral $Path))
    foreach ($name in @($Parameters.Keys | Sort-Object)) {
        if ($name -notmatch '^[A-Za-z][A-Za-z0-9_]*$') {
            Throw-EnvironmentError "内部parameter名が不正です: $name"
        }
        $value = $Parameters[$name]
        if ($value -is [bool]) {
            if ($value) {
                $fragments.Add("-$name")
            }
            continue
        }
        $entries = @($value)
        $literals = @($entries | ForEach-Object { Convert-ToPowerShellLiteral ([string]$_) })
        if ($literals.Count -gt 1) {
            $fragments.Add("-$name")
            $fragments.Add("@(" + ($literals -join ",") + ")")
        } elseif ($literals.Count -eq 1) {
            $fragments.Add("-$name")
            $fragments.Add($literals[0])
        }
    }
    foreach ($argument in $PassThrough) {
        if ($argument -match '^--?[A-Za-z][A-Za-z0-9_-]*$') {
            $fragments.Add($argument)
        } else {
            $fragments.Add((Convert-ToPowerShellLiteral $argument))
        }
    }
    $fragments.Add("3>&1")
    $fragments.Add("4>&1")
    $fragments.Add("5>&1")
    $fragments.Add("6>&1")
    return $fragments -join " "
}

function Invoke-ProjectScript([string]$Path, [hashtable]$Parameters, [string[]]$PassThrough, [bool]$DryRun) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-EnvironmentError "既存scriptが見つかりません: $Path"
    }
    $displayArguments = @("-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text", "-File", $Path)
    $displayArguments += Format-ScriptParameters $Parameters
    $displayArguments += @($PassThrough)
    $powerShell = Join-Path $PSHOME "powershell.exe"
    if (-not (Test-Path -LiteralPath $powerShell -PathType Leaf)) {
        Throw-EnvironmentError "Windows PowerShellが見つかりません: $powerShell"
    }
    Write-CommandPreview $powerShell $displayArguments
    if ($DryRun) {
        return 0
    }

    $invocation = New-ProjectScriptInvocation $Path $Parameters $PassThrough
    $bootstrap = @"
`$ErrorActionPreference = "Stop"
`$ProgressPreference = "SilentlyContinue"
try {
    `$global:LASTEXITCODE = `$null
    $invocation
    `$succeeded = `$?
    `$scriptExitCode = `$LASTEXITCODE
    if (`$null -ne `$scriptExitCode -and `$scriptExitCode -ne 0) { exit ([int]`$scriptExitCode) }
    if (`$succeeded) { exit 0 }
    exit 1
} catch {
    [Console]::Error.WriteLine(`$_.Exception.Message)
    exit 1
}
"@
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
    $arguments = @("-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text", "-EncodedCommand", $encodedCommand)
    return Invoke-NativeCommand $powerShell $arguments $RepositoryRoot $false $false
}

function Assert-ConfiguredBuildDirectory([string]$BuildDirectory, [string]$ConfigureHint) {
    $cache = Join-Path $BuildDirectory "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
        Throw-EnvironmentError "configure済みbuild treeがありません: $BuildDirectory`n先に $ConfigureHint を実行してください"
    }
}

# 使用treeに対応したconfigure案内を返す。
function Get-ConfigureHint($Options, $PresetContext) {
    if ($PresetContext) {
        return "$LauncherCommand configure --preset $($PresetContext.ConfigurePreset)"
    }
    if ($Options.BuildDirectorySpecified) {
        return "cmake -S `"$EngineDirectory`" -B `"$($Options.BuildDirectory)`"、または既存のCMake preset"
    }
    return "$LauncherCommand configure"
}

function Resolve-ConfigurePreset([object]$Document, [string]$Name, [hashtable]$Visited) {
    if ($Visited.ContainsKey($Name)) {
        Throw-UsageError "CMake presetの継承が循環しています: $Name"
    }
    $Visited[$Name] = $true
    $preset = @($Document.configurePresets | Where-Object { $_.name -eq $Name })
    if ($preset.Count -ne 1) {
        Throw-UsageError "configure presetが見つかりません: $Name"
    }
    $preset = $preset[0]

    $binaryDirectory = ""
    $configuration = ""
    foreach ($baseName in @($preset.inherits)) {
        if ([string]::IsNullOrWhiteSpace([string]$baseName)) {
            continue
        }
        $base = Resolve-ConfigurePreset $Document ([string]$baseName) $Visited
        if ($base.BinaryDirectory) {
            $binaryDirectory = $base.BinaryDirectory
        }
        if ($base.Configuration) {
            $configuration = $base.Configuration
        }
    }
    if ($preset.binaryDir) {
        $binaryDirectory = [string]$preset.binaryDir
    }
    if ($preset.cacheVariables -and $preset.cacheVariables.PSObject.Properties["CMAKE_BUILD_TYPE"]) {
        $configuration = [string]$preset.cacheVariables.CMAKE_BUILD_TYPE
    }
    [void]$Visited.Remove($Name)
    return [pscustomobject]@{
        Name = $Name
        BinaryDirectory = $binaryDirectory
        Configuration = $configuration
    }
}

function Get-PresetContext([string]$Name, $Options) {
    $presetPath = Join-Path $EngineDirectory "CMakePresets.json"
    if (-not (Test-Path -LiteralPath $presetPath -PathType Leaf)) {
        Throw-EnvironmentError "CMakePresets.jsonが見つかりません: $presetPath"
    }
    $document = Get-Content -Raw -Encoding UTF8 -LiteralPath $presetPath | ConvertFrom-Json
    $buildPreset = @($document.buildPresets | Where-Object { $_.name -eq $Name })
    if ($buildPreset.Count -gt 1) {
        Throw-UsageError "同名build presetが複数あります: $Name"
    }
    $configureName = $Name
    $buildPresetName = ""
    if ($buildPreset.Count -eq 1) {
        $buildPresetName = $Name
        $configureName = [string]$buildPreset[0].configurePreset
    }
    $resolved = Resolve-ConfigurePreset $document $configureName @{}
    if ([string]::IsNullOrWhiteSpace($resolved.BinaryDirectory)) {
        Throw-UsageError "presetにbinaryDirがありません: $configureName"
    }

    $sourceParent = [System.IO.Path]::GetDirectoryName($EngineDirectory)
    $binaryDirectory = $resolved.BinaryDirectory
    $binaryDirectory = $binaryDirectory.Replace('${sourceDir}', $EngineDirectory)
    $binaryDirectory = $binaryDirectory.Replace('${sourceParentDir}', $sourceParent)
    $binaryDirectory = $binaryDirectory.Replace('${presetName}', $configureName)
    if (-not [System.IO.Path]::IsPathRooted($binaryDirectory)) {
        $binaryDirectory = Join-Path $EngineDirectory $binaryDirectory
    }
    $binaryDirectory = [System.IO.Path]::GetFullPath($binaryDirectory)

    $presetConfiguration = ""
    if ($resolved.Configuration) {
        $presetConfiguration = Convert-ToConfiguration $resolved.Configuration
    }
    if ($Options.ConfigurationSpecified -and $presetConfiguration -and $Options.Configuration -ne $presetConfiguration) {
        Throw-UsageError "指定構成 $($Options.Configuration) はpreset構成 $presetConfiguration と一致しません"
    }
    if (-not $Options.ConfigurationSpecified -and $presetConfiguration) {
        $Options.Configuration = $presetConfiguration
    }
    return [pscustomobject]@{
        ConfigurePreset = $configureName
        BuildPreset = $buildPresetName
        BuildDirectory = $binaryDirectory
    }
}

function Add-ConfigureCacheArguments($Options, [System.Collections.Generic.List[string]]$Arguments) {
    if ($Options.Tests) { $Arguments.Add("-DACS_BUILD_TESTS=ON") }
    if ($Options.Tools) { $Arguments.Add("-DACS_BUILD_TOOLS=ON") }
    if ($Options.DistributionSmoke) { $Arguments.Add("-DACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE=ON") }
    if ($Options.DistributionRoot) { $Arguments.Add("-DACS_DISTRIBUTION_CONSUMER_ROOT=$($Options.DistributionRoot)") }
    if ($Options.AllSamples) {
        $Arguments.Add("-DACS_BUILD_SAMPLES=ON")
        $Arguments.Add("-DACS_ONLY_SAMPLE=")
    } elseif ($Options.Sample) {
        $Arguments.Add("-DACS_BUILD_SAMPLES=ON")
        $Arguments.Add("-DACS_ONLY_SAMPLE=$($Options.Sample)")
    }
    if ($Options.StartupProject) { $Arguments.Add("-DACS_STARTUP_PROJECT=$($Options.StartupProject)") }
    if ($Options.Diligent -or $Options.AllBackends) {
        $Arguments.Add("-DACS_RENDER_DILIGENT=ON")
        $Arguments.Add("-DACS_Render_DILIGENT=ON")
    }
    if ($Options.Scripting -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_SCRIPTING=ON") }
    if ($Options.Steamworks -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_STEAMWORKS=ON") }
    if ($Options.Onnx -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_ML_ONNX=ON") }
    if ($Options.OpenXr -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_OPENXR=ON") }
    if ($Options.CrashReporter -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_CRASH_REPORTER=ON") }
    if ($Options.Telemetry -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_TELEMETRY_FILE=ON") }
    if ($Options.Matchmaker -or $Options.AllBackends) { $Arguments.Add("-DACS_BUILD_LOCAL_MATCHMAKER=ON") }
}

function Invoke-ConfigureOperation($Options, $PresetContext) {
    if ($PresetContext) {
        if ($Options.Open -or $Options.SolutionName -or $Options.Generator) {
            Throw-UsageError "IDE solution用の --open、--name、--generator はpreset構成と併用できません"
        }
        $arguments = [System.Collections.Generic.List[string]]::new()
        $arguments.Add("--preset")
        $arguments.Add($PresetContext.ConfigurePreset)
        if ($Options.Clean) {
            $arguments.Add("--fresh")
        }
        Add-ConfigureCacheArguments $Options $arguments
        foreach ($argument in $Options.PassThrough) {
            $arguments.Add($argument)
        }
        return Invoke-NativeCommand "cmake" $arguments.ToArray() $EngineDirectory $Options.DryRun
    }

    [void](Get-RequiredTool "cmake")
    $parameters = @{}
    if ($Options.Open) { $parameters.Open = $true }
    if ($Options.Clean) { $parameters.Clean = $true }
    if ($Options.Tests) { $parameters.Tests = $true }
    if ($Options.Tools) { $parameters.Tools = $true }
    if ($Options.DistributionSmoke) { $parameters.DistributionSmoke = $true }
    if ($Options.DistributionRoot) { $parameters.DistributionRoot = $Options.DistributionRoot }
    if ($Options.Generator) { $parameters.Generator = $Options.Generator }
    if ($Options.SolutionName) { $parameters.SolutionName = $Options.SolutionName }
    if ($Options.Sample) { $parameters.Sample = $Options.Sample }
    if ($Options.StartupProject) { $parameters.StartupProject = $Options.StartupProject }
    if ($Options.AllSamples) { $parameters.AllSamples = $true }
    if ($Options.Diligent) { $parameters.Diligent = $true }
    if ($Options.Scripting) { $parameters.Scripting = $true }
    if ($Options.Steamworks) { $parameters.Steamworks = $true }
    if ($Options.Onnx) { $parameters.Onnx = $true }
    if ($Options.OpenXr) { $parameters.OpenXr = $true }
    if ($Options.CrashReporter) { $parameters.CrashReporter = $true }
    if ($Options.Telemetry) { $parameters.Telemetry = $true }
    if ($Options.Matchmaker) { $parameters.Matchmaker = $true }
    if ($Options.AllBackends) { $parameters.AllBackends = $true }
    if ($Options.PassThrough.Count -gt 0) { $parameters.CMakeArguments = $Options.PassThrough.ToArray() }
    return Invoke-ProjectScript (Join-Path $AcsRoot "generate.ps1") $parameters @() $Options.DryRun
}

function Invoke-BuildOperation($Options, $PresetContext, [bool]$SkipConfiguredCheck) {
    $arguments = [System.Collections.Generic.List[string]]::new()
    $workingDirectory = $RepositoryRoot
    if ($PresetContext -and $PresetContext.BuildPreset) {
        if (-not $SkipConfiguredCheck) {
            Assert-ConfiguredBuildDirectory $PresetContext.BuildDirectory (Get-ConfigureHint $Options $PresetContext)
        }
        $arguments.Add("--build")
        $arguments.Add("--preset")
        $arguments.Add($PresetContext.BuildPreset)
        $workingDirectory = $EngineDirectory
    } else {
        $buildDirectory = if ($PresetContext) { $PresetContext.BuildDirectory } else { $Options.BuildDirectory }
        if (-not $SkipConfiguredCheck) {
            Assert-ConfiguredBuildDirectory $buildDirectory (Get-ConfigureHint $Options $PresetContext)
        }
        $arguments.Add("--build")
        $arguments.Add($buildDirectory)
        $arguments.Add("--config")
        $arguments.Add($Options.Configuration)
    }
    if ($Options.Target) {
        $arguments.Add("--target")
        $arguments.Add($Options.Target)
    }
    foreach ($argument in $Options.PassThrough) {
        $arguments.Add($argument)
    }
    return Invoke-NativeCommand "cmake" $arguments.ToArray() $workingDirectory $Options.DryRun
}

function Invoke-CtestOperation($Options, $PresetContext, [bool]$SkipConfiguredCheck) {
    $buildDirectory = if ($PresetContext) { $PresetContext.BuildDirectory } else { $Options.BuildDirectory }
    if (-not $SkipConfiguredCheck) {
        Assert-ConfiguredBuildDirectory $buildDirectory (Get-ConfigureHint $Options $PresetContext)
    }
    $arguments = [System.Collections.Generic.List[string]]::new()
    $arguments.Add("--test-dir")
    $arguments.Add($buildDirectory)
    $arguments.Add("-C")
    $arguments.Add($Options.Configuration)
    $arguments.Add("--output-on-failure")
    if ($Options.Filter) {
        $arguments.Add("-R")
        $arguments.Add($Options.Filter)
    }
    foreach ($argument in $Options.PassThrough) {
        $arguments.Add($argument)
    }
    return Invoke-NativeCommand "ctest" $arguments.ToArray() $RepositoryRoot $Options.DryRun
}

function Invoke-FoundationOperation($Options, $PresetContext) {
    $buildDirectory = if ($PresetContext) { $PresetContext.BuildDirectory } else { $Options.BuildDirectory }
    Assert-ConfiguredBuildDirectory $buildDirectory (Get-ConfigureHint $Options $PresetContext)
    [void](Get-RequiredTool "cmake")
    [void](Get-RequiredTool "ctest")
    $performanceExecutable = Join-Path $AcsRoot "Binaries\$($Options.Configuration)\acs_foundation_performance.exe"
    $outputName = "foundation-end-to-end-{0}.json" -f $Options.Configuration.ToLowerInvariant()
    $arguments = [System.Collections.Generic.List[string]]::new()
    $arguments.Add((Join-Path $PSScriptRoot "run_foundation_end_to_end.py"))
    $arguments.Add("--build-dir")
    $arguments.Add($buildDirectory)
    $arguments.Add("--performance-executable")
    $arguments.Add($performanceExecutable)
    $arguments.Add("--output")
    $arguments.Add((Join-Path $AcsRoot "Saved\$outputName"))
    $arguments.Add("--source-root")
    $arguments.Add($RepositoryRoot)
    $arguments.Add("--configuration")
    $arguments.Add($Options.Configuration)
    foreach ($argument in $Options.PassThrough) {
        $arguments.Add($argument)
    }
    return Invoke-NativeCommand "python" $arguments.ToArray() $RepositoryRoot $Options.DryRun
}

function Invoke-DistOperation($Options) {
    Assert-ConfiguredBuildDirectory $DefaultBuildDirectory "$LauncherCommand configure"
    [void](Get-RequiredTool "python")
    $parameters = @{}
    if ($Options.ConfigurationSpecified) {
        $parameters.Configs = @($Options.Configuration)
    } else {
        $parameters.Configs = @("Debug", "Release")
    }
    if ($Options.Deploy) {
        $parameters.Deploy = $Options.Deploy
    }
    return Invoke-ProjectScript (Join-Path $PSScriptRoot "build_single_header.ps1") $parameters $Options.PassThrough.ToArray() $Options.DryRun
}

function Invoke-CleanOperation($Options) {
    $parameters = @{}
    if ($Options.Yes) {
        $parameters.Yes = $true
    }
    return Invoke-ProjectScript (Join-Path $RepositoryRoot "clean-up.ps1") $parameters $Options.PassThrough.ToArray() $Options.DryRun
}

function Show-Help {
    $helpText = @"
ACS project operations

正規launcher:
  cmd.exe    acs.cmd
  PowerShell .\acs.ps1

使い方:
  $LauncherCommand <command> [option] [$PassThroughSeparator 実体へ渡す引数]

command:
  configure  generate.ps1 または --preset のCMake configureを実行
  build      configure済みtreeを cmake --build でbuild
  test       ctestを実行。--foundationなら既存の基盤E2Eを実行
  all        configure -> build -> test を失敗時点で停止して実行
  dist       build_single_header.ps1で配布物を生成
  clean      root clean-up.ps1の確認付き安全cleanを実行
  help       この説明を表示

IDE互換:
  $LauncherCommand generate [option]
  $LauncherCommand open [option]
  $LauncherCommand ide generate [option]
  $LauncherCommand ide open [option]

共通option:
  --config Debug|Release   build/test構成。大小文字は区別しない
  --preset <name>          engine/CMakePresets.jsonの既存presetを使用
  --build-dir <path>       presetを使わないbuild/test tree
  --dry-run, -n            commandを表示するだけでfile/process状態を変更しない

操作option:
  --target <name>          build/allのtarget
  --filter <regex>         test/allのCTest filter
  --deploy <path>          両構成distのmirror先。安全判定は既存配布scriptへ委譲
  --foundation             testをrun_foundation_end_to_end.pyへ切替
  --yes, -y                clean-up.ps1の確認を省略
  --tests --tools --scripting --diligent --all-backends
  --sample <name> --all-samples --name <solution> --open --clean

現在のshellへ貼れる例:
  $LauncherCommand configure --tests --scripting
  $LauncherCommand build --config Debug --target acs_unit_tests
  $LauncherCommand test --config Debug --filter "^ACS.UnitTests$"
  $LauncherCommand all --config Release --target acs_unit_tests --filter "^ACS.UnitTests$"
  $LauncherCommand dist --deploy "C:\ACS SDK"
  $LauncherCommand clean
  $LauncherCommand open --scripting
  $LauncherCommand configure $PassThroughSeparator "-DACS_BUILD_SCRIPTING=ON" "-DUSER_PATH=C:\Path With Spaces"

PowerShellは .\acs.ps1、cmd.exeは acs.cmd を使う。両launcherは同じ操作実体を呼ぶ。
PowerShellでは追加argumentのseparatorを '--' と引用する。cmd.exeでは -- のまま指定する。

`--`以後はconfigureではCMake引数、buildではcmake --build引数、testでは
ctestまたは基盤E2E引数、dist/cleanでは既存PowerShell script引数として、
各argumentの境界を保ったまま渡す。allではconfigure段階だけへ渡す。
"@
    Write-Host $helpText
}

function Invoke-Main([string[]]$Tokens) {
    $options = Convert-ArgumentsToOptions $Tokens
    Assert-OptionBoundary $options
    if ($options.Command -eq "help") {
        Show-Help
        return 0
    }

    $presetContext = $null
    if ($options.Preset) {
        $presetContext = Get-PresetContext $options.Preset $options
        $options.BuildDirectory = $presetContext.BuildDirectory
    }

    switch ($options.Command) {
        "configure" { return Invoke-ConfigureOperation $options $presetContext }
        "build" { return Invoke-BuildOperation $options $presetContext $false }
        "test" {
            if ($options.Foundation) {
                return Invoke-FoundationOperation $options $presetContext
            }
            return Invoke-CtestOperation $options $presetContext $false
        }
        "dist" { return Invoke-DistOperation $options }
        "clean" { return Invoke-CleanOperation $options }
        "all" {
            $options.Tests = $true
            $configurePassThrough = $options.PassThrough
            $exitCode = Invoke-ConfigureOperation $options $presetContext
            if ($exitCode -ne 0) { return $exitCode }
            $options.PassThrough = [System.Collections.Generic.List[string]]::new()
            $skipConfiguredCheck = [bool]$options.DryRun
            $exitCode = Invoke-BuildOperation $options $presetContext $skipConfiguredCheck
            if ($exitCode -ne 0) { return $exitCode }
            $options.Target = ""
            $exitCode = Invoke-CtestOperation $options $presetContext $skipConfiguredCheck
            $options.PassThrough = $configurePassThrough
            return $exitCode
        }
        default { Throw-UsageError "未処理のcommandです: $($options.Command)" }
    }
}

try {
    $entryTokens = [string[]]$args
    if ($entryTokens.Count -eq 1 -and $entryTokens[0] -eq $CmdEnvironmentMarker) {
        $LauncherCommand = "acs.cmd"
        $PassThroughSeparator = "--"
        $entryTokens = [string[]]@(Get-CmdLauncherArguments)
    }
    $result = Invoke-Main $entryTokens
    exit ([int]$result)
} catch [System.ArgumentException] {
    Write-Host "[acs] $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "[acs] $LauncherCommand help で使用例を確認できます" -ForegroundColor Yellow
    exit 2
} catch [System.InvalidOperationException] {
    Write-Host "[acs] $($_.Exception.Message)" -ForegroundColor Red
    exit 3
} catch {
    Write-Host "[acs] operation failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
