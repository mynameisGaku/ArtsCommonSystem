# SPDX-License-Identifier: Apache-2.0
# 単一 header 版 ACS 配布物を dist/ へ再生成する:
#   - dist/acs.h（amalgamate.py で生成）
#   - dist/lib/x64/Debug/acs.lib と dist/lib/x64/Release/acs.lib
#   - dist/acs-distribution.sha256（consumer が全 library を照合）
#
# 前提: 対象構成の engine を先に build しておく。例:
#   cmake --build acs/Intermediate/vs --config Debug   -j
#   cmake --build acs/Intermediate/vs --config Release -j
#
# 使い方:
#   powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1
#   ... -Configs Debug          # 未署名のlocal stagingとして1構成だけ生成
#   ... -Deploy C:\acs          # 両構成をconsumer用の場所へmirror
#   ... -SelfTest               # path/原子的公開の安全機構だけを自己検証
param(
    [string[]]$Configs = @('Debug','Release'),
    [string]$Deploy = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

$repo  = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $repo 'acs\Intermediate\vs'
$dist  = Join-Path $repo 'dist'
$distributionManifestName = 'acs-distribution.sha256'
$distributionManifestSchema = 'ACS_DIST_SHA256_V1'
$distributionAdjacentLibraryNames = @(
    'Diligent-Archiver-static',
    'Diligent-BasicPlatform',
    'Diligent-Common',
    'Diligent-GraphicsAccessories',
    'Diligent-GraphicsEngine',
    'Diligent-GraphicsEngineD3D12-static',
    'Diligent-GraphicsEngineD3DBase',
    'Diligent-GraphicsEngineNextGenBase',
    'Diligent-GraphicsTools',
    'Diligent-Primitives',
    'Diligent-ShaderTools',
    'Diligent-Win32Platform',
    'xxhash'
)
$distributionLibraryNames = @('acs.lib') + @($distributionAdjacentLibraryNames | ForEach-Object { "$_.lib" })
$distributionLibraryCountPerConfiguration = 14
$distributionContractFileCount = 29
$distributionStaticRelativePaths = @('README.md', 'acs.h', 'examples/build_example.cmd', 'examples/check.cpp')
if ($distributionLibraryNames.Count -ne $distributionLibraryCountPerConfiguration -or 1 + (2 * $distributionLibraryNames.Count) -ne $distributionContractFileCount) {
    throw "配布libraryまたはmanifestの固定file数が一致しません"
}

# 構成名を path として悪用できないよう、配布で対応する 2 値へ正規化する。
if (-not $Configs -or @($Configs).Count -eq 0) {
    throw "Configs には Debug または Release を 1 つ以上指定してください"
}
$normalizedConfigs = @()
foreach ($requestedConfig in $Configs) {
    if ([string]::Equals($requestedConfig, 'Debug', [System.StringComparison]::OrdinalIgnoreCase)) {
        $normalizedConfig = 'Debug'
    } elseif ([string]::Equals($requestedConfig, 'Release', [System.StringComparison]::OrdinalIgnoreCase)) {
        $normalizedConfig = 'Release'
    } else {
        throw "未対応または危険な構成名です: $requestedConfig"
    }
    if ($normalizedConfigs -notcontains $normalizedConfig) {
        $normalizedConfigs += $normalizedConfig
    }
}
$Configs = $normalizedConfigs
$isCompleteDistribution = $Configs.Count -eq 2 -and $Configs -contains 'Debug' -and $Configs -contains 'Release'
if ($Deploy -and -not $isCompleteDistribution) {
    throw "deployとnamed manifestの公開にはDebug/Releaseの両構成を同時指定してください"
}

function Get-NormalizedFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "空の path は指定できません"
    }
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Test-ReparsePoint([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    return (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-NoReparseAncestor([string]$Path) {
    $current = [System.IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-ReparsePoint $current) {
            throw "reparse point を経由する path は安全のため拒否します: $current"
        }
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if (-not $parent -or
            [string]::Equals(
                $parent, $current, [System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent
    }
}

function Assert-SafeDeployPath([string]$Path) {
    $full = Get-NormalizedFullPath $Path
    $rootPath = [System.IO.Path]::GetPathRoot($full).TrimEnd('\', '/')
    if ([string]::Equals($full, $rootPath,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "drive root へ /MIR deploy はできません: $full"
    }

    $blockedPaths = @($repo, (Join-Path $repo 'acs'), $build, $dist)
    foreach ($blockedPath in $blockedPaths) {
        $blocked = Get-NormalizedFullPath $blockedPath
        $fullPrefix = $full + [System.IO.Path]::DirectorySeparatorChar
        $blockedPrefix = $blocked + [System.IO.Path]::DirectorySeparatorChar
        $overlaps = [string]::Equals(
                $full, $blocked, [System.StringComparison]::OrdinalIgnoreCase) -or
            $full.StartsWith(
                $blockedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            $blocked.StartsWith(
                $fullPrefix, [System.StringComparison]::OrdinalIgnoreCase)
        if ($overlaps) {
            throw "source/build/dist と重なる path へ deploy はできません: $full"
        }
    }
    Assert-NoReparseAncestor $full
    return $full
}

# root配下をリンク先へ移動せず列挙し、reparse pointを検出した時点で拒否する。
function Get-RegularFilesWithoutReparse([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    if (-not (Test-Path -LiteralPath $normalizedRoot -PathType Container)) {
        throw "通常directoryではない配布rootです: $normalizedRoot"
    }

    $pendingDirectories = [System.Collections.Generic.Stack[string]]::new()
    $regularFiles = [System.Collections.Generic.List[string]]::new()
    $pendingDirectories.Push($normalizedRoot)
    while ($pendingDirectories.Count -gt 0) {
        $directoryPath = $pendingDirectories.Pop()
        $directoryItem = Get-Item -LiteralPath $directoryPath -Force
        if (($directoryItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "配布tree内のreparse pointは拒否します: $directoryPath"
        }
        foreach ($childItem in Get-ChildItem -LiteralPath $directoryPath -Force) {
            if (($childItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "配布tree内のreparse pointは拒否します: $($childItem.FullName)"
            }
            if ($childItem.PSIsContainer) {
                $pendingDirectories.Push($childItem.FullName)
            } else {
                $regularFiles.Add($childItem.FullName)
            }
        }
    }
    return @($regularFiles)
}

# 既存treeがある場合だけ、配下にreparse pointがないことを確認する。
function Assert-NoReparseSubtree([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    if (-not (Test-Path -LiteralPath $normalizedRoot)) { return }
    if (-not (Test-Path -LiteralPath $normalizedRoot -PathType Container)) {
        throw "deploy先が通常directoryではありません: $normalizedRoot"
    }
    Get-RegularFilesWithoutReparse $normalizedRoot | Out-Null
}

# dist全体を走査し、正規file以外が配布へ混入することを拒否する。
function Assert-DistributionTreeAllowlist([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $allowedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($staticRelativePath in $distributionStaticRelativePaths) {
        $staticPath = [System.IO.Path]::GetFullPath((Join-Path $normalizedRoot $staticRelativePath))
        if (-not (Test-Path -LiteralPath $staticPath -PathType Leaf) -or (Get-Item -LiteralPath $staticPath).Length -le 0) {
            throw "ACS配布treeの必須fileがありません: $staticPath"
        }
        $allowedPaths.Add($staticPath) | Out-Null
    }
    $allowedPaths.Add([System.IO.Path]::GetFullPath((Join-Path $normalizedRoot $distributionManifestName))) | Out-Null
    foreach ($configurationName in @('Debug', 'Release')) {
        foreach ($libraryName in $distributionLibraryNames) {
            $allowedLibraryPath = Join-Path $normalizedRoot "lib\x64\$configurationName\$libraryName"
            $allowedPaths.Add([System.IO.Path]::GetFullPath($allowedLibraryPath)) | Out-Null
        }
    }
    foreach ($filePath in Get-RegularFilesWithoutReparse $normalizedRoot) {
        if (-not $allowedPaths.Contains([System.IO.Path]::GetFullPath($filePath))) {
            throw "ACS配布treeへ正規file以外が混入しています: $filePath"
        }
    }
}

function Publish-FileAtomically([string]$TemporaryPath, [string]$DestinationPath) {
    $destinationDirectory = [System.IO.Path]::GetDirectoryName($DestinationPath)
    Assert-NoReparseAncestor $destinationDirectory
    Assert-NoReparseAncestor $DestinationPath
    if ((Test-Path -LiteralPath $DestinationPath) -and
        -not (Test-Path -LiteralPath $DestinationPath -PathType Leaf)) {
        throw "配布 file の出力先が通常 file ではありません: $DestinationPath"
    }
    if (Test-Path -LiteralPath $DestinationPath -PathType Leaf) {
        $backupPath = Join-Path $destinationDirectory (
            '.acs.publish.' + [Guid]::NewGuid().ToString('N') + '.bak')
        try {
            [System.IO.File]::Replace(
                $TemporaryPath, $DestinationPath, $backupPath, $true)
        } finally {
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        }
    } else {
        [System.IO.File]::Move($TemporaryPath, $DestinationPath)
    }
}

# 配布root配下の絶対pathを、manifest用のスラッシュ区切り相対pathへ変換する。
function Get-AcsDistributionRelativePath([string]$Root, [string]$Path) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $rootPrefix = $normalizedRoot + [System.IO.Path]::DirectorySeparatorChar
    $normalizedPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $normalizedPath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "ACS配布fileがrootの外です: $normalizedPath"
    }
    return $normalizedPath.Substring($rootPrefix.Length).Replace('\', '/')
}

# CardGame verifierと同じheaderおよびDebug/Release library全件を列挙する。
function Get-AcsDistributionContractFiles([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseSubtree $normalizedRoot
    Assert-DistributionTreeAllowlist $normalizedRoot

    $headerPath = Join-Path $normalizedRoot 'acs.h'
    $debugLibraryDirectory = Join-Path $normalizedRoot 'lib\x64\Debug'
    $releaseLibraryDirectory = Join-Path $normalizedRoot 'lib\x64\Release'
    $filesByRelativePath = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    if (-not (Test-Path -LiteralPath $headerPath -PathType Leaf) -or (Get-Item -LiteralPath $headerPath).Length -le 0) {
        throw "ACS配布manifestの必須fileがありません: $headerPath"
    }
    $candidateFiles = [System.Collections.Generic.List[string]]::new()
    $candidateFiles.Add($headerPath)
    foreach ($libraryDirectory in @($debugLibraryDirectory, $releaseLibraryDirectory)) {
        if (-not (Test-Path -LiteralPath $libraryDirectory -PathType Container)) {
            throw "ACS配布manifestの必須directoryがありません: $libraryDirectory"
        }
        $actualLibraryFiles = @(Get-RegularFilesWithoutReparse $libraryDirectory)
        $actualLibraryPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($actualLibraryFile in $actualLibraryFiles) {
            $actualLibraryPaths.Add([System.IO.Path]::GetFullPath($actualLibraryFile)) | Out-Null
        }
        foreach ($libraryName in $distributionLibraryNames) {
            $requiredLibraryPath = [System.IO.Path]::GetFullPath((Join-Path $libraryDirectory $libraryName))
            if (-not $actualLibraryPaths.Contains($requiredLibraryPath) -or (Get-Item -LiteralPath $requiredLibraryPath).Length -le 0) {
                throw "ACS配布manifestの必須libraryがありません: $requiredLibraryPath"
            }
            $candidateFiles.Add($requiredLibraryPath)
        }
        if ($actualLibraryPaths.Count -ne $distributionLibraryNames.Count) {
            throw "ACS配布manifestのlibrary集合が正規14件と一致しません: $libraryDirectory"
        }
    }

    foreach ($candidatePath in $candidateFiles) {
        $relativePath = Get-AcsDistributionRelativePath $normalizedRoot $candidatePath
        if ($relativePath.Contains('\') -or [System.IO.Path]::IsPathRooted($relativePath) -or $relativePath.StartsWith('/') -or $relativePath.EndsWith('/') -or $relativePath.Contains('//') -or @($relativePath -split '/' | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
            throw "ACS配布manifestのpathが安全ではありません: $relativePath"
        }
        if ($filesByRelativePath.ContainsKey($relativePath)) {
            throw "ACS配布manifestのfile pathが重複しています: $relativePath"
        }
        $filesByRelativePath.Add($relativePath, [System.IO.Path]::GetFullPath($candidatePath))
    }
    if ($filesByRelativePath.Count -ne $distributionContractFileCount) {
        throw "ACS配布manifestのfile数が正規29件と一致しません: $($filesByRelativePath.Count)"
    }

    $relativePaths = [string[]]@($filesByRelativePath.Keys)
    [System.Array]::Sort($relativePaths, [System.StringComparer]::Ordinal)
    $orderedFiles = [System.Collections.Generic.List[string]]::new()
    foreach ($relativePath in $relativePaths) {
        $orderedFiles.Add($filesByRelativePath[$relativePath])
    }
    return @($orderedFiles)
}

# manifestをUTF-8 BOMなし、LF、相対path昇順のcanonical byte列として作る。
function New-AcsDistributionManifestContent([string]$Root) {
    $manifestLines = [System.Collections.Generic.List[string]]::new()
    $manifestLines.Add($distributionManifestSchema)
    foreach ($distributionFile in Get-AcsDistributionContractFiles $Root) {
        $relativePath = Get-AcsDistributionRelativePath $Root $distributionFile
        $fileHash = (Get-FileHash -LiteralPath $distributionFile -Algorithm SHA256).Hash.ToUpperInvariant()
        $manifestLines.Add("$fileHash  $relativePath")
    }
    return [string]::Join("`n", [string[]]$manifestLines) + "`n"
}

# manifestが現在の配布file集合とcanonical byte単位で一致することを確認する。
function Assert-AcsDistributionManifest([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $manifestPath = Join-Path $normalizedRoot $distributionManifestName
    Assert-NoReparseAncestor $manifestPath
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "ACS配布manifestがありません: $manifestPath"
    }

    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    $expectedBytes = $utf8WithoutBom.GetBytes((New-AcsDistributionManifestContent $normalizedRoot))
    $actualBytes = [System.IO.File]::ReadAllBytes($manifestPath)
    if ($actualBytes.Length -ne $expectedBytes.Length) {
        throw "ACS配布manifestが現在の配布内容と一致しません: $manifestPath"
    }
    for ($byteIndex = 0; $byteIndex -lt $actualBytes.Length; ++$byteIndex) {
        if ($actualBytes[$byteIndex] -ne $expectedBytes[$byteIndex]) {
            throw "ACS配布manifestが現在の配布内容と一致しません: $manifestPath"
        }
    }
}

# 完全な一時fileを同じdirectoryで作り、最後にmanifest名へ原子的に公開する。
function Publish-AcsDistributionManifest([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $manifestPath = Join-Path $normalizedRoot $distributionManifestName
    $temporaryManifestPath = Join-Path $normalizedRoot (".$distributionManifestName." + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($temporaryManifestPath, (New-AcsDistributionManifestContent $normalizedRoot), $utf8WithoutBom)
        Publish-FileAtomically $temporaryManifestPath $manifestPath
        $temporaryManifestPath = ''
    } finally {
        if ($temporaryManifestPath -and (Test-Path -LiteralPath $temporaryManifestPath -PathType Leaf)) {
            Remove-Item -LiteralPath $temporaryManifestPath -Force -ErrorAction SilentlyContinue
        }
    }
    Assert-AcsDistributionManifest $normalizedRoot
    $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToUpperInvariant()
    Write-Host "    配布manifest完了 (SHA-256=$manifestHash)"
}

function Get-DistributionFileManifest([string]$Root, [string[]]$ExcludedRelativePaths = @()) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseSubtree $normalizedRoot

    $rootPrefix = $normalizedRoot + [System.IO.Path]::DirectorySeparatorChar
    $excludedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($excludedRelativePath in $ExcludedRelativePaths) {
        $excludedPaths.Add($excludedRelativePath) | Out-Null
    }
    $manifest = @{}
    foreach ($filePath in Get-RegularFilesWithoutReparse $normalizedRoot) {
        if (-not $filePath.StartsWith(
                $rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "配布manifestのfileがrootの外です: $filePath"
        }
        $relativePath = $filePath.Substring($rootPrefix.Length)
        if ($excludedPaths.Contains($relativePath)) { continue }
        if ($manifest.ContainsKey($relativePath)) {
            throw "配布manifestに重複するfile pathがあります: $relativePath"
        }
        $manifest[$relativePath] = [pscustomobject]@{
            Length = (Get-Item -LiteralPath $filePath).Length
            Sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
        }
    }
    return $manifest
}

function Assert-MirroredDistribution([string]$Source, [string]$Destination, [switch]$ExcludeDistributionManifest) {
    $excludedPaths = if ($ExcludeDistributionManifest) { @($distributionManifestName) } else { @() }
    $sourceManifest = Get-DistributionFileManifest $Source $excludedPaths
    $destinationManifest = Get-DistributionFileManifest $Destination $excludedPaths
    if ($sourceManifest.Count -ne $destinationManifest.Count) {
        throw ("deploy 後の file 数が一致しません: source={0}, destination={1}" -f
            $sourceManifest.Count, $destinationManifest.Count)
    }

    foreach ($relativePath in $sourceManifest.Keys) {
        if (-not $destinationManifest.ContainsKey($relativePath)) {
            throw "deploy 後の file が不足しています: $relativePath"
        }
        $sourceEntry = $sourceManifest[$relativePath]
        $destinationEntry = $destinationManifest[$relativePath]
        if ($sourceEntry.Length -ne $destinationEntry.Length) {
            throw "deploy 後の file size が一致しません: $relativePath"
        }
        if (-not [string]::Equals(
                $sourceEntry.Sha256,
                $destinationEntry.Sha256,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "deploy 後の SHA-256 が一致しません: $relativePath"
        }
    }
}

# payloadの完全一致を確認した後だけ、named manifestを配置先へ原子的に公開する。
function Publish-MirroredDistribution([string]$Source, [string]$Destination) {
    $normalizedSource = Get-NormalizedFullPath $Source
    $normalizedDestination = Get-NormalizedFullPath $Destination
    Assert-NoReparseSubtree $normalizedSource
    Assert-NoReparseSubtree $normalizedDestination
    Assert-AcsDistributionManifest $normalizedSource

    & robocopy $normalizedSource $normalizedDestination /MIR /XF $distributionManifestName /XJ /R:0 /W:0 /NJH /NJS /NDL /NFL /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopyに失敗しました ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0

    Assert-NoReparseSubtree $normalizedDestination
    Assert-MirroredDistribution $normalizedSource $normalizedDestination -ExcludeDistributionManifest

    $sourceManifestPath = Join-Path $normalizedSource $distributionManifestName
    $destinationManifestPath = Join-Path $normalizedDestination $distributionManifestName
    $temporaryManifestPath = Join-Path $normalizedDestination (".$distributionManifestName." + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $sourceManifestPath -Destination $temporaryManifestPath
        Publish-FileAtomically $temporaryManifestPath $destinationManifestPath
        $temporaryManifestPath = ''
    } finally {
        if ($temporaryManifestPath -and (Test-Path -LiteralPath $temporaryManifestPath -PathType Leaf)) {
            Remove-Item -LiteralPath $temporaryManifestPath -Force -ErrorAction SilentlyContinue
        }
    }

    Assert-NoReparseSubtree $normalizedDestination
    Assert-MirroredDistribution $normalizedSource $normalizedDestination
    Assert-AcsDistributionManifest $normalizedDestination
}

function Assert-ExpectedFailure([scriptblock]$Action, [string]$Name) {
    $failedAsExpected = $false
    try {
        & $Action | Out-Null
    } catch {
        $failedAsExpected = $true
    }
    if (-not $failedAsExpected) {
        throw "self-test が拒否を確認できませんでした: $Name"
    }
}

function Invoke-PipelineSelfTest {
    Assert-ExpectedFailure {
        Assert-SafeDeployPath ([System.IO.Path]::GetPathRoot($repo))
    } 'drive root deploy'
    Assert-ExpectedFailure { Assert-SafeDeployPath $repo } 'repository deploy'
    Assert-ExpectedFailure {
        Assert-SafeDeployPath (Join-Path $repo '_unsafe_deploy_probe')
    } 'repository child deploy'
    $repositoryParent = [System.IO.Path]::GetDirectoryName($repo)
    if ($repositoryParent) {
        Assert-ExpectedFailure {
            Assert-SafeDeployPath $repositoryParent
        } 'repository parent deploy'
    }

    $safeDeployProbe = Join-Path (
        [System.IO.Path]::GetTempPath()) (
        'acs-safe-deploy-' + [Guid]::NewGuid().ToString('N'))
    $normalizedProbe = Assert-SafeDeployPath $safeDeployProbe
    if (-not [string]::Equals(
            $normalizedProbe,
            (Get-NormalizedFullPath $safeDeployProbe),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "self-test の安全な deploy path 正規化が一致しません"
    }

    $selfTestRoot = Get-NormalizedFullPath (Join-Path $repo 'acs\Saved')
    $testDirectory = Get-NormalizedFullPath (Join-Path $repo (
        'acs\Saved\single-header-self-test-' + [Guid]::NewGuid().ToString('N'))
    )
    if (-not $testDirectory.StartsWith(
            $selfTestRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "self-test directory が Saved の外です: $testDirectory"
    }
    New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null
    try {
        $destination = Join-Path $testDirectory 'acs.lib'
        $temporary = Join-Path $testDirectory '.new.lib'
        [System.IO.File]::WriteAllText($destination, 'old')
        [System.IO.File]::WriteAllText($temporary, 'new')
        Publish-FileAtomically $temporary $destination
        if ([System.IO.File]::ReadAllText($destination) -ne 'new' -or
            (Test-Path -LiteralPath $temporary)) {
            throw "self-test の既存 library 原子的置換に失敗しました"
        }

        $newDestination = Join-Path $testDirectory 'new.lib'
        $newTemporary = Join-Path $testDirectory '.new-second.lib'
        [System.IO.File]::WriteAllText($newTemporary, 'created')
        Publish-FileAtomically $newTemporary $newDestination
        if ([System.IO.File]::ReadAllText($newDestination) -ne 'created' -or
            (Test-Path -LiteralPath $newTemporary)) {
            throw "self-test の新規 library 公開に失敗しました"
        }

        $mirrorSource = Join-Path $testDirectory 'mirror-source'
        $mirrorDestination = Join-Path $testDirectory 'mirror-destination'
        New-Item -ItemType Directory -Force -Path $mirrorSource | Out-Null
        New-Item -ItemType Directory -Force -Path $mirrorDestination | Out-Null
        [System.IO.File]::WriteAllText(
            (Join-Path $mirrorSource 'same-size.bin'), 'ABCD')
        [System.IO.File]::WriteAllText(
            (Join-Path $mirrorDestination 'same-size.bin'), 'ABCD')
        Assert-MirroredDistribution $mirrorSource $mirrorDestination

        # robocopy can skip a corrupt file when size and timestamp happen to
        # match.  The post-deploy manifest must still reject its byte drift.
        [System.IO.File]::WriteAllText(
            (Join-Path $mirrorDestination 'same-size.bin'), 'WXYZ')
        Assert-ExpectedFailure {
            Assert-MirroredDistribution $mirrorSource $mirrorDestination
        } 'same-size deploy content drift'

        [System.IO.File]::WriteAllText(
            (Join-Path $mirrorDestination 'same-size.bin'), 'ABCD')
        [System.IO.File]::WriteAllText(
            (Join-Path $mirrorDestination 'extra.bin'), 'extra')
        Assert-ExpectedFailure {
            Assert-MirroredDistribution $mirrorSource $mirrorDestination
        } 'extra deploy file'

        $manifestSource = Join-Path $testDirectory 'manifest-source'
        $debugManifestDirectory = Join-Path $manifestSource 'lib\x64\Debug'
        $releaseManifestDirectory = Join-Path $manifestSource 'lib\x64\Release'
        $manifestExampleDirectory = Join-Path $manifestSource 'examples'
        New-Item -ItemType Directory -Force -Path $debugManifestDirectory | Out-Null
        New-Item -ItemType Directory -Force -Path $releaseManifestDirectory | Out-Null
        New-Item -ItemType Directory -Force -Path $manifestExampleDirectory | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $manifestSource 'README.md'), 'fixture-readme')
        [System.IO.File]::WriteAllText((Join-Path $manifestSource 'acs.h'), 'fixture-header')
        [System.IO.File]::WriteAllText((Join-Path $manifestExampleDirectory 'build_example.cmd'), 'fixture-build')
        [System.IO.File]::WriteAllText((Join-Path $manifestExampleDirectory 'check.cpp'), 'fixture-source')
        $configurationFixtures = @(
            [pscustomobject]@{ Name = 'Debug'; Directory = $debugManifestDirectory }
            [pscustomobject]@{ Name = 'Release'; Directory = $releaseManifestDirectory }
        )
        foreach ($configurationFixture in $configurationFixtures) {
            foreach ($libraryName in $distributionLibraryNames) {
                $fixtureContent = "$($configurationFixture.Name)-$libraryName"
                if ($libraryName -eq 'acs.lib' -and $configurationFixture.Name -eq 'Debug') {
                    $fixtureContent = 'ABCD'
                }
                [System.IO.File]::WriteAllText((Join-Path $configurationFixture.Directory $libraryName), $fixtureContent)
            }
        }

        Publish-AcsDistributionManifest $manifestSource
        Assert-AcsDistributionManifest $manifestSource
        $manifestPath = Join-Path $manifestSource $distributionManifestName
        $manifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
        if (($manifestBytes.Length -ge 3 -and $manifestBytes[0] -eq 0xEF -and $manifestBytes[1] -eq 0xBB -and $manifestBytes[2] -eq 0xBF) -or (@($manifestBytes) -contains [byte]0x0D) -or $manifestBytes[$manifestBytes.Length - 1] -ne 0x0A) {
            throw "self-testのmanifest改行またはUTF-8 BOM契約が一致しません"
        }
        $manifestText = [System.IO.File]::ReadAllText($manifestPath, [System.Text.UTF8Encoding]::new($false))
        $manifestLines = @($manifestText -split "`n")
        $actualManifestPaths = [System.Collections.Generic.List[string]]::new()
        for ($lineIndex = 1; $lineIndex -lt $manifestLines.Count - 1; ++$lineIndex) {
            $manifestMatch = [regex]::Match($manifestLines[$lineIndex], '^[0-9A-F]{64}  ([^ ].*)$')
            if (-not $manifestMatch.Success) {
                throw "self-testのmanifest entry形式が一致しません"
            }
            $actualManifestPaths.Add($manifestMatch.Groups[1].Value)
        }
        $expectedManifestPaths = [System.Collections.Generic.List[string]]::new()
        $expectedManifestPaths.Add('acs.h')
        foreach ($configurationName in @('Debug', 'Release')) {
            foreach ($libraryName in $distributionLibraryNames) {
                $expectedManifestPaths.Add("lib/x64/$configurationName/$libraryName")
            }
        }
        $expectedManifestPaths.Sort([System.StringComparer]::Ordinal)
        if ($actualManifestPaths.Count -ne $distributionContractFileCount) {
            throw "self-testのmanifest entry数が29件ではありません"
        }
        if ([string]::Join("`n", [string[]]$actualManifestPaths) -cne [string]::Join("`n", [string[]]$expectedManifestPaths)) {
            throw "self-testのmanifest path順序が一致しません"
        }

        $validManifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
        [System.IO.File]::WriteAllText($manifestPath, "$distributionManifestSchema`n" + ('0' * 64) + "  acs.h`n", [System.Text.UTF8Encoding]::new($false))
        Assert-ExpectedFailure {
            Assert-AcsDistributionManifest $manifestSource
        } 'tampered distribution manifest'
        [System.IO.File]::WriteAllBytes($manifestPath, $validManifestBytes)

        Remove-Item -LiteralPath $manifestPath -Force
        Assert-ExpectedFailure {
            Assert-AcsDistributionManifest $manifestSource
        } 'missing distribution manifest'
        [System.IO.File]::WriteAllBytes($manifestPath, $validManifestBytes)

        $debugLibraryPath = Join-Path $debugManifestDirectory 'acs.lib'
        [System.IO.File]::WriteAllText($debugLibraryPath, 'WXYZ')
        Assert-ExpectedFailure {
            Assert-AcsDistributionManifest $manifestSource
        } 'stale distribution manifest hash'
        [System.IO.File]::WriteAllText($debugLibraryPath, 'ABCD')
        $staleLibraryPath = Join-Path $releaseManifestDirectory 'stale.lib'
        [System.IO.File]::WriteAllText($staleLibraryPath, 'stale')
        Assert-ExpectedFailure {
            Assert-AcsDistributionManifest $manifestSource
        } 'stale distribution manifest file set'
        Remove-Item -LiteralPath $staleLibraryPath -Force

        $outsideLibraryPath = Join-Path $manifestSource 'stale.lib'
        [System.IO.File]::WriteAllText($outsideLibraryPath, 'stale')
        Assert-ExpectedFailure {
            Publish-AcsDistributionManifest $manifestSource
        } 'unexpected library outside canonical directories'
        Remove-Item -LiteralPath $outsideLibraryPath -Force

        $ignoredArtifactPath = Join-Path $manifestExampleDirectory 'check.exe'
        [System.IO.File]::WriteAllText($ignoredArtifactPath, 'ignored')
        Assert-ExpectedFailure {
            Publish-AcsDistributionManifest $manifestSource
        } 'unexpected ignored distribution artifact outside libraries'
        Remove-Item -LiteralPath $ignoredArtifactPath -Force

        $requiredExamplePath = Join-Path $manifestExampleDirectory 'check.cpp'
        $requiredExampleBytes = [System.IO.File]::ReadAllBytes($requiredExamplePath)
        Remove-Item -LiteralPath $requiredExamplePath -Force
        Assert-ExpectedFailure {
            Publish-AcsDistributionManifest $manifestSource
        } 'missing required distribution example'
        [System.IO.File]::WriteAllBytes($requiredExamplePath, $requiredExampleBytes)

        $missingAdjacentLibraryPath = Join-Path $releaseManifestDirectory 'Diligent-Common.lib'
        $missingAdjacentLibraryBytes = [System.IO.File]::ReadAllBytes($missingAdjacentLibraryPath)
        Remove-Item -LiteralPath $missingAdjacentLibraryPath -Force
        Assert-ExpectedFailure {
            Publish-AcsDistributionManifest $manifestSource
        } 'missing adjacent distribution library'
        [System.IO.File]::WriteAllBytes($missingAdjacentLibraryPath, $missingAdjacentLibraryBytes)
        Assert-AcsDistributionManifest $manifestSource

        $manifestDestination = Join-Path $testDirectory 'manifest-destination'
        Publish-MirroredDistribution $manifestSource $manifestDestination
        Assert-MirroredDistribution $manifestSource $manifestDestination
        Assert-AcsDistributionManifest $manifestDestination

        $destinationManifestPath = Join-Path $manifestDestination $distributionManifestName
        $destinationManifestHashBefore = (Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash
        $destinationDebugLibraryPath = Join-Path $manifestDestination 'lib\x64\Debug\acs.lib'
        [System.IO.File]::WriteAllText($debugLibraryPath, 'WXYZ')
        $matchingTimestamp = [DateTime]::UtcNow.AddMinutes(-10)
        [System.IO.File]::SetLastWriteTimeUtc($debugLibraryPath, $matchingTimestamp)
        [System.IO.File]::SetLastWriteTimeUtc($destinationDebugLibraryPath, $matchingTimestamp)
        Publish-AcsDistributionManifest $manifestSource
        Assert-ExpectedFailure {
            Publish-MirroredDistribution $manifestSource $manifestDestination
        } 'same-size same-time skipped payload'
        if ((Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash -cne $destinationManifestHashBefore -or [System.IO.File]::ReadAllText($destinationDebugLibraryPath) -cne 'ABCD') {
            throw "self-testのpayload不一致失敗が旧manifestまたは配置先fileを変更しました"
        }

        [System.IO.File]::WriteAllText($debugLibraryPath, 'ABCD')
        Publish-AcsDistributionManifest $manifestSource
        Publish-MirroredDistribution $manifestSource $manifestDestination

        $manifestHashBeforeLockedUpdate = (Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash
        $manifestLock = [System.IO.File]::Open($destinationManifestPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try {
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestSource $manifestDestination
            } 'parallel locked manifest update'
        } finally {
            $manifestLock.Dispose()
        }
        if ($manifestHashBeforeLockedUpdate -cne (Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash) {
            throw "self-testの並列manifest更新が既存内容を変更しました"
        }

        $junctionTarget = Join-Path $testDirectory 'junction-target'
        $junctionPath = Join-Path $manifestDestination 'junction-probe'
        New-Item -ItemType Directory -Force -Path $junctionTarget | Out-Null
        $junctionSentinel = Join-Path $junctionTarget 'sentinel.txt'
        [System.IO.File]::WriteAllText($junctionSentinel, 'outside')
        & cmd.exe /d /c ('mklink /J "{0}" "{1}"' -f $junctionPath, $junctionTarget) | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-ReparsePoint $junctionPath)) {
            throw "self-testのjunction作成に失敗しました"
        }
        try {
            $manifestHashBeforeJunction = (Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestSource $manifestDestination
            } 'destination subtree junction'
            if (-not (Test-Path -LiteralPath $junctionSentinel -PathType Leaf) -or [System.IO.File]::ReadAllText($junctionSentinel) -cne 'outside' -or $manifestHashBeforeJunction -cne (Get-FileHash -LiteralPath $destinationManifestPath -Algorithm SHA256).Hash) {
                throw "self-testのjunction拒否がroot外fileまたは旧manifestを変更しました"
            }
        } finally {
            if (Test-Path -LiteralPath $junctionPath) {
                [System.IO.Directory]::Delete($junctionPath)
            }
        }

        $remainingManifestTemporaries = @(Get-ChildItem -LiteralPath $manifestDestination -File -Filter (".$distributionManifestName.*.tmp"))
        if ($remainingManifestTemporaries.Count -ne 0) {
            throw "self-testのmanifest一時fileが残っています"
        }
        Write-Host 'acs_distribution_manifest_self_test=ok cases=canonical,tamper,missing,stale,extra,partial,skip,reader_lock,junction,mirror'
    } finally {
        Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "単一 header 配布 pipeline self-test passed"
}

if ($SelfTest) {
    Invoke-PipelineSelfTest
    return
}

# 単一構成の更新では既存manifestを先に失効させ、公開可能なSDKと誤認させない。
if (-not $isCompleteDistribution) {
    $staleManifestPath = Join-Path $dist $distributionManifestName
    Assert-NoReparseAncestor $staleManifestPath
    if (Test-Path -LiteralPath $staleManifestPath) {
        if (-not (Test-Path -LiteralPath $staleManifestPath -PathType Leaf)) {
            throw "既存の配布manifestが通常fileではありません: $staleManifestPath"
        }
        Remove-Item -LiteralPath $staleManifestPath -Force
    }
}

# vswhere から lib.exe を見つける。
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe がありません: $vswhere"
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) {
    throw "Visual Studio C++ toolchain を見つけられません"
}
$vs = @($vs)[0]
$tv = (Get-Content (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt')).Trim()
$libexe = Join-Path $vs "VC\Tools\MSVC\$tv\bin\Hostx64\x64\lib.exe"
if (-not (Test-Path -LiteralPath $libexe -PathType Leaf)) {
    throw "lib.exe がありません: $libexe"
}

# 1) header を統合する。
Write-Host "==> dist/acs.h を生成"
python (Join-Path $PSScriptRoot 'amalgamate.py') --write
if ($LASTEXITCODE -ne 0) {
    throw "dist/acs.h の生成に失敗しました (exit=$LASTEXITCODE)"
}

# 2) 構成ごとに library を統合する。
$requiredAcsLibraries = @(
    'acs_foundation.lib', 'acs_threading.lib', 'acs_memory.lib',
    'acs_container.lib', 'acs_math.lib', 'acs_timing.lib', 'acs_platform.lib',
    'acs_ecs.lib', 'acs_event.lib', 'acs_asset.lib', 'acs_render.lib',
    'acs_app.lib', 'acs_audio.lib', 'acs_network.lib', 'acs_mvvm.lib',
    'acs_ui.lib', 'acs_easy.lib', 'acs_assetpack.lib',
    'acs_gameframework.lib', 'acs_collision.lib',
    'acs_third_party_ufbx.lib'
)
foreach ($cfg in $Configs) {
    $libdir = Join-Path $build $cfg
    if (-not (Test-Path $libdir)) {
        throw "要求された構成 $cfg は未ビルドです: $libdir"
    }
    $libs = @(
        Get-ChildItem (Join-Path $libdir 'acs_*.lib') -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne 'acs_test.lib' } |
            ForEach-Object { $_.FullName }
    )
    if ($libs.Count -eq 0) {
        throw "要求された構成 $cfg の ACS module library がありません: $libdir"
    }
    foreach ($requiredLibrary in $requiredAcsLibraries) {
        $requiredPath = Join-Path $libdir $requiredLibrary
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf) -or
            (Get-Item -LiteralPath $requiredPath).Length -le 0) {
            throw "要求された構成 $cfg の必須 library がありません: $requiredLibrary"
        }
    }
    foreach ($libraryPath in $libs) {
        if ((Get-Item -LiteralPath $libraryPath).Length -le 0) {
            throw "要求された構成 $cfg の library が空です: $libraryPath"
        }
    }

    $imgui = Join-Path $libdir 'imgui.lib'
    $acsImgui = Join-Path $libdir 'acs_imgui.lib'
    if (Test-Path -LiteralPath $acsImgui -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $imgui -PathType Leaf) -or
            (Get-Item -LiteralPath $imgui).Length -le 0) {
            throw "要求された構成 $cfg の imgui library がありません"
        }
        $libs += $imgui
    }
    $acsScripting = Join-Path $libdir 'acs_scripting.lib'
    $lua = Join-Path $libdir 'acs_third_party_lua.lib'
    if ((Test-Path -LiteralPath $acsScripting -PathType Leaf) -and
        (-not (Test-Path -LiteralPath $lua -PathType Leaf) -or
         (Get-Item -LiteralPath $lua).Length -le 0)) {
        throw "要求された構成 $cfg の Lua library がありません"
    }
    # mimalloc-static は _deps 配下に独自名 (mimalloc-debug.lib 等) で出力される。
    # acs_memory が mi_* を参照するため、acs.lib へ統合して consumer 側の
    # 個別library入力を増やさずに解決させる。Windows token API は生成した
    # acs.h の自動link契約から advapi32.lib を供給する。
    $mimalloc = Get-ChildItem (Join-Path $build "_deps\acs_mimalloc-build\$cfg") -Filter 'mimalloc*.lib' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch 'redirect' } | Select-Object -First 1
    if (-not $mimalloc) {
        throw "要求された構成 $cfg の mimalloc library がありません"
    }
    if ($mimalloc.Length -le 0) {
        throw "要求された構成 $cfg の mimalloc library が空です: $($mimalloc.FullName)"
    }
    $libs += $mimalloc.FullName
    $outdir = Join-Path $dist "lib\x64\$cfg"
    Assert-NoReparseAncestor $outdir
    New-Item -ItemType Directory -Force -Path $outdir | Out-Null
    Assert-NoReparseAncestor $outdir
    $outlib = Join-Path $outdir 'acs.lib'

    # Diligent backend + xxhash static library を acs.lib の隣へ置き、
    # consumer の #pragma comment(lib,...)（amalgamate.py の banner）が自動 link
    # できるようにする。FSky/FAtmosphere が呼ぶ CreateRhiComputePipeline と
    # device factory GetEngineFactoryD3D12 は Diligent 側だけに実装される。
    $diligentNames = $distributionAdjacentLibraryNames
    $depsRoot = Join-Path $build '_deps'
    $diligentSources = @{}
    foreach ($n in $diligentNames) {
        $src = Get-ChildItem $depsRoot -Recurse -File -Filter "$n.lib" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\$cfg\\" } | Select-Object -First 1
        if (-not $src) {
            throw "要求された構成 $cfg の Diligent/xxhash library がありません: $n.lib"
        }
        if ($src.Length -le 0) {
            throw "要求された構成 $cfg の Diligent/xxhash library が空です: $n.lib"
        }
        $diligentSources[$n] = $src.FullName
    }

    # 既存 acs.lib を失敗途中で壊さないよう同一 directory の一時出力へ統合し、
    # 成功と非空を確認してから原子的に公開する。response file も一意名にし、
    # 空白や日本語を含む path を quote + UTF-16 で保持する。
    $nonce = [Guid]::NewGuid().ToString('N')
    $temporaryOutlib = Join-Path $outdir ".acs.$nonce.lib"
    $rsp = Join-Path ([System.IO.Path]::GetTempPath()) "acs_merge_${cfg}_$nonce.rsp"
    try {
        $responseLines = @('/NOLOGO', ('/OUT:"{0}"' -f $temporaryOutlib))
        foreach ($libraryPath in $libs) {
            if ($libraryPath -match '["\r\n]') {
                throw "response file に安全に記録できない library path です: $libraryPath"
            }
            $responseLines += ('"{0}"' -f $libraryPath)
        }
        [System.IO.File]::WriteAllLines(
            $rsp, [string[]]$responseLines, [System.Text.Encoding]::Unicode)

        Write-Host "==> $($libs.Count) library を統合 -> $outlib"
        & $libexe "@$rsp" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$cfg の lib.exe 実行に失敗しました" }
        if (-not (Test-Path -LiteralPath $temporaryOutlib -PathType Leaf) -or
            (Get-Item -LiteralPath $temporaryOutlib).Length -le 0) {
            throw "lib.exe が有効な一時 library を生成しませんでした: $cfg"
        }
        Publish-FileAtomically $temporaryOutlib $outlib
        $temporaryOutlib = ''
    } finally {
        Remove-Item -LiteralPath $rsp -Force -ErrorAction SilentlyContinue
        if ($temporaryOutlib -and
            (Test-Path -LiteralPath $temporaryOutlib -PathType Leaf)) {
            Remove-Item -LiteralPath $temporaryOutlib -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host ("    {0:N1} MB" -f ((Get-Item -LiteralPath $outlib).Length/1MB))

    foreach ($n in $diligentNames) {
        Copy-Item $diligentSources[$n] (Join-Path $outdir "$n.lib") -Force
    }
    Write-Host "    Diligent/xxhash library $($diligentNames.Count)/$($diligentNames.Count) 件を同梱"
}

# 3) 両構成を同時生成した場合だけnamed manifestを原子的に公開する。
if ($isCompleteDistribution) {
    Write-Host "==> dist/$distributionManifestName を生成"
    Publish-AcsDistributionManifest $dist
} else {
    Write-Host "==> 単一構成のlocal staging完了（named manifestは未公開）"
}

# 4) 任意で dist/ を consumer 用の場所へ mirror する（例: C:\acs）。
if ($Deploy) {
    $Deploy = Assert-SafeDeployPath $Deploy
    Write-Host "==> dist/ を配置 -> $Deploy"
    Publish-MirroredDistribution $dist $Deploy
    Write-Host "    配置完了 (named manifest・file集合・size・SHA-256一致)"
}
Write-Host "完了"
