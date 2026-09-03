param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot,
    [Parameter(Mandatory = $true)]
    [string] $BuildRoot,
    [Parameter(Mandatory = $true)]
    [string] $Generator,
    [string] $GeneratorPlatform,
    [Parameter(Mandatory = $true)]
    [string] $CCompiler,
    [Parameter(Mandatory = $true)]
    [string] $CxxCompiler,
    [Parameter(Mandatory = $true)]
    [string] $RcCompiler,
    [Parameter(Mandatory = $true)]
    [string] $ManifestTool,
    [Parameter(Mandatory = $true)]
    [string] $FFmpegRoot,
    [string] $FFmpegRuntimeDir,
    [string] $ToolchainFile,
    [string] $VcpkgInstalledDir,
    [string] $VcpkgTargetTriplet,
    [string] $VcpkgOverlayTriplets,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string] $Configuration = 'Release',
    [string] $OutputSuffix = ''
)

$ErrorActionPreference = 'Stop'

$forbiddenNativeInstallPayloadPattern = '(?i)(?:^|[/"\\])d3d8(?:to9)?\.dll(?:["\s]|$)|d3dx8(?:d|_\d+)?\.dll|D3DX9_43\.dll|D3DCompiler_43\.dll|mss32\.dll|binkw32\.dll|native-d3d8-compat'
$forbiddenNativeImportPattern = '(?im)^\s*(?:d3d8(?:to9)?|d3d9|d3dx8(?:d|_\d+)?|d3dx9_43|mss32|binkw32)\.dll\s*$'

foreach ($negativeInstallFixture in @(
    'file(INSTALL FILES "C:/fixture/d3d8to9.dll")',
    'file(INSTALL FILES "C:/fixture/d3dx8d.dll")',
    'file(INSTALL FILES "C:/fixture/licenses/native-d3d8-compat/LICENSE")')) {
    if ($negativeInstallFixture -notmatch $forbiddenNativeInstallPayloadPattern) {
        throw "Native product install-payload audit definition accepted negative fixture '$negativeInstallFixture'."
    }
}
foreach ($negativeImportFixture in @('d3d8.dll', 'd3d8to9.dll', 'd3dx8d.dll',
    'd3dx8_43.dll', 'mss32.dll', 'binkw32.dll')) {
    if ($negativeImportFixture -notmatch $forbiddenNativeImportPattern) {
        throw "Native product import audit definition accepted negative fixture '$negativeImportFixture'."
    }
}

$selectedConfiguration = $Configuration
if ($OutputSuffix -match '[\\/:;]') {
    throw 'OutputSuffix must not contain path or list separators.'
}

function Get-CanonicalPath([string] $Path, [string] $Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description is required."
    }
    $canonicalPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ([string]::IsNullOrWhiteSpace($canonicalPath)) {
        throw "$Description is invalid."
    }
    return $canonicalPath
}

$resolvedSourceRoot = Get-CanonicalPath $SourceRoot 'SourceRoot'
$resolvedAuditBuildRoot = Get-CanonicalPath $BuildRoot 'BuildRoot'
if (-not (Test-Path -LiteralPath $resolvedSourceRoot -PathType Container)) {
    throw "SourceRoot '$resolvedSourceRoot' is not an existing directory."
}
if (Test-Path -LiteralPath $resolvedAuditBuildRoot -PathType Leaf) {
    throw "BuildRoot '$resolvedAuditBuildRoot' is a file, not a dedicated audit directory."
}
$sourceRootPrefix = $resolvedSourceRoot + '\'
$buildRootPrefix = $resolvedAuditBuildRoot + '\'
if ($resolvedAuditBuildRoot.Equals($resolvedSourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedAuditBuildRoot.StartsWith($sourceRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot '$resolvedAuditBuildRoot' must not equal or be inside SourceRoot '$resolvedSourceRoot'."
}
if ($resolvedSourceRoot.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot '$resolvedAuditBuildRoot' must not contain SourceRoot '$resolvedSourceRoot'."
}
$buildRootDrive = [IO.Path]::GetPathRoot($resolvedAuditBuildRoot).TrimEnd('\')
if ($resolvedAuditBuildRoot.Equals($buildRootDrive, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot '$resolvedAuditBuildRoot' must be a dedicated directory below a drive root."
}

$auditRootMarkerName = '.native-product-runtime-audit-root'
$auditRootMarkerContents = 'NativeProductRuntimeAuditRoot=v1'
$auditRootMarkerPath = Join-Path $resolvedAuditBuildRoot $auditRootMarkerName
if (Test-Path -LiteralPath $resolvedAuditBuildRoot -PathType Container) {
    if (Test-Path -LiteralPath $auditRootMarkerPath -PathType Leaf) {
        if ((Get-Content -LiteralPath $auditRootMarkerPath -Raw).Trim() -cne $auditRootMarkerContents) {
            throw "BuildRoot '$resolvedAuditBuildRoot' has an invalid audit-root marker."
        }
    }
    else {
        $existingAuditRootEntries = @(Get-ChildItem -LiteralPath $resolvedAuditBuildRoot -Force)
        if ($existingAuditRootEntries.Count -ne 0) {
            throw "BuildRoot '$resolvedAuditBuildRoot' is not an empty or previously marked dedicated audit directory."
        }
        [IO.File]::WriteAllText($auditRootMarkerPath, $auditRootMarkerContents + [Environment]::NewLine)
    }
}
else {
    New-Item -ItemType Directory -Path $resolvedAuditBuildRoot -Force | Out-Null
    [IO.File]::WriteAllText($auditRootMarkerPath, $auditRootMarkerContents + [Environment]::NewLine)
}
$allowedAuditRootEntryNames = @($auditRootMarkerName, 'Generals', 'ZeroHour')
$unexpectedAuditRootEntries = @(Get-ChildItem -LiteralPath $resolvedAuditBuildRoot -Force |
    Where-Object { $allowedAuditRootEntryNames -notcontains $_.Name })
if ($unexpectedAuditRootEntries.Count -ne 0) {
    throw "BuildRoot '$resolvedAuditBuildRoot' contains unexpected entries: $($unexpectedAuditRootEntries.Name -join ', ')."
}
$SourceRoot = $resolvedSourceRoot
$BuildRoot = $resolvedAuditBuildRoot

function Get-ConfigurationArtifactCandidates(
    [string] $Root,
    [string] $RelativePath,
    [string] $Configuration) {
    $relative = $RelativePath.TrimStart('\', '/')
    $relativeDirectory = Split-Path -Path $relative -Parent
    $fileName = Split-Path -Path $relative -Leaf
    $parentDirectory = if ([string]::IsNullOrEmpty($relativeDirectory)) {
        $Root
    }
    else {
        Join-Path $Root $relativeDirectory
    }
    $candidates = @(
        (Join-Path $parentDirectory $fileName),
        (Join-Path $parentDirectory (Join-Path $Configuration $fileName)),
        (Join-Path $Root (Join-Path $Configuration $relative))
    )
    return @($candidates | ForEach-Object {
        [IO.Path]::GetFullPath($_)
    } | Select-Object -Unique)
}

function Resolve-ConfigurationArtifact(
    [string] $Root,
    [string] $RelativePath,
    [string] $Configuration,
    [string] $Description) {
    $candidates = @(Get-ConfigurationArtifactCandidates $Root $RelativePath $Configuration)
    $configurationCandidates = @($candidates | Where-Object {
        $_ -match ('[\\/]' + [regex]::Escape($Configuration) + '[\\/]')
    })
    $existingConfigurationCandidates = @($configurationCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    })
    if ($existingConfigurationCandidates.Count -gt 0) {
        return $existingConfigurationCandidates[0]
    }
    $existingSingleConfigurationCandidates = @($candidates | Where-Object {
        -not ($configurationCandidates -contains $_) -and
        (Test-Path -LiteralPath $_ -PathType Leaf)
    })
    if ($existingSingleConfigurationCandidates.Count -gt 0) {
        return $existingSingleConfigurationCandidates[0]
    }
    throw "$Description was not found in single- or multi-configuration output paths: $($candidates -join ', ')."
}

$CCompiler = $CCompiler.Replace('\', '/')
$CxxCompiler = $CxxCompiler.Replace('\', '/')
$RcCompiler = $RcCompiler.Replace('\', '/')
$ManifestTool = $ManifestTool.Replace('\', '/')
$FFmpegRoot = $FFmpegRoot.Replace('\', '/')
$ToolchainFile = $ToolchainFile.Replace('\', '/')
$VcpkgInstalledDir = $VcpkgInstalledDir.Replace('\', '/')
$VcpkgOverlayTriplets = $VcpkgOverlayTriplets.Replace('\', '/')
if (-not [string]::IsNullOrWhiteSpace($ToolchainFile)) {
    if (-not (Test-Path -LiteralPath $ToolchainFile -PathType Leaf)) {
        throw "Vcpkg toolchain file '$ToolchainFile' is unavailable."
    }
    if ([string]::IsNullOrWhiteSpace($VcpkgInstalledDir) -or
        -not (Test-Path -LiteralPath $VcpkgInstalledDir -PathType Container)) {
        throw "Vcpkg installed dependency root '$VcpkgInstalledDir' is unavailable."
    }
    if ([string]::IsNullOrWhiteSpace($VcpkgTargetTriplet)) {
        throw 'VcpkgTargetTriplet is required when ToolchainFile is provided.'
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio discovery tool is unavailable.'
}
$vswhereArguments = @(
    '-latest', '-products', '*',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath'
)
$installationPath = (& $vswhere @vswhereArguments | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($installationPath)) {
    throw 'Visual Studio C++ tools are unavailable.'
}
$developerCommand = Join-Path $installationPath 'Common7/Tools/VsDevCmd.bat'
$environmentLines = & cmd.exe /d /s /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 && set"
if ($LASTEXITCODE -ne 0) {
    throw 'Visual Studio x64 developer environment setup failed.'
}
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator),
            $line.Substring($separator + 1), 'Process')
    }
}

if (-not ('NativeProductRuntimeAudit.NativeLoader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace NativeProductRuntimeAudit
{
    public static class NativeLoader
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "LoadLibraryExW", SetLastError = true)]
        public static extern IntPtr LoadLibraryEx(string fileName, IntPtr file, uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool FreeLibrary(IntPtr module);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        public static extern IntPtr GetProcAddress(IntPtr module, string name);
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate IntPtr Direct3DCreate8(uint sdkVersion);
}
'@
}

$loaderSearchDllDirectory = [uint32] 0x00000100
$loaderSearchSystem32 = [uint32] 0x00000800
$loaderFlags = $loaderSearchDllDirectory -bor $loaderSearchSystem32

function Get-PeMachine([string] $Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d) {
            throw "$Path is not a PE image."
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "$Path has no PE signature."
        }
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-SameFile([string] $ExpectedPath, [string] $InstalledPath,
    [string] $Description) {
    if (-not (Test-Path -LiteralPath $ExpectedPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $InstalledPath -PathType Leaf)) {
        throw "$Description is missing."
    }
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $expectedStream = [IO.File]::OpenRead($ExpectedPath)
        try {
            $expectedHash = [BitConverter]::ToString(
                $sha256.ComputeHash($expectedStream)).Replace('-', '')
        }
        finally {
            $expectedStream.Dispose()
        }
        $installedStream = [IO.File]::OpenRead($InstalledPath)
        try {
            $installedHash = [BitConverter]::ToString(
                $sha256.ComputeHash($installedStream)).Replace('-', '')
        }
        finally {
            $installedStream.Dispose()
        }
    }
    finally {
        $sha256.Dispose()
    }
    if ($expectedHash -cne $installedHash) {
        throw "$Description does not match the exact built source artifact."
    }
}

foreach ($product in @(
    @{ Name = 'Generals'; Generals = 'ON'; ZeroHour = 'OFF'; TitleDirectory = 'Generals'; InstallDirectory = 'Generals'; Targets = @('g_generals', 'g_launcher', 'g_skirmish_ai_runner_contract_tests'); Executables = @("generalsv$OutputSuffix.exe", 'launcher.exe', 'g_skirmish_ai_runner_contract_tests.exe'); ContractExecutable = 'g_skirmish_ai_runner_contract_tests.exe'; ContractArguments = @(); LauncherCommand = "generalsv$OutputSuffix.exe" },
    @{ Name = 'ZeroHour'; Generals = 'OFF'; ZeroHour = 'ON'; TitleDirectory = 'GeneralsMD'; InstallDirectory = 'ZeroHour'; Targets = @('z_generals', 'z_launcher', 'z_runtime_regression_tests'); Executables = @("generalszh$OutputSuffix.exe", 'launcher.exe', 'z_runtime_regression_tests.exe'); ContractExecutable = 'z_runtime_regression_tests.exe'; ContractArguments = @('--skirmish-ai-replay-epoch'); LauncherCommand = "generalszh$OutputSuffix.exe" }
)) {
    $productBuildRoot = Join-Path $BuildRoot $product.Name
    $installRoot = Join-Path $productBuildRoot 'InstallRoot'
    $resolvedProductBuildRoot = [IO.Path]::GetFullPath($productBuildRoot).TrimEnd('\')
    $resolvedInstallRoot = [IO.Path]::GetFullPath($installRoot)
    if (-not $resolvedProductBuildRoot.StartsWith($resolvedAuditBuildRoot + '\',
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "Native x64 $($product.Name) build root escaped its audit root."
    }
    if (-not $resolvedInstallRoot.StartsWith($resolvedProductBuildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Native x64 $($product.Name) install root escaped its audit build root."
    }
    if (Test-Path -LiteralPath $resolvedProductBuildRoot) {
        Remove-Item -LiteralPath $resolvedProductBuildRoot -Recurse -Force
    }
    $arguments = @(
        '--fresh',
        '-S', $SourceRoot,
        '-B', $productBuildRoot,
        '-G', $Generator,
        "-DCMAKE_C_COMPILER=$CCompiler",
        "-DCMAKE_CXX_COMPILER=$CxxCompiler",
        "-DCMAKE_RC_COMPILER=$RcCompiler",
        "-DCMAKE_MT=$ManifestTool",
        "-DCMAKE_INSTALL_PREFIX=$($installRoot.Replace('\', '/'))",
        '-DRTS_BUILD_PRODUCT=ON',
        "-DRTS_BUILD_ZEROHOUR=$($product.ZeroHour)",
        "-DRTS_BUILD_GENERALS=$($product.Generals)",
        "-DRTS_BUILD_ZEROHOUR_PRODUCT=$($product.ZeroHour)",
        "-DRTS_BUILD_GENERALS_PRODUCT=$($product.Generals)",
        '-DRTS_BUILD_GENERALS_TOOLS=OFF',
        '-DRTS_BUILD_GENERALS_EXTRAS=OFF',
        '-DRTS_BUILD_GENERALS_DOCS=OFF',
        '-DRTS_BUILD_ZEROHOUR_TOOLS=OFF',
        '-DRTS_BUILD_CORE_TOOLS=OFF',
        '-DRTS_BUILD_CORE_EXTRAS=OFF',
        '-DRTS_BUILD_OPTION_FFMPEG=ON',
        "-DRTS_BUILD_OUTPUT_SUFFIX=$OutputSuffix",
        "-DFFMPEG_ROOT=$FFmpegRoot"
    )
    if (-not [string]::IsNullOrWhiteSpace($ToolchainFile)) {
        $arguments += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
    }
    if (-not [string]::IsNullOrWhiteSpace($FFmpegRuntimeDir)) {
        $arguments += "-DFFMPEG_RUNTIME_DIR=$FFmpegRuntimeDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($VcpkgInstalledDir)) {
        $arguments += "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($VcpkgTargetTriplet)) {
        $arguments += "-DVCPKG_TARGET_TRIPLET=$VcpkgTargetTriplet"
    }
    if (-not [string]::IsNullOrWhiteSpace($VcpkgOverlayTriplets)) {
        $arguments += "-DVCPKG_OVERLAY_TRIPLETS=$VcpkgOverlayTriplets"
    }
    if (-not [string]::IsNullOrWhiteSpace($GeneratorPlatform)) {
        $arguments += @('-A', $GeneratorPlatform)
    }

    & cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) product graph generation failed."
    }

    $installScriptPath = Join-Path $productBuildRoot "$($product.TitleDirectory)/cmake_install.cmake"
    if (-not (Test-Path -LiteralPath $installScriptPath)) {
        throw "Native x64 $($product.Name) title install script was not generated."
    }
    $installScript = (Get-Content -LiteralPath $installScriptPath -Raw).Replace('\', '/')
    $expectedDestination = '${CMAKE_INSTALL_PREFIX}/' + $product.InstallDirectory
    foreach ($executable in $product.Executables) {
        $executableInstallPattern = 'file\(INSTALL DESTINATION "' +
            [regex]::Escape($expectedDestination) + '" TYPE EXECUTABLE FILES "[^"]*/' +
            [regex]::Escape($executable) + '"\)'
        if ($installScript -notmatch $executableInstallPattern) {
            throw "Native x64 $($product.Name) install script is missing $executable below CMAKE_INSTALL_PREFIX/$($product.InstallDirectory)."
        }
    }
    $launcherConfigInstallPattern = 'file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) + '" TYPE FILE FILES "[^"]*/launcher\.lcf"\)'
    if ($installScript -notmatch $launcherConfigInstallPattern) {
        throw "Native x64 $($product.Name) install script is missing launcher.lcf."
    }

    $cachePath = Join-Path $productBuildRoot 'CMakeCache.txt'
    $cache = Get-Content -LiteralPath $cachePath -Raw
    $legacyD3D8Requirement = [regex]::Match($cache,
        '(?m)^RTS_NATIVE_PRODUCT_REQUIRES_LEGACY_D3D8:INTERNAL=(ON|OFF)\r?$')
    if (-not $legacyD3D8Requirement.Success -or
        $legacyD3D8Requirement.Groups[1].Value -cne 'OFF') {
        throw "Native x64 $($product.Name) product does not declare the completed D3D8 device cutover."
    }
    $resourceClosure = [regex]::Match($cache,
        '(?m)^RTS_NATIVE_PRODUCT_RESOURCE_CLOSURE_COMPLETE:INTERNAL=(ON|OFF)\r?$')
    if (-not $resourceClosure.Success -or
        $resourceClosure.Groups[1].Value -cne 'ON') {
        throw "Native x64 $($product.Name) product audit is blocked: sampled-texture and surface ownership has not crossed the D3D11 resource boundary."
    }
    $runtimeDllMatch = [regex]::Match($cache, '(?m)^RTS_FFMPEG_RUNTIME_DLLS:INTERNAL=(.+)$')
    if (-not $runtimeDllMatch.Success) {
        throw "Native x64 $($product.Name) product did not resolve FFmpeg runtime DLLs."
    }
    $runtimeDlls = @($runtimeDllMatch.Groups[1].Value.Trim() -split ';')
    $msvcRuntimeCacheName = if ($selectedConfiguration -eq 'Debug') {
        'RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG'
    }
    else {
        'RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE'
    }
    $msvcRuntimeDllMatch = [regex]::Match($cache,
        '(?m)^' + [regex]::Escape($msvcRuntimeCacheName) + ':INTERNAL=(.+)$')
    if (-not $msvcRuntimeDllMatch.Success) {
        throw "Native x64 $($product.Name) product did not resolve app-local MSVC runtime DLLs."
    }
    $msvcRuntimeDlls = @($msvcRuntimeDllMatch.Groups[1].Value.Trim() -split ';')
    $zlibRuntimeCacheName = if ($selectedConfiguration -eq 'Debug') {
        'RTS_NATIVE_ZLIB_RUNTIME_DLLS_DEBUG'
    }
    else {
        'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE'
    }
    $zlibRuntimeDllMatch = [regex]::Match($cache,
        '(?m)^' + [regex]::Escape($zlibRuntimeCacheName) + ':INTERNAL=(.+)$')
    $zlibRuntimeDlls = if ($zlibRuntimeDllMatch.Success) {
        @($zlibRuntimeDllMatch.Groups[1].Value.Trim() -split ';')
    }
    else {
        @()
    }
    if (-not [string]::IsNullOrWhiteSpace($ToolchainFile) -and
        $VcpkgTargetTriplet -eq 'x64-windows' -and
        $zlibRuntimeDlls.Count -eq 0) {
        throw "Native x64 $($product.Name) product did not resolve the dynamic vcpkg zlib runtime."
    }
    $runtimeInstallBlockPattern = '(?ms)^[ \t]*file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) + '" TYPE FILE FILES(?<Files>.*?)\)[ \t]*\r?$'
    $runtimeInstallBlocks = [regex]::Matches($installScript, $runtimeInstallBlockPattern)
    foreach ($runtimeDll in @($runtimeDlls + $msvcRuntimeDlls + $zlibRuntimeDlls)) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $runtimeDllPattern = '"[^"]*/' + [regex]::Escape($runtimeDllName) + '"'
        $matchingBlock = $runtimeInstallBlocks | Where-Object {
            $_.Groups['Files'].Value -match $runtimeDllPattern
        } | Select-Object -First 1
        if ($null -eq $matchingBlock) {
            throw "Native x64 $($product.Name) install script is missing runtime $runtimeDllName below CMAKE_INSTALL_PREFIX/$($product.InstallDirectory)."
        }
    }
    if ($installScript -match $forbiddenNativeInstallPayloadPattern) {
        throw "Native x64 $($product.Name) install script retains a forbidden D3D8/D3DX/Miles/Bink compatibility payload."
    }

    $selectionPath = Join-Path $productBuildRoot 'product_runtime_selection.txt'
    if (-not (Test-Path -LiteralPath $selectionPath)) {
        throw "Native x64 $($product.Name) architecture selection closure was not generated."
    }
    $selection = Get-Content -LiteralPath $selectionPath -Raw
    if ($selection -notmatch '(?m)^target=rts_product_runtime\r?$' -or
        $selection -notmatch '(?m)^links=rts_native_product_runtime\r?$' -or
        $selection -match '\brts_legacy_product_runtime\b') {
        throw "Native x64 $($product.Name) does not select rts_native_product_runtime exclusively."
    }

    $closurePath = Join-Path $productBuildRoot 'native_product_runtime_link_closure.txt'
    if (-not (Test-Path -LiteralPath $closurePath)) {
        throw "Native x64 $($product.Name) product link closure was not generated."
    }

    $closure = Get-Content -LiteralPath $closurePath -Raw
    if ($closure -notmatch '(?m)^target=rts_native_product_runtime\r?$' -or
        $closure -notmatch '(?m)^requires_legacy_d3d8=OFF\r?$') {
        throw "Native x64 $($product.Name) runtime closure does not report the completed D3D8 cutover."
    }
    if ($closure -notmatch '(?m)^resource_closure_complete=ON\r?$') {
        throw "Native x64 $($product.Name) runtime closure reports an incomplete D3D11 sampled-texture/surface cutover."
    }
    foreach ($required in @('rts_xaudio2', 'bcrypt', 'd3d11', 'dxgi', 'dinput8', 'dxguid')) {
        if ($closure -notmatch ("\b" + [regex]::Escape($required) + "\b")) {
            throw "Native x64 $($product.Name) product is missing required dependency '$required'."
        }
    }
    foreach ($forbidden in @('milesstub', 'binkstub', 'rts_d3d8lib', 'd3d8', 'd3dx8',
        'rts_native_d3d8_compat_boundary', 'd3d8to9')) {
        if ($closure -match ("\b" + [regex]::Escape($forbidden) + "\b")) {
            throw "Native x64 $($product.Name) product retains forbidden dependency '$forbidden'."
        }
    }

    $targetDirectoriesPath = Join-Path $productBuildRoot 'CMakeFiles/TargetDirectories.txt'
    $targetDirectories = Get-Content -LiteralPath $targetDirectoriesPath -Raw
    if ($targetDirectories -match 'core_browserenginewin') {
        throw "Native x64 $($product.Name) product still generates the legacy BrowserEngine COM target."
    }
    $ninjaGraphPath = Join-Path $productBuildRoot 'build.ninja'
    $configurationNinjaGraphPath = Join-Path $productBuildRoot (
        'CMakeFiles/impl-' + $selectedConfiguration + '.ninja')
    if (Test-Path -LiteralPath $ninjaGraphPath) {
        $ninjaGraph = Get-Content -LiteralPath $ninjaGraphPath -Raw
        if (Test-Path -LiteralPath $configurationNinjaGraphPath) {
            $ninjaGraph += Get-Content -LiteralPath $configurationNinjaGraphPath -Raw
        }
        if ($ninjaGraph -match '(BrowserEngine\.idl|BrowserEngine\.tlb|BrowserEngine\.DLL|core_browserenginewin)') {
            throw "Native x64 $($product.Name) product graph still references the legacy BrowserEngine contract."
        }
        if ($ninjaGraph -notmatch 'RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=0') {
            throw "Native x64 $($product.Name) product did not select the browser-safe no-op implementation."
        }
    }

    $buildArguments = @(
        '--build', $productBuildRoot,
        '--config', $selectedConfiguration,
        '--parallel', '4',
        '--target'
    ) + $product.Targets
    & cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) exact product target build failed."
    }

    & cmake --install $productBuildRoot --config $selectedConfiguration --prefix $installRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) installed-runtime generation failed."
    }

    $installedTitleRoot = Join-Path $installRoot $product.InstallDirectory
    foreach ($executable in $product.Executables) {
        $installedExecutable = Join-Path $installedTitleRoot $executable
        if (-not (Test-Path -LiteralPath $installedExecutable -PathType Leaf)) {
            throw "Native x64 $($product.Name) audit did not produce installed executable $executable."
        }
        $builtExecutable = Resolve-ConfigurationArtifact $productBuildRoot (Join-Path $product.TitleDirectory $executable) $selectedConfiguration "Native x64 $($product.Name) built executable $executable"
        Assert-SameFile $builtExecutable $installedExecutable "Native x64 $($product.Name) installed executable $executable"
        if ((Get-PeMachine $installedExecutable) -ne 0x8664) {
            throw "Native x64 $($product.Name) installed executable $executable is not an AMD64 PE image."
        }
        $imports = (& dumpbin.exe /nologo /imports $installedExecutable 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "Native x64 $($product.Name) could not inspect imports for $executable."
        }
        if ($imports -match $forbiddenNativeImportPattern) {
            throw "Native x64 $($product.Name) installed executable $executable directly imports a forbidden legacy D3D8/D3DX/Miles/Bink runtime."
        }
    }

    # An executable-only check misses forbidden legacy dependencies imported
    # by an app-local helper or codec DLL. Audit every installed PE image so
    # the complete redistributable closure is architecture-correct and clean.
    $installedPeImages = @(Get-ChildItem -LiteralPath $installedTitleRoot -Recurse -File | Where-Object {
            $_.Extension -ieq '.exe' -or $_.Extension -ieq '.dll'
        })
    if ($installedPeImages.Count -eq 0) {
        throw "Native x64 $($product.Name) install contains no auditable PE images."
    }
    foreach ($installedPeImage in $installedPeImages) {
        if ((Get-PeMachine $installedPeImage.FullName) -ne 0x8664) {
            throw "Native x64 $($product.Name) app-local PE image $($installedPeImage.Name) is not AMD64."
        }
        $peImports = (& dumpbin.exe /nologo /imports $installedPeImage.FullName 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "Native x64 $($product.Name) could not inspect app-local imports for $($installedPeImage.Name)."
        }
        if ($peImports -match $forbiddenNativeImportPattern) {
            throw "Native x64 $($product.Name) app-local PE image $($installedPeImage.Name) imports a forbidden legacy D3D8/D3DX/Miles/Bink runtime."
        }
    }
    $installedLauncherConfig = Join-Path $installedTitleRoot 'launcher.lcf'
    if (-not (Test-Path -LiteralPath $installedLauncherConfig -PathType Leaf)) {
        throw "Native x64 $($product.Name) audit did not produce launcher.lcf."
    }
    $expectedLauncherConfig = "RUN = . $($product.LauncherCommand) -simulationMode parallel -workerPolicy auto"
    if ((Get-Content -LiteralPath $installedLauncherConfig -Raw).Trim() -cne $expectedLauncherConfig) {
        throw "Native x64 $($product.Name) launcher.lcf does not select $($product.LauncherCommand) with the Stage 5 parallel/automatic policy."
    }
    foreach ($runtimeDll in $runtimeDlls) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $installedRuntimeDll = Join-Path $installedTitleRoot $runtimeDllName
        if (-not (Test-Path -LiteralPath $installedRuntimeDll -PathType Leaf)) {
            throw "Native x64 $($product.Name) audit did not produce installed FFmpeg runtime $runtimeDllName."
        }
        Assert-SameFile $runtimeDll $installedRuntimeDll "Native x64 $($product.Name) installed FFmpeg runtime $runtimeDllName"

        $module = [NativeProductRuntimeAudit.NativeLoader]::LoadLibraryEx(
            $installedRuntimeDll,
            [IntPtr]::Zero,
            $loaderFlags)
        if ($module -eq [IntPtr]::Zero) {
            $loaderError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Native x64 $($product.Name) installed FFmpeg runtime $runtimeDllName failed clean dependency loading with Windows error $loaderError."
        }
        if (-not [NativeProductRuntimeAudit.NativeLoader]::FreeLibrary($module)) {
            $loaderError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Native x64 $($product.Name) installed FFmpeg runtime $runtimeDllName failed to unload with Windows error $loaderError."
        }
    }
    foreach ($runtimeDll in $msvcRuntimeDlls) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $installedRuntimeDll = Join-Path $installedTitleRoot $runtimeDllName
        if (-not (Test-Path -LiteralPath $installedRuntimeDll -PathType Leaf)) {
            throw "Native x64 $($product.Name) audit did not produce app-local MSVC runtime $runtimeDllName."
        }
        Assert-SameFile $runtimeDll $installedRuntimeDll "Native x64 $($product.Name) installed MSVC runtime $runtimeDllName"
    }

    foreach ($runtimeDll in $zlibRuntimeDlls) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $installedRuntimeDll = Join-Path $installedTitleRoot $runtimeDllName
        if (-not (Test-Path -LiteralPath $installedRuntimeDll -PathType Leaf)) {
            throw "Native x64 $($product.Name) audit did not produce app-local zlib runtime $runtimeDllName."
        }
        Assert-SameFile $runtimeDll $installedRuntimeDll "Native x64 $($product.Name) installed zlib runtime $runtimeDllName"
        if ((Get-PeMachine $installedRuntimeDll) -ne 0x8664) {
            throw "Native x64 $($product.Name) zlib runtime $runtimeDllName is not an AMD64 PE image."
        }
        $module = [NativeProductRuntimeAudit.NativeLoader]::LoadLibraryEx(
            $installedRuntimeDll,
            [IntPtr]::Zero,
            $loaderFlags)
        if ($module -eq [IntPtr]::Zero) {
            $loaderError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Native x64 $($product.Name) installed zlib runtime $runtimeDllName failed clean dependency loading with Windows error $loaderError."
        }
        if (-not [NativeProductRuntimeAudit.NativeLoader]::FreeLibrary($module)) {
            $loaderError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Native x64 $($product.Name) installed zlib runtime $runtimeDllName failed to unload with Windows error $loaderError."
        }
    }

    foreach ($forbiddenRuntime in @('d3d8.dll', 'd3d8to9.dll', 'd3dx8.dll',
        'd3dx8d.dll', 'D3DCompiler_43.dll', 'D3DX9_43.dll', 'mss32.dll',
        'binkw32.dll')) {
        $installedForbiddenRuntime = Join-Path $installedTitleRoot $forbiddenRuntime
        if (Test-Path -LiteralPath $installedForbiddenRuntime) {
            throw "Native x64 $($product.Name) install contains forbidden legacy compatibility runtime $forbiddenRuntime."
        }
    }

    $installedContract = Join-Path $installedTitleRoot $product.ContractExecutable
    Push-Location $installedTitleRoot
    try {
        & $installedContract
        $contractExitCode = $LASTEXITCODE
        if ($contractExitCode -eq 0 -and $product.ContractArguments.Count -gt 0) {
            $contractArguments = @($product.ContractArguments)
            & $installedContract @contractArguments
            $contractExitCode = $LASTEXITCODE
        }
    }
    finally {
        Pop-Location
    }
    if ($contractExitCode -ne 0) {
        throw "Native x64 $($product.Name) installed contract utility failed."
    }

    $compatibilityLicenseRoot = Join-Path $installedTitleRoot 'licenses/native-d3d8-compat'
    if (Test-Path -LiteralPath $compatibilityLicenseRoot) {
        throw "Native x64 $($product.Name) install contains the retired D3D8 compatibility license package."
    }
}

Write-Output 'Native x64 Generals and Zero Hour exact product build, install, and runtime dependency audits passed.'
