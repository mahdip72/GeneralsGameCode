param(
    [string]$SourceRoot = (Get-Location).Path,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Text)

    $withoutBlocks = [regex]::Replace($Text, '/\*[\s\S]*?\*/', {
        param($Match)
        return ($Match.Value -replace '[^\r\n]', ' ')
    })
    return [regex]::Replace($withoutBlocks, '//[^\r\n]*', '')
}

function Get-LegacyAdapterMatches {
    param([Parameter(Mandatory = $true)][string]$Text)

    $pattern = '(?i)\b(?:d3d8\.h|d3d8types\.h|dx8wrapper\.h|IDirect3D(?:Device|Texture|Surface|VertexBuffer|IndexBuffer|BaseTexture)?8)\b'
    return @([regex]::Matches((Remove-CppComments $Text), $pattern))
}

if ($SelfTest) {
    $commentOnly = '// dx8wrapper.h' + "`n" + '/* IDirect3DDevice8 */'
    if ((Get-LegacyAdapterMatches $commentOnly).Count -ne 0) {
        throw 'comments must not trigger the native renderer boundary audit'
    }
    if ((Get-LegacyAdapterMatches '#include <d3d8.h>').Count -ne 1) {
        throw 'a legacy adapter include must trigger the native renderer boundary audit'
    }
    Write-Output 'Native renderer boundary audit self-test passed.'
    exit 0
}

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$relativePaths = @(
    'Core/Libraries/Include/Renderer/NativeW3DRenderer.h',
    'Core/Libraries/Source/Renderer/NativeW3DRenderer.cpp',
    'Core/Libraries/Include/Renderer/NativeW3DResources.h',
    'Core/Libraries/Source/Renderer/NativeW3DResources.cpp'
)
$violations = @()
foreach ($relativePath in $relativePaths) {
    $path = Join-Path $root ($relativePath -replace '/', '\\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $violations += "${relativePath}: missing native facade file"
        continue
    }
    $matches = @(Get-LegacyAdapterMatches ([IO.File]::ReadAllText($path)))
    foreach ($match in $matches) {
        $violations += "${relativePath}: legacy adapter token '$($match.Value)'"
    }
}
if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Output $_ }
    exit 1
}
Write-Output 'Native renderer boundary audit found no legacy adapter dependency.'
