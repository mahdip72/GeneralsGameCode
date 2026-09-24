param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [Parameter(Mandatory = $true)]
    [string]$FFmpegConfigDir,

    [Parameter(Mandatory = $true)]
    [string]$CMakeCommand
)

$ErrorActionPreference = 'Stop'

$expectedBuildDirectory = 'Stage3FFmpegGraphAudit'
if ([IO.Path]::GetFileName([IO.Path]::GetFullPath($BuildRoot)) -ne $expectedBuildDirectory) {
    throw "FFmpeg graph audit build directory must be named $expectedBuildDirectory."
}

if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}

$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vsWhere)) {
    throw 'Visual Studio discovery tool is unavailable for the FFmpeg graph audit.'
}

$vsInstall = & $vsWhere -latest -products * -property installationPath
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw 'Visual Studio installation is unavailable for the FFmpeg graph audit.'
}

$vsDevCmd = Join-Path $vsInstall.Trim() 'Common7/Tools/VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw 'Visual Studio developer command environment is unavailable for the FFmpeg graph audit.'
}

$cmakeArguments = @(
    '-S', $SourceRoot,
    '-B', $BuildRoot,
    '-G', 'Visual Studio 17 2022',
    '-A', 'Win32',
    '-DRTS_BUILD_PRODUCT=OFF',
    '-DRTS_BUILD_ZEROHOUR=ON',
    '-DRTS_BUILD_GENERALS=OFF',
    '-DRTS_BUILD_CORE_TOOLS=OFF',
    '-DRTS_BUILD_CORE_EXTRAS=OFF',
    '-DRTS_BUILD_ZEROHOUR_TOOLS=OFF',
    '-DRTS_BUILD_ZEROHOUR_EXTRAS=ON',
    '-DRTS_BUILD_OPTION_FFMPEG=ON',
    '-DRTS_VIDEO_BACKEND_GRAPH_AUDIT=ON',
    '-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON',
    "-DFETCHCONTENT_SOURCE_DIR_BINK=$(Join-Path $SourceRoot 'Core/Tools/VideoBackendSelectionAudit/BinkForbidden')",
    '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
    "-DFFMPEG_DIR=$FFmpegConfigDir"
)
$quotedArguments = ($cmakeArguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
& cmd.exe /d /c ('call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 && "' + $CMakeCommand + '" ' + $quotedArguments)

if ($LASTEXITCODE -ne 0) {
    throw 'FFmpeg-only CMake graph generation failed.'
}

if (Test-Path -LiteralPath (Join-Path $BuildRoot '_deps/bink-src')) {
    throw 'FFmpeg-only CMake graph fetched the Bink dependency.'
}

$solution = Get-ChildItem -LiteralPath $BuildRoot -Filter '*.sln' | Select-Object -First 1
if ($null -eq $solution) {
    throw 'FFmpeg-only CMake graph did not generate a Visual Studio solution.'
}

if ((Get-Content -LiteralPath $solution.FullName -Raw) -match '(?i)binkstub') {
    throw 'FFmpeg-only CMake graph retains the Bink target.'
}

$runtimeTestProject = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter 'z_runtime_regression_tests.vcxproj' |
    Select-Object -First 1
if ($null -eq $runtimeTestProject) {
    throw 'FFmpeg-only CMake graph did not generate the Zero Hour runtime regression project.'
}
$runtimeTestProjectContent = Get-Content -LiteralPath $runtimeTestProject.FullName -Raw
$legacyGraphForbiddenTokens = '(?i)binkstub|BinkVideoPlayer|rts_(?:generals|zerohour)_legacy_renderer|milesstub'
if ($runtimeTestProjectContent -match $legacyGraphForbiddenTokens) {
    throw 'FFmpeg-only runtime regression utility retains a forbidden legacy video, renderer, or Miles dependency.'
}

# The runtime utility reaches the title WW3D2 and GameEngineDevice targets
# through transitive links.  Check those generated projects as well: a
# configure-only audit can otherwise miss an architecture helper that emits a
# dangling library outside the utility project itself.
foreach ($projectName in @(
    'z_ww3d2.vcxproj',
    'z_gameenginedevice.vcxproj'
)) {
    $project = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter $projectName |
        Select-Object -First 1
    if ($null -eq $project) {
        throw "FFmpeg-only CMake graph did not generate $projectName."
    }
    $projectContent = Get-Content -LiteralPath $project.FullName -Raw
    if ($projectContent -match $legacyGraphForbiddenTokens) {
        throw "FFmpeg-only $projectName retains a forbidden legacy video, renderer, or Miles dependency."
    }
}

if ((Get-Content -LiteralPath $solution.FullName -Raw) -notmatch 'z_runtime_regression_tests') {
    throw 'FFmpeg-only CMake graph did not configure the Zero Hour runtime regression utility.'
}

$toolsTestFile = Join-Path $BuildRoot 'Core/Tools/CTestTestfile.cmake'
if (-not (Test-Path -LiteralPath $toolsTestFile) -or (Get-Content -LiteralPath $toolsTestFile -Raw) -notmatch 'core_video_backend_selection_audit') {
    throw 'Video backend source audit is unavailable in an ordinary CMake test configuration.'
}

# CMake accepts an unknown plain library token during generation.  Compile and
# link the focused FFmpeg contract utility so missing graph targets cannot be
# reported as a pass merely because configuration succeeded.  Use the same VS
# environment as configuration and cap MSBuild at two workers to keep this
# diagnostic lane resource-bounded.
$buildCommand = '"' + $CMakeCommand + '" --build "' + $BuildRoot +
    '" --config Release --target core_ffmpeg_graph_link_probe -- /m:2'
& cmd.exe /d /c ('call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 && ' + $buildCommand)
if ($LASTEXITCODE -ne 0) {
    throw 'FFmpeg-only CMake graph link probe failed to compile or link.'
}

$contractExecutable = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter 'core_ffmpeg_graph_link_probe.exe' |
    Where-Object { $_.FullName -match '(?i)\\Release\\' } |
    Select-Object -First 1
if ($null -eq $contractExecutable) {
    throw 'FFmpeg-only graph link probe reported success without producing its Release executable.'
}

Write-Output 'FFmpeg-only CMake graph audit passed.'
