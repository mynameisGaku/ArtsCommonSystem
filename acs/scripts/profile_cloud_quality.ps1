# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
ACS の体積雲 fixture を地平線、天頂、上空の 3 視点で計測する。

.DESCRIPTION
同一の品質設定と決定論的な 3 台のカメラで、Editor の無人計測を直列実行する。
各 profiler report は、実際の 3D／雲描画、GPU 時間、正確な雲品質、時間履歴の再利用、
Editor scheduler 診断が揃わない限り失敗する。

既定の 300 FPS 目標は品質 gate と分けて報告する。目標未達を終了 code へ反映するのは
-RequireTargetFps を指定した場合だけである。

生成物は現在の process の TEMP 配下だけへ置く。既存の report、capture、log、summary は
上書きしない。

.PARAMETER EditorExe
AcsEditor.exe の path。

.PARAMETER Project
3D .acsproject fixture の path。

.PARAMETER OutputDirectory
TEMP 配下の実行専用 directory。省略時は重複しない directory を選ぶ。

.PARAMETER SoakSeconds
地平線、天頂、上空の各取得時間。

.PARAMETER Monitor
secondary は --secondary-monitor、primary は --monitor 0 を使う。none は画面配置を指定しない。

.PARAMETER MonitorIndex
明示する --monitor index。0 以上なら -Monitor より優先する。

.PARAMETER RequireTargetFps
いずれかの視点が -TargetFps 未満なら、しきい値 gate を失敗させる。

.PARAMETER SelfTest
合成 report の parser と検証境界だけを自己試験する。

.PARAMETER DryRun
出力を作らずに入力を検証し、3 本の Editor command を表示する。
#>
[CmdletBinding()]
param(
    [string]$EditorExe,
    [string]$Project,
    [string]$OutputDirectory,

    [ValidateRange(5, 600)]
    [int]$SoakSeconds = 30,

    [ValidateSet("secondary", "primary", "none")]
    [string]$Monitor = "secondary",

    [ValidateRange(-1, 31)]
    [int]$MonitorIndex = -1,

    [ValidateRange(1.0, 2000.0)]
    [double]$TargetFps = 300.0,

    [ValidateRange(30, 600)]
    [int]$StartupGraceSeconds = 120,

    [switch]$RequireTargetFps,
    [switch]$SelfTest,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$script:ExpectedCloudScale = 0.25
$script:ExpectedViewSamples = 192
$script:ExpectedLightSamples = 8
$script:ExpectedSteadyDispatches = 2
$script:ExpectedShadowCacheDispatches = 1
$script:ExpectedShadowCacheLogicalInvocations = 48 * 32 * 48
$script:ExpectedShadowCacheLaunchedThreads = 48 * 32 * 48
$script:ExpectedWorldShadowDispatches = 1
$script:ExpectedWorldShadowLogicalInvocations = 128 * 128
$script:ExpectedWorldShadowLaunchedThreads = 128 * 128
$script:ExpectedWorldShadowSamples = 128 * 128 * 32
$script:ExpectedCompositeDraws = 1
$script:SummarySchemaVersion = 5
$script:ExpectedEditorArtifactRoles = @(
    "ManagedAssembly",
    "NativeRenderer",
    "DependencyManifest",
    "RuntimeConfig"
)

function Add-CloudFault {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Faults,
        [Parameter(Mandatory = $true)]
        [string]$Code
    )

    if (-not $Faults.Contains($Code)) {
        [void]$Faults.Add($Code)
    }
}

function Test-CloudProperties {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$FaultPrefix,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Faults
    )

    if ($null -eq $Value) {
        Add-CloudFault -Faults $Faults -Code ($FaultPrefix + "_MISSING")
        return $false
    }

    $valid = $true
    foreach ($name in $Names) {
        if ($null -eq $Value.PSObject.Properties[$name]) {
            Add-CloudFault `
                -Faults $Faults `
                -Code ($FaultPrefix + "_MISSING_" + $name.ToUpperInvariant())
            $valid = $false
        }
    }
    return $valid
}

function Test-FiniteNumber {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return $false
    }
    if ($Value -isnot [byte] -and
        $Value -isnot [sbyte] -and
        $Value -isnot [int16] -and
        $Value -isnot [uint16] -and
        $Value -isnot [int32] -and
        $Value -isnot [uint32] -and
        $Value -isnot [int64] -and
        $Value -isnot [uint64] -and
        $Value -isnot [single] -and
        $Value -isnot [double] -and
        $Value -isnot [decimal]) {
        return $false
    }
    try {
        $number = [System.Convert]::ToDouble($Value, $script:Invariant)
        return -not [double]::IsNaN($number) -and
            -not [double]::IsInfinity($number)
    }
    catch {
        return $false
    }
}

function Test-PositiveNumber {
    param([AllowNull()][object]$Value)

    return (Test-FiniteNumber -Value $Value) -and
        ([System.Convert]::ToDouble($Value, $script:Invariant) -gt 0.0)
}

function Test-NonNegativeNumber {
    param([AllowNull()][object]$Value)

    return (Test-FiniteNumber -Value $Value) -and
        ([System.Convert]::ToDouble($Value, $script:Invariant) -ge 0.0)
}

function Test-NonNegativeInteger {
    param([AllowNull()][object]$Value)

    if (-not (Test-FiniteNumber -Value $Value)) {
        return $false
    }
    try {
        $number = [System.Convert]::ToDecimal($Value, $script:Invariant)
        return $number -ge [decimal]0 -and
            $number -le [decimal][int64]::MaxValue -and
            [decimal]::Truncate($number) -eq $number
    }
    catch {
        return $false
    }
}

function Test-PositiveInteger {
    param([AllowNull()][object]$Value)

    return (Test-NonNegativeInteger -Value $Value) -and
        ([System.Convert]::ToDecimal($Value, $script:Invariant) -gt
            [decimal]0)
}

function Test-CloudBoolean {
    param([AllowNull()][object]$Value)

    return $Value -is [bool]
}

function Test-NearlyEqual {
    param(
        [AllowNull()][object]$Left,
        [AllowNull()][object]$Right,
        [double]$Tolerance = 0.000001
    )

    if (-not (Test-FiniteNumber -Value $Left) -or
        -not (Test-FiniteNumber -Value $Right)) {
        return $false
    }
    $a = [System.Convert]::ToDouble($Left, $script:Invariant)
    $b = [System.Convert]::ToDouble($Right, $script:Invariant)
    return [math]::Abs($a - $b) -le $Tolerance
}

function ConvertTo-CloudDouble {
    param([AllowNull()][object]$Value)

    if (-not (Test-FiniteNumber -Value $Value)) {
        return $null
    }
    return [System.Convert]::ToDouble($Value, $script:Invariant)
}

function ConvertTo-CloudInt64 {
    param([AllowNull()][object]$Value)

    if (-not (Test-NonNegativeInteger -Value $Value)) {
        return [int64]0
    }
    try {
        return [System.Convert]::ToInt64($Value, $script:Invariant)
    }
    catch {
        return [int64]0
    }
}

function Get-CloudSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::Open(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        $bytes = $sha256.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).
            Replace("-", "").
            ToLowerInvariant()
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        $sha256.Dispose()
    }
}

function Get-CloudFileIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$IncludeVersion
    )

    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Input identity path became a reparse point: $Path"
    }
    if ($item.PSIsContainer -or $item.Length -le 0) {
        throw "Input identity path is not a non-empty file: $Path"
    }
    $fileVersion = $null
    $productVersion = $null
    if ($IncludeVersion) {
        $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
            $item.FullName)
        if (-not [string]::IsNullOrWhiteSpace($version.FileVersion)) {
            $fileVersion = $version.FileVersion
        }
        if (-not [string]::IsNullOrWhiteSpace($version.ProductVersion)) {
            $productVersion = $version.ProductVersion
        }
    }
    return [pscustomobject][ordered]@{
        Path = $item.FullName
        Sha256 = Get-CloudSha256 -Path $item.FullName
        LengthBytes = [int64]$item.Length
        LastWriteUtc = $item.LastWriteTimeUtc.ToString(
            "O",
            $script:Invariant)
        FileVersion = $fileVersion
        ProductVersion = $productVersion
    }
}

function Get-CloudArtifactIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$IncludeVersion
    )

    $identity = Get-CloudFileIdentity `
        -Path $Path `
        -IncludeVersion:$IncludeVersion
    return [pscustomobject][ordered]@{
        Role = $Role
        Path = $identity.Path
        Sha256 = $identity.Sha256
        LengthBytes = $identity.LengthBytes
        LastWriteUtc = $identity.LastWriteUtc
        FileVersion = $identity.FileVersion
        ProductVersion = $identity.ProductVersion
    }
}

function Get-CloudEditorArtifactDefinitions {
    param([Parameter(Mandatory = $true)][string]$EditorPath)

    $editorDirectory = [System.IO.Path]::GetDirectoryName($EditorPath)
    $editorBaseName =
        [System.IO.Path]::GetFileNameWithoutExtension($EditorPath)
    return @(
        [pscustomobject][ordered]@{
            Role = "ManagedAssembly"
            Path = Join-Path $editorDirectory ($editorBaseName + ".dll")
            IncludeVersion = $true
        },
        [pscustomobject][ordered]@{
            Role = "NativeRenderer"
            Path = Join-Path $editorDirectory "acs_editor_abi.dll"
            IncludeVersion = $false
        },
        [pscustomobject][ordered]@{
            Role = "DependencyManifest"
            Path =
                Join-Path $editorDirectory ($editorBaseName + ".deps.json")
            IncludeVersion = $false
        },
        [pscustomobject][ordered]@{
            Role = "RuntimeConfig"
            Path =
                Join-Path `
                    $editorDirectory `
                    ($editorBaseName + ".runtimeconfig.json")
            IncludeVersion = $false
        }
    )
}

function Open-CloudInputLeases {
    param([Parameter(Mandatory = $true)][string[]]$Paths)

    $leases =
        New-Object "System.Collections.Generic.List[System.IO.FileStream]"
    try {
        foreach ($path in $Paths) {
            $stream = [System.IO.File]::Open(
                $path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read)
            [void]$leases.Add($stream)
        }
        return @($leases.ToArray())
    }
    catch {
        foreach ($lease in $leases) {
            $lease.Dispose()
        }
        throw
    }
}

function ConvertTo-CloudUtcTimestamp {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return $null
    }
    try {
        return ([DateTimeOffset]$Value).
            ToUniversalTime().
            ToString("O", $script:Invariant)
    }
    catch {
        return [System.Convert]::ToString($Value, $script:Invariant)
    }
}

function Test-CloudTimestamp {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or
        [string]::IsNullOrWhiteSpace([string]$Value)) {
        return $false
    }
    $timestamp = [DateTimeOffset]::MinValue
    return [DateTimeOffset]::TryParse(
        [string]$Value,
        $script:Invariant,
        [System.Globalization.DateTimeStyles]::RoundtripKind,
        [ref]$timestamp)
}

function Get-CloudRunEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$EditorPath,
        [Parameter(Mandatory = $true)][string]$ProjectPath
    )

    $editorArtifacts = foreach ($definition in
        (Get-CloudEditorArtifactDefinitions -EditorPath $EditorPath)) {
        Get-CloudArtifactIdentity `
            -Role $definition.Role `
            -Path $definition.Path `
            -IncludeVersion:$definition.IncludeVersion
    }

    $osDescription = [Environment]::OSVersion.VersionString
    $osArchitecture = $env:PROCESSOR_ARCHITECTURE
    $frameworkDescription = [Environment]::Version.ToString()
    try {
        $runtimeInformation =
            [System.Runtime.InteropServices.RuntimeInformation]
        $osDescription = $runtimeInformation::OSDescription
        $osArchitecture = $runtimeInformation::OSArchitecture.ToString()
        $frameworkDescription = $runtimeInformation::FrameworkDescription
    }
    catch {
        # Windows PowerShell on an older .NET Framework still has stable
        # Environment fallbacks above.
    }

    $windowsCaption = $null
    $windowsVersion = $null
    $windowsBuild = $null
    $osProbeError = $null
    try {
        $windowsOs = Get-CimInstance `
            -ClassName Win32_OperatingSystem `
            -Property Caption,Version,BuildNumber `
            -OperationTimeoutSec 5 `
            -ErrorAction Stop |
            Select-Object -First 1
        if ($null -ne $windowsOs) {
            $windowsCaption = [string]$windowsOs.Caption
            $windowsVersion = [string]$windowsOs.Version
            $windowsBuild = [string]$windowsOs.BuildNumber
        }
    }
    catch {
        $osProbeError = $_.Exception.Message
    }

    $gpuAdapters =
        New-Object "System.Collections.Generic.List[object]"
    $gpuProbeError = $null
    try {
        $controllers = @(
            Get-CimInstance `
                -ClassName Win32_VideoController `
                -Property Name,AdapterCompatibility,DriverVersion,DriverDate,
                    PNPDeviceID,VideoProcessor,AdapterRAM,Status `
                -OperationTimeoutSec 5 `
                -ErrorAction Stop |
                Sort-Object Name,PNPDeviceID
        )
        foreach ($controller in $controllers) {
            $adapterRam = $null
            if ($null -ne $controller.AdapterRAM) {
                try {
                    $adapterRam = [uint64]$controller.AdapterRAM
                }
                catch {
                    $adapterRam = $null
                }
            }
            [void]$gpuAdapters.Add([pscustomobject][ordered]@{
                Name = [string]$controller.Name
                AdapterCompatibility =
                    [string]$controller.AdapterCompatibility
                DriverVersion = [string]$controller.DriverVersion
                DriverDateUtc =
                    ConvertTo-CloudUtcTimestamp -Value $controller.DriverDate
                PnpDeviceId = [string]$controller.PNPDeviceID
                VideoProcessor = [string]$controller.VideoProcessor
                AdapterRamBytes = $adapterRam
                Status = [string]$controller.Status
            })
        }
    }
    catch {
        $gpuProbeError = $_.Exception.Message
    }

    $powerShellEdition = $null
    if ($null -ne $PSVersionTable.PSObject.Properties["PSEdition"]) {
        $powerShellEdition = [string]$PSVersionTable.PSEdition
    }
    return [pscustomobject][ordered]@{
        SchemaVersion = 2
        CapturedUtc =
            [DateTimeOffset]::UtcNow.ToString("O", $script:Invariant)
        MachineName = [Environment]::MachineName
        OperatingSystem = [pscustomobject][ordered]@{
            Description = $osDescription
            Architecture = $osArchitecture
            WindowsCaption = $windowsCaption
            WindowsVersion = $windowsVersion
            WindowsBuild = $windowsBuild
            ProbeError = $osProbeError
        }
        Runtime = [pscustomobject][ordered]@{
            PowerShellVersion = $PSVersionTable.PSVersion.ToString()
            PowerShellEdition = $powerShellEdition
            FrameworkDescription = $frameworkDescription
            ProcessArchitecture = $env:PROCESSOR_ARCHITECTURE
        }
        EditorExecutable =
            Get-CloudFileIdentity -Path $EditorPath -IncludeVersion
        EditorRuntimeArtifacts = @($editorArtifacts)
        Project = Get-CloudFileIdentity -Path $ProjectPath
        GpuProbe = [pscustomobject][ordered]@{
            Status = if ($gpuAdapters.Count -gt 0) {
                "Available"
            }
            else {
                "Unavailable"
            }
            Error = $gpuProbeError
            Adapters = @($gpuAdapters.ToArray())
        }
    }
}

function Test-CloudRunEnvironment {
    param([AllowNull()][object]$EnvironmentSnapshot)

    $faults = New-Object "System.Collections.Generic.List[string]"
    if (-not (Test-CloudProperties `
            -Value $EnvironmentSnapshot `
            -Names @(
                "SchemaVersion",
                "CapturedUtc",
                "MachineName",
                "OperatingSystem",
                "Runtime",
                "EditorExecutable",
                "EditorRuntimeArtifacts",
                "Project",
                "GpuProbe") `
            -FaultPrefix "ENVIRONMENT" `
            -Faults $faults)) {
        return [pscustomobject][ordered]@{
            Pass = $false
            FaultCodes = @($faults.ToArray())
        }
    }

    if (-not (Test-NonNegativeInteger `
            -Value $EnvironmentSnapshot.SchemaVersion) -or
        [int64]$EnvironmentSnapshot.SchemaVersion -ne 2) {
        Add-CloudFault -Faults $faults -Code "ENVIRONMENT_SCHEMA_UNSUPPORTED"
    }
    if (-not (Test-CloudTimestamp `
            -Value $EnvironmentSnapshot.CapturedUtc)) {
        Add-CloudFault -Faults $faults -Code "ENVIRONMENT_UTC_INVALID"
    }
    if ([string]::IsNullOrWhiteSpace(
            [string]$EnvironmentSnapshot.MachineName)) {
        Add-CloudFault -Faults $faults -Code "ENVIRONMENT_MACHINE_INVALID"
    }

    if (-not (Test-CloudProperties `
            -Value $EnvironmentSnapshot.OperatingSystem `
            -Names @("Description", "Architecture") `
            -FaultPrefix "OPERATING_SYSTEM" `
            -Faults $faults)) {
        # Property-level fault codes are sufficient.
    }
    elseif ([string]::IsNullOrWhiteSpace(
            [string]$EnvironmentSnapshot.OperatingSystem.Description) -or
        [string]::IsNullOrWhiteSpace(
            [string]$EnvironmentSnapshot.OperatingSystem.Architecture)) {
        Add-CloudFault `
            -Faults $faults `
            -Code "OPERATING_SYSTEM_IDENTITY_INVALID"
    }

    if (Test-CloudProperties `
            -Value $EnvironmentSnapshot.Runtime `
            -Names @(
                "PowerShellVersion",
                "FrameworkDescription",
                "ProcessArchitecture") `
            -FaultPrefix "RUNTIME" `
            -Faults $faults) {
        if ([string]::IsNullOrWhiteSpace(
                [string]$EnvironmentSnapshot.Runtime.PowerShellVersion) -or
            [string]::IsNullOrWhiteSpace(
                [string]$EnvironmentSnapshot.Runtime.FrameworkDescription) -or
            [string]::IsNullOrWhiteSpace(
                [string]$EnvironmentSnapshot.Runtime.ProcessArchitecture)) {
            Add-CloudFault -Faults $faults -Code "RUNTIME_IDENTITY_INVALID"
        }
    }

    foreach ($entry in @(
            [pscustomobject]@{
                Name = "EDITOR"
                Value = $EnvironmentSnapshot.EditorExecutable
                VersionRequired = $true
            },
            [pscustomobject]@{
                Name = "PROJECT"
                Value = $EnvironmentSnapshot.Project
                VersionRequired = $false
            })) {
        $identityValid = Test-CloudProperties `
            -Value $entry.Value `
            -Names @(
                "Path",
                "Sha256",
                "LengthBytes",
                "LastWriteUtc",
                "FileVersion",
                "ProductVersion") `
            -FaultPrefix ($entry.Name + "_IDENTITY") `
            -Faults $faults
        if (-not $identityValid) {
            continue
        }
        if ([string]::IsNullOrWhiteSpace([string]$entry.Value.Path) -or
            [string]$entry.Value.Sha256 -notmatch "^[0-9a-f]{64}$" -or
            -not (Test-PositiveInteger -Value $entry.Value.LengthBytes) -or
            -not (Test-CloudTimestamp -Value $entry.Value.LastWriteUtc)) {
            Add-CloudFault `
                -Faults $faults `
                -Code ($entry.Name + "_IDENTITY_INVALID")
        }
        if ($entry.VersionRequired -and
            [string]::IsNullOrWhiteSpace([string]$entry.Value.FileVersion) -and
            [string]::IsNullOrWhiteSpace(
                [string]$entry.Value.ProductVersion)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_VERSION_MISSING"
        }
    }

    $runtimeArtifacts = @($EnvironmentSnapshot.EditorRuntimeArtifacts)
    if ($runtimeArtifacts.Count -ne
        $script:ExpectedEditorArtifactRoles.Count) {
        Add-CloudFault `
            -Faults $faults `
            -Code "EDITOR_RUNTIME_ARTIFACT_COUNT_INVALID"
    }
    $seenArtifactRoles =
        New-Object "System.Collections.Generic.HashSet[string]" (
            [System.StringComparer]::Ordinal)
    foreach ($artifact in $runtimeArtifacts) {
        $artifactValid = Test-CloudProperties `
            -Value $artifact `
            -Names @(
                "Role",
                "Path",
                "Sha256",
                "LengthBytes",
                "LastWriteUtc",
                "FileVersion",
                "ProductVersion") `
            -FaultPrefix "EDITOR_RUNTIME_ARTIFACT" `
            -Faults $faults
        if (-not $artifactValid) {
            continue
        }
        $role = [string]$artifact.Role
        if ($script:ExpectedEditorArtifactRoles -notcontains $role -or
            -not $seenArtifactRoles.Add($role)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_ARTIFACT_ROLE_INVALID"
        }
        if ([string]::IsNullOrWhiteSpace([string]$artifact.Path) -or
            [string]$artifact.Sha256 -notmatch "^[0-9a-f]{64}$" -or
            -not (Test-PositiveInteger -Value $artifact.LengthBytes) -or
            -not (Test-CloudTimestamp -Value $artifact.LastWriteUtc)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_ARTIFACT_IDENTITY_INVALID"
        }
        if ($role -eq "ManagedAssembly" -and
            [string]::IsNullOrWhiteSpace([string]$artifact.FileVersion) -and
            [string]::IsNullOrWhiteSpace(
                [string]$artifact.ProductVersion)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_MANAGED_ASSEMBLY_VERSION_MISSING"
        }
    }
    foreach ($expectedRole in $script:ExpectedEditorArtifactRoles) {
        if (-not $seenArtifactRoles.Contains($expectedRole)) {
            Add-CloudFault `
                -Faults $faults `
                -Code (
                    "EDITOR_RUNTIME_ARTIFACT_MISSING_" +
                    $expectedRole.ToUpperInvariant())
        }
    }

    $gpuProbeValid = Test-CloudProperties `
        -Value $EnvironmentSnapshot.GpuProbe `
        -Names @("Status", "Error", "Adapters") `
        -FaultPrefix "GPU_PROBE" `
        -Faults $faults
    if ($gpuProbeValid) {
        $adapters = @($EnvironmentSnapshot.GpuProbe.Adapters)
        if ([string]$EnvironmentSnapshot.GpuProbe.Status -cne "Available" -or
            $adapters.Count -eq 0) {
            Add-CloudFault -Faults $faults -Code "GPU_IDENTITY_UNAVAILABLE"
        }
        foreach ($adapter in $adapters) {
            if ($null -eq $adapter.PSObject.Properties["Name"] -or
                $null -eq $adapter.PSObject.Properties["DriverVersion"] -or
                $null -eq $adapter.PSObject.Properties["DriverDateUtc"] -or
                [string]::IsNullOrWhiteSpace([string]$adapter.Name) -or
                [string]::IsNullOrWhiteSpace(
                    [string]$adapter.DriverVersion) -or
                -not (Test-CloudTimestamp -Value $adapter.DriverDateUtc)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code "GPU_ADAPTER_OR_DRIVER_INVALID"
            }
        }
    }

    return [pscustomobject][ordered]@{
        Pass = $faults.Count -eq 0
        FaultCodes = @($faults.ToArray())
    }
}

function Test-CloudInputStability {
    param(
        [Parameter(Mandatory = $true)][object]$InitialEnvironment,
        [Parameter(Mandatory = $true)][string]$EditorPath,
        [Parameter(Mandatory = $true)][string]$ProjectPath
    )

    $faults = New-Object "System.Collections.Generic.List[string]"
    $finalEditor = $null
    $finalEditorArtifacts =
        New-Object "System.Collections.Generic.List[object]"
    $finalProject = $null
    try {
        $finalEditor =
            Get-CloudFileIdentity -Path $EditorPath -IncludeVersion
        if ([string]$finalEditor.Sha256 -ne
            [string]$InitialEnvironment.EditorExecutable.Sha256) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_EXECUTABLE_CHANGED_DURING_RUN"
        }
    }
    catch {
        Add-CloudFault `
            -Faults $faults `
            -Code "EDITOR_FINAL_IDENTITY_UNAVAILABLE"
    }
    if ($null -eq $InitialEnvironment.PSObject.Properties[
            "EditorRuntimeArtifacts"] -or
        $null -eq $InitialEnvironment.EditorRuntimeArtifacts) {
        Add-CloudFault `
            -Faults $faults `
            -Code "EDITOR_RUNTIME_ARTIFACT_BASELINE_MISSING"
    }
    else {
        foreach ($initialArtifact in @(
                $InitialEnvironment.EditorRuntimeArtifacts)) {
            $role = [string]$initialArtifact.Role
            $roleCode =
                [regex]::Replace($role, "[^A-Za-z0-9]", "_").
                    ToUpperInvariant()
            if ([string]::IsNullOrWhiteSpace($roleCode)) {
                $roleCode = "UNKNOWN"
            }
            try {
                $finalArtifact = Get-CloudArtifactIdentity `
                    -Role $role `
                    -Path ([string]$initialArtifact.Path) `
                    -IncludeVersion:($role -eq "ManagedAssembly")
                [void]$finalEditorArtifacts.Add($finalArtifact)
                if ([string]$finalArtifact.Sha256 -ne
                    [string]$initialArtifact.Sha256) {
                    Add-CloudFault `
                        -Faults $faults `
                        -Code (
                            "EDITOR_RUNTIME_ARTIFACT_CHANGED_" +
                            $roleCode)
                }
            }
            catch {
                Add-CloudFault `
                    -Faults $faults `
                    -Code (
                        "EDITOR_RUNTIME_ARTIFACT_FINAL_IDENTITY_UNAVAILABLE_" +
                        $roleCode)
            }
        }
    }
    try {
        $finalProject = Get-CloudFileIdentity -Path $ProjectPath
        if ([string]$finalProject.Sha256 -ne
            [string]$InitialEnvironment.Project.Sha256) {
            Add-CloudFault `
                -Faults $faults `
                -Code "PROJECT_CHANGED_DURING_RUN"
        }
    }
    catch {
        Add-CloudFault `
            -Faults $faults `
            -Code "PROJECT_FINAL_IDENTITY_UNAVAILABLE"
    }
    return [pscustomobject][ordered]@{
        Pass = $faults.Count -eq 0
        FaultCodes = @($faults.ToArray())
        FinalEditorExecutable = $finalEditor
        FinalEditorRuntimeArtifacts =
            @($finalEditorArtifacts.ToArray())
        FinalProject = $finalProject
    }
}

function New-CloudValidationResult {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Faults,
        [AllowNull()][object]$Measurements,
        [AllowNull()][object]$Quality
    )

    return [pscustomobject][ordered]@{
        Pass = $Faults.Count -eq 0
        FaultCodes = @($Faults.ToArray())
        Measurements = $Measurements
        Quality = $Quality
    }
}

function Test-CloudQualityReport {
    param([AllowNull()][object]$Report)

    $faults = New-Object "System.Collections.Generic.List[string]"
    $rootProperties = @(
        "SchemaVersion",
        "Result",
        "RequestedSeconds",
        "FaultCodes",
        "ProfilerCapturePath",
        "ProfilerCaptureError",
        "ProfilerSummary"
    )
    if (-not (Test-CloudProperties `
            -Value $Report `
            -Names $rootProperties `
            -FaultPrefix "REPORT" `
            -Faults $faults)) {
        return New-CloudValidationResult `
            -Faults $faults `
            -Measurements $null `
            -Quality $null
    }

    if (-not (Test-PositiveInteger -Value $Report.SchemaVersion) -or
        [int64]$Report.SchemaVersion -lt 2) {
        Add-CloudFault -Faults $faults -Code "REPORT_SCHEMA_UNSUPPORTED"
    }
    if ([string]$Report.Result -cne "PASS") {
        Add-CloudFault -Faults $faults -Code "REPORT_RESULT_NOT_PASS"
    }
    if (@($Report.FaultCodes).Count -ne 0) {
        Add-CloudFault -Faults $faults -Code "REPORT_FAULT_CODES_PRESENT"
    }
    if ($null -ne $Report.ProfilerCaptureError -and
        -not [string]::IsNullOrWhiteSpace(
            [System.Convert]::ToString(
                $Report.ProfilerCaptureError,
                $script:Invariant))) {
        Add-CloudFault -Faults $faults -Code "REPORT_PROFILER_CAPTURE_ERROR"
    }

    $summary = $Report.ProfilerSummary
    $summaryProperties = @(
        "SchemaVersion",
        "SampleCount",
        "UsesObservedCadence",
        "EditorFps",
        "ObservedFrameIntervalMilliseconds",
        "EditorFpsFromP95FrameInterval",
        "GpuQueryMilliseconds",
        "GpuThroughputFromAverageMilliseconds",
        "GpuThroughputFromP95Milliseconds",
        "LatestGpuQueryWindow",
        "LatestRenderState",
        "LatestCloudWorkload",
        "LatestEditorRuntime"
    )
    if (-not (Test-CloudProperties `
            -Value $summary `
            -Names $summaryProperties `
            -FaultPrefix "PROFILER_SUMMARY" `
            -Faults $faults)) {
        return New-CloudValidationResult `
            -Faults $faults `
            -Measurements $null `
            -Quality $null
    }

    if (-not (Test-PositiveInteger -Value $summary.SchemaVersion) -or
        [int64]$summary.SchemaVersion -lt 2) {
        Add-CloudFault `
            -Faults $faults `
            -Code "PROFILER_SUMMARY_SCHEMA_UNSUPPORTED"
    }
    if (-not (Test-PositiveInteger -Value $summary.SampleCount)) {
        Add-CloudFault -Faults $faults -Code "PROFILER_NO_SAMPLES"
    }
    if (-not (Test-CloudBoolean -Value $summary.UsesObservedCadence) -or
        -not [bool]$summary.UsesObservedCadence) {
        Add-CloudFault `
            -Faults $faults `
            -Code "PROFILER_NOT_USING_OBSERVED_CADENCE"
    }

    $metricProperties = @("SampleCount", "Average", "P95")
    $editorMetricValid = Test-CloudProperties `
        -Value $summary.EditorFps `
        -Names $metricProperties `
        -FaultPrefix "EDITOR_FPS" `
        -Faults $faults
    $intervalMetricValid = Test-CloudProperties `
        -Value $summary.ObservedFrameIntervalMilliseconds `
        -Names $metricProperties `
        -FaultPrefix "FRAME_INTERVAL" `
        -Faults $faults
    $gpuMetricValid = Test-CloudProperties `
        -Value $summary.GpuQueryMilliseconds `
        -Names $metricProperties `
        -FaultPrefix "GPU_QUERY" `
        -Faults $faults

    if ($editorMetricValid) {
        if (-not (Test-PositiveInteger -Value $summary.EditorFps.SampleCount) -or
            -not (Test-PositiveNumber -Value $summary.EditorFps.Average) -or
            -not (Test-PositiveNumber -Value $summary.EditorFps.P95)) {
            Add-CloudFault -Faults $faults -Code "EDITOR_FPS_INVALID"
        }
    }
    if ($intervalMetricValid) {
        if (-not (Test-PositiveInteger `
                -Value $summary.ObservedFrameIntervalMilliseconds.SampleCount) -or
            -not (Test-PositiveNumber `
                -Value $summary.ObservedFrameIntervalMilliseconds.Average) -or
            -not (Test-PositiveNumber `
                -Value $summary.ObservedFrameIntervalMilliseconds.P95)) {
            Add-CloudFault -Faults $faults -Code "FRAME_INTERVAL_INVALID"
        }
    }
    if (-not (Test-PositiveNumber `
            -Value $summary.EditorFpsFromP95FrameInterval)) {
        Add-CloudFault -Faults $faults -Code "P95_INTERVAL_FPS_INVALID"
    }
    elseif ($intervalMetricValid -and
        (Test-PositiveNumber `
            -Value $summary.ObservedFrameIntervalMilliseconds.P95)) {
        $expectedP95Fps =
            1000.0 /
            [double]$summary.ObservedFrameIntervalMilliseconds.P95
        if (-not (Test-NearlyEqual `
                -Left $summary.EditorFpsFromP95FrameInterval `
                -Right $expectedP95Fps `
                -Tolerance 0.001)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "P95_INTERVAL_FPS_INCOHERENT"
        }
    }
    if ($gpuMetricValid) {
        if (-not (Test-PositiveInteger `
                -Value $summary.GpuQueryMilliseconds.SampleCount) -or
            -not (Test-PositiveNumber `
                -Value $summary.GpuQueryMilliseconds.Average) -or
            -not (Test-PositiveNumber `
                -Value $summary.GpuQueryMilliseconds.P95)) {
            Add-CloudFault -Faults $faults -Code "GPU_QUERY_INVALID"
        }
    }

    if (-not (Test-PositiveNumber `
            -Value $summary.GpuThroughputFromAverageMilliseconds) -or
        -not (Test-PositiveNumber `
            -Value $summary.GpuThroughputFromP95Milliseconds)) {
        Add-CloudFault -Faults $faults -Code "GPU_THROUGHPUT_INVALID"
    }
    elseif ($gpuMetricValid -and
        (Test-PositiveNumber -Value $summary.GpuQueryMilliseconds.Average) -and
        (Test-PositiveNumber -Value $summary.GpuQueryMilliseconds.P95)) {
        if (-not (Test-NearlyEqual `
                -Left $summary.GpuThroughputFromAverageMilliseconds `
                -Right (1000.0 / [double]$summary.GpuQueryMilliseconds.Average) `
                -Tolerance 0.001)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_AVERAGE_THROUGHPUT_INCOHERENT"
        }
        if (-not (Test-NearlyEqual `
                -Left $summary.GpuThroughputFromP95Milliseconds `
                -Right (1000.0 / [double]$summary.GpuQueryMilliseconds.P95) `
                -Tolerance 0.001)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_P95_THROUGHPUT_INCOHERENT"
        }
    }

    $gpuWindow = $summary.LatestGpuQueryWindow
    $gpuWindowProperties = @(
        "Available",
        "QueryCount",
        "QueryCapacity",
        "LatencyFrames",
        "FrameAverageMilliseconds",
        "FramePeakMilliseconds",
        "OpaqueAverageMilliseconds",
        "OpaquePeakMilliseconds",
        "AtmosphereAverageMilliseconds",
        "AtmospherePeakMilliseconds",
        "CloudAverageMilliseconds",
        "CloudPeakMilliseconds",
        "FogAverageMilliseconds",
        "FogPeakMilliseconds",
        "PostAverageMilliseconds",
        "PostPeakMilliseconds"
    )
    $gpuWindowValid = Test-CloudProperties `
        -Value $gpuWindow `
        -Names $gpuWindowProperties `
        -FaultPrefix "GPU_PASS_WINDOW" `
        -Faults $faults
    if ($gpuWindowValid) {
        if (-not (Test-CloudBoolean -Value $gpuWindow.Available) -or
            -not [bool]$gpuWindow.Available -or
            -not (Test-PositiveInteger -Value $gpuWindow.QueryCount) -or
            -not (Test-PositiveInteger -Value $gpuWindow.QueryCapacity)) {
            Add-CloudFault -Faults $faults -Code "GPU_PASS_WINDOW_UNAVAILABLE"
        }
        if (-not (Test-NonNegativeInteger -Value $gpuWindow.LatencyFrames)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_PASS_WINDOW_LATENCYFRAMES_INVALID"
        }
        if ((Test-PositiveInteger -Value $gpuWindow.QueryCount) -and
            (Test-PositiveInteger -Value $gpuWindow.QueryCapacity) -and
            [int64]$gpuWindow.QueryCount -gt
                [int64]$gpuWindow.QueryCapacity) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_PASS_WINDOW_QUERY_COUNT_EXCEEDS_CAPACITY"
        }
        if (-not (Test-PositiveNumber `
                -Value $gpuWindow.FrameAverageMilliseconds) -or
            -not (Test-PositiveNumber `
                -Value $gpuWindow.FramePeakMilliseconds) -or
            -not (Test-PositiveNumber `
                -Value $gpuWindow.CloudAverageMilliseconds) -or
            -not (Test-PositiveNumber `
                -Value $gpuWindow.CloudPeakMilliseconds)) {
            Add-CloudFault -Faults $faults -Code "GPU_PASS_TIMINGS_INVALID"
        }
        foreach ($property in @(
                "OpaqueAverageMilliseconds",
                "OpaquePeakMilliseconds",
                "AtmosphereAverageMilliseconds",
                "AtmospherePeakMilliseconds",
                "FogAverageMilliseconds",
                "FogPeakMilliseconds",
                "PostAverageMilliseconds",
                "PostPeakMilliseconds")) {
            if (-not (Test-NonNegativeNumber -Value $gpuWindow.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("GPU_PASS_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        if ((Test-PositiveNumber -Value $gpuWindow.FrameAverageMilliseconds) -and
            (Test-PositiveNumber -Value $gpuWindow.FramePeakMilliseconds) -and
            [double]$gpuWindow.FramePeakMilliseconds -lt
                [double]$gpuWindow.FrameAverageMilliseconds) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_FRAME_PEAK_BELOW_AVERAGE"
        }
        if ((Test-PositiveNumber -Value $gpuWindow.CloudAverageMilliseconds) -and
            (Test-PositiveNumber -Value $gpuWindow.CloudPeakMilliseconds) -and
            [double]$gpuWindow.CloudPeakMilliseconds -lt
                [double]$gpuWindow.CloudAverageMilliseconds) {
            Add-CloudFault `
                -Faults $faults `
                -Code "GPU_CLOUD_PEAK_BELOW_AVERAGE"
        }
    }

    $render = $summary.LatestRenderState
    $renderProperties = @(
        "Available",
        "Flags",
        "View3D",
        "CloudsEnabled",
        "FogEnabled",
        "AerialPerspectiveEnabled",
        "GameView",
        "ScenePresentationSuppressed",
        "DrawCalls",
        "DispatchCalls",
        "ViewportWidth",
        "ViewportHeight",
        "CloudWidth",
        "CloudHeight",
        "CloudMarchSteps",
        "CloudLightSteps",
        "CloudRenderScale"
    )
    $renderValid = Test-CloudProperties `
        -Value $render `
        -Names $renderProperties `
        -FaultPrefix "RENDER_STATE" `
        -Faults $faults
    if ($renderValid) {
        if (-not (Test-CloudBoolean -Value $render.Available) -or
            -not [bool]$render.Available) {
            Add-CloudFault -Faults $faults -Code "RENDER_STATE_UNAVAILABLE"
        }
        if (-not (Test-CloudBoolean -Value $render.View3D) -or
            -not [bool]$render.View3D) {
            Add-CloudFault -Faults $faults -Code "RENDER_STATE_NOT_VIEW3D"
        }
        if (-not (Test-CloudBoolean -Value $render.CloudsEnabled) -or
            -not [bool]$render.CloudsEnabled) {
            Add-CloudFault -Faults $faults -Code "RENDER_STATE_CLOUDS_DISABLED"
        }
        foreach ($property in @(
                "FogEnabled",
                "AerialPerspectiveEnabled",
                "GameView")) {
            if (-not (Test-CloudBoolean -Value $render.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code (
                        "RENDER_STATE_" +
                        $property.ToUpperInvariant() +
                        "_INVALID")
            }
        }
        if (-not (Test-CloudBoolean `
                -Value $render.ScenePresentationSuppressed) -or
            [bool]$render.ScenePresentationSuppressed) {
            Add-CloudFault `
                -Faults $faults `
                -Code "RENDER_STATE_PRESENTATION_SUPPRESSED"
        }
        foreach ($property in @("Flags", "DrawCalls", "DispatchCalls")) {
            if (-not (Test-NonNegativeInteger -Value $render.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("RENDER_STATE_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        if ((ConvertTo-CloudInt64 -Value $render.DrawCalls) -eq 0 -and
            (ConvertTo-CloudInt64 -Value $render.DispatchCalls) -eq 0) {
            Add-CloudFault -Faults $faults -Code "RENDER_STATE_NO_WORK"
        }
        foreach ($property in @(
                "ViewportWidth",
                "ViewportHeight",
                "CloudWidth",
                "CloudHeight",
                "CloudMarchSteps",
                "CloudLightSteps")) {
            if (-not (Test-PositiveInteger -Value $render.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("RENDER_STATE_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        if ((ConvertTo-CloudInt64 -Value $render.CloudMarchSteps) -ne
            $script:ExpectedViewSamples) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_VIEW_SAMPLE_COUNT_CHANGED"
        }
        if ((ConvertTo-CloudInt64 -Value $render.CloudLightSteps) -ne
            $script:ExpectedLightSamples) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_LIGHT_SAMPLE_COUNT_CHANGED"
        }
        if (-not (Test-NearlyEqual `
                -Left $render.CloudRenderScale `
                -Right $script:ExpectedCloudScale `
                -Tolerance 0.0000001)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_RENDER_SCALE_CHANGED"
        }
        if ((Test-PositiveInteger -Value $render.ViewportWidth) -and
            (Test-PositiveInteger -Value $render.ViewportHeight) -and
            (Test-PositiveInteger -Value $render.CloudWidth) -and
            (Test-PositiveInteger -Value $render.CloudHeight) -and
            (Test-PositiveNumber -Value $render.CloudRenderScale)) {
            $expectedWidth = [math]::Ceiling(
                [double]$render.ViewportWidth *
                [double]$render.CloudRenderScale)
            $expectedHeight = [math]::Ceiling(
                [double]$render.ViewportHeight *
                [double]$render.CloudRenderScale)
            if ((ConvertTo-CloudInt64 -Value $render.CloudWidth) -ne
                    $expectedWidth -or
                (ConvertTo-CloudInt64 -Value $render.CloudHeight) -ne
                    $expectedHeight) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code "CLOUD_RESOLUTION_INCOHERENT"
            }
        }
    }

    $workload = $summary.LatestCloudWorkload
    $workloadProperties = @(
        "Status",
        "Available",
        "Attempted",
        "Submitted",
        "HistoryWasAvailable",
        "HistoryReused",
        "HistoryInvalidated",
        "TemporalSuperResolution",
        "SkipReason",
        "ProfilerFrameIndex",
        "SubmissionIndex",
        "TraceWidth",
        "TraceHeight",
        "OutputWidth",
        "OutputHeight",
        "SteadyDispatches",
        "OneTimeBakeDispatches",
        "ShadowCacheDispatches",
        "WorldShadowDispatches",
        "TotalComputeDispatches",
        "CompositeDraws",
        "TraceLogicalInvocations",
        "TraceLaunchedThreads",
        "ResolveLogicalInvocations",
        "ResolveLaunchedThreads",
        "OneTimeBakeLogicalInvocations",
        "OneTimeBakeLaunchedThreads",
        "ShadowCacheLogicalInvocations",
        "ShadowCacheLaunchedThreads",
        "WorldShadowLogicalInvocations",
        "WorldShadowLaunchedThreads",
        "TotalLogicalInvocations",
        "TotalLaunchedThreads",
        "MaximumViewSamples",
        "MaximumLightSamples",
        "MaximumWorldShadowSamples"
    )
    $workloadValid = Test-CloudProperties `
        -Value $workload `
        -Names $workloadProperties `
        -FaultPrefix "CLOUD_WORKLOAD" `
        -Faults $faults
    if ($workloadValid) {
        if ([string]$workload.Status -cne "Available" -or
            -not (Test-CloudBoolean -Value $workload.Available) -or
            -not [bool]$workload.Available) {
            Add-CloudFault -Faults $faults -Code "CLOUD_WORKLOAD_UNAVAILABLE"
        }
        if (-not (Test-CloudBoolean -Value $workload.Attempted) -or
            -not [bool]$workload.Attempted -or
            -not (Test-CloudBoolean -Value $workload.Submitted) -or
            -not [bool]$workload.Submitted) {
            Add-CloudFault -Faults $faults -Code "CLOUD_WORKLOAD_NOT_SUBMITTED"
        }
        if (-not (Test-CloudBoolean -Value $workload.HistoryWasAvailable) -or
            -not [bool]$workload.HistoryWasAvailable -or
            -not (Test-CloudBoolean -Value $workload.HistoryReused) -or
            -not [bool]$workload.HistoryReused -or
            -not (Test-CloudBoolean -Value $workload.HistoryInvalidated) -or
            [bool]$workload.HistoryInvalidated) {
            Add-CloudFault -Faults $faults -Code "CLOUD_HISTORY_NOT_REUSED"
        }
        if (-not (Test-CloudBoolean `
                -Value $workload.TemporalSuperResolution) -or
            -not [bool]$workload.TemporalSuperResolution) {
            Add-CloudFault -Faults $faults -Code "CLOUD_TSR_DISABLED"
        }
        if (-not (Test-NonNegativeInteger -Value $workload.SkipReason) -or
            (ConvertTo-CloudInt64 -Value $workload.SkipReason) -ne 0) {
            Add-CloudFault -Faults $faults -Code "CLOUD_WORKLOAD_SKIP_REASON"
        }
        foreach ($property in @(
                "ProfilerFrameIndex",
                "SubmissionIndex",
                "TraceWidth",
                "TraceHeight",
                "OutputWidth",
                "OutputHeight",
                "SteadyDispatches",
                "WorldShadowDispatches",
                "TotalComputeDispatches",
                "CompositeDraws",
                "TraceLogicalInvocations",
                "TraceLaunchedThreads",
                "ResolveLogicalInvocations",
                "ResolveLaunchedThreads",
                "WorldShadowLogicalInvocations",
                "WorldShadowLaunchedThreads",
                "TotalLogicalInvocations",
                "TotalLaunchedThreads",
                "MaximumViewSamples",
                "MaximumLightSamples",
                "MaximumWorldShadowSamples")) {
            if (-not (Test-PositiveInteger -Value $workload.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("CLOUD_WORKLOAD_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        foreach ($property in @(
                "OneTimeBakeDispatches",
                "ShadowCacheDispatches",
                "OneTimeBakeLogicalInvocations",
                "OneTimeBakeLaunchedThreads",
                "ShadowCacheLogicalInvocations",
                "ShadowCacheLaunchedThreads")) {
            if (-not (Test-NonNegativeInteger -Value $workload.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("CLOUD_WORKLOAD_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        if ($renderValid) {
            if ((ConvertTo-CloudInt64 -Value $workload.TraceWidth) -ne
                    (ConvertTo-CloudInt64 -Value $render.CloudWidth) -or
                (ConvertTo-CloudInt64 -Value $workload.TraceHeight) -ne
                    (ConvertTo-CloudInt64 -Value $render.CloudHeight) -or
                (ConvertTo-CloudInt64 -Value $workload.OutputWidth) -ne
                    (ConvertTo-CloudInt64 -Value $render.ViewportWidth) -or
                (ConvertTo-CloudInt64 -Value $workload.OutputHeight) -ne
                    (ConvertTo-CloudInt64 -Value $render.ViewportHeight)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code "CLOUD_WORKLOAD_DIMENSIONS_INCOHERENT"
            }
        }
        $steady = ConvertTo-CloudInt64 -Value $workload.SteadyDispatches
        $bake = ConvertTo-CloudInt64 -Value $workload.OneTimeBakeDispatches
        $shadow = ConvertTo-CloudInt64 -Value $workload.ShadowCacheDispatches
        $worldShadow =
            ConvertTo-CloudInt64 -Value $workload.WorldShadowDispatches
        $totalDispatches =
            ConvertTo-CloudInt64 -Value $workload.TotalComputeDispatches
        if ($totalDispatches -ne
                ($steady + $bake + $shadow + $worldShadow)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_DISPATCH_TOTAL_INCOHERENT"
        }
        $expectedTotalDispatches =
            $script:ExpectedSteadyDispatches +
            $script:ExpectedShadowCacheDispatches +
            $script:ExpectedWorldShadowDispatches
        if ($steady -ne $script:ExpectedSteadyDispatches -or
            $bake -ne 0 -or
            $shadow -ne $script:ExpectedShadowCacheDispatches -or
            $worldShadow -ne $script:ExpectedWorldShadowDispatches -or
            $totalDispatches -ne $expectedTotalDispatches -or
            (ConvertTo-CloudInt64 -Value $workload.CompositeDraws) -ne
                $script:ExpectedCompositeDraws) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_STEADY_WORKLOAD_CHANGED"
        }
        $traceLogical =
            ConvertTo-CloudInt64 -Value $workload.TraceLogicalInvocations
        $traceLaunched =
            ConvertTo-CloudInt64 -Value $workload.TraceLaunchedThreads
        $resolveLogical =
            ConvertTo-CloudInt64 -Value $workload.ResolveLogicalInvocations
        $resolveLaunched =
            ConvertTo-CloudInt64 -Value $workload.ResolveLaunchedThreads
        $bakeLogical =
            ConvertTo-CloudInt64 -Value $workload.OneTimeBakeLogicalInvocations
        $bakeLaunched =
            ConvertTo-CloudInt64 -Value $workload.OneTimeBakeLaunchedThreads
        $shadowLogical =
            ConvertTo-CloudInt64 -Value $workload.ShadowCacheLogicalInvocations
        $shadowLaunched =
            ConvertTo-CloudInt64 -Value $workload.ShadowCacheLaunchedThreads
        $worldShadowLogical =
            ConvertTo-CloudInt64 `
                -Value $workload.WorldShadowLogicalInvocations
        $worldShadowLaunched =
            ConvertTo-CloudInt64 `
                -Value $workload.WorldShadowLaunchedThreads
        $totalLogical =
            ConvertTo-CloudInt64 -Value $workload.TotalLogicalInvocations
        $totalLaunched =
            ConvertTo-CloudInt64 -Value $workload.TotalLaunchedThreads
        if ($traceLaunched -lt $traceLogical -or
            $resolveLaunched -lt $resolveLogical -or
            $bakeLaunched -lt $bakeLogical -or
            $shadowLaunched -lt $shadowLogical -or
            $worldShadowLaunched -lt $worldShadowLogical) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_LAUNCHED_THREADS_BELOW_LOGICAL"
        }
        if ($totalLogical -ne
                ($traceLogical + $resolveLogical +
                 $bakeLogical + $shadowLogical + $worldShadowLogical) -or
            $totalLaunched -ne
                ($traceLaunched + $resolveLaunched +
                 $bakeLaunched + $shadowLaunched + $worldShadowLaunched)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_INVOCATION_TOTAL_INCOHERENT"
        }
        if ($shadowLogical -ne
                $script:ExpectedShadowCacheLogicalInvocations -or
            $shadowLaunched -ne
                $script:ExpectedShadowCacheLaunchedThreads) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_SHADOW_WORKLOAD_CHANGED"
        }
        if ($worldShadowLogical -ne
                $script:ExpectedWorldShadowLogicalInvocations -or
            $worldShadowLaunched -ne
                $script:ExpectedWorldShadowLaunchedThreads) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_WORLD_SHADOW_WORKLOAD_CHANGED"
        }
        $expectedTraceLogical =
            (ConvertTo-CloudInt64 -Value $workload.TraceWidth) *
            (ConvertTo-CloudInt64 -Value $workload.TraceHeight)
        $expectedResolveLogical =
            (ConvertTo-CloudInt64 -Value $workload.OutputWidth) *
            (ConvertTo-CloudInt64 -Value $workload.OutputHeight)
        if ($traceLogical -ne $expectedTraceLogical -or
            $resolveLogical -ne $expectedResolveLogical) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_LOGICAL_INVOCATIONS_INCOHERENT"
        }
        $maximumView =
            ConvertTo-CloudInt64 -Value $workload.MaximumViewSamples
        $maximumLight =
            ConvertTo-CloudInt64 -Value $workload.MaximumLightSamples
        $maximumWorldShadow =
            ConvertTo-CloudInt64 `
                -Value $workload.MaximumWorldShadowSamples
        $expectedMaximumView =
            $traceLogical * $script:ExpectedViewSamples
        $expectedMaximumLight =
            $expectedMaximumView * $script:ExpectedLightSamples
        if ($maximumView -ne $expectedMaximumView) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_MAXIMUM_VIEW_SAMPLES_INCOHERENT"
        }
        if ($maximumLight -ne $expectedMaximumLight) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_MAXIMUM_LIGHT_SAMPLES_INCOHERENT"
        }
        if ($maximumWorldShadow -ne
                $script:ExpectedWorldShadowSamples) {
            Add-CloudFault `
                -Faults $faults `
                -Code "CLOUD_MAXIMUM_WORLD_SHADOW_SAMPLES_INCOHERENT"
        }
    }

    $runtime = $summary.LatestEditorRuntime
    $runtimeProperties = @(
        "NativeAvailable",
        "NativeCallCount",
        "SlowNativeCallCount",
        "GpuBackpressureYieldCount",
        "GpuBackpressureInputRetryCount",
        "GpuBackpressureBackgroundFallbackCount",
        "GpuReadyAfterRetryCount",
        "RenderFairnessYieldCount",
        "LastGpuBackpressureEpochMilliseconds",
        "MaximumGpuBackpressureEpochMilliseconds",
        "PeakPresentedRenderBurstFrames",
        "PeakRenderBurstActiveCpuMilliseconds",
        "LastNativeCallMilliseconds",
        "MaximumNativeCallMilliseconds",
        "LastNativeCallKind",
        "DispatcherAvailable",
        "DispatcherHeartbeatCount",
        "DispatcherStallCount"
    )
    $runtimeValid = Test-CloudProperties `
        -Value $runtime `
        -Names $runtimeProperties `
        -FaultPrefix "EDITOR_RUNTIME" `
        -Faults $faults
    if ($runtimeValid) {
        if (-not (Test-CloudBoolean -Value $runtime.NativeAvailable) -or
            -not [bool]$runtime.NativeAvailable -or
            -not (Test-PositiveInteger -Value $runtime.NativeCallCount)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_NATIVE_UNAVAILABLE"
        }
        if (-not (Test-CloudBoolean -Value $runtime.DispatcherAvailable) -or
            -not [bool]$runtime.DispatcherAvailable -or
            -not (Test-PositiveInteger `
                -Value $runtime.DispatcherHeartbeatCount)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_DISPATCHER_UNAVAILABLE"
        }
        foreach ($property in @(
                "SlowNativeCallCount",
                "GpuBackpressureYieldCount",
                "GpuBackpressureInputRetryCount",
                "GpuBackpressureBackgroundFallbackCount",
                "GpuReadyAfterRetryCount",
                "RenderFairnessYieldCount",
                "PeakPresentedRenderBurstFrames",
                "DispatcherStallCount")) {
            if (-not (Test-NonNegativeInteger -Value $runtime.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("EDITOR_RUNTIME_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        foreach ($property in @(
                "LastGpuBackpressureEpochMilliseconds",
                "MaximumGpuBackpressureEpochMilliseconds",
                "PeakRenderBurstActiveCpuMilliseconds",
                "LastNativeCallMilliseconds",
                "MaximumNativeCallMilliseconds")) {
            if (-not (Test-NonNegativeNumber -Value $runtime.$property)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code ("EDITOR_RUNTIME_" + $property.ToUpperInvariant() + "_INVALID")
            }
        }
        if ([string]::IsNullOrWhiteSpace(
                [string]$runtime.LastNativeCallKind)) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_LAST_NATIVE_CALL_KIND_INVALID"
        }
        if ((Test-NonNegativeNumber `
                -Value $runtime.LastNativeCallMilliseconds) -and
            (Test-NonNegativeNumber `
                -Value $runtime.MaximumNativeCallMilliseconds) -and
            [double]$runtime.MaximumNativeCallMilliseconds -lt
                [double]$runtime.LastNativeCallMilliseconds) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_NATIVE_MAXIMUM_INCOHERENT"
        }
        if ((Test-NonNegativeNumber `
                -Value $runtime.LastGpuBackpressureEpochMilliseconds) -and
            (Test-NonNegativeNumber `
                -Value $runtime.MaximumGpuBackpressureEpochMilliseconds) -and
            [double]$runtime.MaximumGpuBackpressureEpochMilliseconds -lt
                [double]$runtime.LastGpuBackpressureEpochMilliseconds) {
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_RUNTIME_BACKPRESSURE_MAXIMUM_INCOHERENT"
        }
        if ((ConvertTo-CloudInt64 `
                -Value $runtime.GpuBackpressureYieldCount) -gt 0) {
            if (-not (Test-PositiveNumber `
                    -Value $runtime.LastGpuBackpressureEpochMilliseconds) -or
                -not (Test-PositiveNumber `
                    -Value $runtime.MaximumGpuBackpressureEpochMilliseconds)) {
                Add-CloudFault `
                    -Faults $faults `
                    -Code "EDITOR_RUNTIME_BACKPRESSURE_EPOCH_INVALID"
            }
        }
    }

    $measurements = $null
    $quality = $null
    if ($editorMetricValid -and $intervalMetricValid -and
        $gpuMetricValid -and $gpuWindowValid -and $runtimeValid) {
        $measurements = [pscustomobject][ordered]@{
            EditorFpsAverage =
                ConvertTo-CloudDouble -Value $summary.EditorFps.Average
            EditorFpsP95 =
                ConvertTo-CloudDouble -Value $summary.EditorFps.P95
            FrameIntervalP95Milliseconds =
                ConvertTo-CloudDouble `
                    -Value $summary.ObservedFrameIntervalMilliseconds.P95
            EditorFpsFromP95FrameInterval =
                ConvertTo-CloudDouble `
                    -Value $summary.EditorFpsFromP95FrameInterval
            GpuQueryAverageMilliseconds =
                ConvertTo-CloudDouble `
                    -Value $summary.GpuQueryMilliseconds.Average
            GpuQueryP95Milliseconds =
                ConvertTo-CloudDouble `
                    -Value $summary.GpuQueryMilliseconds.P95
            GpuThroughputFromAverageMilliseconds =
                ConvertTo-CloudDouble `
                    -Value $summary.GpuThroughputFromAverageMilliseconds
            GpuThroughputFromP95Milliseconds =
                ConvertTo-CloudDouble `
                    -Value $summary.GpuThroughputFromP95Milliseconds
            CloudAverageMilliseconds =
                ConvertTo-CloudDouble `
                    -Value $gpuWindow.CloudAverageMilliseconds
            CloudPeakMilliseconds =
                ConvertTo-CloudDouble `
                    -Value $gpuWindow.CloudPeakMilliseconds
            Scheduler = [pscustomobject][ordered]@{
                InputPriorityRetries =
                    ConvertTo-CloudInt64 `
                        -Value $runtime.GpuBackpressureInputRetryCount
                BackgroundFallbacks =
                    ConvertTo-CloudInt64 `
                        -Value $runtime.GpuBackpressureBackgroundFallbackCount
                ReadyAfterRetry =
                    ConvertTo-CloudInt64 `
                        -Value $runtime.GpuReadyAfterRetryCount
                FairnessYields =
                    ConvertTo-CloudInt64 `
                        -Value $runtime.RenderFairnessYieldCount
                LastBackpressureEpochMilliseconds =
                    ConvertTo-CloudDouble `
                        -Value $runtime.LastGpuBackpressureEpochMilliseconds
                MaximumBackpressureEpochMilliseconds =
                    ConvertTo-CloudDouble `
                        -Value $runtime.MaximumGpuBackpressureEpochMilliseconds
                PeakPresentedBurstFrames =
                    ConvertTo-CloudInt64 `
                        -Value $runtime.PeakPresentedRenderBurstFrames
                PeakBurstActiveCpuMilliseconds =
                    ConvertTo-CloudDouble `
                        -Value $runtime.PeakRenderBurstActiveCpuMilliseconds
            }
        }
    }

    if ($renderValid -and $workloadValid -and $gpuWindowValid) {
        $quality = [pscustomobject][ordered]@{
            RenderFlags = ConvertTo-CloudInt64 -Value $render.Flags
            CloudsEnabled = [bool]$render.CloudsEnabled
            FogEnabled = [bool]$render.FogEnabled
            AerialPerspectiveEnabled =
                [bool]$render.AerialPerspectiveEnabled
            GameView = [bool]$render.GameView
            ViewportWidth =
                ConvertTo-CloudInt64 -Value $render.ViewportWidth
            ViewportHeight =
                ConvertTo-CloudInt64 -Value $render.ViewportHeight
            CloudWidth =
                ConvertTo-CloudInt64 -Value $render.CloudWidth
            CloudHeight =
                ConvertTo-CloudInt64 -Value $render.CloudHeight
            CloudMarchSteps =
                ConvertTo-CloudInt64 -Value $render.CloudMarchSteps
            CloudLightSteps =
                ConvertTo-CloudInt64 -Value $render.CloudLightSteps
            CloudRenderScale =
                ConvertTo-CloudDouble -Value $render.CloudRenderScale
            GpuQueryCapacity =
                ConvertTo-CloudInt64 -Value $gpuWindow.QueryCapacity
            GpuQueryLatencyFrames =
                ConvertTo-CloudInt64 -Value $gpuWindow.LatencyFrames
            TemporalSuperResolution =
                [bool]$workload.TemporalSuperResolution
            TraceWidth =
                ConvertTo-CloudInt64 -Value $workload.TraceWidth
            TraceHeight =
                ConvertTo-CloudInt64 -Value $workload.TraceHeight
            OutputWidth =
                ConvertTo-CloudInt64 -Value $workload.OutputWidth
            OutputHeight =
                ConvertTo-CloudInt64 -Value $workload.OutputHeight
            SteadyDispatches =
                ConvertTo-CloudInt64 -Value $workload.SteadyDispatches
            OneTimeBakeDispatches =
                ConvertTo-CloudInt64 -Value $workload.OneTimeBakeDispatches
            ShadowCacheDispatches =
                ConvertTo-CloudInt64 -Value $workload.ShadowCacheDispatches
            WorldShadowDispatches =
                ConvertTo-CloudInt64 -Value $workload.WorldShadowDispatches
            TotalComputeDispatches =
                ConvertTo-CloudInt64 -Value $workload.TotalComputeDispatches
            CompositeDraws =
                ConvertTo-CloudInt64 -Value $workload.CompositeDraws
            TraceLogicalInvocations =
                ConvertTo-CloudInt64 -Value $workload.TraceLogicalInvocations
            TraceLaunchedThreads =
                ConvertTo-CloudInt64 -Value $workload.TraceLaunchedThreads
            ResolveLogicalInvocations =
                ConvertTo-CloudInt64 -Value $workload.ResolveLogicalInvocations
            ResolveLaunchedThreads =
                ConvertTo-CloudInt64 -Value $workload.ResolveLaunchedThreads
            WorldShadowLogicalInvocations =
                ConvertTo-CloudInt64 `
                    -Value $workload.WorldShadowLogicalInvocations
            WorldShadowLaunchedThreads =
                ConvertTo-CloudInt64 `
                    -Value $workload.WorldShadowLaunchedThreads
            MaximumViewSamples =
                ConvertTo-CloudInt64 -Value $workload.MaximumViewSamples
            MaximumLightSamples =
                ConvertTo-CloudInt64 -Value $workload.MaximumLightSamples
            MaximumWorldShadowSamples =
                ConvertTo-CloudInt64 `
                    -Value $workload.MaximumWorldShadowSamples
        }
    }

    return New-CloudValidationResult `
        -Faults $faults `
        -Measurements $measurements `
        -Quality $quality
}

function Compare-CloudQuality {
    param(
        [AllowNull()][object]$Horizon,
        [AllowNull()][object]$Zenith,
        [AllowNull()][object]$Above
    )

    $mismatches = New-Object "System.Collections.Generic.List[string]"
    if ($null -eq $Horizon -or
        $null -eq $Zenith -or
        $null -eq $Above) {
        Add-CloudFault `
            -Faults $mismatches `
            -Code "QUALITY_SNAPSHOT_MISSING"
        return [pscustomobject][ordered]@{
            Pass = $false
            MismatchCodes = @($mismatches.ToArray())
        }
    }

    foreach ($candidate in @($Zenith, $Above)) {
        foreach ($property in $Horizon.PSObject.Properties) {
            $name = $property.Name
            $candidateProperty = $candidate.PSObject.Properties[$name]
            if ($null -eq $candidateProperty) {
                Add-CloudFault `
                    -Faults $mismatches `
                    -Code ("QUALITY_MISSING_" + $name.ToUpperInvariant())
                continue
            }

            $left = $property.Value
            $right = $candidateProperty.Value
            $equal = $false
            if ($null -eq $left -or $null -eq $right) {
                $equal = $null -eq $left -and $null -eq $right
            }
            elseif ($left -is [double] -or $left -is [float] -or
                $right -is [double] -or $right -is [float]) {
                $equal = Test-NearlyEqual `
                    -Left $left `
                    -Right $right `
                    -Tolerance 0.0000001
            }
            else {
                $equal = $left -eq $right
            }
            if (-not $equal) {
                Add-CloudFault `
                    -Faults $mismatches `
                    -Code ("QUALITY_MISMATCH_" + $name.ToUpperInvariant())
            }
        }
        foreach ($property in $candidate.PSObject.Properties) {
            if ($null -eq $Horizon.PSObject.Properties[$property.Name]) {
                Add-CloudFault `
                    -Faults $mismatches `
                    -Code (
                        "QUALITY_UNEXPECTED_" +
                        $property.Name.ToUpperInvariant())
            }
        }
    }

    return [pscustomobject][ordered]@{
        Pass = $mismatches.Count -eq 0
        MismatchCodes = @($mismatches.ToArray())
    }
}

function Test-CloudTarget {
    param(
        [AllowNull()][object]$Measurements,
        [double]$Target
    )

    if ($null -eq $Measurements) {
        return $false
    }
    return (Test-PositiveNumber -Value $Measurements.EditorFpsAverage) -and
        [double]$Measurements.EditorFpsAverage -ge $Target -and
        (Test-PositiveNumber `
            -Value $Measurements.EditorFpsFromP95FrameInterval) -and
        [double]$Measurements.EditorFpsFromP95FrameInterval -ge $Target -and
        (Test-PositiveNumber `
            -Value $Measurements.GpuThroughputFromAverageMilliseconds) -and
        [double]$Measurements.GpuThroughputFromAverageMilliseconds -ge
            $Target -and
        (Test-PositiveNumber `
            -Value $Measurements.GpuThroughputFromP95Milliseconds) -and
        [double]$Measurements.GpuThroughputFromP95Milliseconds -ge $Target
}

function Get-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label path is required."
    }
    if ($Path.Contains('"')) {
        throw "$Label path must not contain a quote character."
    }
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-PathBelowRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $rootWithSeparator =
        $Root.TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    return $Path.StartsWith(
        $rootWithSeparator,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeOutputDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$Create
    )

    $tempRoot = [System.IO.Path]::GetFullPath($env:TEMP)
    $candidate = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-PathBelowRoot -Path $candidate -Root $tempRoot)) {
        throw "OutputDirectory must be a child of TEMP: $tempRoot"
    }
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        throw "OutputDirectory resolves to a file: $candidate"
    }

    $existing = $candidate
    while (-not (Test-Path -LiteralPath $existing -PathType Container)) {
        $parent = [System.IO.Path]::GetDirectoryName($existing)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $existing) {
            throw "Could not resolve an existing output ancestor."
        }
        $existing = $parent
    }
    while ($true) {
        $item = Get-Item -LiteralPath $existing -Force
        if (($item.Attributes -band
                [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Output path contains a reparse point: $existing"
        }
        if ($existing.Equals(
                $tempRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        if (-not (Test-PathBelowRoot -Path $existing -Root $tempRoot)) {
            throw "Output ancestor escaped TEMP: $existing"
        }
        $parent = [System.IO.Path]::GetDirectoryName($existing)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $existing) {
            throw "Output ancestor did not reach TEMP."
        }
        $existing = $parent
    }

    if ($Create) {
        [void][System.IO.Directory]::CreateDirectory($candidate)
        $created = Get-Item -LiteralPath $candidate -Force
        if (($created.Attributes -band
                [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Created output directory became a reparse point."
        }
    }
    return $candidate
}

function Assert-NewOutputPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to overwrite an existing result: $Path"
    }
}

function Assert-CloudRegularInputFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label must not be a reparse point: $Path"
    }
    if ($item.Length -le 0) {
        throw "$Label must not be empty: $Path"
    }
    return $item.FullName
}

function ConvertTo-CommandArgument {
    param([AllowEmptyString()][string]$Value)

    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value.Contains('"')) {
        throw "Editor arguments must not contain a quote character."
    }
    if ($Value -match "\s") {
        $escaped = [regex]::Replace($Value, '(\\+)$', '$1$1')
        return '"' + $escaped + '"'
    }
    return $Value
}

function Join-CommandArguments {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    return (($Arguments | ForEach-Object {
        ConvertTo-CommandArgument -Value $_
    }) -join " ")
}

function Get-MonitorArguments {
    if ($MonitorIndex -ge 0) {
        return @("--monitor", [string]$MonitorIndex)
    }
    switch ($Monitor) {
        "secondary" { return @("--secondary-monitor") }
        "primary" { return @("--monitor", "0") }
        default { return @() }
    }
}

function Get-ScenarioDefinitions {
    return @(
        [pscustomobject][ordered]@{
            Name = "horizon"
            Camera = @("0", "-0.08", "18", "0", "2", "0")
        },
        [pscustomobject][ordered]@{
            Name = "zenith"
            Camera = @("0", "-1.5533", "18", "0", "2", "0")
        },
        [pscustomobject][ordered]@{
            Name = "above"
            # 約 10 km の高度から浅く見下ろし、青空と雲頂を同じ画角へ入れる。
            Camera = @("0", "0.35", "8000", "0", "7200", "0")
        }
    )
}

function Get-ScenarioPaths {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return [pscustomobject][ordered]@{
        Report = Join-Path $Root ($Name + "-soak-report.json")
        Capture = Join-Path $Root ($Name + "-profile.csv")
        StandardOutput = Join-Path $Root ($Name + "-stdout.log")
        StandardError = Join-Path $Root ($Name + "-stderr.log")
    }
}

function Get-ScenarioArguments {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][object]$Scenario,
        [Parameter(Mandatory = $true)][object]$Paths
    )

    $arguments = New-Object "System.Collections.Generic.List[string]"
    [void]$arguments.Add($ProjectPath)
    foreach ($value in (Get-MonitorArguments)) {
        [void]$arguments.Add($value)
    }
    foreach ($value in @(
            "--unattended",
            "--show-profiler",
            "--hide-grid",
            "--interaction-soak",
            [string]$SoakSeconds,
            "--interaction-soak-report",
            [string]$Paths.Report,
            "--profiler-capture",
            [string]$Paths.Capture,
            "--camera3d")) {
        [void]$arguments.Add($value)
    }
    foreach ($value in $Scenario.Camera) {
        [void]$arguments.Add([string]$value)
    }
    return @($arguments.ToArray())
}

function Stop-CloudProcessTree {
    param([AllowNull()][System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return
    }
    try {
        if ($Process.HasExited) {
            return
        }
    }
    catch {
        return
    }

    $taskkill = $null
    if (-not [string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        $taskkill = [System.IO.Path]::Combine(
            $env:SystemRoot,
            "System32\taskkill.exe")
    }
    if ($null -ne $taskkill -and
        (Test-Path -LiteralPath $taskkill -PathType Leaf)) {
        try {
            & $taskkill /PID ([string]$Process.Id) /T /F 2>$null |
                Out-Null
            if ($LASTEXITCODE -eq 0) {
                try {
                    if ($Process.WaitForExit(5000)) {
                        return
                    }
                }
                catch {
                    # Fall through to the direct process fallback.
                }
            }
        }
        catch {
            # Fall through to the direct process fallback.
        }
    }
    try {
        $Process.Kill()
    }
    catch {
        # The timeout fault is reported by the caller.
    }
}

function Complete-CloudStreamCopy {
    param(
        [AllowNull()][System.Threading.Tasks.Task]$Task,
        [Parameter(Mandatory = $true)][string]$StreamName,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Faults,
        [int]$TimeoutMilliseconds = 10000
    )

    if ($null -eq $Task) {
        return
    }
    try {
        if (-not $Task.Wait($TimeoutMilliseconds)) {
            Add-CloudFault `
                -Faults $Faults `
                -Code (
                    "EDITOR_PROCESS_" +
                    $StreamName.ToUpperInvariant() +
                    "_DRAIN_TIMEOUT")
            return
        }
        [void]$Task.GetAwaiter().GetResult()
    }
    catch {
        Add-CloudFault `
            -Faults $Faults `
            -Code (
                "EDITOR_PROCESS_" +
                $StreamName.ToUpperInvariant() +
                "_COPY_FAILED")
    }
}

function Invoke-CloudChildProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StandardOutputPath,
        [Parameter(Mandatory = $true)][string]$StandardErrorPath,
        [Parameter(Mandatory = $true)][int]$TimeoutMilliseconds
    )

    Assert-NewOutputPath -Path $StandardOutputPath
    Assert-NewOutputPath -Path $StandardErrorPath
    $faults = New-Object "System.Collections.Generic.List[string]"
    $argumentLine = Join-CommandArguments -Arguments $Arguments
    $process = $null
    $standardOutputStream = $null
    $standardErrorStream = $null
    $standardOutputCopy = $null
    $standardErrorCopy = $null
    $started = $false
    $timedOut = $false
    $exited = $false
    $exitCode = $null
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    try {
        $standardOutputStream = [System.IO.File]::Open(
            $StandardOutputPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        $standardErrorStream = [System.IO.File]::Open(
            $StandardErrorPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)

        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $FilePath
        $startInfo.Arguments = $argumentLine
        $startInfo.WorkingDirectory = $WorkingDirectory
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.WindowStyle =
            [System.Diagnostics.ProcessWindowStyle]::Hidden
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Process.Start returned false."
        }
        $started = $true
        $standardOutputCopy =
            $process.StandardOutput.BaseStream.CopyToAsync(
                $standardOutputStream)
        $standardErrorCopy =
            $process.StandardError.BaseStream.CopyToAsync(
                $standardErrorStream)

        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $timedOut = $true
            Add-CloudFault `
                -Faults $faults `
                -Code "EDITOR_PROCESS_TIMEOUT"
            Stop-CloudProcessTree -Process $process
        }
    }
    catch {
        Add-CloudFault `
            -Faults $faults `
            -Code $(if ($started) {
                "EDITOR_PROCESS_RUNTIME_FAILED"
            }
            else {
                "EDITOR_PROCESS_LAUNCH_FAILED"
            })
        Stop-CloudProcessTree -Process $process
    }
    finally {
        if ($started -and $null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    Stop-CloudProcessTree -Process $process
                }
                if ($process.HasExited -or $process.WaitForExit(10000)) {
                    $process.WaitForExit()
                    $exited = $true
                    $exitCode = [int]$process.ExitCode
                }
                else {
                    Add-CloudFault `
                        -Faults $faults `
                        -Code "EDITOR_PROCESS_DID_NOT_EXIT"
                }
            }
            catch {
                Add-CloudFault `
                    -Faults $faults `
                    -Code "EDITOR_PROCESS_STATUS_FAILED"
            }
        }

        Complete-CloudStreamCopy `
            -Task $standardOutputCopy `
            -StreamName "STDOUT" `
            -Faults $faults
        Complete-CloudStreamCopy `
            -Task $standardErrorCopy `
            -StreamName "STDERR" `
            -Faults $faults

        if ($null -ne $standardOutputStream) {
            $standardOutputStream.Dispose()
        }
        if ($null -ne $standardErrorStream) {
            $standardErrorStream.Dispose()
        }
        if ($null -ne $process) {
            $process.Dispose()
        }
        $stopwatch.Stop()
    }

    if ($exited -and $exitCode -ne 0) {
        Add-CloudFault `
            -Faults $faults `
            -Code "EDITOR_PROCESS_EXIT_NONZERO"
    }
    return [pscustomobject][ordered]@{
        Started = $started
        TimedOut = $timedOut
        Exited = $exited
        ExitCode = $exitCode
        ElapsedSeconds = $stopwatch.Elapsed.TotalSeconds
        FaultCodes = @($faults.ToArray())
    }
}

function Read-CloudReport {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Profiler soak report was not created: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Profiler soak report is a reparse point: $Path"
    }
    $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open(
            $item.FullName,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        if ($stream.Length -le 0 -or $stream.Length -gt 8MB) {
            throw (
                "Profiler soak report has an invalid size: " +
                $stream.Length)
        }
        $reader = New-Object System.IO.StreamReader(
            $stream,
            $strictUtf8,
            $true)
        $json = $reader.ReadToEnd()
        return $json | ConvertFrom-Json
    }
    finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        }
        elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Invoke-CloudScenario {
    param(
        [Parameter(Mandatory = $true)][string]$EditorPath,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$RunDirectory,
        [Parameter(Mandatory = $true)][object]$Scenario,
        [AllowNull()][object]$InitialEnvironment
    )

    [void](Assert-SafeOutputDirectory -Path $RunDirectory)
    $paths = Get-ScenarioPaths -Root $RunDirectory -Name $Scenario.Name
    foreach ($property in $paths.PSObject.Properties) {
        Assert-NewOutputPath -Path ([string]$property.Value)
    }
    $arguments = Get-ScenarioArguments `
        -ProjectPath $ProjectPath `
        -Scenario $Scenario `
        -Paths $paths
    $argumentLine = Join-CommandArguments -Arguments $arguments
    $commandLine =
        (ConvertTo-CommandArgument -Value $EditorPath) + " " + $argumentLine

    if ($DryRun) {
        return [pscustomobject][ordered]@{
            Name = $Scenario.Name
            Camera = @($Scenario.Camera)
            Command = $commandLine
            Paths = $paths
        }
    }

    $startedUtc = [DateTimeOffset]::UtcNow
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $processExecution = $null
    $timedOut = $false
    $exitCode = $null
    $executionFaults =
        New-Object "System.Collections.Generic.List[string]"
    $preLaunchStability = $null
    $postExitStability = $null
    if ($null -eq $InitialEnvironment) {
        Add-CloudFault `
            -Faults $executionFaults `
            -Code "PROVENANCE_BASELINE_MISSING"
    }
    else {
        $preLaunchStability = Test-CloudInputStability `
            -InitialEnvironment $InitialEnvironment `
            -EditorPath $EditorPath `
            -ProjectPath $ProjectPath
        foreach ($fault in @($preLaunchStability.FaultCodes)) {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code ("PRELAUNCH_" + [string]$fault)
        }
    }
    if ($null -ne $preLaunchStability -and $preLaunchStability.Pass) {
        try {
            $processExecution = Invoke-CloudChildProcess `
                -FilePath $EditorPath `
                -Arguments $arguments `
                -WorkingDirectory (Get-Location).ProviderPath `
                -StandardOutputPath $paths.StandardOutput `
                -StandardErrorPath $paths.StandardError `
                -TimeoutMilliseconds (
                    [int](($SoakSeconds + $StartupGraceSeconds) * 1000))
            $timedOut = [bool]$processExecution.TimedOut
            $exitCode = $processExecution.ExitCode
            foreach ($fault in @($processExecution.FaultCodes)) {
                Add-CloudFault `
                    -Faults $executionFaults `
                    -Code ([string]$fault)
            }
        }
        catch {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code "EDITOR_PROCESS_RUNNER_FAILED"
        }
    }
    else {
        Add-CloudFault `
            -Faults $executionFaults `
            -Code "EDITOR_PROCESS_NOT_STARTED_DUE_TO_PROVENANCE"
    }
    $stopwatch.Stop()

    if ($null -ne $InitialEnvironment) {
        $postExitStability = Test-CloudInputStability `
            -InitialEnvironment $InitialEnvironment `
            -EditorPath $EditorPath `
            -ProjectPath $ProjectPath
        foreach ($fault in @($postExitStability.FaultCodes)) {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code ("POSTEXIT_" + [string]$fault)
        }
    }

    [void](Assert-SafeOutputDirectory -Path $RunDirectory)
    $report = $null
    $validation = $null
    try {
        $report = Read-CloudReport -Path $paths.Report
        $validation = Test-CloudQualityReport -Report $report
    }
    catch {
        $reportError = $_.Exception.Message
        Add-CloudFault `
            -Faults $executionFaults `
            -Code "REPORT_READ_FAILED"
    }
    if ($null -eq $validation) {
        $emptyFaults =
            New-Object "System.Collections.Generic.List[string]"
        Add-CloudFault `
            -Faults $emptyFaults `
            -Code "REPORT_VALIDATION_UNAVAILABLE"
        $validation = New-CloudValidationResult `
            -Faults $emptyFaults `
            -Measurements $null `
            -Quality $null
    }

    if ($null -ne $report) {
        $expectedCapture = [System.IO.Path]::GetFullPath($paths.Capture)
        $reportedCapture = ""
        try {
            $reportedCapture =
                [System.IO.Path]::GetFullPath(
                    [string]$report.ProfilerCapturePath)
        }
        catch {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code "CAPTURE_PATH_INVALID"
        }
        if (-not [string]::IsNullOrWhiteSpace($reportedCapture) -and
            -not $reportedCapture.Equals(
                $expectedCapture,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code "CAPTURE_PATH_MISMATCH"
        }
        if (-not (Test-Path -LiteralPath $paths.Capture -PathType Leaf)) {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code "CAPTURE_FILE_MISSING"
        }
        else {
            $captureItem = Get-Item -LiteralPath $paths.Capture -Force
            if ($captureItem.Length -le 0 -or
                ($captureItem.Attributes -band
                    [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                Add-CloudFault `
                    -Faults $executionFaults `
                    -Code "CAPTURE_FILE_INVALID"
            }
        }
        if (-not (Test-NearlyEqual `
                -Left $report.RequestedSeconds `
                -Right $SoakSeconds `
                -Tolerance 0.001)) {
            Add-CloudFault `
                -Faults $executionFaults `
                -Code "REPORT_DURATION_MISMATCH"
        }
    }

    $combinedFaults =
        New-Object "System.Collections.Generic.List[string]"
    foreach ($fault in @($executionFaults.ToArray()) +
        @($validation.FaultCodes)) {
        Add-CloudFault -Faults $combinedFaults -Code ([string]$fault)
    }
    $scenarioPass = $combinedFaults.Count -eq 0

    return [pscustomobject][ordered]@{
        Name = $Scenario.Name
        Camera = @($Scenario.Camera)
        Command = $commandLine
        StartedUtc = $startedUtc.ToString("O", $script:Invariant)
        ElapsedSeconds = $stopwatch.Elapsed.TotalSeconds
        TimedOut = $timedOut
        ProcessStarted =
            $null -ne $processExecution -and
            [bool]$processExecution.Started
        ProcessExited =
            $null -ne $processExecution -and
            [bool]$processExecution.Exited
        ProcessExitCode = $exitCode
        InputStabilityBeforeLaunch = $preLaunchStability
        InputStabilityAfterExit = $postExitStability
        Paths = $paths
        Pass = $scenarioPass
        FaultCodes = @($combinedFaults.ToArray())
        Measurements = $validation.Measurements
        Quality = $validation.Quality
    }
}

function Write-AtomicCloudJson {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $parent = [System.IO.Path]::GetDirectoryName($Path)
    [void](Assert-SafeOutputDirectory -Path $parent)
    Assert-NewOutputPath -Path $Path
    $temporary = Join-Path $parent (
        "." + [System.IO.Path]::GetFileName($Path) +
        "." + [guid]::NewGuid().ToString("N") + ".tmp")
    try {
        $json = $Value | ConvertTo-Json -Depth 12
        [System.IO.File]::WriteAllText(
            $temporary,
            $json + [Environment]::NewLine,
            $script:Utf8NoBom)
        [System.IO.File]::Move($temporary, $Path)
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Format-CloudMetric {
    param(
        [AllowNull()][object]$Value,
        [string]$Format = "0.00"
    )

    if (-not (Test-FiniteNumber -Value $Value)) {
        return "N/A"
    }
    return ([double]$Value).ToString($Format, $script:Invariant)
}

function Show-CloudRunEnvironment {
    param([Parameter(Mandatory = $true)][object]$EnvironmentSnapshot)

    $os = $EnvironmentSnapshot.OperatingSystem
    $editorIdentity = $EnvironmentSnapshot.EditorExecutable
    $osName = if (-not [string]::IsNullOrWhiteSpace(
            [string]$os.WindowsCaption)) {
        [string]$os.WindowsCaption
    }
    else {
        [string]$os.Description
    }
    $identityRows = @(
        [pscustomobject][ordered]@{
            Artifact = "EditorAppHost"
            Version = if (-not [string]::IsNullOrWhiteSpace(
                    [string]$editorIdentity.ProductVersion)) {
                $editorIdentity.ProductVersion
            }
            else {
                $editorIdentity.FileVersion
            }
            Sha256 = $editorIdentity.Sha256
            Path = $editorIdentity.Path
        }
        foreach ($artifact in @(
                $EnvironmentSnapshot.EditorRuntimeArtifacts)) {
            [pscustomobject][ordered]@{
                Artifact = $artifact.Role
                Version = if (-not [string]::IsNullOrWhiteSpace(
                        [string]$artifact.ProductVersion)) {
                    $artifact.ProductVersion
                }
                elseif (-not [string]::IsNullOrWhiteSpace(
                        [string]$artifact.FileVersion)) {
                    $artifact.FileVersion
                }
                else {
                    "-"
                }
                Sha256 = $artifact.Sha256
                Path = $artifact.Path
            }
        }
        [pscustomobject][ordered]@{
            Artifact = "Project"
            Version = "-"
            Sha256 = $EnvironmentSnapshot.Project.Sha256
            Path = $EnvironmentSnapshot.Project.Path
        }
    )
    $gpuRows = foreach ($adapter in @(
            $EnvironmentSnapshot.GpuProbe.Adapters)) {
        [pscustomobject][ordered]@{
            Adapter = $adapter.Name
            Driver = $adapter.DriverVersion
            DriverDateUtc = $adapter.DriverDateUtc
        }
    }

    Write-Host ""
    Write-Host "ACS cloud capture provenance"
    Write-Host (
        "UTC: {0} | OS: {1} ({2})" -f
        $EnvironmentSnapshot.CapturedUtc,
        $osName,
        $os.Architecture)
    $identityTable =
        $identityRows |
        Format-Table -AutoSize |
        Out-String -Width 300
    Write-Host $identityTable.TrimEnd()
    if (@($gpuRows).Count -gt 0) {
        $gpuTable =
            $gpuRows |
            Format-Table -AutoSize |
            Out-String -Width 220
        Write-Host $gpuTable.TrimEnd()
    }
    else {
        Write-Host (
            "GPU adapter/driver: unavailable ({0})" -f
            $EnvironmentSnapshot.GpuProbe.Error)
    }
}

function Show-CloudResultTable {
    param(
        [Parameter(Mandatory = $true)][object[]]$Scenarios,
        [double]$Target
    )

    $rows = foreach ($scenario in $Scenarios) {
        $measurements = $scenario.Measurements
        $targetMet = Test-CloudTarget `
            -Measurements $measurements `
            -Target $Target
        [pscustomobject][ordered]@{
            View = $scenario.Name
            Quality = if ($scenario.Pass) { "PASS" } else { "FAIL" }
            Target = if ($targetMet) {
                "MET"
            }
            else {
                "MISS"
            }
            EditorAvg = if ($null -ne $measurements) {
                Format-CloudMetric -Value $measurements.EditorFpsAverage
            }
            else { "N/A" }
            P95IntFps = if ($null -ne $measurements) {
                Format-CloudMetric `
                    -Value $measurements.EditorFpsFromP95FrameInterval
            }
            else { "N/A" }
            GpuAvgFps = if ($null -ne $measurements) {
                Format-CloudMetric `
                    -Value $measurements.GpuThroughputFromAverageMilliseconds
            }
            else { "N/A" }
            GpuP95Fps = if ($null -ne $measurements) {
                Format-CloudMetric `
                    -Value $measurements.GpuThroughputFromP95Milliseconds
            }
            else { "N/A" }
            CloudAvgMs = if ($null -ne $measurements) {
                Format-CloudMetric -Value $measurements.CloudAverageMilliseconds
            }
            else { "N/A" }
            CloudPeakMs = if ($null -ne $measurements) {
                Format-CloudMetric -Value $measurements.CloudPeakMilliseconds
            }
            else { "N/A" }
            Retry = if ($null -ne $measurements) {
                $measurements.Scheduler.InputPriorityRetries
            }
            else { "N/A" }
            Ready = if ($null -ne $measurements) {
                $measurements.Scheduler.ReadyAfterRetry
            }
            else { "N/A" }
            Fallback = if ($null -ne $measurements) {
                $measurements.Scheduler.BackgroundFallbacks
            }
            else { "N/A" }
            Fairness = if ($null -ne $measurements) {
                $measurements.Scheduler.FairnessYields
            }
            else { "N/A" }
            BusyMaxMs = if ($null -ne $measurements) {
                Format-CloudMetric `
                    -Value (
                        $measurements.Scheduler.
                            MaximumBackpressureEpochMilliseconds)
            }
            else { "N/A" }
            Burst = if ($null -ne $measurements) {
                $measurements.Scheduler.PeakPresentedBurstFrames
            }
            else { "N/A" }
        }
    }
    Write-Host ""
    Write-Host (
        "ACS cloud quality/performance comparison (target {0:0.##} FPS)" -f
        $Target)
    $resultTable =
        $rows |
        Format-Table -AutoSize |
        Out-String -Width 220
    Write-Host $resultTable.TrimEnd()
}

function New-SyntheticCloudReport {
    $traceWidth = 216
    $traceHeight = 110
    $outputWidth = 864
    $outputHeight = 438
    $traceLogical = $traceWidth * $traceHeight
    $resolveLogical = $outputWidth * $outputHeight
    $maximumView = $traceLogical * $script:ExpectedViewSamples
    $maximumLight = $maximumView * $script:ExpectedLightSamples
    $worldShadowLogical =
        $script:ExpectedWorldShadowLogicalInvocations
    $worldShadowLaunched =
        $script:ExpectedWorldShadowLaunchedThreads
    return [pscustomobject][ordered]@{
        SchemaVersion = 2
        Result = "PASS"
        RequestedSeconds = 30
        FaultCodes = @()
        ProfilerCapturePath = "C:\Temp\synthetic.csv"
        ProfilerCaptureError = $null
        ProfilerSummary = [pscustomobject][ordered]@{
            SchemaVersion = 2
            SampleCount = 120
            UsesObservedCadence = $true
            EditorFps = [pscustomobject]@{
                SampleCount = 119
                Average = 250.0
                P95 = 260.0
            }
            ObservedFrameIntervalMilliseconds = [pscustomobject]@{
                SampleCount = 119
                Average = 4.0
                P95 = 5.0
            }
            EditorFpsFromP95FrameInterval = 200.0
            GpuQueryMilliseconds = [pscustomobject]@{
                SampleCount = 120
                Average = 2.5
                P95 = 3.0
            }
            GpuThroughputFromAverageMilliseconds = 400.0
            GpuThroughputFromP95Milliseconds = 333.3333333333333
            LatestGpuQueryWindow = [pscustomobject][ordered]@{
                Available = $true
                QueryCount = 120
                QueryCapacity = 120
                LatencyFrames = 2
                FrameAverageMilliseconds = 2.5
                FramePeakMilliseconds = 3.2
                OpaqueAverageMilliseconds = 0.02
                OpaquePeakMilliseconds = 0.04
                AtmosphereAverageMilliseconds = 0.03
                AtmospherePeakMilliseconds = 0.05
                CloudAverageMilliseconds = 2.1
                CloudPeakMilliseconds = 2.8
                FogAverageMilliseconds = 0.01
                FogPeakMilliseconds = 0.02
                PostAverageMilliseconds = 0.1
                PostPeakMilliseconds = 0.2
            }
            LatestRenderState = [pscustomobject][ordered]@{
                Available = $true
                Flags = 95
                View3D = $true
                CloudsEnabled = $true
                FogEnabled = $true
                AerialPerspectiveEnabled = $true
                GameView = $false
                ScenePresentationSuppressed = $false
                DrawCalls = 32
                DispatchCalls = 4
                ViewportWidth = $outputWidth
                ViewportHeight = $outputHeight
                CloudWidth = $traceWidth
                CloudHeight = $traceHeight
                CloudMarchSteps = $script:ExpectedViewSamples
                CloudLightSteps = $script:ExpectedLightSamples
                CloudRenderScale = $script:ExpectedCloudScale
            }
            LatestCloudWorkload = [pscustomobject][ordered]@{
                Status = "Available"
                Available = $true
                Attempted = $true
                Submitted = $true
                HistoryWasAvailable = $true
                HistoryReused = $true
                HistoryInvalidated = $false
                TemporalSuperResolution = $true
                SkipReason = 0
                ProfilerFrameIndex = 2000
                SubmissionIndex = 1990
                TraceWidth = $traceWidth
                TraceHeight = $traceHeight
                OutputWidth = $outputWidth
                OutputHeight = $outputHeight
                SteadyDispatches = 2
                OneTimeBakeDispatches = 0
                ShadowCacheDispatches = 1
                WorldShadowDispatches = 1
                TotalComputeDispatches = 4
                CompositeDraws = 1
                TraceLogicalInvocations = $traceLogical
                TraceLaunchedThreads = 24192
                ResolveLogicalInvocations = $resolveLogical
                ResolveLaunchedThreads = 380160
                OneTimeBakeLogicalInvocations = 0
                OneTimeBakeLaunchedThreads = 0
                ShadowCacheLogicalInvocations =
                    $script:ExpectedShadowCacheLogicalInvocations
                ShadowCacheLaunchedThreads =
                    $script:ExpectedShadowCacheLaunchedThreads
                WorldShadowLogicalInvocations =
                    $worldShadowLogical
                WorldShadowLaunchedThreads =
                    $worldShadowLaunched
                TotalLogicalInvocations =
                    $traceLogical + $resolveLogical +
                    $script:ExpectedShadowCacheLogicalInvocations +
                    $worldShadowLogical
                TotalLaunchedThreads =
                    24192 + 380160 +
                    $script:ExpectedShadowCacheLaunchedThreads +
                    $worldShadowLaunched
                MaximumViewSamples = $maximumView
                MaximumLightSamples = $maximumLight
                MaximumWorldShadowSamples =
                    $script:ExpectedWorldShadowSamples
            }
            LatestEditorRuntime = [pscustomobject][ordered]@{
                NativeAvailable = $true
                NativeCallCount = 4000
                SlowNativeCallCount = 1
                GpuBackpressureYieldCount = 200
                GpuBackpressureInputRetryCount = 600
                GpuBackpressureBackgroundFallbackCount = 2
                GpuReadyAfterRetryCount = 198
                RenderFairnessYieldCount = 100
                LastGpuBackpressureEpochMilliseconds = 2.5
                MaximumGpuBackpressureEpochMilliseconds = 4.5
                PeakPresentedRenderBurstFrames = 8
                PeakRenderBurstActiveCpuMilliseconds = 7.5
                LastNativeCallMilliseconds = 0.3
                MaximumNativeCallMilliseconds = 20.0
                LastNativeCallKind = "render"
                DispatcherAvailable = $true
                DispatcherHeartbeatCount = 80
                DispatcherStallCount = 0
            }
        }
    }
}

function New-SyntheticCloudEnvironment {
    $artifactIndex = 0
    $runtimeArtifacts = foreach ($role in
        $script:ExpectedEditorArtifactRoles) {
        $artifactIndex++
        [pscustomobject][ordered]@{
            Role = $role
            Path = "C:\Synthetic\$role.bin"
            Sha256 = ([string]$artifactIndex) * 64
            LengthBytes = 2048 + $artifactIndex
            LastWriteUtc = "2026-07-28T00:00:00.0000000Z"
            FileVersion = if ($role -eq "ManagedAssembly") {
                "1.0.0.0"
            }
            else {
                $null
            }
            ProductVersion = if ($role -eq "ManagedAssembly") {
                "1.0.0"
            }
            else {
                $null
            }
        }
    }
    return [pscustomobject][ordered]@{
        SchemaVersion = 2
        CapturedUtc = "2026-07-28T00:00:00.0000000+00:00"
        MachineName = "synthetic-host"
        OperatingSystem = [pscustomobject][ordered]@{
            Description = "Synthetic Windows"
            Architecture = "X64"
            WindowsCaption = "Synthetic Windows"
            WindowsVersion = "10.0"
            WindowsBuild = "99999"
            ProbeError = $null
        }
        Runtime = [pscustomobject][ordered]@{
            PowerShellVersion = "7.0"
            PowerShellEdition = "Core"
            FrameworkDescription = ".NET synthetic"
            ProcessArchitecture = "AMD64"
        }
        EditorExecutable = [pscustomobject][ordered]@{
            Path = "C:\Synthetic\AcsEditor.exe"
            Sha256 = "a" * 64
            LengthBytes = 1024
            LastWriteUtc = "2026-07-28T00:00:00.0000000Z"
            FileVersion = "1.0.0.0"
            ProductVersion = "1.0.0"
        }
        EditorRuntimeArtifacts = @($runtimeArtifacts)
        Project = [pscustomobject][ordered]@{
            Path = "C:\Synthetic\CloudQuality.acsproject"
            Sha256 = "b" * 64
            LengthBytes = 128
            LastWriteUtc = "2026-07-28T00:00:00.0000000Z"
            FileVersion = $null
            ProductVersion = $null
        }
        GpuProbe = [pscustomobject][ordered]@{
            Status = "Available"
            Error = $null
            Adapters = @(
                [pscustomobject][ordered]@{
                    Name = "Synthetic GPU"
                    AdapterCompatibility = "Synthetic"
                    DriverVersion = "1.2.3"
                    DriverDateUtc = "2026-07-01T00:00:00.0000000Z"
                    PnpDeviceId = "PCI\SYNTHETIC"
                    VideoProcessor = "Synthetic"
                    AdapterRamBytes = [uint64]8589934592
                    Status = "OK"
                }
            )
        }
    }
}

function Copy-CloudObject {
    param([Parameter(Mandatory = $true)][object]$Value)

    return $Value | ConvertTo-Json -Depth 12 | ConvertFrom-Json
}

function Invoke-CloudProfilerSelfTest {
    $failures = New-Object "System.Collections.Generic.List[string]"
    $assertions = 0
    function Assert-CloudSelfTest {
        param([bool]$Condition, [string]$Name)

        $script:CloudSelfTestAssertions++
        if (-not $Condition) {
            [void]$script:CloudSelfTestFailures.Add($Name)
        }
    }

    $script:CloudSelfTestAssertions = 0
    $script:CloudSelfTestFailures = $failures
    $validReport = New-SyntheticCloudReport
    $valid = Test-CloudQualityReport -Report $validReport
    Assert-CloudSelfTest $valid.Pass "valid report"

    $validEnvironment = New-SyntheticCloudEnvironment
    $environmentValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $validEnvironment
    Assert-CloudSelfTest `
        $environmentValidation.Pass `
        "valid provenance environment"
    $missingDriver = Copy-CloudObject -Value $validEnvironment
    $missingDriver.GpuProbe.Adapters[0].DriverVersion = ""
    $missingDriverValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $missingDriver
    Assert-CloudSelfTest `
        (-not $missingDriverValidation.Pass -and
         $missingDriverValidation.FaultCodes -contains
            "GPU_ADAPTER_OR_DRIVER_INVALID") `
        "GPU driver identity required"
    $missingDriverDate = Copy-CloudObject -Value $validEnvironment
    $missingDriverDate.GpuProbe.Adapters[0].DriverDateUtc = $null
    $missingDriverDateValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $missingDriverDate
    Assert-CloudSelfTest `
        (-not $missingDriverDateValidation.Pass -and
         $missingDriverDateValidation.FaultCodes -contains
            "GPU_ADAPTER_OR_DRIVER_INVALID") `
        "GPU driver date required"
    $badEnvironmentSchema = Copy-CloudObject -Value $validEnvironment
    $badEnvironmentSchema.SchemaVersion = $null
    $badEnvironmentSchemaValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $badEnvironmentSchema
    Assert-CloudSelfTest `
        (-not $badEnvironmentSchemaValidation.Pass -and
         $badEnvironmentSchemaValidation.FaultCodes -contains
            "ENVIRONMENT_SCHEMA_UNSUPPORTED") `
        "provenance schema null rejected"
    $missingNativeArtifact = Copy-CloudObject -Value $validEnvironment
    $missingNativeArtifact.EditorRuntimeArtifacts = @(
        $missingNativeArtifact.EditorRuntimeArtifacts |
            Where-Object Role -NE "NativeRenderer")
    $missingNativeArtifactValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $missingNativeArtifact
    Assert-CloudSelfTest `
        (-not $missingNativeArtifactValidation.Pass -and
         $missingNativeArtifactValidation.FaultCodes -contains
            "EDITOR_RUNTIME_ARTIFACT_MISSING_NATIVERENDERER") `
        "native renderer provenance required"
    Assert-CloudSelfTest `
        (-not (Test-CloudTarget `
            -Measurements $valid.Measurements `
            -Target 300.0)) `
        "default target miss remains measurable"

    $faster = Copy-CloudObject -Value $validReport
    $faster.ProfilerSummary.EditorFps.Average = 350.0
    $faster.ProfilerSummary.ObservedFrameIntervalMilliseconds.P95 = 3.0
    $faster.ProfilerSummary.EditorFpsFromP95FrameInterval =
        333.3333333333333
    $fasterValidation = Test-CloudQualityReport -Report $faster
    Assert-CloudSelfTest `
        (Test-CloudTarget `
            -Measurements $fasterValidation.Measurements `
            -Target 300.0) `
        "target met boundary"

    $faulted = Copy-CloudObject -Value $validReport
    $faulted.Result = "FAIL"
    $faulted.FaultCodes = @("EXAMPLE")
    $faultedValidation = Test-CloudQualityReport -Report $faulted
    Assert-CloudSelfTest `
        (-not $faultedValidation.Pass -and
         $faultedValidation.FaultCodes -contains "REPORT_RESULT_NOT_PASS" -and
         $faultedValidation.FaultCodes -contains "REPORT_FAULT_CODES_PRESENT") `
        "report fault propagation"
    $lowercaseResult = Copy-CloudObject -Value $validReport
    $lowercaseResult.Result = "pass"
    $lowercaseResultValidation =
        Test-CloudQualityReport -Report $lowercaseResult
    Assert-CloudSelfTest `
        ($lowercaseResultValidation.FaultCodes -contains
            "REPORT_RESULT_NOT_PASS") `
        "result enum is case sensitive"
    $stringBoolean = Copy-CloudObject -Value $validReport
    $stringBoolean.ProfilerSummary.UsesObservedCadence = "true"
    $stringBooleanValidation =
        Test-CloudQualityReport -Report $stringBoolean
    Assert-CloudSelfTest `
        ($stringBooleanValidation.FaultCodes -contains
            "PROFILER_NOT_USING_OBSERVED_CADENCE") `
        "boolean strings rejected by schema"

    $wrongSteps = Copy-CloudObject -Value $validReport
    $wrongSteps.ProfilerSummary.LatestRenderState.CloudMarchSteps = 191
    $wrongStepsValidation = Test-CloudQualityReport -Report $wrongSteps
    Assert-CloudSelfTest `
        ($wrongStepsValidation.FaultCodes -contains
            "CLOUD_VIEW_SAMPLE_COUNT_CHANGED") `
        "view sample fail closed"

    $wrongScale = Copy-CloudObject -Value $validReport
    $wrongScale.ProfilerSummary.LatestRenderState.CloudRenderScale = 0.5
    $wrongScaleValidation = Test-CloudQualityReport -Report $wrongScale
    Assert-CloudSelfTest `
        ($wrongScaleValidation.FaultCodes -contains
            "CLOUD_RENDER_SCALE_CHANGED") `
        "render scale fail closed"

    $stringMetric = Copy-CloudObject -Value $validReport
    $stringMetric.ProfilerSummary.EditorFps.Average = "250.0"
    $stringMetricValidation =
        Test-CloudQualityReport -Report $stringMetric
    Assert-CloudSelfTest `
        ($stringMetricValidation.FaultCodes -contains
            "EDITOR_FPS_INVALID") `
        "numeric strings rejected by schema"

    $nanMetric = Copy-CloudObject -Value $validReport
    $nanMetric.ProfilerSummary.EditorFps.Average = [double]::NaN
    $nanMetricValidation = Test-CloudQualityReport -Report $nanMetric
    Assert-CloudSelfTest `
        ($nanMetricValidation.FaultCodes -contains "EDITOR_FPS_INVALID") `
        "NaN metric rejected without throwing"

    $noGpu = Copy-CloudObject -Value $validReport
    $noGpu.ProfilerSummary.GpuQueryMilliseconds.SampleCount = 0
    $noGpu.ProfilerSummary.LatestGpuQueryWindow.Available = $false
    $noGpuValidation = Test-CloudQualityReport -Report $noGpu
    Assert-CloudSelfTest `
        (-not $noGpuValidation.Pass -and
         $noGpuValidation.FaultCodes -contains "GPU_QUERY_INVALID" -and
         $noGpuValidation.FaultCodes -contains
            "GPU_PASS_WINDOW_UNAVAILABLE") `
        "GPU evidence required"
    $nullLatency = Copy-CloudObject -Value $validReport
    $nullLatency.ProfilerSummary.LatestGpuQueryWindow.LatencyFrames = $null
    $nullLatencyValidation =
        Test-CloudQualityReport -Report $nullLatency
    Assert-CloudSelfTest `
        ($nullLatencyValidation.FaultCodes -contains
            "GPU_PASS_WINDOW_LATENCYFRAMES_INVALID") `
        "GPU latency null rejected"
    $excessQueries = Copy-CloudObject -Value $validReport
    $excessQueries.ProfilerSummary.LatestGpuQueryWindow.QueryCount = 121
    $excessQueriesValidation =
        Test-CloudQualityReport -Report $excessQueries
    Assert-CloudSelfTest `
        ($excessQueriesValidation.FaultCodes -contains
            "GPU_PASS_WINDOW_QUERY_COUNT_EXCEEDS_CAPACITY") `
        "GPU query count bounded by capacity"

    $noHistory = Copy-CloudObject -Value $validReport
    $noHistory.ProfilerSummary.LatestCloudWorkload.HistoryReused = $false
    $noHistoryValidation = Test-CloudQualityReport -Report $noHistory
    Assert-CloudSelfTest `
        ($noHistoryValidation.FaultCodes -contains
            "CLOUD_HISTORY_NOT_REUSED") `
        "history reuse required"
    $stringHistoryBoolean = Copy-CloudObject -Value $validReport
    $stringHistoryBoolean.ProfilerSummary.LatestCloudWorkload.
        HistoryInvalidated = "false"
    $stringHistoryBooleanValidation =
        Test-CloudQualityReport -Report $stringHistoryBoolean
    Assert-CloudSelfTest `
        ($stringHistoryBooleanValidation.FaultCodes -contains
            "CLOUD_HISTORY_NOT_REUSED") `
        "history boolean strings rejected"

    $badMaximum = Copy-CloudObject -Value $validReport
    $badMaximum.ProfilerSummary.LatestCloudWorkload.MaximumViewSamples--
    $badMaximumValidation = Test-CloudQualityReport -Report $badMaximum
    Assert-CloudSelfTest `
        ($badMaximumValidation.FaultCodes -contains
            "CLOUD_MAXIMUM_VIEW_SAMPLES_INCOHERENT") `
        "maximum view work coherent"
    $badWorldMaximum = Copy-CloudObject -Value $validReport
    $badWorldMaximum.ProfilerSummary.LatestCloudWorkload.
        MaximumWorldShadowSamples--
    $badWorldMaximumValidation =
        Test-CloudQualityReport -Report $badWorldMaximum
    Assert-CloudSelfTest `
        ($badWorldMaximumValidation.FaultCodes -contains
            "CLOUD_MAXIMUM_WORLD_SHADOW_SAMPLES_INCOHERENT") `
        "maximum world-shadow work coherent"

    $extraDispatch = Copy-CloudObject -Value $validReport
    $extraDispatch.ProfilerSummary.LatestCloudWorkload.
        ShadowCacheDispatches = 2
    $extraDispatch.ProfilerSummary.LatestCloudWorkload.
        TotalComputeDispatches = 5
    $extraDispatchValidation =
        Test-CloudQualityReport -Report $extraDispatch
    Assert-CloudSelfTest `
        ($extraDispatchValidation.FaultCodes -contains
            "CLOUD_STEADY_WORKLOAD_CHANGED") `
        "second steady shadow dispatch rejected"
    $extraWorldShadowDispatch = Copy-CloudObject -Value $validReport
    $extraWorldShadowDispatch.ProfilerSummary.LatestCloudWorkload.
        WorldShadowDispatches = 2
    $extraWorldShadowDispatch.ProfilerSummary.LatestCloudWorkload.
        TotalComputeDispatches = 5
    $extraWorldShadowDispatchValidation =
        Test-CloudQualityReport -Report $extraWorldShadowDispatch
    Assert-CloudSelfTest `
        ($extraWorldShadowDispatchValidation.FaultCodes -contains
            "CLOUD_STEADY_WORKLOAD_CHANGED") `
        "second world-shadow dispatch rejected"
    $changedShadowWork = Copy-CloudObject -Value $validReport
    $changedShadowWork.ProfilerSummary.LatestCloudWorkload.
        ShadowCacheLogicalInvocations--
    $changedShadowWork.ProfilerSummary.LatestCloudWorkload.
        TotalLogicalInvocations--
    $changedShadowWorkValidation =
        Test-CloudQualityReport -Report $changedShadowWork
    Assert-CloudSelfTest `
        ($changedShadowWorkValidation.FaultCodes -contains
            "CLOUD_SHADOW_WORKLOAD_CHANGED") `
        "steady shadow workload dimensions fixed"
    $changedWorldShadowWork = Copy-CloudObject -Value $validReport
    $changedWorldShadowWork.ProfilerSummary.LatestCloudWorkload.
        WorldShadowLogicalInvocations--
    $changedWorldShadowWork.ProfilerSummary.LatestCloudWorkload.
        TotalLogicalInvocations--
    $changedWorldShadowWorkValidation =
        Test-CloudQualityReport -Report $changedWorldShadowWork
    Assert-CloudSelfTest `
        ($changedWorldShadowWorkValidation.FaultCodes -contains
            "CLOUD_WORLD_SHADOW_WORKLOAD_CHANGED") `
        "world-shadow workload dimensions fixed"
    $nullZeroWork = Copy-CloudObject -Value $validReport
    $nullZeroWork.ProfilerSummary.LatestCloudWorkload.
        ShadowCacheDispatches = $null
    $nullZeroWorkValidation =
        Test-CloudQualityReport -Report $nullZeroWork
    Assert-CloudSelfTest `
        ($nullZeroWorkValidation.FaultCodes -contains
            "CLOUD_WORKLOAD_SHADOWCACHEDISPATCHES_INVALID") `
        "null zero-work diagnostic rejected"
    $nullWorldShadowWork = Copy-CloudObject -Value $validReport
    $nullWorldShadowWork.ProfilerSummary.LatestCloudWorkload.
        WorldShadowDispatches = $null
    $nullWorldShadowWorkValidation =
        Test-CloudQualityReport -Report $nullWorldShadowWork
    Assert-CloudSelfTest `
        ($nullWorldShadowWorkValidation.FaultCodes -contains
            "CLOUD_WORKLOAD_WORLDSHADOWDISPATCHES_INVALID") `
        "null world-shadow diagnostic rejected"

    $missingScheduler = Copy-CloudObject -Value $validReport
    $missingScheduler.ProfilerSummary.LatestEditorRuntime.PSObject.Properties.Remove(
        "GpuBackpressureInputRetryCount")
    $missingSchedulerValidation =
        Test-CloudQualityReport -Report $missingScheduler
    Assert-CloudSelfTest `
        ($missingSchedulerValidation.FaultCodes -contains
            "EDITOR_RUNTIME_MISSING_GPUBACKPRESSUREINPUTRETRYCOUNT") `
        "scheduler diagnostic required"
    $nullScheduler = Copy-CloudObject -Value $validReport
    $nullScheduler.ProfilerSummary.LatestEditorRuntime.
        GpuBackpressureYieldCount = $null
    $nullSchedulerValidation =
        Test-CloudQualityReport -Report $nullScheduler
    Assert-CloudSelfTest `
        ($nullSchedulerValidation.FaultCodes -contains
            "EDITOR_RUNTIME_GPUBACKPRESSUREYIELDCOUNT_INVALID") `
        "scheduler null diagnostic rejected"

    $sameQuality = Compare-CloudQuality `
        -Horizon $valid.Quality `
        -Zenith $valid.Quality `
        -Above $valid.Quality
    Assert-CloudSelfTest $sameQuality.Pass "3 視点で同じ品質を受理"
    $changedQuality = Copy-CloudObject -Value $valid.Quality
    $changedQuality.CloudWidth++
    $differentQuality = Compare-CloudQuality `
        -Horizon $valid.Quality `
        -Zenith $valid.Quality `
        -Above $changedQuality
    Assert-CloudSelfTest `
        (-not $differentQuality.Pass -and
         $differentQuality.MismatchCodes -contains
            "QUALITY_MISMATCH_CLOUDWIDTH") `
        "上空視点の品質差を拒否"
    $extendedQuality = Copy-CloudObject -Value $valid.Quality
    $extendedQuality | Add-Member -NotePropertyName FutureQualityField `
        -NotePropertyValue 1
    $asymmetricQuality = Compare-CloudQuality `
        -Horizon $valid.Quality `
        -Zenith $valid.Quality `
        -Above $extendedQuality
    Assert-CloudSelfTest `
        (-not $asymmetricQuality.Pass -and
         $asymmetricQuality.MismatchCodes -contains
            "QUALITY_UNEXPECTED_FUTUREQUALITYFIELD") `
        "3 視点の品質項目を対称に比較"

    $missingAboveQuality = Compare-CloudQuality `
        -Horizon $valid.Quality `
        -Zenith $valid.Quality `
        -Above $null
    Assert-CloudSelfTest `
        (-not $missingAboveQuality.Pass -and
         $missingAboveQuality.MismatchCodes -contains
            "QUALITY_SNAPSHOT_MISSING") `
        "上空視点の品質 snapshot を必須化"

    $scenarioDefinitions = @(Get-ScenarioDefinitions)
    $aboveDefinition = $scenarioDefinitions |
        Where-Object Name -EQ "above" |
        Select-Object -First 1
    Assert-CloudSelfTest `
        ($scenarioDefinitions.Count -eq 3 -and
         $null -ne $aboveDefinition -and
         @($aboveDefinition.Camera).Count -eq 6 -and
         $aboveDefinition.Camera[1] -eq "0.35" -and
         $aboveDefinition.Camera[2] -eq "8000" -and
         $aboveDefinition.Camera[4] -eq "7200") `
        "決定論的な上空視点を定義"

    $badP95 = Copy-CloudObject -Value $validReport
    $badP95.ProfilerSummary.EditorFpsFromP95FrameInterval = 201.0
    $badP95Validation = Test-CloudQualityReport -Report $badP95
    Assert-CloudSelfTest `
        ($badP95Validation.FaultCodes -contains
            "P95_INTERVAL_FPS_INCOHERENT") `
        "p95 interval conversion"

    $safeOutput = Join-Path `
        $env:TEMP `
        ("acs-cloud-selftest-" + [guid]::NewGuid().ToString("N"))
    $safeOutputResult =
        Assert-SafeOutputDirectory -Path $safeOutput
    Assert-CloudSelfTest `
        ($safeOutputResult -eq [System.IO.Path]::GetFullPath($safeOutput)) `
        "TEMP output accepted without creation"
    $outsideTemp = Join-Path `
        ([System.IO.Path]::GetPathRoot($env:TEMP)) `
        ("acs-cloud-outside-" + [guid]::NewGuid().ToString("N"))
    $outsideRejected = $false
    try {
        [void](Assert-SafeOutputDirectory -Path $outsideTemp)
    }
    catch {
        $outsideRejected = $true
    }
    Assert-CloudSelfTest $outsideRejected "outside TEMP rejected"
    $existingOutputRejected = $false
    try {
        Assert-NewOutputPath -Path $PSCommandPath
    }
    catch {
        $existingOutputRejected = $true
    }
    Assert-CloudSelfTest `
        $existingOutputRejected `
        "existing output rejected"

    $selfIdentity = Get-CloudFileIdentity -Path $PSCommandPath
    $selfRuntimeArtifacts = @(
        Get-CloudArtifactIdentity `
            -Role "NativeRenderer" `
            -Path $PSCommandPath
    )
    $selfBaseline = [pscustomobject]@{
        EditorExecutable = $selfIdentity
        EditorRuntimeArtifacts = @($selfRuntimeArtifacts)
        Project = $selfIdentity
    }
    $stableSelf = Test-CloudInputStability `
        -InitialEnvironment $selfBaseline `
        -EditorPath $PSCommandPath `
        -ProjectPath $PSCommandPath
    Assert-CloudSelfTest $stableSelf.Pass "provenance hash revalidation"
    $changedBaseline = Copy-CloudObject -Value $selfBaseline
    $changedBaseline.Project.Sha256 = "0" * 64
    $changedBaseline.EditorRuntimeArtifacts[0].Sha256 = "0" * 64
    $changedSelf = Test-CloudInputStability `
        -InitialEnvironment $changedBaseline `
        -EditorPath $PSCommandPath `
        -ProjectPath $PSCommandPath
    Assert-CloudSelfTest `
        (-not $changedSelf.Pass -and
         $changedSelf.FaultCodes -contains "PROJECT_CHANGED_DURING_RUN") `
        "provenance hash mismatch rejected"
    Assert-CloudSelfTest `
        (-not $changedSelf.Pass -and
         $changedSelf.FaultCodes -contains
            "EDITOR_RUNTIME_ARTIFACT_CHANGED_NATIVERENDERER") `
        "native renderer hash mismatch rejected"

    $argumentPaths = [pscustomobject]@{
        Report = Join-Path $env:TEMP "synthetic-report.json"
        Capture = Join-Path $env:TEMP "synthetic-capture.csv"
    }
    $argumentScenario = [pscustomobject]@{
        Name = "synthetic"
        Camera = @("0", "0", "18", "0", "2", "0")
    }
    $scenarioArguments = Get-ScenarioArguments `
        -ProjectPath "C:\Synthetic\CloudQuality.acsproject" `
        -Scenario $argumentScenario `
        -Paths $argumentPaths
    Assert-CloudSelfTest `
        ($scenarioArguments -contains "--unattended" -and
         $scenarioArguments -contains "--camera3d" -and
         $scenarioArguments -contains "--interaction-soak-report" -and
            $scenarioArguments -contains "--profiler-capture") `
        "unattended profiler command contract"
    $quotedTrailingSlash =
        ConvertTo-CommandArgument -Value 'C:\Synthetic Space\'
    Assert-CloudSelfTest `
        ($quotedTrailingSlash -eq '"C:\Synthetic Space\\"') `
        "Windows trailing slash argument quoting"

    $processSelfTestRoot = Join-Path `
        $env:TEMP `
        ("acs-cloud-process-selftest-" + [guid]::NewGuid().ToString("N"))
    $processSelfTestCleanupPassed = $false
    $timeoutChildGone = $false
    $timeoutChild = $null
    try {
        $processSelfTestRoot = Assert-SafeOutputDirectory `
            -Path $processSelfTestRoot `
            -Create
        # 実行中の本体を再利用し、Windows PowerShellとPowerShell 7の実行名の差を吸収する。
        $selfTestPowerShell = Assert-CloudRegularInputFile `
            -Path (Get-Process -Id $PID).Path `
            -Label "Self-test PowerShell"
        $selfTestCmd = Assert-CloudRegularInputFile `
            -Path (
                Join-Path $env:SystemRoot "System32\cmd.exe") `
            -Label "Self-test command processor"
        $encodePowerShellArguments = {
            param([Parameter(Mandatory = $true)][string]$Command)

            $encoded = [System.Convert]::ToBase64String(
                [System.Text.Encoding]::Unicode.GetBytes($Command))
            return @(
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-EncodedCommand",
                $encoded
            )
        }

        $zeroOutput = Join-Path $processSelfTestRoot "zero-stdout.log"
        $zeroError = Join-Path $processSelfTestRoot "zero-stderr.log"
        $zeroResult = Invoke-CloudChildProcess `
            -FilePath $selfTestCmd `
            -Arguments @(
                "/d",
                "/s",
                "/c",
                (
                    "echo cloud-zero-stdout & " +
                    "echo cloud-zero-stderr 1>&2 & exit /b 0")
            ) `
            -WorkingDirectory $processSelfTestRoot `
            -StandardOutputPath $zeroOutput `
            -StandardErrorPath $zeroError `
            -TimeoutMilliseconds 10000
        Assert-CloudSelfTest `
            ($zeroResult.Started -and
             $zeroResult.Exited -and
             -not $zeroResult.TimedOut -and
             $zeroResult.ExitCode -eq 0 -and
             @($zeroResult.FaultCodes).Count -eq 0) `
            "direct process exit zero"
        Assert-CloudSelfTest `
            (([System.IO.File]::ReadAllText($zeroOutput) -match
                "cloud-zero-stdout") -and
             ([System.IO.File]::ReadAllText($zeroError) -match
                "cloud-zero-stderr")) `
            "direct process async stdout and stderr"

        $nonzeroOutput = Join-Path $processSelfTestRoot "nonzero-stdout.log"
        $nonzeroError = Join-Path $processSelfTestRoot "nonzero-stderr.log"
        $nonzeroResult = Invoke-CloudChildProcess `
            -FilePath $selfTestCmd `
            -Arguments @(
                "/d",
                "/s",
                "/c",
                "echo cloud-seven & exit /b 7"
            ) `
            -WorkingDirectory $processSelfTestRoot `
            -StandardOutputPath $nonzeroOutput `
            -StandardErrorPath $nonzeroError `
            -TimeoutMilliseconds 10000
        Assert-CloudSelfTest `
            ($nonzeroResult.Started -and
             $nonzeroResult.Exited -and
             -not $nonzeroResult.TimedOut -and
             $nonzeroResult.ExitCode -eq 7 -and
             $nonzeroResult.FaultCodes -contains
                "EDITOR_PROCESS_EXIT_NONZERO") `
            "direct process nonzero exit preserved"

        $timeoutOutput = Join-Path $processSelfTestRoot "timeout-stdout.log"
        $timeoutError = Join-Path $processSelfTestRoot "timeout-stderr.log"
        $timeoutChildPidPath =
            Join-Path $processSelfTestRoot "timeout-child.pid"
        $powerShellLiteral = $selfTestPowerShell.Replace("'", "''")
        $pidPathLiteral = $timeoutChildPidPath.Replace("'", "''")
        $treeCommand = @(
            '$startInfo = New-Object System.Diagnostics.ProcessStartInfo'
            ('$startInfo.FileName = ''{0}''' -f $powerShellLiteral)
            '$startInfo.Arguments = ''-NoLogo -NoProfile -NonInteractive -Command "Start-Sleep -Seconds 60"'''
            '$startInfo.UseShellExecute = $false'
            '$startInfo.CreateNoWindow = $true'
            '$startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden'
            '$child = New-Object System.Diagnostics.Process'
            '$child.StartInfo = $startInfo'
            '[void]$child.Start()'
            ('[System.IO.File]::WriteAllText(''{0}'', [string]$child.Id)' -f
                $pidPathLiteral)
            '$child.WaitForExit()'
        ) -join [Environment]::NewLine
        $timeoutResult = Invoke-CloudChildProcess `
            -FilePath $selfTestPowerShell `
            -Arguments (& $encodePowerShellArguments $treeCommand) `
            -WorkingDirectory $processSelfTestRoot `
            -StandardOutputPath $timeoutOutput `
            -StandardErrorPath $timeoutError `
            -TimeoutMilliseconds 3000
        Assert-CloudSelfTest `
            ($timeoutResult.Started -and
             $timeoutResult.Exited -and
             $timeoutResult.TimedOut -and
             $timeoutResult.FaultCodes -contains "EDITOR_PROCESS_TIMEOUT") `
            "direct process timeout"

        $timeoutChildPid = 0
        if (Test-Path -LiteralPath $timeoutChildPidPath -PathType Leaf) {
            [void][int]::TryParse(
                [System.IO.File]::ReadAllText($timeoutChildPidPath).Trim(),
                [ref]$timeoutChildPid)
        }
        if ($timeoutChildPid -gt 0) {
            $timeoutChild = Get-Process `
                -Id $timeoutChildPid `
                -ErrorAction SilentlyContinue
        }
        $timeoutChildGone =
            $timeoutChildPid -gt 0 -and $null -eq $timeoutChild
        Assert-CloudSelfTest `
            $timeoutChildGone `
            "timeout terminates descendant process"
    }
    catch {
        Assert-CloudSelfTest $false (
            "direct process runner unexpected failure: " +
            $_.Exception.Message)
    }
    finally {
        if ($null -ne $timeoutChild) {
            Stop-CloudProcessTree -Process $timeoutChild
            $timeoutChild.Dispose()
        }
        try {
            if (Test-Path -LiteralPath $processSelfTestRoot) {
                [void](Assert-SafeOutputDirectory `
                    -Path $processSelfTestRoot)
                foreach ($entry in Get-ChildItem `
                        -LiteralPath $processSelfTestRoot `
                        -Force) {
                    if ($entry.PSIsContainer -or
                        ($entry.Attributes -band
                            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                        throw (
                            "Unexpected self-test cleanup entry: " +
                            $entry.FullName)
                    }
                    [System.IO.File]::Delete($entry.FullName)
                }
                [System.IO.Directory]::Delete(
                    $processSelfTestRoot,
                    $false)
            }
            $processSelfTestCleanupPassed =
                -not (Test-Path -LiteralPath $processSelfTestRoot)
        }
        catch {
            $processSelfTestCleanupPassed = $false
        }
    }
    Assert-CloudSelfTest `
        $processSelfTestCleanupPassed `
        "direct process self-test artifacts cleaned"

    $assertions = $script:CloudSelfTestAssertions
    Remove-Variable -Name CloudSelfTestAssertions -Scope Script
    Remove-Variable -Name CloudSelfTestFailures -Scope Script
    if ($failures.Count -ne 0) {
        foreach ($failure in $failures) {
            Write-Host "[FAIL] $failure" -ForegroundColor Red
        }
        throw "Cloud profiler self-test failed: $($failures.Count)/$assertions"
    }
    Write-Host "Cloud profiler self-test: ALL PASS ($assertions assertions)"
}

if ($SelfTest) {
    Invoke-CloudProfilerSelfTest
    exit 0
}

$inputLeases = @()
try {
    $editorPath = Get-CanonicalPath -Path $EditorExe -Label "EditorExe"
    $projectPath = Get-CanonicalPath -Path $Project -Label "Project"
    $editorPath = Assert-CloudRegularInputFile `
        -Path $editorPath `
        -Label "Editor executable"
    $projectPath = Assert-CloudRegularInputFile `
        -Path $projectPath `
        -Label "Project"
    if ([System.IO.Path]::GetExtension($projectPath) -ne ".acsproject") {
        throw "Project must have the .acsproject extension: $projectPath"
    }
    $artifactDefinitions =
        Get-CloudEditorArtifactDefinitions -EditorPath $editorPath
    foreach ($definition in $artifactDefinitions) {
        $definition.Path = Assert-CloudRegularInputFile `
            -Path $definition.Path `
            -Label ("Editor " + $definition.Role)
    }
    $leasePaths = @($editorPath, $projectPath) +
        @($artifactDefinitions | ForEach-Object { $_.Path })
    $inputLeases = @(
        Open-CloudInputLeases -Paths $leasePaths
    )
    $runEnvironment = Get-CloudRunEnvironment `
        -EditorPath $editorPath `
        -ProjectPath $projectPath
    $environmentValidation =
        Test-CloudRunEnvironment -EnvironmentSnapshot $runEnvironment

    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $runName =
            "acs-cloud-quality-" +
            [DateTime]::UtcNow.ToString(
                "yyyyMMddTHHmmssfffZ",
                $script:Invariant) +
            "-" + [guid]::NewGuid().ToString("N")
        $OutputDirectory = Join-Path $env:TEMP $runName
    }
    $runDirectory = Get-CanonicalPath `
        -Path $OutputDirectory `
        -Label "OutputDirectory"
    $runDirectory = Assert-SafeOutputDirectory `
        -Path $runDirectory `
        -Create:(-not $DryRun)
    $summaryPath = Join-Path $runDirectory "cloud-quality-summary.json"
    Assert-NewOutputPath -Path $summaryPath

    $definitions = Get-ScenarioDefinitions
    if ($DryRun) {
        Write-Host "Cloud profiler dry run; no files or processes created."
        Show-CloudRunEnvironment -EnvironmentSnapshot $runEnvironment
        foreach ($definition in $definitions) {
            $plan = Invoke-CloudScenario `
                -EditorPath $editorPath `
                -ProjectPath $projectPath `
                -RunDirectory $runDirectory `
                -Scenario $definition
            Write-Host ("[{0}] {1}" -f $plan.Name, $plan.Command)
        }
        Write-Host "Summary: $summaryPath"
        exit 0
    }

    $scenarioResults =
        New-Object "System.Collections.Generic.List[object]"
    foreach ($definition in $definitions) {
        Write-Host ("Running cloud scenario: {0}" -f $definition.Name)
        $result = Invoke-CloudScenario `
            -EditorPath $editorPath `
            -ProjectPath $projectPath `
            -RunDirectory $runDirectory `
            -Scenario $definition `
            -InitialEnvironment $runEnvironment
        [void]$scenarioResults.Add($result)
    }

    $horizon = $scenarioResults |
        Where-Object Name -EQ "horizon" |
        Select-Object -First 1
    $zenith = $scenarioResults |
        Where-Object Name -EQ "zenith" |
        Select-Object -First 1
    $above = $scenarioResults |
        Where-Object Name -EQ "above" |
        Select-Object -First 1
    $qualityComparison = Compare-CloudQuality `
        -Horizon $horizon.Quality `
        -Zenith $zenith.Quality `
        -Above $above.Quality
    $inputStability = Test-CloudInputStability `
        -InitialEnvironment $runEnvironment `
        -EditorPath $editorPath `
        -ProjectPath $projectPath
    $provenancePass =
        $environmentValidation.Pass -and $inputStability.Pass
    $qualityGatePass =
        $horizon.Pass -and
        $zenith.Pass -and
        $above.Pass -and
        $qualityComparison.Pass -and
        $provenancePass
    $qualityFaults =
        New-Object "System.Collections.Generic.List[string]"
    foreach ($fault in @($horizon.FaultCodes)) {
        Add-CloudFault `
            -Faults $qualityFaults `
            -Code ("HORIZON_" + [string]$fault)
    }
    foreach ($fault in @($zenith.FaultCodes)) {
        Add-CloudFault `
            -Faults $qualityFaults `
            -Code ("ZENITH_" + [string]$fault)
    }
    foreach ($fault in @($above.FaultCodes)) {
        Add-CloudFault `
            -Faults $qualityFaults `
            -Code ("ABOVE_" + [string]$fault)
    }
    foreach ($fault in @($qualityComparison.MismatchCodes) +
        @($environmentValidation.FaultCodes) +
        @($inputStability.FaultCodes)) {
        Add-CloudFault -Faults $qualityFaults -Code ([string]$fault)
    }
    $horizonTargetMet = Test-CloudTarget `
        -Measurements $horizon.Measurements `
        -Target $TargetFps
    $zenithTargetMet = Test-CloudTarget `
        -Measurements $zenith.Measurements `
        -Target $TargetFps
    $aboveTargetMet = Test-CloudTarget `
        -Measurements $above.Measurements `
        -Target $TargetFps
    $targetMet =
        $horizonTargetMet -and
        $zenithTargetMet -and
        $aboveTargetMet
    $thresholdFaults =
        New-Object "System.Collections.Generic.List[string]"
    if (-not $horizonTargetMet) {
        Add-CloudFault `
            -Faults $thresholdFaults `
            -Code "HORIZON_TARGET_FPS_NOT_MET"
    }
    if (-not $zenithTargetMet) {
        Add-CloudFault `
            -Faults $thresholdFaults `
            -Code "ZENITH_TARGET_FPS_NOT_MET"
    }
    if (-not $aboveTargetMet) {
        Add-CloudFault `
            -Faults $thresholdFaults `
            -Code "ABOVE_TARGET_FPS_NOT_MET"
    }
    $overallPass =
        $qualityGatePass -and
        (-not $RequireTargetFps -or $targetMet)
    $overallFaults =
        New-Object "System.Collections.Generic.List[string]"
    foreach ($fault in @($qualityFaults.ToArray())) {
        Add-CloudFault -Faults $overallFaults -Code ([string]$fault)
    }
    if ($RequireTargetFps) {
        foreach ($fault in @($thresholdFaults.ToArray())) {
            Add-CloudFault -Faults $overallFaults -Code ([string]$fault)
        }
    }
    $resultText = if ($overallPass) { "PASS" } else { "FAIL" }
    $qualityText = if ($qualityGatePass) { "PASS" } else { "FAIL" }
    $targetText = if ($targetMet) { "MET" } else { "MISS" }

    $summary = [pscustomobject][ordered]@{
        SchemaVersion = $script:SummarySchemaVersion
        Result = $resultText
        FaultCodes = @($overallFaults.ToArray())
        GeneratedUtc =
            [DateTimeOffset]::UtcNow.ToString("O", $script:Invariant)
        EditorExe = $editorPath
        Project = $projectPath
        OutputDirectory = $runDirectory
        SoakSeconds = $SoakSeconds
        Monitor = $Monitor
        MonitorIndex = $MonitorIndex
        TargetFps = $TargetFps
        RequireTargetFps = [bool]$RequireTargetFps
        RunEnvironment = $runEnvironment
        ProvenanceGate = [pscustomobject][ordered]@{
            Result = if ($provenancePass) { "PASS" } else { "FAIL" }
            Pass = $provenancePass
            FaultCodes = @(
                @($environmentValidation.FaultCodes) +
                @($inputStability.FaultCodes)
            )
            InputStability = $inputStability
        }
        QualityGate = [pscustomobject][ordered]@{
            Result = $qualityText
            Pass = $qualityGatePass
            FaultCodes = @($qualityFaults.ToArray())
        }
        TargetGate = [pscustomobject][ordered]@{
            Result = $targetText
            Met = $targetMet
            Required = [bool]$RequireTargetFps
            FaultCodes = @($thresholdFaults.ToArray())
        }
        QualityComparison = $qualityComparison
        Scenarios = @($scenarioResults.ToArray())
    }
    Write-AtomicCloudJson -Value $summary -Path $summaryPath
    Show-CloudRunEnvironment -EnvironmentSnapshot $runEnvironment
    Show-CloudResultTable `
        -Scenarios @($scenarioResults.ToArray()) `
        -Target $TargetFps
    Write-Host ("Quality gate: {0}" -f $qualityText)
    Write-Host (
        "Target gate: {0}{1}" -f
        $targetText,
        $(if ($RequireTargetFps) { " (required)" } else { " (informational)" }))
    Write-Host "Summary: $summaryPath"
    if (-not $overallPass) {
        exit 1
    }
    exit 0
}
catch {
    Write-Error $_ -ErrorAction Continue
    exit 2
}
finally {
    foreach ($lease in @($inputLeases)) {
        if ($null -ne $lease) {
            $lease.Dispose()
        }
    }
}
