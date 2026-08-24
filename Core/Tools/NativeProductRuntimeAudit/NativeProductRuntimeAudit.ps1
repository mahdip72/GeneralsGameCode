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
    [string] $FFmpegRoot
)

$ErrorActionPreference = 'Stop'

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
    }
}
'@
}

$loaderSearchDllDirectory = [uint32] 0x00000100
$loaderSearchSystem32 = [uint32] 0x00000800
$loaderFlags = $loaderSearchDllDirectory -bor $loaderSearchSystem32

foreach ($product in @(
    @{ Name = 'Generals'; Generals = 'ON'; ZeroHour = 'OFF'; TitleDirectory = 'Generals'; InstallDirectory = 'Generals'; Targets = @('g_generals', 'g_launcher'); Executables = @('generalsv.exe', 'launcher.exe'); LauncherCommand = 'generalsv.exe' },
    @{ Name = 'ZeroHour'; Generals = 'OFF'; ZeroHour = 'ON'; TitleDirectory = 'GeneralsMD'; InstallDirectory = 'ZeroHour'; Targets = @('z_generals', 'z_launcher', 'z_runtime_regression_tests'); Executables = @('generalszh.exe', 'launcher.exe', 'z_runtime_regression_tests.exe'); LauncherCommand = 'generalszh.exe' }
)) {
    $productBuildRoot = Join-Path $BuildRoot $product.Name
    $installRoot = Join-Path $productBuildRoot 'InstallRoot'
    $resolvedProductBuildRoot = [IO.Path]::GetFullPath($productBuildRoot).TrimEnd('\')
    $resolvedInstallRoot = [IO.Path]::GetFullPath($installRoot)
    if (-not $resolvedInstallRoot.StartsWith($resolvedProductBuildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Native x64 $($product.Name) install root escaped its audit build root."
    }
    if (Test-Path -LiteralPath $resolvedInstallRoot) {
        Remove-Item -LiteralPath $resolvedInstallRoot -Recurse -Force
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
    $msvcRuntimeDllMatch = [regex]::Match($cache, '(?m)^RTS_NATIVE_MSVC_RUNTIME_DLLS:INTERNAL=(.+)$')
    if (-not $msvcRuntimeDllMatch.Success) {
        throw "Native x64 $($product.Name) product did not resolve app-local MSVC runtime DLLs."
    }
    $msvcRuntimeDlls = @($msvcRuntimeDllMatch.Groups[1].Value.Trim() -split ';')
    $runtimeInstallBlockPattern = '(?ms)^\s*file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) + '" TYPE FILE FILES(?<Files>[^)]*)\)'
    $runtimeInstallBlocks = [regex]::Matches($installScript, $runtimeInstallBlockPattern)
    foreach ($runtimeDll in @($runtimeDlls + $msvcRuntimeDlls)) {
        $runtimeDllName = [IO.Path]::GetFileName($runtimeDll)
        $runtimeDllPattern = '"[^"]*/' + [regex]::Escape($runtimeDllName) + '"'
        $matchingBlock = $runtimeInstallBlocks | Where-Object {
            $_.Groups['Files'].Value -match $runtimeDllPattern
        } | Select-Object -First 1
        if ($null -eq $matchingBlock) {
            throw "Native x64 $($product.Name) install script is missing runtime $runtimeDllName below CMAKE_INSTALL_PREFIX/$($product.InstallDirectory)."
        }
    }

    $closurePath = Join-Path $productBuildRoot 'native_product_runtime_link_closure.txt'
    if (-not (Test-Path -LiteralPath $closurePath)) {
        throw "Native x64 $($product.Name) product link closure was not generated."
    }

    $closure = Get-Content -LiteralPath $closurePath -Raw
    foreach ($required in @('rts_xaudio2', 'rts_d3d8_headers', 'bcrypt', 'd3d11', 'dxgi', 'dinput8', 'dxguid')) {
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
    if (Test-Path -LiteralPath $ninjaGraphPath) {
        $ninjaGraph = Get-Content -LiteralPath $ninjaGraphPath -Raw
        if ($ninjaGraph -match '(BrowserEngine\.idl|BrowserEngine\.tlb|BrowserEngine\.DLL|core_browserenginewin)') {
            throw "Native x64 $($product.Name) product graph still references the legacy BrowserEngine contract."
        }
        if ($ninjaGraph -notmatch 'RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=0') {
            throw "Native x64 $($product.Name) product did not select the browser-safe no-op implementation."
        }
    }

    $buildArguments = @(
        '--build', $productBuildRoot,
        '--config', 'Release',
        '--parallel', '4',
        '--target'
    ) + $product.Targets
    & cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) exact product target build failed."
    }

    & cmake --install $productBuildRoot --config Release --prefix $installRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Native x64 $($product.Name) installed-runtime generation failed."
    }

    $installedTitleRoot = Join-Path $installRoot $product.InstallDirectory
    foreach ($executable in $product.Executables) {
        $installedExecutable = Join-Path $installedTitleRoot $executable
        if (-not (Test-Path -LiteralPath $installedExecutable -PathType Leaf)) {
            throw "Native x64 $($product.Name) audit did not produce installed executable $executable."
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
    }
}

Write-Output 'Native x64 Generals and Zero Hour exact product build, install, and runtime dependency audits passed.'
