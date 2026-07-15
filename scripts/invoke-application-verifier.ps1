param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,
    [string[]]$Layers = @('Networking'),
    [string[]]$ExecutableArguments = @(),
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 120,
    [string]$ReportDirectory,
    [switch]$RequireAvailable
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (!$ReportDirectory) {
    # 既定の証跡を実行単位で分離し、並列実行や再実行で上書きしない。
    $ReportRoot = Join-Path $repositoryRoot 'acs\Saved\Diagnostics\ApplicationVerifier'
    $Timestamp = [DateTime]::UtcNow.ToString(
        'yyyyMMddTHHmmssfffZ',
        [Globalization.CultureInfo]::InvariantCulture)
    $TargetSlug = [System.IO.Path]::GetFileNameWithoutExtension($ExecutablePath) -replace '[^A-Za-z0-9._-]', '_'
    $LayerSlug = (($Layers | ForEach-Object { $_ -replace '[^A-Za-z0-9._-]', '_' }) -join '+')
    $ReportDirectory = Join-Path $ReportRoot "$Timestamp-$TargetSlug-$LayerSlug-$PID"
}
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$script:SummaryLogPath = Join-Path $ReportDirectory 'summary.log'
$script:Utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText(
    $script:SummaryLogPath,
    "memory_diagnostic_method=application_verifier summary_version=1`r`n",
    $script:Utf8WithoutBom)

function Write-DiagnosticRecord
{
    param([string]$Record)

    Write-Host $Record
    [System.IO.File]::AppendAllText($script:SummaryLogPath, "$Record`r`n", $script:Utf8WithoutBom)
}

function Invoke-AppVerifierCommand
{
    param(
        [string]$ApplicationVerifierPath,
        [string[]]$CommandArguments,
        [string]$Phase
    )

    $process = Start-Process -FilePath $ApplicationVerifierPath -ArgumentList $CommandArguments `
        -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "Application Verifier phase '$Phase' failed with exit code $($process.ExitCode)"
    }
}

function Test-IsAdministrator
{
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

$resolvedExecutable = Resolve-Path -LiteralPath $ExecutablePath -ErrorAction Stop
if (!(Test-Path -LiteralPath $resolvedExecutable.Path -PathType Leaf)) {
    throw "Application Verifier target is not a file: $ExecutablePath"
}
if ($Layers.Count -eq 0) {
    throw 'At least one Application Verifier layer is required'
}

$applicationVerifierPath = Join-Path $env:SystemRoot 'System32\appverif.exe'
$targetName = [System.IO.Path]::GetFileName($resolvedExecutable.Path)
$machineExecutablePath = $resolvedExecutable.Path.Replace('\', '/').Replace(' ', '%20')
$layerList = $Layers -join ','
Write-DiagnosticRecord "memory_diagnostic_method=application_verifier target=$targetName executable_path=$machineExecutablePath layers=$layerList"
$MachineReportDirectory = [System.IO.Path]::GetFullPath($ReportDirectory).Replace('\', '/').Replace(' ', '%20')
Write-DiagnosticRecord "memory_diagnostic_method=application_verifier report_directory=$MachineReportDirectory"

if (!(Test-Path -LiteralPath $applicationVerifierPath -PathType Leaf)) {
    Write-DiagnosticRecord 'memory_diagnostic_method=application_verifier capability=unavailable reason=tool_not_installed status=inconclusive'
    if ($RequireAvailable) { throw 'Application Verifier is not installed' }
    exit 0
}
if (!(Test-IsAdministrator)) {
    Write-DiagnosticRecord 'memory_diagnostic_method=application_verifier capability=unavailable reason=administrator_required status=inconclusive'
    if ($RequireAvailable) { throw 'Application Verifier requires an elevated administrator session' }
    exit 0
}

$stdoutLogPath = Join-Path $ReportDirectory 'run.stdout.log'
$stderrLogPath = Join-Path $ReportDirectory 'run.stderr.log'
$xmlLogPath = Join-Path $ReportDirectory 'application-verifier.xml'
$PreviousVerifierLogPath = $env:VERIFIER_LOG_PATH
$bSettingsEnabled = $false
$RunExitCode = -1
$bTimedOut = $false
$bDiagnosticFailed = $false
$bSettingsCleanupFailed = $false
$ExportExitCode = -1
$ExportStatus = 'failed'
$ExportReason = 'not_attempted'
$StopCount = 0

try {
    $env:VERIFIER_LOG_PATH = $ReportDirectory
    Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
        -CommandArguments @('-delete', 'logs', '-for', $targetName) -Phase 'delete_previous_logs'
    Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
        -CommandArguments (@('-enable') + $Layers + @('-for', $targetName)) -Phase 'enable_layers'
    $bSettingsEnabled = $true
    Write-DiagnosticRecord 'memory_diagnostic_method=application_verifier phase=run status=running'

    $RunProcessArguments = @{
        FilePath = $resolvedExecutable.Path
        RedirectStandardOutput = $stdoutLogPath
        RedirectStandardError = $stderrLogPath
        PassThru = $true
        WindowStyle = 'Hidden'
    }
    if ($ExecutableArguments.Count -gt 0) {
        $RunProcessArguments.ArgumentList = $ExecutableArguments
    }
    $RunProcess = Start-Process @RunProcessArguments
    # Windows PowerShell 5.1 は終了前にハンドルを取得しないと ExitCode を空値として返す。
    [void]$RunProcess.Handle
    if (!$RunProcess.WaitForExit($TimeoutSeconds * 1000)) {
        $bTimedOut = $true
        $RunProcess.Kill()
    }
    # リダイレクト済みストリームと終了コードが確定するまで待機する。
    $RunProcess.WaitForExit()
    $RunProcess.Refresh()
    $RunExitCode = $RunProcess.ExitCode

    if (Test-Path -LiteralPath $xmlLogPath) {
        Remove-Item -LiteralPath $xmlLogPath -Force
    }
    $exportProcess = Start-Process -FilePath $applicationVerifierPath `
        -ArgumentList @('-export', 'log', '-for', $targetName, '-with', "To=$xmlLogPath") `
        -Wait -PassThru -WindowStyle Hidden
    $ExportExitCode = $exportProcess.ExitCode

    if ($ExportExitCode -ne 0) {
        $ExportReason = 'export_process_failed'
    }
    elseif (!(Test-Path -LiteralPath $xmlLogPath -PathType Leaf)) {
        $ExportReason = 'xml_missing'
    }
    else {
        try {
            [xml]$verifierDocument = Get-Content -LiteralPath $xmlLogPath -Raw -Encoding UTF8
            $stopNodes = $verifierDocument.SelectNodes('//*[@StopCode or @StopID or @StopId]')
            $errorAttributeNodes = $verifierDocument.SelectNodes("//*[@Severity='Error' or @Severity='ERROR']")
            $errorElementNodes = $verifierDocument.SelectNodes("//*[local-name()='Severity' and (text()='Error' or text()='ERROR')]")
            $StopCount = $stopNodes.Count + $errorAttributeNodes.Count + $errorElementNodes.Count
            $ExportStatus = 'ok'
            $ExportReason = 'none'
        }
        catch {
            $ExportReason = 'xml_parse_failed'
        }
    }

    $bMemoryErrorDetected = $StopCount -gt 0 -or $RunExitCode -ne 0 -or $bTimedOut
    $bExportFailed = $ExportStatus -ne 'ok'
    $bDiagnosticFailed = $bMemoryErrorDetected -or $bExportFailed
    Write-DiagnosticRecord "memory_diagnostic_method=application_verifier phase=run exit_code=$RunExitCode timed_out=$($bTimedOut.ToString().ToLowerInvariant()) status=$(if ($bDiagnosticFailed) { 'failed' } else { 'ok' })"
    Write-DiagnosticRecord "memory_diagnostic_method=application_verifier log_export=$ExportStatus export_exit_code=$ExportExitCode xml_present=$((Test-Path -LiteralPath $xmlLogPath -PathType Leaf).ToString().ToLowerInvariant()) verifier_stop_count=$StopCount reason=$ExportReason status=$(if ($bExportFailed) { 'inconclusive' } else { 'ok' })"
    if ($bExportFailed) {
        Write-DiagnosticRecord 'memory_diagnostic_method=application_verifier memory_error_detected=inconclusive status=failed reason=log_export_failed'
    }
    else {
        Write-DiagnosticRecord "memory_diagnostic_method=application_verifier memory_error_detected=$($bMemoryErrorDetected.ToString().ToLowerInvariant()) status=$(if ($bMemoryErrorDetected) { 'failed' } else { 'ok' })"
    }
}
finally {
    if ($bSettingsEnabled) {
        try {
            Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
                -CommandArguments @('-delete', 'settings', '-for', $targetName) -Phase 'delete_settings'
        }
        catch {
            $bSettingsCleanupFailed = $true
            Write-DiagnosticRecord "memory_diagnostic_method=application_verifier phase=delete_settings status=failed message=$($_.Exception.Message.Replace(' ', '_'))"
        }
    }
    $env:VERIFIER_LOG_PATH = $PreviousVerifierLogPath
}

if ($bDiagnosticFailed -or $bSettingsCleanupFailed) { exit 1 }
