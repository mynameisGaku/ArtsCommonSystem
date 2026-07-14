param(
    [string]$Preset = 'diligent-address-sanitizer',
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipRun,
    [ValidateRange(1, 64)]
    [int]$BuildParallelism = 4,
    [string]$ReportDirectory
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$engineRoot = Join-Path $repositoryRoot 'acs\engine'
$presetDocumentPath = Join-Path $engineRoot 'CMakePresets.json'
if (!$ReportDirectory) {
    $ReportDirectory = Join-Path $repositoryRoot 'acs\Saved\Diagnostics\AddressSanitizer'
}
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$script:SummaryLogPath = Join-Path $ReportDirectory 'summary.log'
$script:Utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText(
    $script:SummaryLogPath,
    "memory_diagnostic_method=address_sanitizer summary_version=1`r`n",
    $script:Utf8WithoutBom)

function Write-DiagnosticRecord
{
    param([string]$Record)

    Write-Host $Record
    [System.IO.File]::AppendAllText($script:SummaryLogPath, "$Record`r`n", $script:Utf8WithoutBom)
}

function Write-Utf8LogWithoutBom
{
    param(
        [string]$LogPath,
        [object[]]$Lines
    )

    $text = @($Lines | ForEach-Object { [string]$_ }) -join "`r`n"
    if ($text.Length -ne 0) {
        $text += "`r`n"
    }
    [System.IO.File]::WriteAllText($LogPath, $text, $script:Utf8WithoutBom)
}

function Get-PresetObject
{
    param(
        [object[]]$PresetObjects,
        [string]$PresetName,
        [string]$PresetKind
    )

    $matches = @($PresetObjects | Where-Object { $_.name -eq $PresetName })
    if ($matches.Count -ne 1) {
        throw "$PresetKind preset '$PresetName' was not found uniquely in $presetDocumentPath"
    }
    return $matches[0]
}

function Get-InheritedPresetValue
{
    param(
        [object]$PresetObject,
        [object[]]$PresetObjects,
        [string]$PropertyName,
        [string]$PresetKind,
        [int]$Depth = 0
    )

    if ($Depth -gt 32) {
        throw "$PresetKind preset inheritance is cyclic or too deep"
    }

    $property = $PresetObject.PSObject.Properties[$PropertyName]
    if ($null -ne $property -and $null -ne $property.Value -and [string]$property.Value -ne '') {
        return $property.Value
    }

    foreach ($parentName in @($PresetObject.inherits)) {
        $parent = Get-PresetObject -PresetObjects $PresetObjects -PresetName $parentName -PresetKind $PresetKind
        $value = Get-InheritedPresetValue -PresetObject $parent -PresetObjects $PresetObjects `
            -PropertyName $PropertyName -PresetKind $PresetKind -Depth ($Depth + 1)
        if ($null -ne $value -and [string]$value -ne '') {
            return $value
        }
    }
    return $null
}

function Resolve-CMakePresetPath
{
    param(
        [string]$PathValue,
        [string]$ConfigurePresetName
    )

    $resolved = $PathValue.Replace('${sourceDir}', $engineRoot)
    $resolved = $resolved.Replace('${sourceParentDir}', (Split-Path -Parent $engineRoot))
    $resolved = $resolved.Replace('${presetName}', $ConfigurePresetName)
    if ($resolved -match '\$\{') {
        throw "Unsupported CMake preset macro remains in path '$PathValue'"
    }
    return [System.IO.Path]::GetFullPath($resolved)
}

function Get-CMakeCacheValue
{
    param(
        [string]$CachePath,
        [string]$VariableName
    )

    $prefix = "${VariableName}:"
    foreach ($line in Get-Content -LiteralPath $CachePath -Encoding UTF8) {
        if (!$line.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            return $line.Substring($separator + 1)
        }
    }
    return $null
}

function Resolve-AddressSanitizerTestExecutable
{
    $cachePath = Join-Path $binaryDirectory 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "CMake cache for preset '$Preset' was not found at $cachePath"
    }

    $cacheSourceDirectory = Get-CMakeCacheValue -CachePath $cachePath -VariableName 'CMAKE_HOME_DIRECTORY'
    if (!$cacheSourceDirectory) {
        throw "CMAKE_HOME_DIRECTORY was not found in $cachePath"
    }
    $normalizedCacheSource = [System.IO.Path]::GetFullPath($cacheSourceDirectory).TrimEnd('\', '/')
    $normalizedEngineRoot = [System.IO.Path]::GetFullPath($engineRoot).TrimEnd('\', '/')
    if (![string]::Equals($normalizedCacheSource, $normalizedEngineRoot,
                         [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Preset '$Preset' cache belongs to '$cacheSourceDirectory', not '$engineRoot'"
    }

    $addressSanitizerEnabled = Get-CMakeCacheValue -CachePath $cachePath `
        -VariableName 'ACS_ENABLE_ADDRESS_SANITIZER'
    if (!$addressSanitizerEnabled -or
        @('1', 'ON', 'TRUE', 'YES') -notcontains $addressSanitizerEnabled.ToUpperInvariant()) {
        throw "Preset '$Preset' does not enable ACS_ENABLE_ADDRESS_SANITIZER in $cachePath"
    }

    $layoutRoot = Get-CMakeCacheValue -CachePath $cachePath -VariableName 'ACS_LAYOUT_ROOT'
    if (!$layoutRoot) {
        throw "ACS_LAYOUT_ROOT was not found in $cachePath"
    }
    if (![System.IO.Path]::IsPathRooted($layoutRoot)) {
        $layoutRoot = Join-Path $binaryDirectory $layoutRoot
    }
    $layoutRoot = [System.IO.Path]::GetFullPath($layoutRoot)
    $binaryRoot = Join-Path $layoutRoot 'Binaries'
    $testExecutables = @(Get-ChildItem -LiteralPath $binaryRoot -Filter 'acs_unit_tests.exe' -File -Recurse `
        -ErrorAction SilentlyContinue)
    if ($testExecutables.Count -ne 1) {
        throw "Expected exactly one AddressSanitizer test executable under $binaryRoot; found $($testExecutables.Count)"
    }

    $machineBinaryDirectory = $binaryDirectory.Replace('\', '/').Replace(' ', '%20')
    $machineExecutablePath = $testExecutables[0].FullName.Replace('\', '/').Replace(' ', '%20')
    Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer artifact_validation=ok preset=$Preset sanitizer_enabled=true binary_directory=$machineBinaryDirectory executable_path=$machineExecutablePath"
    return $testExecutables[0]
}

function Assert-TestExecutableFreshness
{
    param([System.IO.FileInfo]$TestExecutable)

    $normalizedBinaryDirectory = [System.IO.Path]::GetFullPath($binaryDirectory).TrimEnd('\', '/')
    $normalizedExecutablePath = [System.IO.Path]::GetFullPath($TestExecutable.FullName)
    $binaryDirectoryPrefix = $normalizedBinaryDirectory + [System.IO.Path]::DirectorySeparatorChar
    if (!$normalizedExecutablePath.StartsWith($binaryDirectoryPrefix,
                                               [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "AddressSanitizer layout must remain inside preset binaryDir: $normalizedExecutablePath"
    }

    $cachePath = Join-Path $binaryDirectory 'CMakeCache.txt'
    $makeProgram = Get-CMakeCacheValue -CachePath $cachePath -VariableName 'CMAKE_MAKE_PROGRAM'
    if (!$makeProgram -or !(Test-Path -LiteralPath $makeProgram -PathType Leaf)) {
        throw "CMAKE_MAKE_PROGRAM was not found for artifact freshness inspection in $cachePath"
    }

    $relativeTarget = $normalizedExecutablePath.Substring($binaryDirectoryPrefix.Length).Replace('\', '/')
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $targetInputs = @(& $makeProgram -C $binaryDirectory -t inputs $relativeTarget 2>&1)
        $inputInspectionExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    Write-Utf8LogWithoutBom -LogPath (Join-Path $ReportDirectory 'artifact-inputs.log') -Lines $targetInputs
    if ($inputInspectionExitCode -ne 0) {
        throw "Build graph input inspection failed with exit code $inputInspectionExitCode"
    }

    $newestInput = $null
    foreach ($inputPath in $targetInputs) {
        $candidatePath = ([string]$inputPath).Trim()
        if (!$candidatePath -or $candidatePath -eq '.') {
            continue
        }
        if (![System.IO.Path]::IsPathRooted($candidatePath)) {
            $candidatePath = Join-Path $binaryDirectory $candidatePath
        }
        if (!(Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
            continue
        }

        $candidate = Get-Item -LiteralPath $candidatePath
        if ($candidate.LastWriteTimeUtc -gt $TestExecutable.LastWriteTimeUtc -and
            ($null -eq $newestInput -or $candidate.LastWriteTimeUtc -gt $newestInput.LastWriteTimeUtc)) {
            $newestInput = $candidate
        }
    }
    if ($null -ne $newestInput) {
        $machineInputPath = $newestInput.FullName.Replace('\', '/').Replace(' ', '%20')
        Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer artifact_freshness=failed reason=input_newer_than_executable input_path=$machineInputPath"
        throw "AddressSanitizer test executable is older than build input $($newestInput.FullName)"
    }

    Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer artifact_freshness=ok build_inputs_newer=false'
}

function Assert-AddressSanitizerInstrumentation
{
    param([System.IO.FileInfo]$TestExecutable)

    if (!(Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
        throw 'dumpbin.exe was not found after Visual Studio environment initialization'
    }

    $inspectionOutput = Invoke-LoggedCommand 'artifact_instrumentation_inspection' {
        & dumpbin.exe /imports $TestExecutable.FullName
    } (Join-Path $ReportDirectory 'artifact-instrumentation.log') -SuppressConsoleOutput
    $joinedInspection = $inspectionOutput -join "`n"
    $hasAddressSanitizerRuntime =
        $joinedInspection -match '(?im)^\s*clang_rt\.asan_dynamic-[^\s]+\.dll\s*$'
    $hasInstrumentedAccessChecks = $joinedInspection -match '\b__asan_report_(load|store)'
    $hasGlobalRegistration = $joinedInspection -match '\b__asan_register_globals\b'
    if (!$hasAddressSanitizerRuntime -or !$hasInstrumentedAccessChecks -or !$hasGlobalRegistration) {
        Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer binary_instrumented=false status=failed reason=pe_imports_missing'
        throw "Test executable is not verifiably AddressSanitizer-instrumented: $($TestExecutable.FullName)"
    }

    Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer binary_instrumented=true inspection=pe_imports status=ok'
}

function Assert-AddressSanitizerCompilerCapability
{
    param([System.IO.FileInfo]$TestExecutable)

    $capabilityOutput = Invoke-LoggedCommand 'artifact_compiler_capability' {
        & $TestExecutable.FullName --address-sanitizer-capability-probe
    } (Join-Path $ReportDirectory 'artifact-capability-probe.log') -SuppressConsoleOutput
    $joinedCapabilityOutput = $capabilityOutput -join "`n"
    if ($joinedCapabilityOutput -notmatch
        'memory_diagnostic_method=address_sanitizer\s+binary_instrumented=true\s+compiler_macro=__SANITIZE_ADDRESS__') {
        Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer compiler_instrumented=false status=failed reason=capability_record_missing'
        throw "Test executable did not confirm compiler-level AddressSanitizer instrumentation: $($TestExecutable.FullName)"
    }

    Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer compiler_instrumented=true inspection=runtime_capability_record status=ok'
}

function Import-VisualStudioEnvironment
{
    $candidates = @()
    $visualStudioLocator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $visualStudioLocator) {
        $installationPath = & $visualStudioLocator -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidates += (Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat')
        }
    }
    $candidates += @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
    )
    $developerCommand = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$developerCommand) { throw 'VsDevCmd.bat was not found' }

    $developerShell = Join-Path (Split-Path -Parent $developerCommand) 'Launch-VsDevShell.ps1'
    if (Test-Path -LiteralPath $developerShell) {
        & $developerShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        return
    }

    # バッチが設定した環境変数を現在のPowerShellプロセスへ取り込む。
    $commandLine = 'call "' + $developerCommand + '" -arch=amd64 -host_arch=amd64 >nul && set'
    $environmentLines = @(& $env:ComSpec /d /c $commandLine)
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed' }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) { continue }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

function Invoke-LoggedCommand
{
    param(
        [string]$Name,
        [scriptblock]$Command,
        [string]$LogPath,
        [switch]$AllowFailure,
        [switch]$SuppressConsoleOutput
    )

    Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer phase=$Name status=running"
    # Windows PowerShell はnative stderrをErrorRecordへ変換するため、終了コードを
    # 取得するまで一時的に停止動作を無効化する。
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $Command 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if (!$SuppressConsoleOutput) {
        $output | ForEach-Object { Write-Host $_ }
    }
    Write-Utf8LogWithoutBom -LogPath $LogPath -Lines $output
    $script:LastLoggedCommandExitCode = $exitCode
    if ($exitCode -ne 0) {
        Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer phase=$Name status=failed exit_code=$exitCode"
        if (!$AllowFailure) {
            throw "AddressSanitizer $Name failed with exit code $exitCode"
        }
        return $output
    }
    Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer phase=$Name status=ok"
    return $output
}

function Assert-AddressSanitizerRunResult
{
    param(
        [object[]]$Output,
        [int]$ExitCode
    )

    $joinedOutput = $Output -join "`n"
    if ($joinedOutput -match 'ERROR:\s*AddressSanitizer|SUMMARY:\s*AddressSanitizer|LeakSanitizer:') {
        Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer memory_error_detected=true'
        throw 'AddressSanitizer reported a memory error'
    }
    if ($ExitCode -ne 0) {
        Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer memory_error_detected=inconclusive status=failed exit_code=$ExitCode"
        throw "AddressSanitizer test process failed with exit code $ExitCode"
    }

    Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer memory_error_detected=false status=ok'
}

function Test-UnsupportedAddressSanitizerLeakDetection
{
    param(
        [object[]]$Output,
        [int]$ExitCode
    )

    if ($ExitCode -eq 0) {
        return $false
    }

    $joinedOutput = $Output -join "`n"
    $unsupported = $joinedOutput -match 'AddressSanitizer:\s*detect_leaks is not supported on this platform'
    $memoryError = $joinedOutput -match 'ERROR:\s*AddressSanitizer|SUMMARY:\s*AddressSanitizer|LeakSanitizer:'
    return $unsupported -and !$memoryError
}

if (!(Test-Path -LiteralPath $presetDocumentPath -PathType Leaf)) {
    throw "CMake preset document was not found at $presetDocumentPath"
}
$presetDocument = Get-Content -LiteralPath $presetDocumentPath -Raw -Encoding UTF8 | ConvertFrom-Json
$buildPresetObject = Get-PresetObject -PresetObjects @($presetDocument.buildPresets) `
    -PresetName $Preset -PresetKind 'build'
$configurePresetName = [string](Get-InheritedPresetValue -PresetObject $buildPresetObject `
    -PresetObjects @($presetDocument.buildPresets) -PropertyName 'configurePreset' -PresetKind 'build')
if (!$configurePresetName) {
    throw "Build preset '$Preset' does not resolve to a configure preset"
}
$configurePresetObject = Get-PresetObject -PresetObjects @($presetDocument.configurePresets) `
    -PresetName $configurePresetName -PresetKind 'configure'
$binaryDirectoryTemplate = [string](Get-InheritedPresetValue -PresetObject $configurePresetObject `
    -PresetObjects @($presetDocument.configurePresets) -PropertyName 'binaryDir' -PresetKind 'configure')
if (!$binaryDirectoryTemplate) {
    throw "Configure preset '$configurePresetName' does not resolve to a binaryDir"
}
$binaryDirectory = Resolve-CMakePresetPath -PathValue $binaryDirectoryTemplate `
    -ConfigurePresetName $configurePresetName
Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer preset_resolution=ok build_preset=$Preset configure_preset=$configurePresetName"

if (!(Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'cmake.exe was not found'
}
Import-VisualStudioEnvironment
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe was not found after Visual Studio environment initialization'
}

if (!$SkipConfigure) {
    Invoke-LoggedCommand 'configure' {
        Push-Location $engineRoot
        try { & cmake.exe --preset $configurePresetName }
        finally { Pop-Location }
    } (Join-Path $ReportDirectory 'configure.log') | Out-Null
}

if (!$SkipBuild) {
    Invoke-LoggedCommand 'build' {
        Push-Location $engineRoot
        try { & cmake.exe --build --preset $Preset --parallel $BuildParallelism --target acs_unit_tests }
        finally { Pop-Location }
    } (Join-Path $ReportDirectory 'build.log') | Out-Null
}

if ($SkipRun) {
    Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer status=not_measured reason=run_skipped'
    exit 0
}

try {
    $testExecutable = Resolve-AddressSanitizerTestExecutable
    Assert-TestExecutableFreshness -TestExecutable $testExecutable
    Assert-AddressSanitizerInstrumentation -TestExecutable $testExecutable
    Assert-AddressSanitizerCompilerCapability -TestExecutable $testExecutable
} catch {
    Write-DiagnosticRecord "memory_diagnostic_method=address_sanitizer artifact_validation=failed preset=$Preset status=failed"
    throw
}

$previousOptions = $env:ASAN_OPTIONS
try {
    $baseOptions = 'halt_on_error=1:abort_on_error=1:allocator_may_return_null=0:symbolize=1'
    $probeLog = Join-Path $ReportDirectory 'run-leak-detection-probe.log'
    $runLog = Join-Path $ReportDirectory 'run.log'

    # LeakSanitizer対応ランタイムでは、この実行をそのまま最終検査結果として使う。
    $env:ASAN_OPTIONS = "$baseOptions`:detect_leaks=1"
    $output = Invoke-LoggedCommand 'run_leak_detection_probe' {
        & $testExecutable.FullName
    } $probeLog -AllowFailure
    $runExitCode = $script:LastLoggedCommandExitCode

    if (Test-UnsupportedAddressSanitizerLeakDetection -Output $output -ExitCode $runExitCode) {
        Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer capability=leak_detection available=false reason=runtime_unsupported'

        # MSVC版などLeakSanitizer未対応環境でも、境界外アクセス、解放後使用、
        # 二重解放などのAddressSanitizer検査は省略しない。
        $env:ASAN_OPTIONS = $baseOptions
        $output = Invoke-LoggedCommand 'run' {
            & $testExecutable.FullName
        } $runLog -AllowFailure
        $runExitCode = $script:LastLoggedCommandExitCode
    } else {
        Write-DiagnosticRecord 'memory_diagnostic_method=address_sanitizer capability=leak_detection available=true'
        Copy-Item -LiteralPath $probeLog -Destination $runLog -Force
    }
} finally {
    $env:ASAN_OPTIONS = $previousOptions
}

Assert-AddressSanitizerRunResult -Output $output -ExitCode $runExitCode
