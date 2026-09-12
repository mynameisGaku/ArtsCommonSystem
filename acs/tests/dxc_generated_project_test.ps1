# SPDX-License-Identifier: Apache-2.0
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ArtifactRoot,
    [ValidateSet('Red', 'Green')][string]$Phase = 'Green',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidatePattern('^[a-zA-Z0-9-]{1,8}$')][string]$RunName = ('r' + [Guid]::NewGuid().ToString('N').Substring(0, 6)),
    [string]$BaselineProjectManager = '',
    [ValidateSet('Visual Studio 18 2026', 'Visual Studio 17 2022')][string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
# 保存先を明示入力とし、その下の新規検査フォルダーだけを使用する。
if ($ArtifactRoot -notmatch '^(?:[a-zA-Z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)') { throw 'ArtifactRoot must be an absolute Windows path' }
$artifactRoot = [IO.Path]::GetFullPath($ArtifactRoot).TrimEnd([char[]]@('\', '/'))
if ($artifactRoot -eq [IO.Path]::GetPathRoot($artifactRoot).TrimEnd([char[]]@('\', '/'))) { throw 'A drive or share root is not a test artifact folder' }
$runRoot = "$artifactRoot/$RunName"
$acsRoot = Split-Path $PSScriptRoot -Parent
$editorSource = "$acsRoot/editor/AcsEditor"
$managerSource = "$acsRoot/editor/AcsEditor/ProjectManager.cs"
$editorFullPath = [IO.Path]::GetFullPath($editorSource).TrimEnd([char[]]@('\', '/'))
if ($artifactRoot.Equals($editorFullPath, [StringComparison]::OrdinalIgnoreCase) -or $artifactRoot.StartsWith($editorFullPath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'ArtifactRoot must not be inside the Editor source tree' }
if (Test-Path -LiteralPath $runRoot) { throw "Run folder already exists: $runRoot" }
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
# 初回dotnet設定とMSBuild中間物も指定の実験folderへ閉じる。
$savedDotnetHome = $env:DOTNET_CLI_HOME
$savedTelemetry = $env:DOTNET_CLI_TELEMETRY_OPTOUT
$savedFirstRun = $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE
$savedCertificate = $env:DOTNET_GENERATE_ASPNET_CERTIFICATE
$env:DOTNET_CLI_HOME = "$artifactRoot/dotnet-home"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'

# 外部toolの終了codeとログを保持し、失敗を継続扱いにしない。
function Invoke-TestTool([string]$Name, [string]$Program, [string[]]$Arguments) {
    $output = & $Program @Arguments 2>&1
    $code = $LASTEXITCODE
    $output | Out-File -LiteralPath "$runRoot/$Name.log"
    if ($code -ne 0) { $output | Select-Object -Last 30 | Write-Output; throw "$Name failed: $code" }
    Write-Output "PASS: $Name"
}

try {
    Copy-Item -LiteralPath $managerSource -Destination "$runRoot/ProjectManager.before.cs"
    # 追加のsource比較は実生成試験を補助し、テンプレート外への編集を検出する。
    if ($BaselineProjectManager) {
        $baselinePath = [IO.Path]::GetFullPath($BaselineProjectManager)
        if (-not $baselinePath.StartsWith([IO.Path]::GetFullPath($artifactRoot) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Baseline must be inside the experiment folder' }
        $baseline = [IO.File]::ReadAllText($baselinePath).Replace("`r`n", "`n")
        $current = [IO.File]::ReadAllText($managerSource).Replace("`r`n", "`n")
        $runtimeLines = @('            $"acs_dxc_runtime({ident})\n" +', '            $"acs_dxc_runtime({ident}_reflect)\n" +')
        foreach ($line in $runtimeLines) { $current = $current.Replace($line + "`n", '') }
        if ($current -cne $baseline) { throw 'ProjectManager changed outside the two runtime template lines' }
        Write-Output 'PASS: ProjectManager differs only by the two template calls'
    }
    # 実Editorのsourceを無変更で隔離する。WPFの一時projectも実験folder内に生成させる。
    $editorSnapshot = "$runRoot/editor-source"
    New-Item -ItemType Directory -Path $editorSnapshot -Force | Out-Null
    Get-ChildItem -LiteralPath $editorSource -Force | Where-Object { $_.Name -notin @('bin', 'obj') -and $_.Name -notlike '*_wpftmp.csproj' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $editorSnapshot -Recurse }
    if ((Get-FileHash -LiteralPath "$editorSnapshot/ProjectManager.cs").Hash -ne (Get-FileHash -LiteralPath "$runRoot/ProjectManager.before.cs").Hash) { throw 'ProjectManager changed during snapshot capture' }
    $editorProject = "$editorSnapshot/AcsEditor.csproj"
    # 実Editor assemblyを構築するが、そのentry pointは実行しない。
    $editorOut = "$runRoot/editor-out/"
    $editorObj = "$runRoot/editor-obj/"
    Invoke-TestTool 'build-editor' dotnet @('build', $editorProject, '-c', $Configuration, '--nologo', '-v:minimal', "-p:OutputPath=$editorOut", "-p:BaseOutputPath=$editorOut", "-p:IntermediateOutputPath=$editorObj", "-p:BaseIntermediateOutputPath=$editorObj", "-p:MSBuildProjectExtensionsPath=$editorObj", '-p:UseSharedCompilation=false')
    $testOut = "$runRoot/test-out/"
    $testObj = "$runRoot/test-obj/"
    Invoke-TestTool 'build-test' dotnet @('build', "$PSScriptRoot/dxc_generated_project_test.csproj", '-c', $Configuration, '--nologo', '-v:minimal', "-p:OutputPath=$testOut", "-p:BaseOutputPath=$testOut", "-p:IntermediateOutputPath=$testObj", "-p:BaseIntermediateOutputPath=$testObj", "-p:MSBuildProjectExtensionsPath=$testObj", '-p:UseSharedCompilation=false')
    Invoke-TestTool 'invoke-generation' dotnet @("${testOut}dxc_generated_project_test.dll", "${editorOut}AcsEditor.dll", "$runRoot/f", $artifactRoot)
    # 親fixtureだけを用意し、実生成されたSource/CMakeListsは加工せず読み込む。
    # 指定rootは維持し、MSBuildの追跡fileが長くなるfixture内の名前だけを短くする。
    $parentSource = "$runRoot/s"
    New-Item -ItemType Directory -Path $parentSource -Force | Out-Null
    $parentText = 'cmake_minimum_required(VERSION 3.24)' + "`n" + 'project(DxcGeneratedFixture LANGUAGES CXX)' + "`n" + 'include("${TEST_HARNESS}")' + "`n"
    [IO.File]::WriteAllText("$parentSource/CMakeLists.txt", $parentText)
    $expectedPayload = @('dxcompiler.dll', 'dxil.dll', 'Licenses/ThirdParty/DXC-v1.9.2602.24/LICENSE-LLVM.txt', 'Licenses/ThirdParty/DXC-v1.9.2602.24/LICENSE-MIT.txt', 'Licenses/ThirdParty/DXC-v1.9.2602.24/LICENSE-MS.txt', 'Licenses/ThirdParty/DXC-v1.9.2602.24/LICENSE.txt', 'Licenses/ThirdParty/DXC-v1.9.2602.24/ReleaseNotes.md', 'Licenses/ThirdParty/DXC-v1.9.2602.24/ThirdPartyNotices.txt', 'Licenses/ThirdParty/DXC-v1.9.2602.24/LICENSE-DXC-Source.txt')
    foreach ($kind in @('exe', 'reflect')) {
        $buildRoot = "$runRoot/$($kind.Substring(0, 1))"
        $projectRoot = "$runRoot/f/$kind project"
        $configureArgs = @('-S', $parentSource, '-B', $buildRoot, '-G', $Generator, '-A', 'x64', "-DTEST_HARNESS=$($PSScriptRoot.Replace('\','/'))/dxc_generated_project_test.cmake", "-DACS_CMAKE_DIR=$($acsRoot.Replace('\','/'))/engine/cmake", "-DGENERATED_SOURCE_DIR=$projectRoot/Source", '-DACS_RENDER_DX12_RAW=ON', "-DFETCHCONTENT_BASE_DIR=$artifactRoot/d")
        $output = & cmake @configureArgs 2>&1
        $code = $LASTEXITCODE
        $output | Out-File -LiteralPath "$runRoot/configure-$kind.log"
        if ($Phase -eq 'Red') {
            $text = $output -join "`n"
            if ($code -eq 0 -or $text -notmatch 'DXC_GENERATED_MISSING_RUNTIME: DxcFixture\s' -or $text -notmatch 'DXC_GENERATED_MISSING_RUNTIME: DxcFixture_reflect') { throw 'Expected both actual generated targets to lack runtime registration' }
            Write-Output "RED confirmed: $kind fixture, both target registrations absent"
            continue
        }
        if ($code -ne 0) { $output | Write-Output; throw "Generated $kind configure failed" }
        # exe側の配置でreflect側の欠落を隠さないよう、別projectで片方だけをbuildする。
        $target = if ($kind -eq 'exe') { 'DxcFixture' } else { 'DxcFixture_reflect' }
        Invoke-TestTool "build-generated-$kind" cmake @('--build', $buildRoot, '--config', $Configuration, '--target', $target, '--parallel', '2')
        $outputRoot = "$projectRoot/Binaries/$Configuration"
        foreach ($relative in $expectedPayload) {
            if (-not (Test-Path -LiteralPath "$outputRoot/$relative" -PathType Leaf)) { throw "Generated $kind payload missing: $relative" }
        }
        if ((Get-FileHash -LiteralPath "$outputRoot/dxcompiler.dll").Hash -ne '9B5E10ED756C461B4EC2C83A99F1D6ACE20E97826E9C0B0E966B7B1CD6F2AEC6') { throw 'Generated compiler is not the pinned official x64 DLL' }
        if ((Get-FileHash -LiteralPath "$outputRoot/dxil.dll").Hash -ne 'CBCFE883A09FD0CA1F98ABDF3A9553B560895E3283A136DA82A8381253A169DF') { throw 'Generated validator is not the pinned official x64 DLL' }
        Write-Output "PASS: generated $kind target independently deployed both fixed DLLs and seven notices/license files"
    }
    Write-Output "Completed $Phase evidence: $runRoot"
}
finally {
    $env:DOTNET_CLI_HOME = $savedDotnetHome
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = $savedTelemetry
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = $savedFirstRun
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE = $savedCertificate
}
