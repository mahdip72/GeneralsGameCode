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

foreach ($product in @(
    @{ Name = 'Generals'; Generals = 'ON'; ZeroHour = 'OFF'; TitleDirectory = 'Generals'; InstallDirectory = 'Generals'; Executable = 'generalsv.exe' },
    @{ Name = 'ZeroHour'; Generals = 'OFF'; ZeroHour = 'ON'; TitleDirectory = 'GeneralsMD'; InstallDirectory = 'ZeroHour'; Executable = 'generalszh.exe' }
)) {
    $productBuildRoot = Join-Path $BuildRoot $product.Name
    $installRoot = Join-Path $productBuildRoot 'InstallRoot'
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
    $executableInstallPattern = 'file\(INSTALL DESTINATION "' +
        [regex]::Escape($expectedDestination) + '" TYPE EXECUTABLE FILES "[^"]*/' +
        [regex]::Escape($product.Executable) + '"\)'
    if ($installScript -notmatch $executableInstallPattern) {
        throw "Native x64 $($product.Name) product is not installable below CMAKE_INSTALL_PREFIX/$($product.InstallDirectory)."
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
}

Write-Output 'Native x64 Generals and Zero Hour product runtime graph audits passed.'
