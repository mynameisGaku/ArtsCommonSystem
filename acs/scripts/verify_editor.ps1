# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Runs the ACS Editor verification suites through one isolated entry point.

.DESCRIPTION
Every build, log, temporary project, and application profile is redirected to
a unique directory below the current user's temporary directory. The script
does not clean or overwrite repository Binaries/Intermediate/Saved trees and
does not read or update the user's normal Editor settings.

Steps are aggregated rather than fail-fast: an independent suite still runs
after another suite fails. A step whose prerequisite failed is reported as
SKIP, and FAIL or SKIP produces exit code 1.

.PARAMETER Mode
fast    - isolated Editor build plus a short, high-signal managed smoke set.
managed - isolated Editor build, every registered managed self-test, Blueprint
          self-test, and the package CLI self-test.
full    - managed mode plus the cloud profiler harness self-test and an
          isolated native configure/build/CTest run.

.PARAMETER DryRun
Validates the entry point and prints the complete command plan without
creating a temporary directory or launching child processes.

.PARAMETER KeepArtifacts
Keeps the unique temporary verification directory for diagnostics. It is
removed by default before the final result summary is printed.
#>
[CmdletBinding()]
param(
    [ValidateSet("fast", "managed", "full")]
    [string]$Mode = "fast",

    [ValidateSet("Debug", "Release")]
    [string]$NativeConfiguration = "Debug",

    [switch]$DryRun,
    [switch]$KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:VerificationResults =
    New-Object "System.Collections.Generic.List[object]"
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Add-VerificationResult {
    param(
        [Parameter(Mandatory = $true)][string]$Group,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$State,
        [int]$ExitCode,
        [double]$Seconds,
        [string]$Command,
        [string]$Detail
    )

    [void]$script:VerificationResults.Add([pscustomobject]@{
        Group = $Group
        Name = $Name
        State = $State
        ExitCode = $ExitCode
        Seconds = $Seconds
        Command = $Command
        Detail = $Detail
    })
}

function ConvertTo-DisplayArgument {
    param([AllowEmptyString()][string]$Value)

    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value.Contains('"')) {
        throw "Verification arguments must not contain a quote character: $Value"
    }
    if ($Value -match "\s") {
        return '"' + $Value + '"'
    }
    return $Value
}

function Join-ProcessArguments {
    param([string[]]$Arguments)

    return (($Arguments | ForEach-Object {
        ConvertTo-DisplayArgument -Value $_
    }) -join " ")
}

function Get-OutputTail {
    param(
        [AllowEmptyString()][string]$Text,
        [int]$MaximumCharacters = 6000
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return ""
    }
    $trimmed = $Text.Trim()
    if ($trimmed.Length -le $MaximumCharacters) {
        return $trimmed
    }
    return "[output truncated to final $MaximumCharacters characters]`n" +
        $trimmed.Substring($trimmed.Length - $MaximumCharacters)
}

function Resolve-VerificationTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Group
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        Add-VerificationResult `
            -Group $Group `
            -Name "tool: $Name" `
            -State "FAIL" `
            -ExitCode 127 `
            -Seconds 0 `
            -Command $Name `
            -Detail "Required command was not found on PATH."
        Write-Host "[FAIL] required command not found: $Name" -ForegroundColor Red
        return $null
    }

    $path = $command.Source
    if ([string]::IsNullOrWhiteSpace($path)) {
        $path = $command.Name
    }
    Add-VerificationResult `
        -Group $Group `
        -Name "tool: $Name" `
        -State "PASS" `
        -ExitCode 0 `
        -Seconds 0 `
        -Command $path `
        -Detail ""
    return $path
}

function Stop-VerificationProcessTree {
    param([System.Diagnostics.Process]$Process)

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

    $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
    if (Test-Path -LiteralPath $taskkill) {
        try {
            & $taskkill /PID ([string]$Process.Id) /T /F 2>$null |
                Out-Null
            if ($LASTEXITCODE -eq 0) {
                return
            }
        }
        catch {
            # Fall through to the direct-process fallback.
        }
    }

    try {
        $Process.Kill()
    }
    catch {
        # Timeout is already reported by the caller.
    }
}

function Read-VerificationProcessOutput {
    param(
        [AllowNull()][object]$Task,
        [Parameter(Mandatory = $true)][string]$StreamName,
        [int]$TimeoutMilliseconds = 5000
    )

    if ($null -eq $Task) {
        return [pscustomobject]@{
            Text = ""
            Complete = $true
        }
    }
    try {
        if ($Task.Wait($TimeoutMilliseconds)) {
            return [pscustomobject]@{
                Text = [string]$Task.GetAwaiter().GetResult()
                Complete = $true
            }
        }
        return [pscustomobject]@{
            Text = "[$StreamName capture did not finish after process termination.]"
            Complete = $false
        }
    }
    catch {
        return [pscustomobject]@{
            Text = "[$StreamName capture failed: $($_.Exception.Message)]"
            Complete = $false
        }
    }
}

function Invoke-VerificationStep {
    param(
        [Parameter(Mandatory = $true)][string]$Group,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, string]]$Environment,
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [int]$TimeoutSeconds = 300
    )

    $argumentText = Join-ProcessArguments -Arguments $Arguments
    $displayCommand = ConvertTo-DisplayArgument -Value $FilePath
    if (-not [string]::IsNullOrWhiteSpace($argumentText)) {
        $displayCommand += " " + $argumentText
    }

    if ($DryRun) {
        Write-Host "[PLAN][$Group] $Name"
        Write-Host "  $displayCommand"
        Add-VerificationResult `
            -Group $Group `
            -Name $Name `
            -State "PLAN" `
            -ExitCode 0 `
            -Seconds 0 `
            -Command $displayCommand `
            -Detail ""
        return $true
    }

    Write-Host "[RUN ][$Group] $Name"
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = $null
    $standardOutput = ""
    $standardError = ""
    $exitCode = 125
    $detail = ""
    $timedOut = $false
    $outputTask = $null
    $errorTask = $null

    try {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $FilePath
        $startInfo.Arguments = $argumentText
        $startInfo.WorkingDirectory = $WorkingDirectory
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true

        foreach ($entry in $Environment.GetEnumerator()) {
            $startInfo.EnvironmentVariables[$entry.Key] = $entry.Value
        }

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Process.Start returned false."
        }

        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            Stop-VerificationProcessTree -Process $process
            [void]$process.WaitForExit(10000)
        }

        $outputCapture = Read-VerificationProcessOutput `
            -Task $outputTask `
            -StreamName "stdout"
        $errorCapture = Read-VerificationProcessOutput `
            -Task $errorTask `
            -StreamName "stderr"
        $standardOutput = $outputCapture.Text
        $standardError = $errorCapture.Text
        if ($timedOut) {
            $exitCode = 124
            $detail = "Timed out after $TimeoutSeconds seconds."
        }
        else {
            $exitCode = $process.ExitCode
            if (-not $outputCapture.Complete -or -not $errorCapture.Complete) {
                $exitCode = 125
                $detail =
                    "The process exited but redirected output did not drain completely."
            }
        }
    }
    catch {
        $detail = $_.Exception.Message
        if ($null -ne $process) {
            Stop-VerificationProcessTree -Process $process
        }
    }
    finally {
        $stopwatch.Stop()
        if ($null -ne $process) {
            $process.Dispose()
        }
    }

    $safeLogName = ($Group + "-" + $Name) -replace "[^A-Za-z0-9_.-]", "_"
    $combinedOutput = @(
        "COMMAND: $displayCommand"
        "EXIT: $exitCode"
        "DETAIL: $detail"
        ""
        "STDOUT:"
        $standardOutput
        ""
        "STDERR:"
        $standardError
    ) -join [Environment]::NewLine
    try {
        [System.IO.File]::WriteAllText(
            (Join-Path $LogDirectory ($safeLogName + ".log")),
            $combinedOutput,
            $script:Utf8NoBom)
    }
    catch {
        $logFailure = "Could not write the isolated verification log: " +
            $_.Exception.Message
        $detail = if ([string]::IsNullOrWhiteSpace($detail)) {
            $logFailure
        }
        else {
            $detail + " " + $logFailure
        }
        if ($exitCode -eq 0) {
            $exitCode = 125
        }
    }

    $passed = $exitCode -eq 0
    $state = if ($passed) { "PASS" } else { "FAIL" }
    Add-VerificationResult `
        -Group $Group `
        -Name $Name `
        -State $state `
        -ExitCode $exitCode `
        -Seconds $stopwatch.Elapsed.TotalSeconds `
        -Command $displayCommand `
        -Detail $detail

    if ($passed) {
        Write-Host (
            "[PASS][$Group] $Name ({0:N1}s)" -f
            $stopwatch.Elapsed.TotalSeconds) -ForegroundColor Green
    }
    else {
        Write-Host (
            "[FAIL][$Group] $Name (exit $exitCode, {0:N1}s)" -f
            $stopwatch.Elapsed.TotalSeconds) -ForegroundColor Red
        if (-not [string]::IsNullOrWhiteSpace($detail)) {
            Write-Host "  $detail" -ForegroundColor Red
        }
        $tail = Get-OutputTail -Text (
            $standardOutput + [Environment]::NewLine + $standardError)
        if (-not [string]::IsNullOrWhiteSpace($tail)) {
            Write-Host $tail
        }
    }
    return $passed
}

function Add-SkippedVerificationStep {
    param(
        [Parameter(Mandatory = $true)][string]$Group,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    Write-Host "[SKIP][$Group] $Name - $Reason" -ForegroundColor Yellow
    Add-VerificationResult `
        -Group $Group `
        -Name $Name `
        -State "SKIP" `
        -ExitCode 1 `
        -Seconds 0 `
        -Command "" `
        -Detail $Reason
}

function Copy-VerificationSourceDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $excludedDirectories = @(
        ".git",
        ".vs",
        "bin",
        "obj",
        "TestResults"
    )
    [void](New-Item -ItemType Directory -Path $Destination -Force)

    $pending =
        New-Object "System.Collections.Generic.Stack[System.IO.DirectoryInfo]"
    $pending.Push((Get-Item -LiteralPath $Source))
    $sourcePrefix = [System.IO.Path]::GetFullPath($Source).
        TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar

    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory.FullName -Force) {
            if (($entry.Attributes -band
                 [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Verification source contains a reparse point: $($entry.FullName)"
            }

            if ($entry.PSIsContainer) {
                if ($excludedDirectories -contains $entry.Name) {
                    continue
                }
                $pending.Push($entry)
                continue
            }

            $relative = $entry.FullName.Substring($sourcePrefix.Length)
            $target = Join-Path $Destination $relative
            $targetParent = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $targetParent)) {
                [void](New-Item -ItemType Directory -Path $targetParent -Force)
            }
            [System.IO.File]::Copy($entry.FullName, $target, $false)
        }
    }
}

function Initialize-VerificationSourceSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$EditorSource,
        [Parameter(Mandatory = $true)][string]$PackageSource,
        [Parameter(Mandatory = $true)][string]$SnapshotRoot
    )

    if ($DryRun) {
        Write-Host "[PLAN][preflight] snapshot managed source"
        Add-VerificationResult `
            -Group "preflight" `
            -Name "snapshot managed source" `
            -State "PLAN" `
            -ExitCode 0 `
            -Seconds 0 `
            -Command "copy ordinary source files to $SnapshotRoot" `
            -Detail ""
        return $true
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        Copy-VerificationSourceDirectory `
            -Source $EditorSource `
            -Destination (Join-Path $SnapshotRoot "editor\AcsEditor")
        Copy-VerificationSourceDirectory `
            -Source $PackageSource `
            -Destination (Join-Path $SnapshotRoot "tools\acspackage")
        $stopwatch.Stop()
        Add-VerificationResult `
            -Group "preflight" `
            -Name "snapshot managed source" `
            -State "PASS" `
            -ExitCode 0 `
            -Seconds $stopwatch.Elapsed.TotalSeconds `
            -Command "isolated source snapshot" `
            -Detail ""
        Write-Host (
            "[PASS][preflight] snapshot managed source ({0:N1}s)" -f
            $stopwatch.Elapsed.TotalSeconds) -ForegroundColor Green
        return $true
    }
    catch {
        $stopwatch.Stop()
        Add-VerificationResult `
            -Group "preflight" `
            -Name "snapshot managed source" `
            -State "FAIL" `
            -ExitCode 1 `
            -Seconds $stopwatch.Elapsed.TotalSeconds `
            -Command "isolated source snapshot" `
            -Detail $_.Exception.Message
        Write-Host (
            "[FAIL][preflight] snapshot managed source: " +
            $_.Exception.Message) -ForegroundColor Red
        return $false
    }
}

function Assert-IsolatedSessionPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TemporaryRoot
    )

    $candidate = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($TemporaryRoot).
        TrimEnd([char[]]@(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar))
    $parent = [System.IO.Path]::GetDirectoryName($candidate)
    $leaf = [System.IO.Path]::GetFileName($candidate)
    if (-not $parent.Equals(
            $root,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $leaf -notmatch "^ae-[0-9a-f]{16}$") {
        throw "Refusing to use a non-isolated verification path: $candidate"
    }
}

function Remove-IsolatedTreeEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$SessionRoot
    )

    $candidate = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($SessionRoot)
    $boundary = $root.TrimEnd([char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $candidate.Equals(
            $root,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $candidate.StartsWith(
            $boundary,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean an entry outside the verification session: $candidate"
    }

    $entry = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
    $attributes = $entry.Attributes
    if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        if (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0) {
            [System.IO.Directory]::Delete($candidate, $false)
        }
        else {
            [System.IO.File]::Delete($candidate)
        }
        return
    }

    if ($entry.PSIsContainer) {
        foreach ($child in Get-ChildItem `
                -LiteralPath $candidate `
                -Force `
                -ErrorAction Stop) {
            Remove-IsolatedTreeEntry `
                -Path $child.FullName `
                -SessionRoot $root
        }
        [System.IO.Directory]::Delete($candidate, $false)
        return
    }

    if (($attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0) {
        [System.IO.File]::SetAttributes(
            $candidate,
            ($attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)))
    }
    [System.IO.File]::Delete($candidate)
}

function Remove-IsolatedVerificationDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TemporaryRoot
    )

    try {
        Assert-IsolatedSessionPath `
            -Path $Path `
            -TemporaryRoot $TemporaryRoot
        if (-not (Test-Path -LiteralPath $Path)) {
            return $true
        }

        $rootEntry = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
        if (($rootEntry.Attributes -band
             [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recursively clean a reparse-point session root: $Path"
        }
        Remove-IsolatedTreeEntry -Path $Path -SessionRoot $Path
        return $true
    }
    catch {
        Write-Warning (
            "Could not safely remove isolated verification directory: " +
            $_.Exception.Message)
        return $false
    }
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$acsRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$editorProject = Join-Path $acsRoot "editor\AcsEditor\AcsEditor.csproj"
$packageProject = Join-Path $acsRoot "tools\acspackage\AcsPackage.csproj"
$editorSourceDirectory = Split-Path -Parent $editorProject
$packageSourceDirectory = Split-Path -Parent $packageProject
$nativeSource = Join-Path $acsRoot "engine"
$cloudProfilerScript =
    Join-Path $scriptDirectory "profile_cloud_quality.ps1"

$requiredPaths = @(
    $editorProject,
    $packageProject,
    (Join-Path $nativeSource "CMakeLists.txt")
)
if ($Mode -eq "full") {
    $requiredPaths += $cloudProfilerScript
}
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        Write-Error "Required verification input is missing: $requiredPath"
        exit 2
    }
}

$temporaryRoot = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath())
$sessionRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot (
        # Keep this root short. Package/asset self-tests add their own GUID
        # directories, and native Win32 tools still enforce legacy path limits.
        "ae-" + [Guid]::NewGuid().ToString("N").Substring(16, 16))))
Assert-IsolatedSessionPath `
    -Path $sessionRoot `
    -TemporaryRoot $temporaryRoot

$logs = Join-Path $sessionRoot "logs"
$sourceSnapshot = Join-Path $sessionRoot "source"
$snapshotEditorProject =
    Join-Path $sourceSnapshot "editor\AcsEditor\AcsEditor.csproj"
$snapshotPackageProject =
    Join-Path $sourceSnapshot "tools\acspackage\AcsPackage.csproj"
$editorOutput = Join-Path $sessionRoot "managed\editor-out"
$packageOutput = Join-Path $sessionRoot "managed\package-out"
$nativeBuild = Join-Path $sessionRoot "native\build"
$nativeLayout = Join-Path $sessionRoot "native\layout"
$isolatedProfile = Join-Path $sessionRoot "profile"
$isolatedTemp = Join-Path $sessionRoot "tmp"
$script:SessionCleanupArmed = -not $DryRun -and -not $KeepArtifacts

trap {
    $fatalMessage = $_.Exception.Message
    if ($script:SessionCleanupArmed) {
        [void](Remove-IsolatedVerificationDirectory `
            -Path $sessionRoot `
            -TemporaryRoot $temporaryRoot)
        $script:SessionCleanupArmed = $false
    }
    Write-Error `
        -Message ("ACS Editor verification aborted: " + $fatalMessage) `
        -ErrorAction Continue
    exit 2
}

if (-not $DryRun) {
    if (Test-Path -LiteralPath $sessionRoot) {
        throw "Refusing to reuse an existing verification directory: $sessionRoot"
    }
    [void][System.IO.Directory]::CreateDirectory($sessionRoot)
    $sessionAttributes = [System.IO.File]::GetAttributes($sessionRoot)
    if (($sessionAttributes -band
         [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Verification session root must be an ordinary directory: $sessionRoot"
    }
    foreach ($directory in @(
            $logs,
            $editorOutput,
            $packageOutput,
            $nativeLayout,
            (Join-Path $isolatedProfile "AppData\Roaming"),
            (Join-Path $isolatedProfile "AppData\Local"),
            $isolatedTemp,
            (Join-Path $sessionRoot "nuget"))) {
        [void](New-Item `
            -ItemType Directory `
            -Path $directory `
            -Force)
    }
}

$processEnvironment =
    New-Object "System.Collections.Generic.Dictionary[string,string]" (
        [System.StringComparer]::OrdinalIgnoreCase)
$processEnvironment["APPDATA"] =
    Join-Path $isolatedProfile "AppData\Roaming"
$processEnvironment["LOCALAPPDATA"] =
    Join-Path $isolatedProfile "AppData\Local"
$processEnvironment["TEMP"] = $isolatedTemp
$processEnvironment["TMP"] = $isolatedTemp
$processEnvironment["DOTNET_SKIP_FIRST_TIME_EXPERIENCE"] = "1"
$processEnvironment["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
$processEnvironment["DOTNET_NOLOGO"] = "1"
$processEnvironment["DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE"] = "1"
$processEnvironment["MSBUILDDISABLENODEREUSE"] = "1"
$processEnvironment["NUGET_PACKAGES"] = Join-Path $sessionRoot "nuget"

$sourceSnapshotReady = Initialize-VerificationSourceSnapshot `
    -EditorSource $editorSourceDirectory `
    -PackageSource $packageSourceDirectory `
    -SnapshotRoot $sourceSnapshot

$dotnet = Resolve-VerificationTool -Name "dotnet" -Group "preflight"
$cmake = $null
$ctest = $null
$powerShell = $null
if ($Mode -eq "full") {
    $cmake = Resolve-VerificationTool -Name "cmake" -Group "preflight"
    $ctest = Resolve-VerificationTool -Name "ctest" -Group "preflight"
    $powerShell =
        Resolve-VerificationTool -Name "powershell.exe" -Group "preflight"
}

$fastEditorTests = @(
    "--abi-contract-selftest",
    "--document-host-selftest",
    "--material-preview-selftest",
    "--asset-browser-selftest",
    "--camera-authoring-selftest",
    "--profiler-selftest",
    "--operation-diagnostics-selftest",
    "--project-launcher-responsiveness-selftest",
    "--package-responsiveness-selftest",
    "--package-metadata-editor-selftest"
)
$allEditorTests = @(
    "--abi-contract-selftest",
    "--autosave-selftest",
    "--scene-save-selftest",
    "--document-host-selftest",
    "--project-settings-selftest",
    "--scene-editor-migration-selftest",
    "--material-workflow-selftest",
    "--material-preview-selftest",
    "--asset-creation-selftest",
    "--asset-import-selftest",
    "--asset-browser-selftest",
    "--asset-package-readiness-selftest",
    "--thumbnail-ddc-selftest",
    "--workspace-selftest",
    "--camera-authoring-selftest",
    "--profiler-selftest",
    "--operation-diagnostics-selftest",
    "--project-launcher-responsiveness-selftest",
    "--package-responsiveness-selftest",
    "--package-metadata-editor-selftest",
    "--editor-reliability-selftest",
    "--bptest"
)
$editorTests = if ($Mode -eq "fast") {
    $fastEditorTests
}
else {
    $allEditorTests
}

$editorReady = $false
if ($null -ne $dotnet -and $sourceSnapshotReady) {
    $editorBuildArguments = @(
        "build",
        $snapshotEditorProject,
        "--configuration", "Release",
        "--runtime", "win-x64",
        "--output", $editorOutput,
        "-p:UseSharedCompilation=false",
        "--nologo"
    )
    $editorReady = Invoke-VerificationStep `
        -Group "managed" `
        -Name "build Editor" `
        -FilePath $dotnet `
        -Arguments $editorBuildArguments `
        -WorkingDirectory $acsRoot `
        -Environment $processEnvironment `
        -LogDirectory $logs `
        -TimeoutSeconds 600
}
else {
    $editorBuildReason = if ($null -eq $dotnet) {
        "dotnet is unavailable."
    }
    else {
        "The isolated source snapshot is unavailable."
    }
    Add-SkippedVerificationStep `
        -Group "managed" `
        -Name "build Editor" `
        -Reason $editorBuildReason
}

$editorExecutable = Join-Path $editorOutput "AcsEditor.exe"
if ($editorReady -and -not $DryRun -and
    -not (Test-Path -LiteralPath $editorExecutable -PathType Leaf)) {
    Add-VerificationResult `
        -Group "managed" `
        -Name "locate Editor executable" `
        -State "FAIL" `
        -ExitCode 1 `
        -Seconds 0 `
        -Command $editorExecutable `
        -Detail "The isolated build succeeded but did not publish AcsEditor.exe."
    Write-Host (
        "[FAIL][managed] isolated build did not publish $editorExecutable") `
        -ForegroundColor Red
    $editorReady = $false
}

foreach ($selfTest in $editorTests) {
    $name = $selfTest.TrimStart("-")
    if ($editorReady) {
        [void](Invoke-VerificationStep `
            -Group "managed" `
            -Name $name `
            -FilePath $editorExecutable `
            -Arguments @($selfTest) `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 300)
    }
    else {
        Add-SkippedVerificationStep `
            -Group "managed" `
            -Name $name `
            -Reason "The isolated Editor build is unavailable."
    }
}

if ($Mode -ne "fast") {
    $packageReady = $false
    if ($null -ne $dotnet -and $sourceSnapshotReady) {
        $packageBuildArguments = @(
            "build",
            $snapshotPackageProject,
            "--configuration", "Release",
            "--output", $packageOutput,
            "-p:UseSharedCompilation=false",
            "--nologo"
        )
        $packageReady = Invoke-VerificationStep `
            -Group "package" `
            -Name "build package CLI" `
            -FilePath $dotnet `
            -Arguments $packageBuildArguments `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 600
    }
    else {
        $packageBuildReason = if ($null -eq $dotnet) {
            "dotnet is unavailable."
        }
        else {
            "The isolated source snapshot is unavailable."
        }
        Add-SkippedVerificationStep `
            -Group "package" `
            -Name "build package CLI" `
            -Reason $packageBuildReason
    }

    $packageAssembly = Join-Path $packageOutput "acspackage.dll"
    if ($packageReady -and -not $DryRun -and
        -not (Test-Path -LiteralPath $packageAssembly -PathType Leaf)) {
        Add-VerificationResult `
            -Group "package" `
            -Name "locate package CLI" `
            -State "FAIL" `
            -ExitCode 1 `
            -Seconds 0 `
            -Command $packageAssembly `
            -Detail "The isolated build succeeded but did not publish acspackage.dll."
        Write-Host (
            "[FAIL][package] isolated build did not publish $packageAssembly") `
            -ForegroundColor Red
        $packageReady = $false
    }

    if ($packageReady) {
        [void](Invoke-VerificationStep `
            -Group "package" `
            -Name "deterministic package smoke" `
            -FilePath $dotnet `
            -Arguments @($packageAssembly, "--self-test") `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 600)

        if ($Mode -eq "full") {
            $distributionArtifacts =
                Join-Path $isolatedTemp "distribution-e2e"
            [void](Invoke-VerificationStep `
                -Group "package" `
                -Name "distribution E2E" `
                -FilePath $dotnet `
                -Arguments @(
                    $packageAssembly,
                    "distribution-e2e",
                    "--artifacts",
                    $distributionArtifacts
                ) `
                -WorkingDirectory $acsRoot `
                -Environment $processEnvironment `
                -LogDirectory $logs `
                -TimeoutSeconds 600)
        }
    }
    else {
        Add-SkippedVerificationStep `
            -Group "package" `
            -Name "deterministic package smoke" `
            -Reason "The isolated package CLI build is unavailable."
        if ($Mode -eq "full") {
            Add-SkippedVerificationStep `
                -Group "package" `
                -Name "distribution E2E" `
                -Reason "The isolated package CLI build is unavailable."
        }
    }
}

if ($Mode -eq "full") {
    if ($null -ne $powerShell) {
        [void](Invoke-VerificationStep `
            -Group "rendering" `
            -Name "cloud profiler harness self-test" `
            -FilePath $powerShell `
            -Arguments @(
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy", "Bypass",
                "-File", $cloudProfilerScript,
                "-SelfTest"
            ) `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 120)
    }
    else {
        Add-SkippedVerificationStep `
            -Group "rendering" `
            -Name "cloud profiler harness self-test" `
            -Reason "powershell.exe is unavailable."
    }

    $nativeConfigured = $false
    if ($null -ne $cmake) {
        $nativeConfigureArguments = @(
            "-S", $nativeSource,
            "-B", $nativeBuild,
            "-DACS_LAYOUT_ROOT=$nativeLayout",
            "-DACS_BUILD_TESTS=ON",
            "-DACS_BUILD_SAMPLES=OFF",
            "-DACS_BUILD_TOOLS=OFF"
        )
        $nativeConfigured = Invoke-VerificationStep `
            -Group "native" `
            -Name "configure native tests" `
            -FilePath $cmake `
            -Arguments $nativeConfigureArguments `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 900
    }
    else {
        Add-SkippedVerificationStep `
            -Group "native" `
            -Name "configure native tests" `
            -Reason "cmake is unavailable."
    }

    $nativeBuilt = $false
    if ($nativeConfigured) {
        $nativeBuilt = Invoke-VerificationStep `
            -Group "native" `
            -Name "build native tests" `
            -FilePath $cmake `
            -Arguments @(
                "--build", $nativeBuild,
                "--config", $NativeConfiguration,
                "--parallel"
            ) `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 2400
    }
    else {
        Add-SkippedVerificationStep `
            -Group "native" `
            -Name "build native tests" `
            -Reason "Native configure did not succeed."
    }

    if ($nativeBuilt -and $null -ne $ctest) {
        [void](Invoke-VerificationStep `
            -Group "native" `
            -Name "CTest" `
            -FilePath $ctest `
            -Arguments @(
                "--test-dir", $nativeBuild,
                "-C", $NativeConfiguration,
                "--output-on-failure",
                "--no-tests=error"
            ) `
            -WorkingDirectory $acsRoot `
            -Environment $processEnvironment `
            -LogDirectory $logs `
            -TimeoutSeconds 1800)
    }
    else {
        $nativeTestReason = if ($null -eq $ctest) {
            "ctest is unavailable."
        }
        else {
            "Native build did not succeed."
        }
        Add-SkippedVerificationStep `
            -Group "native" `
            -Name "CTest" `
            -Reason $nativeTestReason
    }
}

if ($KeepArtifacts -and -not $DryRun) {
    Write-Host "Verification artifacts: $sessionRoot"
}
elseif (-not $DryRun) {
    $cleanupPassed = Remove-IsolatedVerificationDirectory `
        -Path $sessionRoot `
        -TemporaryRoot $temporaryRoot
    $script:SessionCleanupArmed = $false
    Add-VerificationResult `
        -Group "cleanup" `
        -Name "remove isolated artifacts" `
        -State $(if ($cleanupPassed) { "PASS" } else { "FAIL" }) `
        -ExitCode $(if ($cleanupPassed) { 0 } else { 1 }) `
        -Seconds 0 `
        -Command $sessionRoot `
        -Detail $(if ($cleanupPassed) {
            ""
        }
        else {
            "The isolated verification directory could not be removed safely."
        })
}

Write-Host ""
Write-Host "ACS Editor verification summary ($Mode)"
$summary = $script:VerificationResults |
    Select-Object Group, State, Name, ExitCode,
        @{Name = "Seconds"; Expression = { "{0:N1}" -f $_.Seconds }}
Write-Host ($summary | Format-Table -AutoSize | Out-String)

$failedCount = @(
    $script:VerificationResults |
        Where-Object { $_.State -eq "FAIL" }
).Count
$skippedCount = @(
    $script:VerificationResults |
        Where-Object { $_.State -eq "SKIP" }
).Count

if ($failedCount -gt 0 -or $skippedCount -gt 0) {
    Write-Host (
        "ACS Editor verification failed: $failedCount failure(s), " +
        "$skippedCount skipped step(s).") -ForegroundColor Red
    exit 1
}

if ($DryRun) {
    Write-Host "Dry-run complete; no child process or artifact was created." `
        -ForegroundColor Green
}
else {
    Write-Host "ACS Editor verification passed." -ForegroundColor Green
}
exit 0
