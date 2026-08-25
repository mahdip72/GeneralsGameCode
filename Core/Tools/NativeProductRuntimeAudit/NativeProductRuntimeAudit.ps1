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
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string] $Configuration = 'Release',
    [string] $OutputSuffix = ''
)

$ErrorActionPreference = 'Stop'

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
    @{ Name = 'Generals'; Generals = 'ON'; ZeroHour = 'OFF'; TitleDirectory = 'Generals'; InstallDirectory = 'Generals'; Targets = @('g_generals', 'g_launcher', 'core_native_d3d8_compatibility_test'); Executables = @("generalsv$OutputSuffix.exe", 'launcher.exe'); LauncherCommand = "generalsv$OutputSuffix.exe" },
    @{ Name = 'ZeroHour'; Generals = 'OFF'; ZeroHour = 'ON'; TitleDirectory = 'GeneralsMD'; InstallDirectory = 'ZeroHour'; Targets = @('z_generals', 'z_launcher', 'z_runtime_regression_tests', 'core_native_d3d8_compatibility_test'); Executables = @("generalszh$OutputSuffix.exe", 'launcher.exe', 'z_runtime_regression_tests.exe'); LauncherCommand = "generalszh$OutputSuffix.exe" }
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
    $d3dxRuntimeDllMatch = [regex]::Match($cache, '(?m)^RTS_NATIVE_D3DX_RUNTIME_DLLS:INTERNAL=(.+)$')
    if (-not $d3dxRuntimeDllMatch.Success) {
        throw "Native x64 $($product.Name) product did not resolve app-local D3DX compatibility DLLs."
    }
    $d3dxRuntimeDlls = @($d3dxRuntimeDllMatch.Groups[1].Value.Trim() -split ';')
    $runtimeInstallBlockPattern = '(?ms)^[ \t]*file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) + '" TYPE FILE FILES(?<Files>.*?)\)[ \t]*\r?$'
    $runtimeInstallBlocks = [regex]::Matches($installScript, $runtimeInstallBlockPattern)
    foreach ($runtimeDll in @($runtimeDlls + $msvcRuntimeDlls + $d3dxRuntimeDlls)) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $runtimeDllPattern = '"[^"]*/' + [regex]::Escape($runtimeDllName) + '"'
        $matchingBlock = $runtimeInstallBlocks | Where-Object {
            $_.Groups['Files'].Value -match $runtimeDllPattern
        } | Select-Object -First 1
        if ($null -eq $matchingBlock) {
            throw "Native x64 $($product.Name) install script is missing runtime $runtimeDllName below CMAKE_INSTALL_PREFIX/$($product.InstallDirectory)."
        }
    }
    $d3d8InstallPattern = 'file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) +
        '" TYPE SHARED_LIBRARY FILES "(?<Source>[^"]*/d3d8\.dll)"\)'
    $d3d8InstallMatches = @([regex]::Matches($installScript, $d3d8InstallPattern))
    if ($d3d8InstallMatches.Count -eq 0) {
        throw "Native x64 $($product.Name) install script is missing the app-local d3d8.dll compatibility module."
    }
    $expectedD3D8Sources = @(Get-ConfigurationArtifactCandidates $productBuildRoot '_deps/native_d3d8_compat-build/d3d8.dll' $selectedConfiguration |
        ForEach-Object { $_.Replace('\', '/') })
    $configuredD3D8Sources = @($d3d8InstallMatches | ForEach-Object {
        [IO.Path]::GetFullPath($_.Groups['Source'].Value).Replace('\', '/')
    })
    $hasExpectedD3D8Source = @($configuredD3D8Sources | Where-Object {
        $configuredSource = $_
        @($expectedD3D8Sources | Where-Object {
            $_.Equals($configuredSource, [StringComparison]::OrdinalIgnoreCase)
        }).Count -gt 0
    }).Count -gt 0
    if (-not $hasExpectedD3D8Source) {
        throw "Native x64 $($product.Name) install script selects an unexpected d3d8.dll source."
    }

    $closurePath = Join-Path $productBuildRoot 'native_product_runtime_link_closure.txt'
    if (-not (Test-Path -LiteralPath $closurePath)) {
        throw "Native x64 $($product.Name) product link closure was not generated."
    }

    $closure = Get-Content -LiteralPath $closurePath -Raw
    foreach ($required in @('rts_xaudio2', 'rts_d3d8_headers', 'rts_native_d3d8_compat_boundary', 'bcrypt', 'd3d11', 'dxgi', 'dinput8', 'dxguid')) {
        if ($closure -notmatch ("\b" + [regex]::Escape($required) + "\b")) {
            throw "Native x64 $($product.Name) product is missing required dependency '$required'."
        }
    }
    foreach ($forbidden in @('milesstub', 'binkstub', 'rts_d3d8lib', 'd3d8')) {
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
        if ($imports -match '(?im)^\s*d3d[89]\.dll\s*$') {
            throw "Native x64 $($product.Name) installed executable $executable directly imports a legacy D3D runtime."
        }
    }
    $installedLauncherConfig = Join-Path $installedTitleRoot 'launcher.lcf'
    if (-not (Test-Path -LiteralPath $installedLauncherConfig -PathType Leaf)) {
        throw "Native x64 $($product.Name) audit did not produce launcher.lcf."
    }
    $expectedLauncherConfig = "RUN = . $($product.LauncherCommand)"
    if ((Get-Content -LiteralPath $installedLauncherConfig -Raw).Trim() -cne $expectedLauncherConfig) {
        throw "Native x64 $($product.Name) launcher.lcf does not select $($product.LauncherCommand)."
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

    foreach ($runtimeDll in $d3dxRuntimeDlls) {
        $runtimeName = [IO.Path]::GetFileName($runtimeDll)
        $installedRuntimeDll = Join-Path $installedTitleRoot $runtimeName
        Assert-SameFile $runtimeDll $installedRuntimeDll "Native x64 $($product.Name) installed compatibility runtime $runtimeName"
        if ((Get-PeMachine $installedRuntimeDll) -ne 0x8664) {
            throw "Native x64 $($product.Name) compatibility runtime $runtimeName is not an AMD64 PE image."
        }
    }
    $builtD3D8 = Resolve-ConfigurationArtifact $productBuildRoot '_deps/native_d3d8_compat-build/d3d8.dll' $selectedConfiguration "Native x64 $($product.Name) built d3d8 compatibility module"
    $installedD3D8 = Join-Path $installedTitleRoot 'd3d8.dll'
    Assert-SameFile $builtD3D8 $installedD3D8 "Native x64 $($product.Name) installed d3d8 compatibility module"
    if ((Get-PeMachine $installedD3D8) -ne 0x8664) {
        throw "Native x64 $($product.Name) d3d8 compatibility module is not an AMD64 PE image."
    }
    $d3d8Imports = (& dumpbin.exe /nologo /imports $installedD3D8 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $d3d8Imports -notmatch '(?im)^\s*d3d9\.dll\s*$') {
        throw "Native x64 $($product.Name) d3d8 compatibility module does not import the expected system D3D9 runtime."
    }

    $compatibilityProbe = Resolve-ConfigurationArtifact $productBuildRoot 'Core/core_native_d3d8_compatibility_test.exe' $selectedConfiguration "Native x64 $($product.Name) built compatibility behavior probe"
    & $compatibilityProbe $installedD3D8 (Join-Path $installedTitleRoot 'D3DCompiler_43.dll') (Join-Path $installedTitleRoot 'D3DX9_43.dll')
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) installed compatibility runtime behavior probe failed."
    }

    $compatibilityLicenseRoot = Join-Path $installedTitleRoot 'licenses/native-d3d8-compat'
    foreach ($licenseName in @('d3d8to9-LICENSE.md', 'LICENSE.txt', 'NOTICE.md')) {
        if (-not (Test-Path -LiteralPath (Join-Path $compatibilityLicenseRoot $licenseName) -PathType Leaf)) {
            throw "Native x64 $($product.Name) install is missing compatibility notice $licenseName."
        }
    }
}

Write-Output 'Native x64 Generals and Zero Hour exact product build, install, and runtime dependency audits passed.'
