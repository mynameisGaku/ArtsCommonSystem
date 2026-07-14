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
    $ReportDirectory = Join-Path $repositoryRoot 'acs\Saved\Diagnostics\ApplicationVerifier'
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
$previousVerifierLogPath = $env:VERIFIER_LOG_PATH
$settingsEnabled = $false
$runExitCode = -1
$timedOut = $false
$diagnosticFailed = $false
$settingsCleanupFailed = $false

try {
    $env:VERIFIER_LOG_PATH = $ReportDirectory
    Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
        -CommandArguments @('-delete', 'logs', '-for', $targetName) -Phase 'delete_previous_logs'
    Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
        -CommandArguments (@('-enable') + $Layers + @('-for', $targetName)) -Phase 'enable_layers'
    $settingsEnabled = $true
    Write-DiagnosticRecord 'memory_diagnostic_method=application_verifier phase=run status=running'

    $runProcess = Start-Process -FilePath $resolvedExecutable.Path -ArgumentList $ExecutableArguments `
        -RedirectStandardOutput $stdoutLogPath -RedirectStandardError $stderrLogPath `
        -PassThru -WindowStyle Hidden
    if (!$runProcess.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        $runProcess.Kill()
        $runProcess.WaitForExit()
    }
    $runExitCode = $runProcess.ExitCode

    if (Test-Path -LiteralPath $xmlLogPath) {
        Remove-Item -LiteralPath $xmlLogPath -Force
    }
    $exportProcess = Start-Process -FilePath $applicationVerifierPath `
        -ArgumentList @('-export', 'log', '-for', $targetName, '-with', "To=$xmlLogPath") `
        -Wait -PassThru -WindowStyle Hidden

    $stopCount = 0
    $exportStatus = 'unavailable'
    if ($exportProcess.ExitCode -eq 0 -and (Test-Path -LiteralPath $xmlLogPath -PathType Leaf)) {
        [xml]$verifierDocument = Get-Content -LiteralPath $xmlLogPath -Raw -Encoding UTF8
        $stopNodes = $verifierDocument.SelectNodes('//*[@StopCode or @StopID or @StopId]')
        $errorAttributeNodes = $verifierDocument.SelectNodes("//*[@Severity='Error' or @Severity='ERROR']")
        $errorElementNodes = $verifierDocument.SelectNodes("//*[local-name()='Severity' and (text()='Error' or text()='ERROR')]")
        $stopCount = $stopNodes.Count + $errorAttributeNodes.Count + $errorElementNodes.Count
        $exportStatus = 'ok'
    }

    $memoryErrorDetected = $stopCount -gt 0 -or $runExitCode -ne 0 -or $timedOut
    $diagnosticFailed = $memoryErrorDetected
    Write-DiagnosticRecord "memory_diagnostic_method=application_verifier phase=run exit_code=$runExitCode timed_out=$($timedOut.ToString().ToLowerInvariant()) status=$(if ($memoryErrorDetected) { 'failed' } else { 'ok' })"
    Write-DiagnosticRecord "memory_diagnostic_method=application_verifier log_export=$exportStatus verifier_stop_count=$stopCount"
    Write-DiagnosticRecord "memory_diagnostic_method=application_verifier memory_error_detected=$($memoryErrorDetected.ToString().ToLowerInvariant()) status=$(if ($memoryErrorDetected) { 'failed' } else { 'ok' })"
}
finally {
    if ($settingsEnabled) {
        try {
            Invoke-AppVerifierCommand -ApplicationVerifierPath $applicationVerifierPath `
                -CommandArguments @('-delete', 'settings', '-for', $targetName) -Phase 'delete_settings'
        }
        catch {
            $settingsCleanupFailed = $true
            Write-DiagnosticRecord "memory_diagnostic_method=application_verifier phase=delete_settings status=failed message=$($_.Exception.Message.Replace(' ', '_'))"
        }
    }
    $env:VERIFIER_LOG_PATH = $previousVerifierLogPath
}

if ($diagnosticFailed -or $settingsCleanupFailed) { exit 1 }
