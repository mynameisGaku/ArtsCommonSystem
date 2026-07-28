# SPDX-License-Identifier: Apache-2.0
# 単一 header 版 ACS 配布物を dist/ へ再生成する:
#   - dist/acs.h（amalgamate.py で生成）
#   - dist/lib/x64/Debug/acs.lib と dist/lib/x64/Release/acs.lib
#
# 前提: 対象構成の engine を先に build しておく。例:
#   cmake --build acs/Intermediate/vs --config Debug   -j
#   cmake --build acs/Intermediate/vs --config Release -j
#
# 使い方:
#   powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1
#   ... -Configs Debug          # 1 構成だけ生成
#   ... -Deploy C:\acs          # consumer 用の場所へ dist/ も mirror
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

function Publish-FileAtomically([string]$TemporaryPath, [string]$DestinationPath) {
    $destinationDirectory = [System.IO.Path]::GetDirectoryName($DestinationPath)
    Assert-NoReparseAncestor $destinationDirectory
    Assert-NoReparseAncestor $DestinationPath
    if ((Test-Path -LiteralPath $DestinationPath) -and
        -not (Test-Path -LiteralPath $DestinationPath -PathType Leaf)) {
        throw "配布 library の出力先が通常 file ではありません: $DestinationPath"
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

function Get-DistributionFileManifest([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    if (-not (Test-Path -LiteralPath $normalizedRoot -PathType Container)) {
        throw "配布 manifest の root が通常 directory ではありません: $normalizedRoot"
    }

    $rootPrefix = $normalizedRoot + [System.IO.Path]::DirectorySeparatorChar
    $manifest = @{}
    foreach ($file in Get-ChildItem -LiteralPath $normalizedRoot -Recurse -File -Force) {
        Assert-NoReparseAncestor $file.FullName
        if (-not $file.FullName.StartsWith(
                $rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "配布 manifest の file が root の外です: $($file.FullName)"
        }
        $relativePath = $file.FullName.Substring($rootPrefix.Length)
        if ($manifest.ContainsKey($relativePath)) {
            throw "配布 manifest に重複する file path があります: $relativePath"
        }
        $manifest[$relativePath] = [pscustomobject]@{
            Length = $file.Length
            Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    return $manifest
}

function Assert-MirroredDistribution([string]$Source, [string]$Destination) {
    $sourceManifest = Get-DistributionFileManifest $Source
    $destinationManifest = Get-DistributionFileManifest $Destination
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
    } finally {
        Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "単一 header 配布 pipeline self-test passed"
}

if ($SelfTest) {
    Invoke-PipelineSelfTest
    return
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
    'acs_container.lib', 'acs_math.lib', 'acs_platform.lib',
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
    # リンク入力を増やさずに解決させる。
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
    $diligentNames = @(
        'Diligent-Archiver-static','Diligent-BasicPlatform','Diligent-Common',
        'Diligent-GraphicsAccessories','Diligent-GraphicsEngine','Diligent-GraphicsEngineD3D12-static',
        'Diligent-GraphicsEngineD3DBase','Diligent-GraphicsEngineNextGenBase','Diligent-GraphicsTools',
        'Diligent-Primitives','Diligent-ShaderTools','Diligent-Win32Platform','xxhash')
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

# 3) 任意で dist/ を consumer 用の場所へ mirror する（例: C:\acs）。
if ($Deploy) {
    $Deploy = Assert-SafeDeployPath $Deploy
    Write-Host "==> dist/ を配置 -> $Deploy"
    & robocopy $dist $Deploy /MIR /NJH /NJS /NDL /NFL /NP | Out-Null
    # robocopy の exit code は 0..7 が成功、8 以上が実エラー。
    if ($LASTEXITCODE -ge 8) { throw "robocopy に失敗しました ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Assert-MirroredDistribution $dist $Deploy
    Write-Host "    配置完了 (file 集合・size・SHA-256 一致)"
}
Write-Host "完了"
