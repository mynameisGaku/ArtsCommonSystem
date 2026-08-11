# ACS Editor ABI 検証で使う native DLL の物理パスと作業ディレクトリを管理する。

$script:AcsEditorAbiTestScriptsRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)

function Assert-AcsEditorAbiPhysicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $currentPath = [System.IO.Path]::GetFullPath($Path)
    while ($true) {
        $item = Get-Item -LiteralPath $currentPath -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Editor ABI test path contains a reparse point: $currentPath"
        }

        $parent = [System.IO.Directory]::GetParent($currentPath)
        if ($null -eq $parent) {
            break
        }

        $currentPath = $parent.FullName
    }
}

function New-AcsEditorAbiTestContext {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration
    )

    $sourceRoot = [System.IO.Path]::GetFullPath((Join-Path $script:AcsEditorAbiTestScriptsRoot '..'))
    $nativeDirectory = [System.IO.Path]::GetFullPath((Join-Path $sourceRoot "Binaries\$Configuration"))
    $nativeDll = [System.IO.Path]::GetFullPath((Join-Path $nativeDirectory 'acs_editor_abi.dll'))

    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
        throw "ACS source root is missing or not a directory: $sourceRoot"
    }
    if (-not (Test-Path -LiteralPath $nativeDirectory -PathType Container)) {
        throw "Editor ABI output directory is missing or not a directory: $nativeDirectory"
    }
    if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) {
        throw "Editor ABI DLL is missing or not a file: $nativeDll"
    }

    $nativeDllItem = Get-Item -LiteralPath $nativeDll -Force
    if ($nativeDllItem.PSIsContainer -or (($nativeDllItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Editor ABI DLL must be a regular file: $nativeDll"
    }

    Assert-AcsEditorAbiPhysicalPath -Path $sourceRoot
    Assert-AcsEditorAbiPhysicalPath -Path $nativeDirectory
    Assert-AcsEditorAbiPhysicalPath -Path $nativeDll

    [pscustomobject]@{
        SourceRoot = $sourceRoot
        NativeDirectory = $nativeDirectory
        NativeDll = $nativeDll
        CSharpNativeDll = $nativeDll.Replace('"', '""')
    }
}

function Invoke-AcsEditorAbiTest {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
    )

    $context = New-AcsEditorAbiTestContext -Configuration $Configuration
    $originalCurrentDirectory = [Environment]::CurrentDirectory
    try {
        [Environment]::CurrentDirectory = $context.NativeDirectory
        & $Body $context
    }
    finally {
        [Environment]::CurrentDirectory = $originalCurrentDirectory
    }
}
