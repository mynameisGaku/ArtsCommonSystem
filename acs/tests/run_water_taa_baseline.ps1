[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FixtureExe,

    [Parameter(Mandatory = $true)]
    [string]$ArtifactRoot,

    [string]$Configuration = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Configuration -notin @("Debug", "Release")) {
    throw "evidence runner requires Configuration=Debug or Release, got: $Configuration"
}

$script:ExpectedQualityQ = "fixture-contract:q1-no-jitter"
$script:ExpectedQualityQSource = "fixture-contract-label"
$script:ModeContracts = [ordered]@{
    R0 = [ordered]@{ taa_enabled = $false; wake_enabled = $false; history_invalidated_immediately_before_wake = $false }
    B0 = [ordered]@{ taa_enabled = $true;  wake_enabled = $false; history_invalidated_immediately_before_wake = $false }
    R1 = [ordered]@{ taa_enabled = $false; wake_enabled = $true;  history_invalidated_immediately_before_wake = $false }
    B1 = [ordered]@{ taa_enabled = $true;  wake_enabled = $true;  history_invalidated_immediately_before_wake = $false }
    C1 = [ordered]@{ taa_enabled = $true;  wake_enabled = $true;  history_invalidated_immediately_before_wake = $true }
}

function Invoke-GitText {
    param([Parameter(Mandatory = $true)][string[]]$GitArguments)

    $output = & git -C $script:RepositoryRoot @GitArguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $joined = [string]::Join([Environment]::NewLine, @($output | ForEach-Object { $_.ToString() }))
        throw "git $($GitArguments -join ' ') failed with exit code ${exitCode}: $joined"
    }
    return [string]::Join([Environment]::NewLine, @($output | ForEach-Object { $_.ToString() })).TrimEnd()
}

function Get-GitEvidence {
    return [pscustomobject][ordered]@{
        head = Invoke-GitText -GitArguments @("rev-parse", "HEAD")
        status = Invoke-GitText -GitArguments @("status", "--porcelain=v1", "--untracked-files=all")
        diff = Invoke-GitText -GitArguments @("diff", "--binary", "--no-ext-diff", "--", "engine/CMakeLists.txt", "tests/CMakeLists.txt", "tests/WaterTaaBaselineFixture.cpp", "tests/run_water_taa_baseline.ps1")
    }
}

function Assert-NoReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "reparse point is not accepted: $Path"
    }
}

function Assert-NoReparseAncestors {
    param([Parameter(Mandatory = $true)][string]$Path)

    $current = [System.IO.DirectoryInfo]([System.IO.Path]::GetFullPath($Path))
    while ($null -ne $current) {
        if (Test-Path -LiteralPath $current.FullName) {
            Assert-NoReparsePoint -Path $current.FullName
        }
        $current = $current.Parent
    }
}

function Assert-EmptyDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "expected artifact directory does not exist: $Path"
    }
    if (@(Get-ChildItem -LiteralPath $Path -Force).Count -ne 0) {
        throw "artifact directory must be new and empty: $Path"
    }
}

function Assert-DirectoryEntries {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string[]]$Expected)

    $actual = @((Get-ChildItem -LiteralPath $Path -Force | ForEach-Object { $_.Name }) | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if ($actual.Count -ne $wanted.Count -or (Compare-Object -ReferenceObject $wanted -DifferenceObject $actual)) {
        throw "unexpected artifact residue in ${Path}: expected [$($wanted -join ', ')], actual [$($actual -join ', ')]"
    }
}

function Get-ArtifactResidue {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string[]]$Expected)

    Assert-NoReparsePoint -Path $Path
    $actual = @()
    foreach ($entry in @(Get-ChildItem -LiteralPath $Path -Force)) {
        Assert-NoReparsePoint -Path $entry.FullName
        $actual += $entry.Name
    }
    $actual = @($actual | Sort-Object)
    $unexpected = @($actual | Where-Object { $Expected -notcontains $_ })
    return [pscustomobject][ordered]@{
        scope = "direct entries under the unique artifact base"
        actual_entries = $actual
        expected_entries = @($Expected | Sort-Object)
        residue_entries = $unexpected
        residue_count = $unexpected.Count
    }
}

function New-FixtureExecutableSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$SourceExe,
        [Parameter(Mandatory = $true)][string]$ArtifactBase,
        [Parameter(Mandatory = $true)][string]$ExpectedConfiguration
    )

    $snapshotPath = Join-Path $ArtifactBase ("fixture-" + $ExpectedConfiguration + ".exe")
    if (Test-Path -LiteralPath $snapshotPath) {
        throw "fixture executable snapshot already exists: $snapshotPath"
    }
    $sourceHash = (Get-FileHash -LiteralPath $SourceExe -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $SourceExe -Destination $snapshotPath -ErrorAction Stop
    Assert-NoReparsePoint -Path $snapshotPath
    $snapshotHash = (Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
    if ($sourceHash -ne $snapshotHash) {
        throw "fixture executable snapshot hash does not match its source"
    }
    [System.IO.File]::SetAttributes(
        $snapshotPath,
        ([System.IO.File]::GetAttributes($snapshotPath) -bor [System.IO.FileAttributes]::ReadOnly))
    $snapshotIsReadOnly = (([System.IO.File]::GetAttributes($snapshotPath) -band [System.IO.FileAttributes]::ReadOnly) -ne 0)
    if (-not $snapshotIsReadOnly) {
        throw "fixture executable snapshot could not be marked read-only"
    }
    Assert-DirectoryEntries -Path $ArtifactBase -Expected @(Split-Path -Leaf $snapshotPath)
    return [pscustomobject][ordered]@{
        configuration = $ExpectedConfiguration
        source_path = $SourceExe
        source_sha256_before = $sourceHash
        immutable_executable_path = $snapshotPath
        immutable_executable_sha256 = $snapshotHash
        immutable_executable_read_only = $snapshotIsReadOnly
    }
}

function Get-InputHashes {
    $hashes = [ordered]@{}
    foreach ($entry in $script:InputPaths.GetEnumerator()) {
        if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
            throw "hash input does not exist: $($entry.Key) = $($entry.Value)"
        }
        $hashes[$entry.Key] = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
    }
    return [pscustomobject]$hashes
}

function Assert-RequiredMetadata {
    param(
        [Parameter(Mandatory = $true)][psobject]$Metadata,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$ExpectedConfiguration,
        [Parameter(Mandatory = $true)][string]$RawPath,
        [Parameter(Mandatory = $true)][int]$ExpectedProcessId
    )

    $modeContract = $script:ModeContracts[$Mode]
    if ($null -eq $modeContract) {
        throw "missing expected mode contract for $Mode"
    }
    foreach ($propertyName in @("mode", "configuration", "taa_enabled", "wake_enabled", "history_invalidated_immediately_before_wake", "backend", "process_id", "adapter_luid", "adapter_vendor_id", "adapter_device_id", "adapter_name", "quality_q", "quality_q_source", "halton_sequence_length", "halton_index", "halton_jitter_pixels", "warmup_frame_count", "capture_frame_count", "viewport", "raw_format", "raw_bytes", "wake")) {
        if ($null -eq $Metadata.PSObject.Properties[$propertyName]) {
            throw "metadata misses required $propertyName for $Mode"
        }
    }
    if ($null -eq $Metadata.wake.PSObject.Properties["accepted_samples"]) {
        throw "metadata misses wake.accepted_samples for $Mode"
    }
    if ([string]$Metadata.mode -ne $Mode) {
        throw "metadata mode does not match invocation: expected $Mode, got $($Metadata.mode)"
    }
    if ([string]$Metadata.configuration -ne $ExpectedConfiguration) {
        throw "metadata configuration does not match invocation: expected $ExpectedConfiguration, got $($Metadata.configuration)"
    }
    if ([string]$Metadata.backend -ne "DX12") {
        throw "metadata backend does not identify raw DX12 for $Mode"
    }
    if ([int]$Metadata.process_id -le 0 -or [int]$Metadata.process_id -ne $ExpectedProcessId) {
        throw "metadata process id does not match the nonzero spawned process for $Mode"
    }
    if ([string]::IsNullOrWhiteSpace([string]$Metadata.adapter_luid) -or
        [string]::IsNullOrWhiteSpace([string]$Metadata.adapter_vendor_id) -or
        [string]::IsNullOrWhiteSpace([string]$Metadata.adapter_device_id) -or
        [string]::IsNullOrWhiteSpace([string]$Metadata.adapter_name)) {
        throw "metadata misses adapter LUID/vendor/device/name for $Mode"
    }
    if ([bool]$Metadata.taa_enabled -ne [bool]$modeContract.taa_enabled -or
        [bool]$Metadata.wake_enabled -ne [bool]$modeContract.wake_enabled -or
        [bool]$Metadata.history_invalidated_immediately_before_wake -ne [bool]$modeContract.history_invalidated_immediately_before_wake) {
        throw "metadata mode-specific TAA/wake/history contract does not match $Mode"
    }
    $acceptedSamples = [int]$Metadata.wake.accepted_samples
    if (([bool]$modeContract.wake_enabled -and $acceptedSamples -le 0) -or
        (-not [bool]$modeContract.wake_enabled -and $acceptedSamples -ne 0)) {
        throw "metadata accepted wake samples do not match $Mode"
    }
    if ([string]$Metadata.quality_q -ne $script:ExpectedQualityQ -or
        [string]$Metadata.quality_q_source -ne $script:ExpectedQualityQSource -or
        [int]$Metadata.halton_sequence_length -ne 1 -or
        [int]$Metadata.halton_index -ne 0 -or
        [int]$Metadata.capture_frame_count -ne ([int]$Metadata.warmup_frame_count + 1)) {
        throw "metadata does not record the fixture quality/Halton/frame contract for $Mode"
    }
    [int64]$width = [int64]$Metadata.viewport.width
    [int64]$height = [int64]$Metadata.viewport.height
    if ($width -le 0 -or $height -le 0 -or
        $height -gt ([int64]::MaxValue / 4) -or
        $width -gt [math]::Floor(([int64]::MaxValue / 4) / $height)) {
        throw "metadata has an invalid viewport or raw contract for $Mode"
    }
    [int64]$expectedRawBytes = $width * $height * 4
    $rawFile = Get-Item -LiteralPath $RawPath -Force
    Assert-NoReparsePoint -Path $rawFile.FullName
    if ([string]$Metadata.raw_format -ne "B8G8R8A8_UNORM" -or
        [int64]$Metadata.raw_bytes -ne $expectedRawBytes -or
        [int64]$rawFile.Length -ne $expectedRawBytes) {
        throw "metadata/raw file does not satisfy raw_bytes=width*height*4 for $Mode"
    }
}

function Get-ComparableMetadata {
    param([Parameter(Mandatory = $true)][psobject]$Metadata)

    return [ordered]@{
        configuration = [string]$Metadata.configuration
        backend = [string]$Metadata.backend
        adapter_luid = [string]$Metadata.adapter_luid
        adapter_vendor_id = [string]$Metadata.adapter_vendor_id
        adapter_device_id = [string]$Metadata.adapter_device_id
        quality_q = [string]$Metadata.quality_q
        quality_q_source = [string]$Metadata.quality_q_source
        halton_sequence_length = [string]$Metadata.halton_sequence_length
        halton_index = [string]$Metadata.halton_index
        halton_jitter_pixels = ($Metadata.halton_jitter_pixels | ConvertTo-Json -Compress)
        warmup_frame_count = [string]$Metadata.warmup_frame_count
        capture_frame_count = [string]$Metadata.capture_frame_count
        viewport = ($Metadata.viewport | ConvertTo-Json -Compress)
        timing = ($Metadata.timing | ConvertTo-Json -Depth 8 -Compress)
        camera = ($Metadata.camera | ConvertTo-Json -Depth 8 -Compress)
        water = ($Metadata.water | ConvertTo-Json -Depth 8 -Compress)
        order = [string]$Metadata.order
        raw_format = [string]$Metadata.raw_format
        raw_bytes = [string]$Metadata.raw_bytes
    }
}

function Get-AbsoluteDifference {
    param([Parameter(Mandatory = $true)][byte[]]$Left, [Parameter(Mandatory = $true)][byte[]]$Right)

    if ($Left.Length -ne $Right.Length) {
        throw "raw byte lengths differ: $($Left.Length) vs $($Right.Length)"
    }
    [uint64]$sum = 0
    for ($index = 0; $index -lt $Left.Length; ++$index) {
        $sum += [uint64][Math]::Abs(([int]$Left[$index]) - ([int]$Right[$index]))
    }
    return $sum
}

function Get-DifferenceOfDifferences {
    param(
        [Parameter(Mandatory = $true)][byte[]]$B1,
        [Parameter(Mandatory = $true)][byte[]]$B0,
        [Parameter(Mandatory = $true)][byte[]]$R1,
        [Parameter(Mandatory = $true)][byte[]]$R0
    )

    if ($B1.Length -ne $B0.Length -or $B1.Length -ne $R1.Length -or $B1.Length -ne $R0.Length) {
        throw "difference-of-differences requires equal raw byte lengths"
    }
    [uint64]$sum = 0
    for ($index = 0; $index -lt $B1.Length; ++$index) {
        $historyWake = ([int]$B1[$index]) - ([int]$B0[$index])
        $referenceWake = ([int]$R1[$index]) - ([int]$R0[$index])
        $sum += [uint64][Math]::Abs($historyWake - $referenceWake)
    }
    return $sum
}

function Write-JsonUtf8 {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][object]$Value)

    $text = $Value | ConvertTo-Json -Depth 16
    [System.IO.File]::WriteAllText($Path, $text + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

$script:RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Assert-NoReparseAncestors -Path $script:RepositoryRoot
if (-not (Test-Path -LiteralPath $FixtureExe -PathType Leaf)) {
    throw "fixture executable does not exist: $FixtureExe"
}
$FixtureExe = (Get-Item -LiteralPath $FixtureExe -Force).FullName
Assert-NoReparseAncestors -Path $FixtureExe

$script:InputPaths = [ordered]@{
    engine_cmake = Join-Path $script:RepositoryRoot "engine/CMakeLists.txt"
    tests_cmake = Join-Path $script:RepositoryRoot "tests/CMakeLists.txt"
    fixture_cpp = Join-Path $script:RepositoryRoot "tests/WaterTaaBaselineFixture.cpp"
    runner_ps1 = Join-Path $script:RepositoryRoot "tests/run_water_taa_baseline.ps1"
    fixture_exe = $FixtureExe
}

$beforeGit = Get-GitEvidence
$beforeHashes = Get-InputHashes
$beforeHashJson = $beforeHashes | ConvertTo-Json -Compress

if ([string]::IsNullOrWhiteSpace($ArtifactRoot)) {
    throw "ArtifactRoot must name an external artifact parent"
}
$artifactParent = [System.IO.Path]::GetFullPath($ArtifactRoot)
$repositoryPrefix = $script:RepositoryRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if ($artifactParent.Equals($script:RepositoryRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
    $artifactParent.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ArtifactRoot must remain outside the source tree: $artifactParent"
}
if (-not (Test-Path -LiteralPath $artifactParent)) {
    [System.IO.Directory]::CreateDirectory($artifactParent) | Out-Null
}
Assert-NoReparseAncestors -Path $artifactParent
$artifactBase = Join-Path $artifactParent ("wt-{0}-{1}" -f $Configuration.Substring(0, 1), [Guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $artifactBase) {
    throw "generated artifact base already exists: $artifactBase"
}
[System.IO.Directory]::CreateDirectory($artifactBase) | Out-Null
Assert-NoReparsePoint -Path $artifactBase
Assert-EmptyDirectory -Path $artifactBase
$fixtureSnapshot = New-FixtureExecutableSnapshot -SourceExe $FixtureExe -ArtifactBase $artifactBase -ExpectedConfiguration $Configuration
if ($fixtureSnapshot.source_sha256_before -ne [string]$beforeHashes.fixture_exe) {
    throw "fixture executable snapshot does not match the pre-run executable hash"
}

$modes = @("R0", "B0", "R1", "B1", "C1")
$runs = @()
foreach ($mode in $modes) {
    for ($repeat = 1; $repeat -le 3; ++$repeat) {
        $runDirectory = Join-Path $artifactBase ("{0}-repeat-{1:D2}" -f $mode, $repeat)
        if (Test-Path -LiteralPath $runDirectory) {
            throw "fresh-process artifact directory already exists: $runDirectory"
        }
        [System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
        Assert-NoReparsePoint -Path $runDirectory
        Assert-EmptyDirectory -Path $runDirectory

        # GUI subsystem executables need an explicit wait; quote --out as one argument.
        $argumentLine = '--mode ' + $mode + ' --configuration "' + $Configuration + '" --out "' + $runDirectory + '"'
        $fixtureProcess = Start-Process -FilePath $fixtureSnapshot.immutable_executable_path -ArgumentList $argumentLine -Wait -PassThru -NoNewWindow
        if ($null -eq $fixtureProcess -or [int]$fixtureProcess.Id -le 0) {
            throw "fixture process did not report a nonzero process id for $mode repeat $repeat"
        }
        $fixtureExitCode = $fixtureProcess.ExitCode
        if ($fixtureExitCode -ne 0) {
            throw "fixture process failed for $mode repeat $repeat with exit code $fixtureExitCode"
        }

        Assert-DirectoryEntries -Path $runDirectory -Expected @("frame.bgra8", "metadata.json")
        $rawPath = Join-Path $runDirectory "frame.bgra8"
        $metadataPath = Join-Path $runDirectory "metadata.json"
        Assert-NoReparsePoint -Path $metadataPath
        $metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-RequiredMetadata -Metadata $metadata -Mode $mode -ExpectedConfiguration $Configuration -RawPath $rawPath -ExpectedProcessId ([int]$fixtureProcess.Id)
        $rawHash = (Get-FileHash -LiteralPath $rawPath -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
        $runs += [pscustomobject][ordered]@{
            mode = $mode
            repeat = $repeat
            process_id = [int]$fixtureProcess.Id
            artifact_directory = $runDirectory
            raw_sha256 = $rawHash
            metadata = $metadata
            comparable_metadata = Get-ComparableMetadata -Metadata $metadata
        }
    }
}

$invalidProcessIds = @($runs | Where-Object { [int]$_.process_id -le 0 })
if ($runs.Count -ne ($modes.Count * 3) -or $invalidProcessIds.Count -ne 0) {
    throw "evidence must record 15 fresh processes with nonzero process ids"
}

$canonical = [ordered]@{}
$determinism = [ordered]@{}
foreach ($mode in $modes) {
    $modeRuns = @($runs | Where-Object { $_.mode -eq $mode })
    $rawHashes = @($modeRuns | ForEach-Object { $_.raw_sha256 } | Select-Object -Unique)
    $determinism[$mode] = [ordered]@{
        repeats = $modeRuns.Count
        raw_sha256 = $rawHashes
        deterministic = ($rawHashes.Count -eq 1)
    }
    if ($modeRuns.Count -ne 3 -or $rawHashes.Count -ne 1) {
        throw "raw capture is not deterministic across three fresh processes for $mode"
    }
    $canonical[$mode] = $modeRuns[0]
}

$baselineMetadataJson = $canonical["R0"].comparable_metadata | ConvertTo-Json -Compress
foreach ($run in $runs) {
    $currentMetadataJson = $run.comparable_metadata | ConvertTo-Json -Compress
    if ($currentMetadataJson -ne $baselineMetadataJson) {
        throw "fresh-process metadata mismatch for $($run.mode) repeat $($run.repeat)"
    }
}

$rawR0 = [System.IO.File]::ReadAllBytes((Join-Path $canonical["R0"].artifact_directory "frame.bgra8"))
$rawB0 = [System.IO.File]::ReadAllBytes((Join-Path $canonical["B0"].artifact_directory "frame.bgra8"))
$rawR1 = [System.IO.File]::ReadAllBytes((Join-Path $canonical["R1"].artifact_directory "frame.bgra8"))
$rawB1 = [System.IO.File]::ReadAllBytes((Join-Path $canonical["B1"].artifact_directory "frame.bgra8"))
$rawC1 = [System.IO.File]::ReadAllBytes((Join-Path $canonical["C1"].artifact_directory "frame.bgra8"))

[uint64]$b1R1Difference = Get-AbsoluteDifference -Left $rawB1 -Right $rawR1
[uint64]$c1R1Difference = Get-AbsoluteDifference -Left $rawC1 -Right $rawR1
[uint64]$wakeReferenceDenominator = Get-AbsoluteDifference -Left $rawR1 -Right $rawR0
[uint64]$differenceOfDifferences = Get-DifferenceOfDifferences -B1 $rawB1 -B0 $rawB0 -R1 $rawR1 -R0 $rawR0
$normalizedDifferenceOfDifferences = if ($wakeReferenceDenominator -gt 0) { [double]$differenceOfDifferences / [double]$wakeReferenceDenominator } else { $null }

$b1NotR1 = ($canonical["B1"].raw_sha256 -ne $canonical["R1"].raw_sha256) -and ($b1R1Difference -gt 0)
$c1EqualsR1 = ($canonical["C1"].raw_sha256 -eq $canonical["R1"].raw_sha256) -and ($c1R1Difference -eq 0)
$nonzeroDenominator = $wakeReferenceDenominator -gt 0
$positiveDifferenceOfDifferences = $differenceOfDifferences -gt 0
$classified = $b1NotR1 -and $c1EqualsR1 -and $nonzeroDenominator -and $positiveDifferenceOfDifferences

$afterGit = Get-GitEvidence
$afterHashes = Get-InputHashes
$afterHashJson = $afterHashes | ConvertTo-Json -Compress
$snapshotHashAfter = (Get-FileHash -LiteralPath $fixtureSnapshot.immutable_executable_path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
if ($beforeGit.head -ne $afterGit.head -or $beforeGit.status -ne $afterGit.status -or $beforeGit.diff -ne $afterGit.diff) {
    throw "git HEAD/status/diff changed while evidence ran"
}
if ($beforeHashJson -ne $afterHashJson) {
    throw "source/script/executable hash changed while evidence ran"
}
if ($fixtureSnapshot.immutable_executable_sha256 -ne $snapshotHashAfter -or
    $fixtureSnapshot.immutable_executable_sha256 -ne [string]$afterHashes.fixture_exe) {
    throw "fixture executable snapshot/source hash changed while evidence ran"
}
$fixtureSnapshot | Add-Member -NotePropertyName immutable_executable_sha256_after -NotePropertyValue $snapshotHashAfter

$artifactExpectedEntries = @($runs | ForEach-Object { Split-Path -Leaf $_.artifact_directory })
$artifactExpectedEntries += Split-Path -Leaf $fixtureSnapshot.immutable_executable_path
$artifactExpectedEntries += "manifest.json"
$artifactResidueBeforeManifest = Get-ArtifactResidue -Path $artifactBase -Expected $artifactExpectedEntries
[int]$residueCount = [int]$artifactResidueBeforeManifest.residue_count
if ($residueCount -ne 0) {
    throw "unexpected residue exists under the unique artifact base before manifest publication"
}

$manifest = [ordered]@{
    schema = "acs.water_taa_evidence.v1"
    evidence_only = $true
    production_fix_claim = $false
    configuration = $Configuration
    artifact_root = $artifactBase
    repeat_count_per_mode = 3
    fresh_process_count = $runs.Count
    all_process_ids_nonzero = $true
    fixture_executable_snapshot = $fixtureSnapshot
    quality_q_contract = [ordered]@{
        label = $script:ExpectedQualityQ
        source = $script:ExpectedQualityQSource
    }
    mode_matrix = [ordered]@{
        R0 = "no wake, TAA off"
        B0 = "no wake, TAA history"
        R1 = "wake, TAA off"
        B1 = "wake, TAA history"
        C1 = "wake, history invalidated immediately before injection/capture"
    }
    mode_contracts = $script:ModeContracts
    update_add_wake_render_order = "fixed-update; C1-invalidate-history-immediately-before-wake; add-wake; render-water; resolve-post; readback-final-swapchain"
    git_before = $beforeGit
    git_after = $afterGit
    input_hashes_before = $beforeHashes
    input_hashes_after = $afterHashes
    source_tree_unchanged = $true
    raw_determinism = $determinism
    runs = $runs
    metrics = [ordered]@{
        b1_minus_r1_abs_sum = $b1R1Difference
        c1_minus_r1_abs_sum = $c1R1Difference
        wake_reference_denominator_abs_sum = $wakeReferenceDenominator
        difference_of_differences_abs_sum = $differenceOfDifferences
        difference_of_differences_normalized = $normalizedDifferenceOfDifferences
    }
    classification_requirements = [ordered]@{
        b1_not_equal_r1 = $b1NotR1
        c1_equal_r1 = $c1EqualsR1
        nonzero_denominator = $nonzeroDenominator
        positive_difference_of_differences = $positiveDifferenceOfDifferences
    }
    classification = if ($classified) { "wake-specific-temporal-residual-observed" } else { "inconclusive-required-evidence-condition-failed" }
    visual_quality_threshold = $null
    artifact_residue = [ordered]@{
        scope = $artifactResidueBeforeManifest.scope
        expected_entries = $artifactResidueBeforeManifest.expected_entries
        residue_entries = $artifactResidueBeforeManifest.residue_entries
        residue_count = $residueCount
    }
    residue_count = $residueCount
}
$manifestPath = Join-Path $artifactBase "manifest.json"
Write-JsonUtf8 -Path $manifestPath -Value $manifest
$artifactResidueAfterManifest = Get-ArtifactResidue -Path $artifactBase -Expected $artifactExpectedEntries
if ([int]$artifactResidueAfterManifest.residue_count -ne $residueCount) {
    throw "artifact residue changed during manifest publication"
}
Assert-DirectoryEntries -Path $artifactBase -Expected $artifactExpectedEntries

if (-not $classified) {
    throw "evidence classification requires B1!=R1, C1=R1, a nonzero wake denominator, and difference-of-differences > 0; see $manifestPath"
}

Write-Output "Water TAA evidence artifact: $artifactBase"
