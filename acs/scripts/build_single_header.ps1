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

# drive・share・extended・volume形式のrootを末尾separator付きで返す。
function Get-AcsPathRoot([string]$FullPath) {
    $rootPatterns = @(
        '^(\\\\\?\\Volume\{[0-9A-Fa-f-]+\})(?:[\\/]|$)',
        '^(\\\\\?\\UNC\\[^\\/]+\\[^\\/]+)(?:[\\/]|$)',
        '^(\\\\\?\\[A-Za-z]:)(?:[\\/]|$)',
        '^(\\\\[^\\/]+\\[^\\/]+)(?:[\\/]|$)'
    )
    foreach ($rootPattern in $rootPatterns) {
        $rootMatch = [regex]::Match($FullPath, $rootPattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($rootMatch.Success) {
            return $rootMatch.Groups[1].Value.TrimEnd('\', '/') + '\'
        }
    }

    $pathRoot = [System.IO.Path]::GetPathRoot($FullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot)) {
        throw "絶対pathのrootを特定できません: $FullPath"
    }
    return $pathRoot.TrimEnd('\', '/') + '\'
}

# root separatorを保持した絶対pathへ正規化する。
function Get-NormalizedFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "空の path は指定できません"
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $pathRoot = Get-AcsPathRoot $fullPath
    $trimmedFullPath = $fullPath.TrimEnd('\', '/')
    if ([string]::Equals($trimmedFullPath, $pathRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        return $pathRoot
    }
    return $trimmedFullPath
}

# drive・UNC・extended・volume rootのseparatorを失わず、1階層上のpathを返す。
function Get-AcsParentPath([string]$Path) {
    $normalizedPath = Get-NormalizedFullPath $Path
    $pathRoot = Get-AcsPathRoot $normalizedPath
    if ([string]::Equals($normalizedPath.TrimEnd('\', '/'), $pathRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $parentPath = [System.IO.Path]::GetDirectoryName($normalizedPath)
    if ([string]::IsNullOrWhiteSpace($parentPath)) {
        throw "絶対pathのparentを特定できません: $normalizedPath"
    }
    if ([string]::Equals($parentPath.TrimEnd('\', '/'), $pathRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        return $pathRoot
    }
    return Get-NormalizedFullPath $parentPath
}

function Test-ReparsePoint([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -ErrorAction Stop)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    return (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-NoReparseAncestor([string]$Path) {
    $current = Get-NormalizedFullPath $Path
    while ($current) {
        if (Test-ReparsePoint $current) {
            throw "reparse point を経由する path は安全のため拒否します: $current"
        }
        $current = Get-AcsParentPath $current
    }
}

if (-not ('AcsDistributionDirectoryPinNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class AcsDistributionDirectoryPinNative
{
    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        public uint Low;
        public uint High;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public FileTime CreationTime;
        public FileTime LastAccessTime;
        public FileTime LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation information);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file,
        System.Text.StringBuilder path,
        uint pathLength,
        uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetShortPathNameW(
        string longPath,
        System.Text.StringBuilder shortPath,
        uint pathLength);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateDirectoryW(
        string path,
        IntPtr securityAttributes);

    public static SafeFileHandle OpenDirectory(string path)
    {
        const uint readAttributes = 0x00000080;
        const uint shareReadAndWrite = 0x00000003;
        const uint openExisting = 3;
        const uint backupSemanticsAndOpenReparsePoint = 0x02200000;
        SafeFileHandle handle = CreateFileW(path, readAttributes, shareReadAndWrite, IntPtr.Zero, openExisting, backupSemanticsAndOpenReparsePoint, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "配布rootのdirectory handleを取得できません");
        }
        return handle;
    }

    public static string GetIdentity(SafeFileHandle handle)
    {
        ByHandleFileInformation information;
        if (!GetFileInformationByHandle(handle, out information))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "配布rootのdirectory identityを取得できません");
        }
        const uint reparsePoint = 0x00000400;
        if ((information.FileAttributes & reparsePoint) != 0)
        {
            throw new InvalidOperationException("配布root自体がreparse pointです");
        }
        return String.Format("{0:X8}:{1:X8}{2:X8}", information.VolumeSerialNumber, information.FileIndexHigh, information.FileIndexLow);
    }

    public static string GetFinalPath(SafeFileHandle handle)
    {
        System.Text.StringBuilder path = new System.Text.StringBuilder(32768);
        uint length = GetFinalPathNameByHandleW(handle, path, (uint)path.Capacity, 0);
        if (length == 0 || length >= path.Capacity)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "配布rootの物理pathを取得できません");
        }
        return path.ToString();
    }

    public static string TryGetShortPath(string longPath)
    {
        System.Text.StringBuilder path = new System.Text.StringBuilder(32768);
        uint length = GetShortPathNameW(longPath, path, (uint)path.Capacity);
        if (length == 0 || length >= path.Capacity)
        {
            return String.Empty;
        }
        return path.ToString();
    }

    public static bool TryCreateDirectory(string path)
    {
        if (CreateDirectoryW(path, IntPtr.Zero))
        {
            return true;
        }
        int error = Marshal.GetLastWin32Error();
        const int alreadyExists = 183;
        if (error == alreadyExists && System.IO.Directory.Exists(path))
        {
            return false;
        }
        throw new Win32Exception(error, "配布directoryを作成できません");
    }
}
'@
}

# 物理directoryと未作成部分を表す文字列から、大小文字非依存のmutex名を作る。
function Get-AcsDistributionMutexNameForKey([string]$Key) {
    $rootBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Key.ToUpperInvariant())
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $rootHash = [System.BitConverter]::ToString($sha256.ComputeHash($rootBytes)).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
    return "Global\ACS.Distribution.Publish.$rootHash"
}

# 名前付きmutexを待機せず取得し、競合時は変更前に拒否する。
function Enter-AcsDistributionNamedMutex([string]$MutexName, [string]$Operation, [string]$Root) {
    $mutex = $null
    $acquired = $false
    $recoveredAbandoned = $false
    try {
        $mutex = [System.Threading.Mutex]::new($false, $MutexName)
        try {
            $acquired = $mutex.WaitOne(0)
        } catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
            $recoveredAbandoned = $true
        }
        if (-not $acquired) {
            throw "同じ配布rootの別writerが実行中です: operation=$Operation root=$Root"
        }
        if ($recoveredAbandoned) {
            Write-Warning "異常終了した配布writerの排他を回収しました: operation=$Operation root=$Root"
        }
        return [pscustomobject]@{
            Operation = $Operation
            Name = $MutexName
            Mutex = $mutex
            Acquired = $true
            RecoveredAbandoned = $recoveredAbandoned
        }
    } catch {
        if ($mutex) {
            if ($acquired) {
                try {
                    $mutex.ReleaseMutex()
                } catch [System.ApplicationException] {
                    # 所有権を確認できない場合もhandleを閉じ、元の失敗を維持する。
                }
            }
            $mutex.Dispose()
        }
        throw
    }
}

# 現在のthreadが所有する名前付きmutexを解放する。
function Exit-AcsDistributionNamedMutex($Lock) {
    if (-not $Lock -or -not $Lock.Acquired) { return }
    try {
        $Lock.Mutex.ReleaseMutex()
        $Lock.Acquired = $false
    } finally {
        $Lock.Mutex.Dispose()
    }
}

# root自体の削除・差替えを処理中だけ拒否し、file identityを保存する。
function Open-AcsDistributionDirectoryPin([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    if (-not (Test-Path -LiteralPath $normalizedRoot -PathType Container)) {
        throw "配布rootが通常directoryではありません: $normalizedRoot"
    }
    $handle = [AcsDistributionDirectoryPinNative]::OpenDirectory($normalizedRoot)
    try {
        $identity = [AcsDistributionDirectoryPinNative]::GetIdentity($handle)
        return [pscustomobject]@{
            Root = $normalizedRoot
            Identity = $identity
            FinalPath = [AcsDistributionDirectoryPinNative]::GetFinalPath($handle)
            Handle = $handle
        }
    } catch {
        $handle.Dispose()
        throw
    }
}

# rootの親directory identityと実体名から、作成前後で共通の排他情報を作る。
function Get-AcsDistributionNamespaceDescriptor([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    $rootPin = $null
    $parentPin = $null
    try {
        if (Test-Path -LiteralPath $normalizedRoot) {
            if (-not (Test-Path -LiteralPath $normalizedRoot -PathType Container)) {
                throw "配布rootが通常directoryではありません: $normalizedRoot"
            }
            $rootPin = Open-AcsDistributionDirectoryPin $normalizedRoot
            $physicalParent = Get-AcsParentPath $rootPin.FinalPath
            $remainingPath = [System.IO.Path]::GetFileName($rootPin.FinalPath)
            if ([string]::IsNullOrWhiteSpace($physicalParent) -or [string]::IsNullOrWhiteSpace($remainingPath)) {
                throw "drive rootは配布rootに指定できません: $normalizedRoot"
            }
            $parentPin = Open-AcsDistributionDirectoryPin $physicalParent
        } else {
            $remainingParts = [System.Collections.Generic.List[string]]::new()
            $existingParent = $normalizedRoot
            while (-not (Test-Path -LiteralPath $existingParent)) {
                $leaf = [System.IO.Path]::GetFileName($existingParent)
                if ([string]::IsNullOrWhiteSpace($leaf)) {
                    throw "配布rootの既存parentを特定できません: $normalizedRoot"
                }
                $remainingParts.Insert(0, $leaf)
                $existingParent = Get-AcsParentPath $existingParent
            }
            if (-not (Test-Path -LiteralPath $existingParent -PathType Container)) {
                throw "配布rootの既存parentが通常directoryではありません: $existingParent"
            }
            $parentPin = Open-AcsDistributionDirectoryPin $existingParent
            $remainingPath = [string]::Join('\', [string[]]$remainingParts)
        }
        $namespaceKeys = [System.Collections.Generic.List[string]]::new()
        $descendantPath = $remainingPath
        $currentAncestorPin = $parentPin
        while ($currentAncestorPin) {
            $namespaceKeys.Add("NAMESPACE:$($currentAncestorPin.Identity):$($descendantPath.ToUpperInvariant())")
            $higherAncestorPath = Get-AcsParentPath $currentAncestorPin.FinalPath
            $currentAncestorLeaf = [System.IO.Path]::GetFileName($currentAncestorPin.FinalPath)
            if ([string]::IsNullOrWhiteSpace($higherAncestorPath) -or [string]::IsNullOrWhiteSpace($currentAncestorLeaf)) {
                break
            }
            $descendantPath = Join-Path $currentAncestorLeaf $descendantPath
            if ($currentAncestorPin -ne $parentPin) {
                Close-AcsDistributionDirectoryPin $currentAncestorPin
                $currentAncestorPin = $null
            }
            $currentAncestorPin = Open-AcsDistributionDirectoryPin $higherAncestorPath
        }
        if ($currentAncestorPin -and $currentAncestorPin -ne $parentPin) {
            Close-AcsDistributionDirectoryPin $currentAncestorPin
            $currentAncestorPin = $null
        }
        $namespaceMutexNames = @($namespaceKeys | ForEach-Object { Get-AcsDistributionMutexNameForKey $_ } | Sort-Object -Unique)
        return [pscustomobject]@{
            Root = $normalizedRoot
            ParentPin = $parentPin
            RootPin = $rootPin
            NamespaceKeys = [string[]]$namespaceKeys
            NamespaceMutexNames = [string[]]$namespaceMutexNames
            NamespaceMutexName = Get-AcsDistributionMutexNameForKey $namespaceKeys[0]
        }
    } catch {
        if ($currentAncestorPin -and $currentAncestorPin -ne $parentPin) {
            Close-AcsDistributionDirectoryPin $currentAncestorPin
        }
        Close-AcsDistributionDirectoryPin $rootPin
        Close-AcsDistributionDirectoryPin $parentPin
        throw
    }
}

# 物理的に同じ配布rootで共通になる名前空間mutex名を返す。
function Get-AcsDistributionOperationMutexName([string]$Root) {
    $descriptor = Get-AcsDistributionNamespaceDescriptor $Root
    try {
        return $descriptor.NamespaceMutexName
    } finally {
        Close-AcsDistributionDirectoryPin $descriptor.RootPin
        Close-AcsDistributionDirectoryPin $descriptor.ParentPin
    }
}

# 同じ物理配布rootへのwriterを待機せず排他し、競合時は変更前に拒否する。
function Enter-AcsDistributionOperationLock([string]$Root, [string]$Operation) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $textMutexName = Get-AcsDistributionMutexNameForKey ("PATH:" + $normalizedRoot.Replace('/', '\'))
    $textLock = Enter-AcsDistributionNamedMutex $textMutexName $Operation $normalizedRoot
    $descriptor = $null
    $namespaceLocks = [System.Collections.Generic.List[object]]::new()
    $identityLock = $null
    try {
        $descriptor = Get-AcsDistributionNamespaceDescriptor $normalizedRoot
        foreach ($namespaceMutexName in $descriptor.NamespaceMutexNames) {
            $namespaceLocks.Add((Enter-AcsDistributionNamedMutex $namespaceMutexName $Operation $normalizedRoot))
        }
        if ($descriptor.RootPin) {
            $identityMutexName = Get-AcsDistributionMutexNameForKey ("IDENTITY:" + $descriptor.RootPin.Identity)
            $identityLock = Enter-AcsDistributionNamedMutex $identityMutexName $Operation $normalizedRoot
        }
        Exit-AcsDistributionNamedMutex $textLock
        $textLock = $null
        return [pscustomobject]@{
            Root = $normalizedRoot
            Operation = $Operation
            NamespaceKeys = $descriptor.NamespaceKeys
            NamespaceLocks = [object[]]$namespaceLocks
            IdentityLock = $identityLock
            ParentPin = $descriptor.ParentPin
            RootPin = $descriptor.RootPin
            RecoveredAbandoned = ((@($namespaceLocks | Where-Object { $_.RecoveredAbandoned }).Count -ne 0) -or ($identityLock -and $identityLock.RecoveredAbandoned))
        }
    } catch {
        Exit-AcsDistributionNamedMutex $identityLock
        for ($namespaceLockIndex = $namespaceLocks.Count - 1; $namespaceLockIndex -ge 0; --$namespaceLockIndex) {
            Exit-AcsDistributionNamedMutex $namespaceLocks[$namespaceLockIndex]
        }
        if ($descriptor) {
            Close-AcsDistributionDirectoryPin $descriptor.RootPin
            Close-AcsDistributionDirectoryPin $descriptor.ParentPin
        }
        Exit-AcsDistributionNamedMutex $textLock
        throw
    }
}

# 未作成rootを生成した直後、名前空間lockを保持したままroot identity lockを重ねる。
function Complete-AcsDistributionOperationLock($Lock) {
    if ($Lock.RootPin) { return }
    $descriptor = Get-AcsDistributionNamespaceDescriptor $Lock.Root
    $identityLock = $null
    $committedIdentityLock = $false
    try {
        if (-not $descriptor.RootPin) {
            throw "作成後の配布root identityを取得できません: $($Lock.Root)"
        }
        $identityMutexName = Get-AcsDistributionMutexNameForKey ("IDENTITY:" + $descriptor.RootPin.Identity)
        $identityLock = Enter-AcsDistributionNamedMutex $identityMutexName $Lock.Operation $Lock.Root
        $Lock.RootPin = $descriptor.RootPin
        $Lock.IdentityLock = $identityLock
        $Lock.RecoveredAbandoned = ($Lock.RecoveredAbandoned -or $identityLock.RecoveredAbandoned)
        $descriptor.RootPin = $null
        $committedIdentityLock = $true
    } finally {
        if (-not $committedIdentityLock) {
            Exit-AcsDistributionNamedMutex $identityLock
        }
        Close-AcsDistributionDirectoryPin $descriptor.RootPin
        Close-AcsDistributionDirectoryPin $descriptor.ParentPin
    }
}

# このprocessがcreate-onlyで作成した空の通常directoryだけを逆順に戻す。
function Remove-AcsCreatedEmptyDirectoryChain([string[]]$CreatedDirectories) {
    for ($directoryIndex = $CreatedDirectories.Count - 1; $directoryIndex -ge 0; --$directoryIndex) {
        $createdDirectory = $CreatedDirectories[$directoryIndex]
        try {
            if (-not (Test-Path -LiteralPath $createdDirectory -PathType Container) -or (Test-ReparsePoint $createdDirectory)) {
                continue
            }
            $firstChild = Get-ChildItem -LiteralPath $createdDirectory -Force -ErrorAction Stop | Select-Object -First 1
            if ($firstChild) {
                continue
            }
            [System.IO.Directory]::Delete($createdDirectory, $false)
        } catch {
            Write-Warning "作成途中の空directoryをrollbackできませんでした: $createdDirectory"
        }
    }
}

# 未作成部分を親からcreate-onlyで生成し、このprocessが作成したdirectoryを返す。
function Ensure-AcsDistributionDirectoryChain([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    Assert-NoReparseAncestor $normalizedRoot
    $missingParts = [System.Collections.Generic.List[string]]::new()
    $existingAncestor = $normalizedRoot
    while (-not (Test-Path -LiteralPath $existingAncestor)) {
        $missingLeaf = [System.IO.Path]::GetFileName($existingAncestor)
        $existingAncestor = Get-AcsParentPath $existingAncestor
        if ([string]::IsNullOrWhiteSpace($missingLeaf) -or [string]::IsNullOrWhiteSpace($existingAncestor)) {
            throw "配布rootの既存parentを特定できません: $normalizedRoot"
        }
        $missingParts.Insert(0, $missingLeaf)
    }
    if (-not (Test-Path -LiteralPath $existingAncestor -PathType Container)) {
        throw "配布rootの既存parentが通常directoryではありません: $existingAncestor"
    }

    $createdDirectories = [System.Collections.Generic.List[string]]::new()
    $currentDirectory = $existingAncestor
    try {
        foreach ($missingPart in $missingParts) {
            Assert-NoReparseAncestor $currentDirectory
            $currentDirectory = Join-Path $currentDirectory $missingPart
            if ([AcsDistributionDirectoryPinNative]::TryCreateDirectory($currentDirectory)) {
                $createdDirectories.Add($currentDirectory)
            }
            Assert-NoReparseAncestor $currentDirectory
            if (-not (Test-Path -LiteralPath $currentDirectory -PathType Container)) {
                throw "作成後の配布pathが通常directoryではありません: $currentDirectory"
            }
        }
        return [string[]]$createdDirectories
    } catch {
        Remove-AcsCreatedEmptyDirectoryChain ([string[]]$createdDirectories)
        throw
    }
}

# directory確保後は作成者に関係なくroot identity排他を完成し、移行失敗時だけ自作の空chainを戻す。
function Ensure-AcsDistributionOperationRoot($Lock, [scriptblock]$BeforeEnsure, [scriptblock]$BeforeIdentityMigration) {
    if ($BeforeEnsure) {
        & $BeforeEnsure $Lock.Root | Out-Null
    }
    $createdDirectories = @(Ensure-AcsDistributionDirectoryChain $Lock.Root)
    try {
        if ($BeforeIdentityMigration) {
            & $BeforeIdentityMigration $Lock ([string[]]$createdDirectories) | Out-Null
        }
        if (-not $Lock.RootPin) {
            Complete-AcsDistributionOperationLock $Lock
        }
        return [string[]]$createdDirectories
    } catch {
        Remove-AcsCreatedEmptyDirectoryChain ([string[]]$createdDirectories)
        throw
    }
}

# deploy destinationのensureとidentity移行を同じ失敗rollback契約で行う。
function Ensure-AcsDeployDestinationOperationRoot($Lock, [scriptblock]$BeforeEnsure, [scriptblock]$BeforeIdentityMigration) {
    return @(Ensure-AcsDistributionOperationRoot $Lock $BeforeEnsure $BeforeIdentityMigration)
}

# source generation rootのensureとidentity移行を同じ失敗rollback契約で行う。
function Ensure-AcsSourceGenerationOperationRoot($Lock, [scriptblock]$BeforeEnsure, [scriptblock]$BeforeIdentityMigration) {
    return @(Ensure-AcsDistributionOperationRoot $Lock $BeforeEnsure $BeforeIdentityMigration)
}

# 現在のthreadが所有する配布排他とdirectory pinを解放する。
function Exit-AcsDistributionOperationLock($Lock) {
    if (-not $Lock) { return }
    Close-AcsDistributionDirectoryPin $Lock.RootPin
    Close-AcsDistributionDirectoryPin $Lock.ParentPin
    try {
        Exit-AcsDistributionNamedMutex $Lock.IdentityLock
    } finally {
        for ($namespaceLockIndex = $Lock.NamespaceLocks.Count - 1; $namespaceLockIndex -ge 0; --$namespaceLockIndex) {
            Exit-AcsDistributionNamedMutex $Lock.NamespaceLocks[$namespaceLockIndex]
        }
    }
}

# pathが処理開始時に固定した同じdirectoryを指すことを確認する。
function Assert-AcsDistributionDirectoryPin($Pin) {
    Assert-NoReparseAncestor $Pin.Root
    $currentHandle = [AcsDistributionDirectoryPinNative]::OpenDirectory($Pin.Root)
    try {
        $currentIdentity = [AcsDistributionDirectoryPinNative]::GetIdentity($currentHandle)
    } finally {
        $currentHandle.Dispose()
    }
    if ($currentIdentity -cne $Pin.Identity) {
        throw "配布rootのdirectory identityが処理中に変化しました: $($Pin.Root)"
    }
}

# 配布rootを固定していたdirectory handleを閉じる。
function Close-AcsDistributionDirectoryPin($Pin) {
    if ($Pin -and $Pin.Handle) {
        try {
            $Pin.Handle.Dispose()
        } catch {
            Write-Warning "配布rootのdirectory handleを閉じられませんでした: $($Pin.Root)"
        }
    }
}

# path自体または最深既存ancestorからvolume rootまでをpinし、物理identity列を返す。
function Get-AcsPhysicalPathDescriptor([string]$Path) {
    $normalizedPath = Get-NormalizedFullPath $Path
    Assert-NoReparseAncestor $normalizedPath
    $remainingParts = [System.Collections.Generic.List[string]]::new()
    $existingAncestor = $normalizedPath
    while (-not (Test-Path -LiteralPath $existingAncestor -ErrorAction Stop)) {
        $remainingLeaf = [System.IO.Path]::GetFileName($existingAncestor)
        $existingParent = Get-AcsParentPath $existingAncestor
        if ([string]::IsNullOrWhiteSpace($remainingLeaf) -or [string]::IsNullOrWhiteSpace($existingParent)) {
            throw "pathの最深既存ancestorを特定できません: $normalizedPath"
        }
        $remainingParts.Insert(0, $remainingLeaf)
        $existingAncestor = $existingParent
    }
    if (-not (Test-Path -LiteralPath $existingAncestor -PathType Container -ErrorAction Stop)) {
        throw "pathの最深既存ancestorが通常directoryではありません: $existingAncestor"
    }

    $ancestorPins = [System.Collections.Generic.List[object]]::new()
    try {
        $currentAncestorPath = $existingAncestor
        while ($currentAncestorPath) {
            $ancestorPin = Open-AcsDistributionDirectoryPin $currentAncestorPath
            $ancestorPins.Add($ancestorPin)
            $currentAncestorPath = Get-AcsParentPath $ancestorPin.FinalPath
        }
        if ($ancestorPins.Count -eq 0) {
            throw "pathの物理ancestor identityを取得できません: $normalizedPath"
        }
        return [pscustomobject]@{
            Root = $normalizedPath
            Exists = ($remainingParts.Count -eq 0)
            ExistingAncestorIdentity = $ancestorPins[0].Identity
            RemainingParts = [string[]]$remainingParts
            AncestorIdentities = [string[]]@($ancestorPins | ForEach-Object { $_.Identity })
            AncestorPins = [object[]]$ancestorPins
        }
    } catch {
        for ($ancestorIndex = $ancestorPins.Count - 1; $ancestorIndex -ge 0; --$ancestorIndex) {
            Close-AcsDistributionDirectoryPin $ancestorPins[$ancestorIndex]
        }
        throw
    }
}

# physical descriptorが保持する全ancestor pinを逆順に解放する。
function Close-AcsPhysicalPathDescriptor($Descriptor) {
    if (-not $Descriptor) { return }
    for ($ancestorIndex = $Descriptor.AncestorPins.Count - 1; $ancestorIndex -ge 0; --$ancestorIndex) {
        Close-AcsDistributionDirectoryPin $Descriptor.AncestorPins[$ancestorIndex]
    }
}

# 大小文字を区別せず、先頭component列が同じnamespaceを表すか確認する。
function Test-AcsPathComponentPrefix([string[]]$Prefix, [string[]]$Path) {
    if ($Prefix.Count -gt $Path.Count) { return $false }
    for ($componentIndex = 0; $componentIndex -lt $Prefix.Count; ++$componentIndex) {
        if (-not [string]::Equals($Prefix[$componentIndex], $Path[$componentIndex], [System.StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
    }
    return $true
}

# 2つのpathが同じ物理directory、ancestor、descendantの関係にあるか確認する。
function Test-AcsPhysicalPathOverlap($Left, $Right) {
    if ($Left.Exists -and @($Right.AncestorIdentities | Where-Object { [string]::Equals($_, $Left.ExistingAncestorIdentity, [System.StringComparison]::Ordinal) }).Count -ne 0) {
        return $true
    }
    if ($Right.Exists -and @($Left.AncestorIdentities | Where-Object { [string]::Equals($_, $Right.ExistingAncestorIdentity, [System.StringComparison]::Ordinal) }).Count -ne 0) {
        return $true
    }
    if ([string]::Equals($Left.ExistingAncestorIdentity, $Right.ExistingAncestorIdentity, [System.StringComparison]::Ordinal)) {
        return (Test-AcsPathComponentPrefix $Left.RemainingParts $Right.RemainingParts) -or
            (Test-AcsPathComponentPrefix $Right.RemainingParts $Left.RemainingParts)
    }
    return $false
}

function Assert-SafeDeployPath([string]$Path) {
    $full = Get-NormalizedFullPath $Path
    $rootPath = Get-AcsPathRoot $full
    if ([string]::Equals($full.TrimEnd('\', '/'), $rootPath.TrimEnd('\', '/'),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "drive root へ /MIR deploy はできません: $full"
    }

    $blockedPaths = @($repo, (Join-Path $repo 'acs'), $build, $dist)
    $candidateDescriptor = $null
    try {
        $candidateDescriptor = Get-AcsPhysicalPathDescriptor $full
        foreach ($blockedPath in $blockedPaths) {
            $blockedDescriptor = $null
            try {
                $blockedDescriptor = Get-AcsPhysicalPathDescriptor $blockedPath
                if (Test-AcsPhysicalPathOverlap $candidateDescriptor $blockedDescriptor) {
                    throw "source/build/dist と物理的に重なる path へ deploy はできません: $full"
                }
            } finally {
                Close-AcsPhysicalPathDescriptor $blockedDescriptor
            }
        }
    } finally {
        Close-AcsPhysicalPathDescriptor $candidateDescriptor
    }
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
    $destinationDirectory = Get-AcsParentPath $DestinationPath
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
    $operationLock = Enter-AcsDistributionOperationLock $normalizedRoot 'manifest-publish'
    $directoryPin = $null
    try {
        Assert-NoReparseSubtree $normalizedRoot
        $directoryPin = Open-AcsDistributionDirectoryPin $normalizedRoot
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
        Assert-AcsDistributionDirectoryPin $directoryPin
        Assert-AcsDistributionManifest $normalizedRoot
        $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToUpperInvariant()
        Write-Host "    配布manifest完了 (SHA-256=$manifestHash)"
    } finally {
        Close-AcsDistributionDirectoryPin $directoryPin
        Exit-AcsDistributionOperationLock $operationLock
    }
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

# lock拒否の前後で配布tree全fileの集合、size、SHA-256を比較する。
function Get-DistributionTreeStateSignature([string]$Root) {
    $manifest = Get-DistributionFileManifest $Root
    $relativePaths = [string[]]@($manifest.Keys)
    [System.Array]::Sort($relativePaths, [System.StringComparer]::Ordinal)
    $stateLines = [System.Collections.Generic.List[string]]::new()
    foreach ($relativePath in $relativePaths) {
        $entry = $manifest[$relativePath]
        $stateLines.Add("$relativePath|$($entry.Length)|$($entry.Sha256.ToUpperInvariant())")
    }
    return [string]::Join("`n", [string[]]$stateLines)
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
    $sourceOperationLock = Enter-AcsDistributionOperationLock $normalizedSource 'deploy-source-read'
    $destinationOperationLock = $null
    $sourceDirectoryPin = $null
    $destinationDirectoryPin = $null
    try {
        $destinationOperationLock = Enter-AcsDistributionOperationLock $normalizedDestination 'deploy-destination-publish'
        Assert-NoReparseSubtree $normalizedSource
        Assert-AcsDistributionManifest $normalizedSource
        $sourceDirectoryPin = Open-AcsDistributionDirectoryPin $normalizedSource

        Assert-NoReparseAncestor $normalizedDestination
        Ensure-AcsDeployDestinationOperationRoot $destinationOperationLock | Out-Null
        Assert-NoReparseSubtree $normalizedDestination
        $destinationDirectoryPin = Open-AcsDistributionDirectoryPin $normalizedDestination
        if ($sourceDirectoryPin.Identity -ceq $destinationDirectoryPin.Identity) {
            throw "sourceとdeploy先が同じ物理directoryです: source=$normalizedSource destination=$normalizedDestination"
        }

        & robocopy $normalizedSource $normalizedDestination /MIR /XF $distributionManifestName /XJ /R:0 /W:0 /NJH /NJS /NDL /NFL /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopyに失敗しました ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0

        Assert-AcsDistributionDirectoryPin $sourceDirectoryPin
        Assert-AcsDistributionDirectoryPin $destinationDirectoryPin
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

        Assert-AcsDistributionDirectoryPin $sourceDirectoryPin
        Assert-AcsDistributionDirectoryPin $destinationDirectoryPin
        Assert-NoReparseSubtree $normalizedDestination
        Assert-MirroredDistribution $normalizedSource $normalizedDestination
        Assert-AcsDistributionManifest $normalizedDestination
    } finally {
        Close-AcsDistributionDirectoryPin $destinationDirectoryPin
        Close-AcsDistributionDirectoryPin $sourceDirectoryPin
        try {
            Exit-AcsDistributionOperationLock $destinationOperationLock
        } finally {
            Exit-AcsDistributionOperationLock $sourceOperationLock
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

# 別processで指定mutexを保持し、並行writer拒否とabandoned回収を検証する。
function Start-AcsNamedMutexHolder([string]$MutexName, [string]$ReadyDirectory) {
    $readyPath = Join-Path $ReadyDirectory ('.acs-operation-lock-ready-' + [Guid]::NewGuid().ToString('N'))
    $escapedMutexName = $MutexName.Replace("'", "''")
    $escapedReadyPath = $readyPath.Replace("'", "''")
    $holderCommand = @"
`$mutex = [System.Threading.Mutex]::new(`$false, '$escapedMutexName')
`$owned = `$false
try {
    `$owned = `$mutex.WaitOne()
    [System.IO.File]::WriteAllText('$escapedReadyPath', 'ready')
    while (`$true) { [System.Threading.Thread]::Sleep(100) }
} finally {
    if (`$owned) { `$mutex.ReleaseMutex() }
    `$mutex.Dispose()
}
"@
    $encodedCommand = [System.Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($holderCommand))
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encodedCommand) -PassThru -WindowStyle Hidden
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $readyPath -PathType Leaf)) {
        if ($process.HasExited) {
            throw "operation lock保持processが準備前に終了しました: exit=$($process.ExitCode)"
        }
        if ([DateTime]::UtcNow -ge $readyDeadline) {
            $process.Kill()
            $process.WaitForExit()
            throw "operation lock保持processの準備がtimeoutしました"
        }
        [System.Threading.Thread]::Sleep(25)
    }
    return [pscustomobject]@{
        Process = $process
        MutexName = $MutexName
        ReadyPath = $readyPath
    }
}

# 別processでoperation namespace mutexを保持する。
function Start-AcsOperationLockHolder([string]$Root, [string]$ReadyDirectory) {
    return Start-AcsNamedMutexHolder (Get-AcsDistributionOperationMutexName $Root) $ReadyDirectory
}

# test用の保持processを強制終了し、mutexをabandoned状態へ移す。
function Stop-AcsOperationLockHolder($Holder) {
    if (-not $Holder) { return }
    try {
        if (-not $Holder.Process.HasExited) {
            $Holder.Process.Kill()
            $Holder.Process.WaitForExit()
        }
    } catch [System.InvalidOperationException] {
        # 終了確認とKillの間にprocessが終了した場合は既に排他が解放されている。
    }
    Remove-Item -LiteralPath $Holder.ReadyPath -Force -ErrorAction SilentlyContinue
}

# 同じ物理rootを指す別名が、排他と自己deploy拒否を共有することを確認する。
function Assert-AcsDistributionAliasSafety([string]$CanonicalRoot, [string]$AliasRoot, [string]$ReadyDirectory, [string]$Name) {
    $canonicalMutexName = Get-AcsDistributionOperationMutexName $CanonicalRoot
    $aliasMutexName = Get-AcsDistributionOperationMutexName $AliasRoot
    if ($canonicalMutexName -cne $aliasMutexName) {
        throw "self-testの物理alias mutexが一致しません: $Name"
    }
    $canonicalPin = Open-AcsDistributionDirectoryPin $CanonicalRoot
    $aliasPin = Open-AcsDistributionDirectoryPin $AliasRoot
    try {
        if ($canonicalPin.Identity -cne $aliasPin.Identity) {
            throw "self-testの物理alias identityが一致しません: $Name"
        }
    } finally {
        Close-AcsDistributionDirectoryPin $aliasPin
        Close-AcsDistributionDirectoryPin $canonicalPin
    }
    $stateBefore = Get-DistributionTreeStateSignature $CanonicalRoot
    $holder = $null
    try {
        $holder = Start-AcsOperationLockHolder $AliasRoot $ReadyDirectory
        Assert-ExpectedFailure {
            Publish-AcsDistributionManifest $CanonicalRoot
        } "$Name alias writer"
        if ($stateBefore -cne (Get-DistributionTreeStateSignature $CanonicalRoot)) {
            throw "self-testの物理alias lock拒否がtreeを変更しました: $Name"
        }
    } finally {
        Stop-AcsOperationLockHolder $holder
    }
    Assert-ExpectedFailure {
        Publish-MirroredDistribution $CanonicalRoot $AliasRoot
    } "$Name self deploy"
    if ($stateBefore -cne (Get-DistributionTreeStateSignature $CanonicalRoot)) {
        throw "self-testの物理alias自己deploy拒否がtreeを変更しました: $Name"
    }
}

# self-test専用の未使用driveへdirectoryを割り当て、SUBST aliasを返す。
function New-AcsSubstAlias([string]$Target) {
    foreach ($driveLetter in @('Z', 'Y', 'X', 'V', 'U', 'T', 'S', 'R', 'Q', 'P')) {
        $driveRoot = "${driveLetter}:\"
        if (Test-Path -LiteralPath $driveRoot) { continue }
        & subst.exe "${driveLetter}:" $Target | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $driveRoot -PathType Container)) {
            $global:LASTEXITCODE = 0
            return $driveRoot
        }
        $global:LASTEXITCODE = 0
    }
    throw "self-test用SUBST driveを確保できません"
}

# canonical root配下のpathを、同じ物理directoryを指すalias root配下へ写す。
function ConvertTo-AcsRootAliasPath([string]$Path, [string]$CanonicalRoot, [string]$AliasRoot) {
    $normalizedPath = Get-NormalizedFullPath $Path
    $normalizedCanonicalRoot = Get-NormalizedFullPath $CanonicalRoot
    $normalizedAliasRoot = Get-NormalizedFullPath $AliasRoot
    if ([string]::Equals($normalizedPath, $normalizedCanonicalRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $normalizedAliasRoot
    }
    $canonicalPrefix = $normalizedCanonicalRoot.TrimEnd('\', '/') + '\'
    if (-not $normalizedPath.StartsWith($canonicalPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "self-testのalias変換元がcanonical root配下ではありません: path=$normalizedPath root=$normalizedCanonicalRoot"
    }
    return Get-NormalizedFullPath (Join-Path $normalizedAliasRoot $normalizedPath.Substring($canonicalPrefix.Length))
}

# drive pathをlocalhost管理共有経由のUNC aliasへ変換する。
function ConvertTo-AcsLocalhostAdminSharePath([string]$Path) {
    $normalizedPath = Get-NormalizedFullPath $Path
    $pathRoot = Get-AcsPathRoot $normalizedPath
    if ($pathRoot -notmatch '^([A-Za-z]):\\$') {
        throw "self-testのlocalhost UNC変換元がdrive pathではありません: $normalizedPath"
    }
    return Get-NormalizedFullPath ("\\localhost\$($Matches[1])$\" + $normalizedPath.Substring($pathRoot.Length))
}

# 未作成末尾を維持したまま、最深既存ancestorから8.3短縮名aliasを作る。
function Get-AcsShortAliasPath([string]$Path) {
    $normalizedPath = Get-NormalizedFullPath $Path
    $remainingParts = [System.Collections.Generic.List[string]]::new()
    $existingAncestor = $normalizedPath
    while (-not (Test-Path -LiteralPath $existingAncestor -ErrorAction Stop)) {
        $remainingLeaf = [System.IO.Path]::GetFileName($existingAncestor)
        $existingParent = Get-AcsParentPath $existingAncestor
        if ([string]::IsNullOrWhiteSpace($remainingLeaf) -or [string]::IsNullOrWhiteSpace($existingParent)) {
            return ''
        }
        $remainingParts.Insert(0, $remainingLeaf)
        $existingAncestor = $existingParent
    }
    $shortPath = [AcsDistributionDirectoryPinNative]::TryGetShortPath($existingAncestor)
    if ([string]::IsNullOrWhiteSpace($shortPath)) { return '' }
    foreach ($remainingPart in $remainingParts) {
        $shortPath = Join-Path $shortPath $remainingPart
    }
    return $shortPath.TrimEnd('\', '/')
}

# alias拒否の前後でtree、代表payload、named manifestのbyte identityを比較する。
function Get-AcsDistributionFixtureSnapshot([string]$Root) {
    $normalizedRoot = Get-NormalizedFullPath $Root
    $treeState = Get-DistributionTreeStateSignature $normalizedRoot
    $payloadHash = (Get-FileHash -LiteralPath (Join-Path $normalizedRoot 'acs.h') -Algorithm SHA256).Hash.ToUpperInvariant()
    $manifestHash = (Get-FileHash -LiteralPath (Join-Path $normalizedRoot $distributionManifestName) -Algorithm SHA256).Hash.ToUpperInvariant()
    return "$treeState`nPAYLOAD=$payloadHash`nMANIFEST=$manifestHash"
}

# volume root直下を作れない環境だけ、pure descriptor検証へ安全にfallbackする。
function Test-AcsAccessDeniedError($ErrorRecord) {
    $currentException = $ErrorRecord.Exception
    while ($currentException) {
        if ($currentException -is [System.UnauthorizedAccessException]) { return $true }
        if ($currentException -is [System.ComponentModel.Win32Exception] -and $currentException.NativeErrorCode -eq 5) { return $true }
        $currentException = $currentException.InnerException
    }
    return $false
}

function Invoke-PipelineSelfTest {
    $driveRoot = Get-AcsPathRoot (Get-NormalizedFullPath $repo)
    if ($driveRoot -notmatch '^([A-Za-z]):\\$') {
        throw "self-testのdrive rootを特定できません: $driveRoot"
    }
    $driveLetter = $Matches[1]
    $shareRoot = '\\localhost\{0}$\' -f $driveLetter
    $extendedDriveRoot = '\\?\{0}' -f $driveRoot
    $extendedShareRoot = '\\?\UNC\localhost\{0}$\' -f $driveLetter
    # 通常checkoutでは表示driveをそのままvolume GUID検索へ使う。
    $volumeQueryRoot = $driveRoot
    $volumeRootOutput = @(& mountvol.exe $volumeQueryRoot /L)
    $volumeRootExitCode = $LASTEXITCODE
    if ($volumeRootExitCode -ne 0 -or @($volumeRootOutput | ForEach-Object { $_.Trim() } | Where-Object { $_ }).Count -ne 1) {
        # SUBSTなどmountvolが直接扱えないaliasは、pinしたrepoの物理final pathへ解決する。
        $repositoryPinForVolume = Open-AcsDistributionDirectoryPin $repo

        # pinから得た物理repository pathのroot。
        $physicalRepositoryRoot = ''
        try {
            $physicalRepositoryRoot = Get-AcsPathRoot $repositoryPinForVolume.FinalPath
        } finally {
            Close-AcsDistributionDirectoryPin $repositoryPinForVolume
        }

        if ($physicalRepositoryRoot -match '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}\\$') {
            $volumeQueryRoot = $physicalRepositoryRoot
            $volumeRootOutput = @($physicalRepositoryRoot)
            $volumeRootExitCode = 0
        } else {
            if ($physicalRepositoryRoot -match '^\\\\\?\\([A-Za-z]:\\)$') {
                $volumeQueryRoot = $Matches[1]
            } elseif ($physicalRepositoryRoot -match '^[A-Za-z]:\\$') {
                $volumeQueryRoot = $physicalRepositoryRoot
            } else {
                throw "self-testの物理volume検索rootを特定できません: alias=$driveRoot physical=$physicalRepositoryRoot"
            }
            $volumeRootOutput = @(& mountvol.exe $volumeQueryRoot /L)
            $volumeRootExitCode = $LASTEXITCODE
        }
    }
    $global:LASTEXITCODE = 0
    $volumeRoots = @($volumeRootOutput | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($volumeRootExitCode -ne 0 -or $volumeRoots.Count -ne 1) {
        throw "self-testのvolume GUID rootを取得できません: drive=$driveRoot physical=$volumeQueryRoot exit=$volumeRootExitCode"
    }
    $rootNormalizationCases = @(
        [pscustomobject]@{ Name = 'drive'; Path = $driveRoot },
        [pscustomobject]@{ Name = 'share'; Path = $shareRoot },
        [pscustomobject]@{ Name = 'extended-drive'; Path = $extendedDriveRoot },
        [pscustomobject]@{ Name = 'extended-share'; Path = $extendedShareRoot },
        [pscustomobject]@{ Name = 'volume-actual'; Path = $volumeRoots[0] },
        [pscustomobject]@{ Name = 'volume-synthetic'; Path = '\\?\Volume{00000000-0000-0000-0000-000000000000}\' }
    )
    foreach ($rootNormalizationCase in $rootNormalizationCases) {
        $normalizedRoot = Get-NormalizedFullPath $rootNormalizationCase.Path
        if (-not $normalizedRoot.EndsWith('\') -or -not [string]::Equals($normalizedRoot, $rootNormalizationCase.Path, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "self-testのroot separator正規化が一致しません: $($rootNormalizationCase.Name) expected=$($rootNormalizationCase.Path) actual=$normalizedRoot"
        }
        if ($null -ne (Get-AcsParentPath $normalizedRoot)) {
            throw "self-testのroot parent停止点が一致しません: $($rootNormalizationCase.Name)"
        }
        $rootLeafPath = $normalizedRoot + 'acs-v5-parent-probe'
        $rootLeafParent = Get-AcsParentPath $rootLeafPath
        if (-not [string]::Equals($rootLeafParent, $normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase) -or -not $rootLeafParent.EndsWith('\')) {
            throw "self-testのroot直下parent separatorが一致しません: $($rootNormalizationCase.Name) expected=$normalizedRoot actual=$rootLeafParent"
        }
        Assert-ExpectedFailure {
            Assert-SafeDeployPath $rootNormalizationCase.Path
        } "$($rootNormalizationCase.Name) root deploy"
    }

    Assert-ExpectedFailure {
        Assert-SafeDeployPath $driveRoot
    } 'drive root deploy'
    Assert-ExpectedFailure { Assert-SafeDeployPath $repo } 'repository deploy'
    Assert-ExpectedFailure {
        Assert-SafeDeployPath (Join-Path $repo '_unsafe_deploy_probe')
    } 'repository child deploy'
    $repositoryParent = Get-AcsParentPath $repo
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

    $directRootNonce = [Guid]::NewGuid().ToString('N')
    $existingDirectRoot = Join-Path $driveRoot "acs-v4-existing-$directRootNonce"
    $missingDirectRoot = Join-Path $driveRoot "acs-v4-missing-$directRootNonce"
    try {
        if (-not [AcsDistributionDirectoryPinNative]::TryCreateDirectory($existingDirectRoot)) {
            throw "self-testの既存drive直下fixtureが衝突しました: $existingDirectRoot"
        }
        $existingDirectDescriptor = Get-AcsDistributionNamespaceDescriptor $existingDirectRoot
        try {
            if (-not $existingDirectDescriptor.RootPin -or -not $existingDirectDescriptor.ParentPin) {
                throw "self-testの既存C:\<leaf> descriptorがidentityを固定しませんでした"
            }
        } finally {
            Close-AcsDistributionDirectoryPin $existingDirectDescriptor.RootPin
            Close-AcsDistributionDirectoryPin $existingDirectDescriptor.ParentPin
        }

        $missingDirectDescriptor = Get-AcsDistributionNamespaceDescriptor $missingDirectRoot
        try {
            if ($missingDirectDescriptor.RootPin -or -not $missingDirectDescriptor.ParentPin) {
                throw "self-testの未作成C:\<leaf> descriptorが既存rootを誤認しました"
            }
        } finally {
            Close-AcsDistributionDirectoryPin $missingDirectDescriptor.RootPin
            Close-AcsDistributionDirectoryPin $missingDirectDescriptor.ParentPin
        }
        $missingDirectLock = Enter-AcsDistributionOperationLock $missingDirectRoot 'self-test-drive-leaf'
        try {
            $createdDirectRoots = @(Ensure-AcsDistributionOperationRoot $missingDirectLock)
            if ($createdDirectRoots.Count -ne 1 -or -not $missingDirectLock.RootPin -or -not $missingDirectLock.IdentityLock) {
                throw "self-testの未作成C:\<leaf> identity移行が完成しませんでした"
            }
        } finally {
            Exit-AcsDistributionOperationLock $missingDirectLock
        }
    } finally {
        Remove-AcsCreatedEmptyDirectoryChain @($missingDirectRoot, $existingDirectRoot)
    }
    if ((Test-Path -LiteralPath $missingDirectRoot) -or (Test-Path -LiteralPath $existingDirectRoot)) {
        throw "self-testのC:\acs-v4-* fixtureが残っています"
    }

    $volumeDirectLeaf = 'acs-v5-volume-' + [Guid]::NewGuid().ToString('N')
    $volumeDirectRoot = (Get-NormalizedFullPath $volumeRoots[0]) + $volumeDirectLeaf
    $driveAliasVolumeDirectRoot = $volumeQueryRoot + $volumeDirectLeaf
    $volumeRootPin = Open-AcsDistributionDirectoryPin $volumeQueryRoot
    try {
        $volumeRootIdentity = $volumeRootPin.Identity
    } finally {
        Close-AcsDistributionDirectoryPin $volumeRootPin
    }
    $volumeDirectDescriptor = Get-AcsDistributionNamespaceDescriptor $volumeDirectRoot
    try {
        if ($volumeDirectDescriptor.RootPin -or -not $volumeDirectDescriptor.ParentPin -or $volumeDirectDescriptor.ParentPin.Identity -cne $volumeRootIdentity) {
            throw "self-testのactual volume root直下descriptorが既存volume identityを固定しませんでした"
        }
    } finally {
        Close-AcsDistributionDirectoryPin $volumeDirectDescriptor.RootPin
        Close-AcsDistributionDirectoryPin $volumeDirectDescriptor.ParentPin
    }
    $volumePhysicalDescriptor = Get-AcsPhysicalPathDescriptor $volumeDirectRoot
    try {
        if ($volumePhysicalDescriptor.Exists -or $volumePhysicalDescriptor.ExistingAncestorIdentity -cne $volumeRootIdentity -or $volumePhysicalDescriptor.RemainingParts.Count -ne 1 -or $volumePhysicalDescriptor.RemainingParts[0] -cne $volumeDirectLeaf) {
            throw "self-testのactual volume root直下physical descriptorが一致しません"
        }
    } finally {
        Close-AcsPhysicalPathDescriptor $volumePhysicalDescriptor
    }
    if ((Get-AcsDistributionOperationMutexName $volumeDirectRoot) -cne (Get-AcsDistributionOperationMutexName $driveAliasVolumeDirectRoot)) {
        throw "self-testのactual volume root直下mutexがdrive aliasと一致しません"
    }

    $volumeDirectLock = Enter-AcsDistributionOperationLock $volumeDirectRoot 'self-test-volume-root-leaf'
    $createdVolumeDirectRoots = @()
    $volumeDirectCreationPermitted = $false
    try {
        try {
            $createdVolumeDirectRoots = @(Ensure-AcsDistributionOperationRoot $volumeDirectLock)
            $volumeDirectCreationPermitted = $true
            if ($createdVolumeDirectRoots.Count -ne 1 -or -not $volumeDirectLock.RootPin -or -not $volumeDirectLock.IdentityLock) {
                throw "self-testのactual volume root直下identity移行が完成しませんでした"
            }
            $driveAliasVolumePin = Open-AcsDistributionDirectoryPin $driveAliasVolumeDirectRoot
            try {
                if ($driveAliasVolumePin.Identity -cne $volumeDirectLock.RootPin.Identity) {
                    throw "self-testのactual volume root直下identityがdrive aliasと一致しません"
                }
            } finally {
                Close-AcsDistributionDirectoryPin $driveAliasVolumePin
            }
        } catch {
            if (-not (Test-AcsAccessDeniedError $_)) { throw }
            Write-Warning "actual volume root直下を作成する権限がないため、pure helper・descriptor・mutex testのみ実行しました"
        }
    } finally {
        Exit-AcsDistributionOperationLock $volumeDirectLock
        Remove-AcsCreatedEmptyDirectoryChain ([string[]]$createdVolumeDirectRoots)
    }
    if ((Test-Path -LiteralPath $volumeDirectRoot) -or (Test-Path -LiteralPath $driveAliasVolumeDirectRoot)) {
        throw "self-testのactual volume root直下fixtureが残っています"
    }
    if ($volumeDirectCreationPermitted -and $createdVolumeDirectRoots.Count -ne 1) {
        throw "self-testのactual volume root直下cleanup契約を実測できませんでした"
    }
    $volumeDirectTestMode = if ($volumeDirectCreationPermitted) { 'descriptor,mutex,ensure,identity,cleanup' } else { 'helper,descriptor,mutex' }
    Write-Host "acs_distribution_volume_root_self_test=ok mode=$volumeDirectTestMode"

    $selfTestRoot = Get-NormalizedFullPath (Join-Path $repo 'acs\Saved')
    $testDirectory = Get-NormalizedFullPath (Join-Path $repo (
        'acs\Saved\sh-' + [Guid]::NewGuid().ToString('N'))
    )
    if (-not $testDirectory.StartsWith(
            $selfTestRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "self-test directory が Saved の外です: $testDirectory"
    }
    New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null
    $substAliasRoot = ''
    $blockedSubstAliasRoot = ''
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

        $pinProbeRoot = Join-Path $testDirectory 'directory-pin-probe'
        $pinProbeMoved = Join-Path $testDirectory 'directory-pin-probe-moved'
        New-Item -ItemType Directory -Path $pinProbeRoot | Out-Null
        $pinProbe = Open-AcsDistributionDirectoryPin $pinProbeRoot
        $pinProbeMovedDuringTest = $false
        try {
            try {
                Move-Item -LiteralPath $pinProbeRoot -Destination $pinProbeMoved
                $pinProbeMovedDuringTest = $true
            } catch {
                if (-not (Test-Path -LiteralPath $pinProbeRoot -PathType Container) -or (Test-Path -LiteralPath $pinProbeMoved)) {
                    throw
                }
                # filesystemがrenameを拒否した場合も、固定したidentityが有効なことを確認する。
            }
            if ($pinProbeMovedDuringTest) {
                Assert-ExpectedFailure {
                    Assert-AcsDistributionDirectoryPin $pinProbe
                } 'changed distribution root identity'
            } else {
                Assert-AcsDistributionDirectoryPin $pinProbe
            }
        } finally {
            Close-AcsDistributionDirectoryPin $pinProbe
        }
        if ($pinProbeMovedDuringTest) {
            Move-Item -LiteralPath $pinProbeMoved -Destination $pinProbeRoot
        } else {
            Move-Item -LiteralPath $pinProbeRoot -Destination $pinProbeMoved
            Move-Item -LiteralPath $pinProbeMoved -Destination $pinProbeRoot
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

        $manifestSource = Join-Path $testDirectory 'manifest source'
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

        # Enter後にmutex非協調processがrootを作った場合も、ensure後にidentity lockを必ず重ねる。
        $externalCreationRoot = Join-Path $testDirectory 'external-root'
        $externalCreationLock = Enter-AcsDistributionOperationLock $externalCreationRoot 'self-test-external-create'
        try {
            $externalCreationDirectories = @(Ensure-AcsDeployDestinationOperationRoot $externalCreationLock {
                param($operationRoot)
                [System.IO.Directory]::CreateDirectory($operationRoot) | Out-Null
            })
            if ($externalCreationDirectories.Count -ne 0 -or -not $externalCreationLock.RootPin -or -not $externalCreationLock.IdentityLock -or -not $externalCreationLock.IdentityLock.Acquired) {
                throw "self-testの外部作成相当rootがidentity排他を完成しませんでした"
            }
        } finally {
            Exit-AcsDistributionOperationLock $externalCreationLock
            Remove-AcsCreatedEmptyDirectoryChain @($externalCreationRoot)
        }

        $sourceExternalCreationRoot = Join-Path $testDirectory 'source-external'
        $sourceExternalCreationLock = Enter-AcsDistributionOperationLock $sourceExternalCreationRoot 'source-distribution-generation'
        try {
            $sourceExternalCreationDirectories = @(Ensure-AcsSourceGenerationOperationRoot $sourceExternalCreationLock {
                param($operationRoot)
                [System.IO.Directory]::CreateDirectory($operationRoot) | Out-Null
            })
            if ($sourceExternalCreationDirectories.Count -ne 0 -or -not $sourceExternalCreationLock.RootPin -or -not $sourceExternalCreationLock.IdentityLock -or -not $sourceExternalCreationLock.IdentityLock.Acquired) {
                throw "self-testのsource generation外部作成相当rootがidentity排他を完成しませんでした"
            }
        } finally {
            Exit-AcsDistributionOperationLock $sourceExternalCreationLock
            Remove-AcsCreatedEmptyDirectoryChain @($sourceExternalCreationRoot)
        }

        # 外部作成rootのidentity移行失敗は既存payloadとmanifestを変更しない。
        $externalMigrationRoot = Join-Path $testDirectory 'external-fail'
        $externalMigrationLock = Enter-AcsDistributionOperationLock $externalMigrationRoot 'self-test-external-migration-failure'
        $externalMigrationProbe = [pscustomobject]@{
            Holder = $null
            DestinationState = ''
        }
        $sourceStateBeforeIdentityMigration = Get-DistributionTreeStateSignature $manifestSource
        try {
            Assert-ExpectedFailure {
                Ensure-AcsDeployDestinationOperationRoot $externalMigrationLock {
                    param($operationRoot)
                    [System.IO.Directory]::CreateDirectory($operationRoot) | Out-Null
                    [System.IO.File]::WriteAllText((Join-Path $operationRoot 'payload.bin'), 'external-payload')
                    [System.IO.File]::WriteAllText((Join-Path $operationRoot $distributionManifestName), 'external-manifest')
                } {
                    param($operationLock, $createdDirectories)
                    if (@($createdDirectories).Count -ne 0) {
                        throw "外部作成相当rootをself作成として記録しました"
                    }
                    $externalMigrationProbe.DestinationState = Get-DistributionTreeStateSignature $operationLock.Root
                    $migrationPin = Open-AcsDistributionDirectoryPin $operationLock.Root
                    try {
                        $migrationMutexName = Get-AcsDistributionMutexNameForKey ("IDENTITY:" + $migrationPin.Identity)
                    } finally {
                        Close-AcsDistributionDirectoryPin $migrationPin
                    }
                    $externalMigrationProbe.Holder = Start-AcsNamedMutexHolder $migrationMutexName $testDirectory
                } | Out-Null
            } 'externally created root identity migration'
            if (-not $externalMigrationProbe.Holder -or $externalMigrationProbe.Holder.Process.HasExited -or $externalMigrationLock.RootPin -or $externalMigrationLock.IdentityLock) {
                throw "self-testの外部作成root identity mutex競合を確定できませんでした"
            }
            if (-not (Test-Path -LiteralPath $externalMigrationRoot -PathType Container) -or $sourceStateBeforeIdentityMigration -cne (Get-DistributionTreeStateSignature $manifestSource) -or $externalMigrationProbe.DestinationState -cne (Get-DistributionTreeStateSignature $externalMigrationRoot)) {
                throw "self-testのidentity移行失敗がpayloadまたはmanifestを変更しました"
            }
        } finally {
            Stop-AcsOperationLockHolder $externalMigrationProbe.Holder
            Exit-AcsDistributionOperationLock $externalMigrationLock
            Remove-Item -LiteralPath $externalMigrationRoot -Recurse -Force -ErrorAction SilentlyContinue
        }

        # self作成した空chainだけをidentity移行失敗時にrollbackし、既存parentは残す。
        $rollbackOwnerRoot = Join-Path $testDirectory 'rollback-owner'
        $rollbackSentinel = Join-Path $rollbackOwnerRoot 'sentinel.txt'
        $rollbackTarget = Join-Path $rollbackOwnerRoot 'p1\p2\dest'
        New-Item -ItemType Directory -Path $rollbackOwnerRoot | Out-Null
        [System.IO.File]::WriteAllText($rollbackSentinel, 'owner')
        $rollbackLock = Enter-AcsDistributionOperationLock $rollbackTarget 'self-test-created-chain-rollback'
        $rollbackProbe = [pscustomobject]@{ Holder = $null }
        try {
            Assert-ExpectedFailure {
                Ensure-AcsDeployDestinationOperationRoot $rollbackLock $null {
                    param($operationLock, $createdDirectories)
                    if (@($createdDirectories).Count -ne 3) {
                        throw "self-testのself作成chain件数が一致しません"
                    }
                    $migrationPin = Open-AcsDistributionDirectoryPin $operationLock.Root
                    try {
                        $migrationMutexName = Get-AcsDistributionMutexNameForKey ("IDENTITY:" + $migrationPin.Identity)
                    } finally {
                        Close-AcsDistributionDirectoryPin $migrationPin
                    }
                    $rollbackProbe.Holder = Start-AcsNamedMutexHolder $migrationMutexName $testDirectory
                } | Out-Null
            } 'self-created root identity migration'
            if (-not $rollbackProbe.Holder -or $rollbackProbe.Holder.Process.HasExited -or $rollbackLock.RootPin -or $rollbackLock.IdentityLock) {
                throw "self-testのself作成root identity mutex競合を確定できませんでした"
            }
            if ((Test-Path -LiteralPath (Join-Path $rollbackOwnerRoot 'p1')) -or -not (Test-Path -LiteralPath $rollbackSentinel -PathType Leaf) -or [System.IO.File]::ReadAllText($rollbackSentinel) -cne 'owner') {
                throw "self-testのidentity移行rollbackがself作成空chain以外を変更しました"
            }
        } finally {
            Stop-AcsOperationLockHolder $rollbackProbe.Holder
            Exit-AcsDistributionOperationLock $rollbackLock
        }

        $substAliasRoot = New-AcsSubstAlias $testDirectory
        $substManifestSource = Join-Path $substAliasRoot 'manifest source'
        Assert-AcsDistributionAliasSafety $manifestSource $substManifestSource $testDirectory 'SUBST'
        $shortManifestSource = [AcsDistributionDirectoryPinNative]::TryGetShortPath($manifestSource)
        if (-not [string]::IsNullOrWhiteSpace($shortManifestSource) -and -not [string]::Equals($manifestSource, $shortManifestSource, [System.StringComparison]::OrdinalIgnoreCase)) {
            Assert-AcsDistributionAliasSafety $manifestSource $shortManifestSource $testDirectory '8.3'
        } else {
            Write-Warning "8.3 aliasが無効なvolumeのため、物理identity共通化の短縮名testを省略します"
        }
        $manifestSourceRoot = [System.IO.Path]::GetPathRoot($manifestSource)
        if ($manifestSourceRoot -match '^([A-Za-z]):\\$') {
            $localhostManifestSource = "\\localhost\$($Matches[1])$\" + $manifestSource.Substring($manifestSourceRoot.Length)
            if (Test-Path -LiteralPath $localhostManifestSource -PathType Container) {
                Assert-AcsDistributionAliasSafety $manifestSource $localhostManifestSource $testDirectory 'localhost UNC'
            } else {
                Write-Warning "localhost管理共有へ接続できないため、UNC alias testを省略します"
            }
        }

        $blockedRoots = @(
            [pscustomobject]@{ Name = 'repo'; Path = $repo },
            [pscustomobject]@{ Name = 'acs'; Path = (Join-Path $repo 'acs') },
            [pscustomobject]@{ Name = 'build'; Path = $build },
            [pscustomobject]@{ Name = 'dist'; Path = $dist }
        )
        $blockedOverlapCases = [System.Collections.Generic.List[object]]::new()
        foreach ($blockedRoot in $blockedRoots) {
            $blockedOverlapCases.Add([pscustomobject]@{ Name = "$($blockedRoot.Name)-same"; Path = $blockedRoot.Path })
            # rootでは存在しないancestorを追加せず、canonical root自体の拒否を明示確認する。
            $blockedAncestor = Get-AcsParentPath $blockedRoot.Path
            if ($blockedAncestor) {
                $blockedOverlapCases.Add([pscustomobject]@{ Name = "$($blockedRoot.Name)-ancestor"; Path = $blockedAncestor })
            } else {
                Assert-ExpectedFailure {
                    Assert-SafeDeployPath $blockedRoot.Path
                } "physical overlap canonical $($blockedRoot.Name)-root"
            }
            $blockedOverlapCases.Add([pscustomobject]@{ Name = "$($blockedRoot.Name)-descendant"; Path = (Join-Path $blockedRoot.Path '__acs_v5_unsafe_deploy_probe__') })
        }

        $blockedAliasProviders = [System.Collections.Generic.List[object]]::new()
        # 通常は広いancestorをalias変換範囲にし、parent chainが尽きる場合はrepo自身へ留める。
        $blockedSubstTarget = $repo
        $repositoryParentForAlias = Get-AcsParentPath $repo
        if ($repositoryParentForAlias) {
            $blockedSubstTarget = $repositoryParentForAlias
            # repository parentより上にalias変換可能なancestorがある場合だけ範囲を広げる。
            $repositoryAncestorForAlias = Get-AcsParentPath $repositoryParentForAlias
            if ($repositoryAncestorForAlias) {
                $blockedSubstTarget = $repositoryAncestorForAlias
            }
        }
        $blockedSubstAliasRoot = New-AcsSubstAlias $blockedSubstTarget
        $blockedAliasProviders.Add([pscustomobject]@{
            Name = 'SUBST'
            Kind = 'root'
            CanonicalRoot = $blockedSubstTarget
            AliasRoot = $blockedSubstAliasRoot
        })

        $localhostRepository = ConvertTo-AcsLocalhostAdminSharePath $repo
        if (Test-Path -LiteralPath $localhostRepository -PathType Container) {
            $blockedAliasProviders.Add([pscustomobject]@{ Name = 'localhost-UNC'; Kind = 'localhost' })
        } else {
            Write-Warning "localhost管理共有へ接続できないため、physical overlapのUNC testを省略します"
        }

        $shortRepository = Get-AcsShortAliasPath $repo
        if (-not [string]::IsNullOrWhiteSpace($shortRepository) -and -not [string]::Equals($shortRepository, $repo, [System.StringComparison]::OrdinalIgnoreCase)) {
            $blockedAliasProviders.Add([pscustomobject]@{ Name = '8.3'; Kind = 'short' })
        } else {
            Write-Warning "8.3 aliasが無効なvolumeのため、physical overlapの短縮名testを省略します"
        }

        $canonicalOverlapSnapshot = Get-AcsDistributionFixtureSnapshot $manifestSource
        $physicalOverlapCaseCount = 0
        foreach ($blockedAliasProvider in $blockedAliasProviders) {
            if ($blockedAliasProvider.Kind -ceq 'root') {
                $aliasManifestSourceForOverlap = ConvertTo-AcsRootAliasPath $manifestSource $blockedAliasProvider.CanonicalRoot $blockedAliasProvider.AliasRoot
            } elseif ($blockedAliasProvider.Kind -ceq 'localhost') {
                $aliasManifestSourceForOverlap = ConvertTo-AcsLocalhostAdminSharePath $manifestSource
            } else {
                $aliasManifestSourceForOverlap = Get-AcsShortAliasPath $manifestSource
            }
            $aliasOverlapSnapshot = Get-AcsDistributionFixtureSnapshot $aliasManifestSourceForOverlap
            if ($canonicalOverlapSnapshot -cne $aliasOverlapSnapshot) {
                throw "self-testのphysical overlap開始時tree・payload・manifestがaliasと一致しません: $($blockedAliasProvider.Name)"
            }

            foreach ($blockedOverlapCase in $blockedOverlapCases) {
                if ($blockedAliasProvider.Kind -ceq 'root') {
                    $aliasBlockedPath = ConvertTo-AcsRootAliasPath $blockedOverlapCase.Path $blockedAliasProvider.CanonicalRoot $blockedAliasProvider.AliasRoot
                } elseif ($blockedAliasProvider.Kind -ceq 'localhost') {
                    $aliasBlockedPath = ConvertTo-AcsLocalhostAdminSharePath $blockedOverlapCase.Path
                } else {
                    $aliasBlockedPath = Get-AcsShortAliasPath $blockedOverlapCase.Path
                }
                if ([string]::IsNullOrWhiteSpace($aliasBlockedPath)) {
                    throw "self-testのphysical overlap aliasを作れません: provider=$($blockedAliasProvider.Name) case=$($blockedOverlapCase.Name)"
                }
                Assert-ExpectedFailure {
                    Assert-SafeDeployPath $aliasBlockedPath
                } "physical overlap $($blockedAliasProvider.Name) $($blockedOverlapCase.Name)"
                ++$physicalOverlapCaseCount
            }

            if ($canonicalOverlapSnapshot -cne (Get-AcsDistributionFixtureSnapshot $manifestSource) -or $aliasOverlapSnapshot -cne (Get-AcsDistributionFixtureSnapshot $aliasManifestSourceForOverlap)) {
                throw "self-testのphysical overlap拒否がcanonical/alias tree・payload・manifestを変更しました: $($blockedAliasProvider.Name)"
            }
        }
        Write-Host "acs_distribution_physical_overlap_self_test=ok providers=$($blockedAliasProviders.Count) cases=$physicalOverlapCaseCount state=tree,payload,manifest"

        $sourceMutexName = Get-AcsDistributionOperationMutexName $manifestSource
        $caseVariantSource = $manifestSource.ToUpperInvariant()
        $separatorVariantSource = $manifestSource.Replace('\', '/') + '/./'
        if ($sourceMutexName -cne (Get-AcsDistributionOperationMutexName $caseVariantSource) -or $sourceMutexName -cne (Get-AcsDistributionOperationMutexName $separatorVariantSource)) {
            throw "self-testのWindows path大小文字・区切りmutex正規化が一致しません"
        }
        $sourceStateBeforeLockRefusal = Get-DistributionTreeStateSignature $manifestSource
        $sourceLockHolder = $null
        $abandonedMutexProbe = $null
        try {
            $sourceLockHolder = Start-AcsOperationLockHolder $caseVariantSource $testDirectory
            $abandonedMutexProbe = [System.Threading.Mutex]::new($false, $sourceLockHolder.MutexName)
            $parallelRoot = Join-Path $testDirectory 'independent operation root'
            New-Item -ItemType Directory -Path $parallelRoot | Out-Null
            $parallelLock = Enter-AcsDistributionOperationLock $parallelRoot 'self-test-independent-root'
            Exit-AcsDistributionOperationLock $parallelLock
            Assert-ExpectedFailure {
                Publish-AcsDistributionManifest $manifestSource
            } 'parallel source distribution writer'
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestSource (Join-Path $testDirectory 'source-lock destination')
            } 'source lock before destination creation'
            if ($sourceStateBeforeLockRefusal -cne (Get-DistributionTreeStateSignature $manifestSource)) {
                throw "self-testのsource lock拒否がpayloadまたはmanifestを変更しました"
            }
            if (Test-Path -LiteralPath (Join-Path $testDirectory 'source-lock destination')) {
                throw "self-testのsource lock拒否がdeploy先を作成しました"
            }
            Stop-AcsOperationLockHolder $sourceLockHolder
            $recoveredLock = Enter-AcsDistributionOperationLock $manifestSource 'self-test-abandoned-recovery'
            try {
                if (-not $recoveredLock.RecoveredAbandoned) {
                    throw "self-testがabandoned operation mutexを回収しませんでした"
                }
            } finally {
                Exit-AcsDistributionOperationLock $recoveredLock
            }
        } finally {
            Stop-AcsOperationLockHolder $sourceLockHolder
            if ($abandonedMutexProbe) {
                $abandonedMutexProbe.Dispose()
            }
        }

        $cleanupRoot = Join-Path $testDirectory 'operation-lock-cleanup'
        New-Item -ItemType Directory -Path $cleanupRoot | Out-Null
        $cleanupLock = $null
        $intentionalCleanupFailureObserved = $false
        try {
            $cleanupLock = Enter-AcsDistributionOperationLock $cleanupRoot 'self-test-exception-cleanup'
            throw "self-test operation lock cleanup probe"
        } catch {
            $intentionalCleanupFailureObserved = $true
        } finally {
            Exit-AcsDistributionOperationLock $cleanupLock
        }
        if (-not $intentionalCleanupFailureObserved) {
            throw "self-testのoperation lock例外cleanup経路を通過しませんでした"
        }
        $cleanupRetryLock = Enter-AcsDistributionOperationLock $cleanupRoot 'self-test-exception-retry'
        Exit-AcsDistributionOperationLock $cleanupRetryLock

        $permissionRoot = Join-Path $testDirectory 'operation-lock-permission'
        New-Item -ItemType Directory -Path $permissionRoot | Out-Null
        $permissionAcl = Get-Acl -LiteralPath $permissionRoot
        $permissionRule = [System.Security.AccessControl.FileSystemAccessRule]::new([System.Security.Principal.WindowsIdentity]::GetCurrent().User, [System.Security.AccessControl.FileSystemRights]::FullControl, [System.Security.AccessControl.AccessControlType]::Deny)
        try {
            $restrictedAcl = Get-Acl -LiteralPath $permissionRoot
            $restrictedAcl.AddAccessRule($permissionRule)
            Set-Acl -LiteralPath $permissionRoot -AclObject $restrictedAcl
            Assert-ExpectedFailure {
                $unexpectedPermissionLock = Enter-AcsDistributionOperationLock $permissionRoot 'self-test-permission-failure'
                Exit-AcsDistributionOperationLock $unexpectedPermissionLock
            } 'operation lock permission failure'
        } finally {
            Set-Acl -LiteralPath $permissionRoot -AclObject $permissionAcl
        }
        if (@(Get-ChildItem -LiteralPath $permissionRoot -Force).Count -ne 0) {
            throw "self-testの権限拒否経路が配布rootを変更しました"
        }

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

        $manifestDestination = Join-Path $testDirectory 'manifest destination'
        $substManifestDestination = Join-Path $substAliasRoot 'manifest destination'
        if ((Get-AcsDistributionOperationMutexName $manifestDestination) -cne (Get-AcsDistributionOperationMutexName $substManifestDestination)) {
            throw "self-testの未作成SUBST deploy先mutexが一致しません"
        }
        $sourceBeforeMissingDestinationLock = Get-DistributionTreeStateSignature $manifestSource
        $missingDestinationHolder = $null
        try {
            $missingDestinationHolder = Start-AcsOperationLockHolder $substManifestDestination $testDirectory
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestSource $manifestDestination
            } 'nonexistent destination alias writer'
            if ((Test-Path -LiteralPath $manifestDestination) -or $sourceBeforeMissingDestinationLock -cne (Get-DistributionTreeStateSignature $manifestSource)) {
                throw "self-testの未作成deploy先lock拒否がsourceまたはdeploy先を変更しました"
            }
        } finally {
            Stop-AcsOperationLockHolder $missingDestinationHolder
        }
        Publish-MirroredDistribution $manifestSource $manifestDestination
        Assert-MirroredDistribution $manifestSource $manifestDestination
        Assert-AcsDistributionManifest $manifestDestination
        Assert-AcsDistributionAliasSafety $manifestDestination $substManifestDestination $testDirectory 'SUBST destination'
        $nestedManifestDestination = Join-Path $testDirectory 'p1\p2\dest'
        $nestedDescriptorBefore = Get-AcsDistributionNamespaceDescriptor $nestedManifestDestination
        try {
            $nestedMutexNamesBefore = [string[]]$nestedDescriptorBefore.NamespaceMutexNames
        } finally {
            Close-AcsDistributionDirectoryPin $nestedDescriptorBefore.RootPin
            Close-AcsDistributionDirectoryPin $nestedDescriptorBefore.ParentPin
        }
        Publish-MirroredDistribution $manifestSource $nestedManifestDestination
        Assert-MirroredDistribution $manifestSource $nestedManifestDestination
        Assert-AcsDistributionManifest $nestedManifestDestination
        $nestedDescriptorAfter = Get-AcsDistributionNamespaceDescriptor $nestedManifestDestination
        try {
            $nestedMutexNamesAfter = [string[]]$nestedDescriptorAfter.NamespaceMutexNames
        } finally {
            Close-AcsDistributionDirectoryPin $nestedDescriptorAfter.RootPin
            Close-AcsDistributionDirectoryPin $nestedDescriptorAfter.ParentPin
        }
        if (@($nestedMutexNamesBefore | Where-Object { $nestedMutexNamesAfter -contains $_ }).Count -eq 0) {
            throw "self-testの複数階層deploy先lockが作成前後で重なりません"
        }
        $sourceBeforeReverseLock = Get-DistributionTreeStateSignature $manifestSource
        $destinationBeforeReverseLock = Get-DistributionTreeStateSignature $manifestDestination
        $reverseHolder = $null
        try {
            $reverseHolder = Start-AcsOperationLockHolder $manifestSource $testDirectory
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestDestination $manifestSource
            } 'reverse direction writer'
            if ($sourceBeforeReverseLock -cne (Get-DistributionTreeStateSignature $manifestSource) -or $destinationBeforeReverseLock -cne (Get-DistributionTreeStateSignature $manifestDestination)) {
                throw "self-testの逆方向lock拒否がsourceまたはdeploy先を変更しました"
            }
        } finally {
            Stop-AcsOperationLockHolder $reverseHolder
        }

        $destinationMutexName = Get-AcsDistributionOperationMutexName $manifestDestination
        $caseVariantDestination = $manifestDestination.ToLowerInvariant()
        if ($destinationMutexName -cne (Get-AcsDistributionOperationMutexName $caseVariantDestination)) {
            throw "self-testのdeploy path大小文字mutex正規化が一致しません"
        }
        $sourceStateBeforeDestinationLockRefusal = Get-DistributionTreeStateSignature $manifestSource
        $destinationStateBeforeLockRefusal = Get-DistributionTreeStateSignature $manifestDestination
        $destinationLockHolder = $null
        try {
            $destinationLockHolder = Start-AcsOperationLockHolder $caseVariantDestination $testDirectory
            Assert-ExpectedFailure {
                Publish-MirroredDistribution $manifestSource $manifestDestination
            } 'parallel destination distribution writer'
            if ($sourceStateBeforeDestinationLockRefusal -cne (Get-DistributionTreeStateSignature $manifestSource) -or $destinationStateBeforeLockRefusal -cne (Get-DistributionTreeStateSignature $manifestDestination)) {
                throw "self-testのdestination lock拒否がsourceまたはdeploy先を変更しました"
            }
        } finally {
            Stop-AcsOperationLockHolder $destinationLockHolder
        }
        Publish-MirroredDistribution $manifestSource $manifestDestination
        if (@(Get-ChildItem -LiteralPath $manifestSource, $manifestDestination -Recurse -Force -File -Filter '*.lock').Count -ne 0) {
            throw "self-testのoperation lock fileが配布treeへ混入しました"
        }
        Assert-DistributionTreeAllowlist $manifestSource
        Assert-DistributionTreeAllowlist $manifestDestination

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
                $unexpectedJunctionLock = Enter-AcsDistributionOperationLock (Join-Path $junctionPath 'nested-root') 'self-test-reparse-parent'
                Exit-AcsDistributionOperationLock $unexpectedJunctionLock
            } 'operation lock parent junction'
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
        Write-Host 'acs_distribution_manifest_self_test=ok cases=canonical,tamper,missing,stale,extra,partial,source_lock,destination_lock,alias_lock,self_deploy,physical_overlap,nested_transition,external_create,migration_rollback,root_normalization,root_parent,direct_root,volume_root,abandoned,cleanup,permission,root_identity,skip,reader_lock,junction,mirror'
    } finally {
        Stop-AcsOperationLockHolder $sourceLockHolder
        Stop-AcsOperationLockHolder $destinationLockHolder
        if ($blockedSubstAliasRoot) {
            & subst.exe $blockedSubstAliasRoot.Substring(0, 2) /D | Out-Null
            $global:LASTEXITCODE = 0
        }
        if ($substAliasRoot) {
            & subst.exe $substAliasRoot.Substring(0, 2) /D | Out-Null
            $global:LASTEXITCODE = 0
        }
        Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "単一 header 配布 pipeline self-test passed"
}

if ($SelfTest) {
    Invoke-PipelineSelfTest
    return
}

# 危険なdeploy先はsource distを含む任意の配布物を変更する前に拒否する。
if ($Deploy) {
    $Deploy = Assert-SafeDeployPath $Deploy
}

$sourceGenerationLock = Enter-AcsDistributionOperationLock $dist 'source-distribution-generation'
$sourceGenerationDirectoryPin = $null
try {
Assert-NoReparseAncestor $dist
Ensure-AcsSourceGenerationOperationRoot $sourceGenerationLock | Out-Null
Assert-NoReparseSubtree $dist
$sourceGenerationDirectoryPin = Open-AcsDistributionDirectoryPin $dist

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
    'acs_subsystem.lib',
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
    Write-Host "==> dist/ を配置 -> $Deploy"
    Publish-MirroredDistribution $dist $Deploy
    Write-Host "    配置完了 (named manifest・file集合・size・SHA-256一致)"
}
Assert-AcsDistributionDirectoryPin $sourceGenerationDirectoryPin
Write-Host "完了"
} finally {
    Close-AcsDistributionDirectoryPin $sourceGenerationDirectoryPin
    Exit-AcsDistributionOperationLock $sourceGenerationLock
}
