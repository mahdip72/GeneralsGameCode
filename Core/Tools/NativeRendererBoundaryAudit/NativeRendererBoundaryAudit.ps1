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

function Get-RawFacadeLifetimeMatches {
    param([Parameter(Mandatory = $true)][string]$RelativePath,
          [Parameter(Mandatory = $true)][string]$Text)

    $withoutComments = Remove-CppComments $Text
    if ($RelativePath -like '*NativeW3DResources*') {
        return @([regex]::Matches($withoutComments, '\bNativeW3DRenderer\s*\*\s*(?:renderer|m_renderer)\s*;'))
    }
    if ($RelativePath -like '*NativeW3DRenderer*') {
        return @([regex]::Matches($withoutComments, '\bm_resources\b'))
    }
    return @()
}

if ($SelfTest) {
    $commentOnly = '// dx8wrapper.h' + "`n" + '/* IDirect3DDevice8 */'
    if ((Get-LegacyAdapterMatches $commentOnly).Count -ne 0) {
        throw 'comments must not trigger the native renderer boundary audit'
    }
    if ((Get-LegacyAdapterMatches '#include <d3d8.h>').Count -ne 1) {
        throw 'a legacy adapter include must trigger the native renderer boundary audit'
    }

    if ((Get-RawFacadeLifetimeMatches 'Core/Libraries/Source/Renderer/NativeW3DResources.cpp' 'NativeW3DRenderer *renderer;').Count -ne 1) {
        throw 'a raw resource-to-renderer lifetime link must trigger the native renderer boundary audit'
    }
    if ((Get-RawFacadeLifetimeMatches 'Core/Libraries/Include/Renderer/NativeW3DRenderer.h' 'NativeW3DResources *m_resources;').Count -ne 1) {
        throw 'a raw renderer-to-resource lifetime link must trigger the native renderer boundary audit'
    }
    Write-Output 'Native renderer boundary audit self-test passed.'
    exit 0
}

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$relativePaths = @(
    'Core/Libraries/Include/Renderer/NativeW3DRenderer.h',
    'Core/Libraries/Source/Renderer/NativeW3DRenderer.cpp',
    'Core/Libraries/Include/Renderer/NativeW3DResources.h',
    'Core/Libraries/Source/Renderer/NativeW3DResources.cpp',
    'Core/Libraries/Include/Renderer/NativeW3DRenderState.h',
    'Core/Libraries/Source/Renderer/NativeW3DRenderState.cpp'
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
    $lifetimeMatches = @(Get-RawFacadeLifetimeMatches $relativePath ([IO.File]::ReadAllText($path)))
    foreach ($match in $lifetimeMatches) {
        $violations += "${relativePath}: raw facade lifetime token '$($match.Value)'"
    }
}
if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Output $_ }
    exit 1
}
Write-Output 'Native renderer boundary audit found no legacy adapter dependency.'
