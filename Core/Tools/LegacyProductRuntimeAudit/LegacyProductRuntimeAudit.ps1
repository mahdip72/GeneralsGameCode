param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Test-DependencyToken([string]$Content, [string]$Dependency)
{
    return $Content -match ("\b" + [regex]::Escape($Dependency) + "\b")
}

if ($SelfTest) {
    if (-not (Test-DependencyToken 'target_link_libraries(example PRIVATE binkstub)' 'binkstub')) {
        throw 'Legacy product runtime audit self-test did not detect an exact dependency token.'
    }
    if (Test-DependencyToken 'target_link_libraries(example PRIVATE binkstub_extra)' 'binkstub') {
        throw 'Legacy product runtime audit self-test matched a dependency prefix.'
    }
    Write-Output 'Legacy product runtime audit self-test passed.'
    exit 0
}

$runtimeModule = Join-Path $SourceRoot 'cmake/legacy-product-runtime.cmake'
if (-not (Test-Path -LiteralPath $runtimeModule)) {
    throw "Legacy product runtime module is missing: $runtimeModule"
}

$requiredConsumers = @(
    'Core/GameEngine/CMakeLists.txt',
    'Core/GameEngineDevice/CMakeLists.txt',
    'Core/Libraries/Source/WWVegas/CMakeLists.txt',
    'Generals/Code/Libraries/Source/WWVegas/CMakeLists.txt',
    'GeneralsMD/Code/Libraries/Source/WWVegas/CMakeLists.txt',
    'Generals/Code/Main/CMakeLists.txt',
    'GeneralsMD/Code/Main/CMakeLists.txt'
)

$legacyDependencies = @('binkstub', 'milesstub', 'rts_d3d8lib', 'dinput8', 'dxguid')
foreach ($relativePath in $requiredConsumers) {
    $path = Join-Path $SourceRoot $relativePath
    $content = Get-Content -LiteralPath $path -Raw
    if ($content -notmatch '\brts_legacy_product_runtime\b') {
        throw "Missing rts_legacy_product_runtime boundary in $relativePath"
    }

    foreach ($dependency in $legacyDependencies) {
        if (Test-DependencyToken $content $dependency) {
            throw "Legacy dependency $dependency bypasses rts_legacy_product_runtime in $relativePath"
        }
    }
}

Write-Output 'Legacy product runtime dependency audit passed.'
