param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot
)

$files = @(
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.h',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.h',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp'
)

$servicePattern = '(?i)\bD3DX(?:[A-Za-z0-9_]+|_[A-Za-z0-9_]+)\b|<d3dx[^>]*>'
$violations = @()
foreach ($relativePath in $files) {
    $path = Join-Path $SourceRoot $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $violations += "missing boundary file: $relativePath"
        continue
    }
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $path) {
        $lineNumber++
        if ($line -match $servicePattern) {
            $violations += "${relativePath}:$lineNumber`: $line"
        }
    }
}

if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output 'Legacy D3DX service boundary is clean.'
