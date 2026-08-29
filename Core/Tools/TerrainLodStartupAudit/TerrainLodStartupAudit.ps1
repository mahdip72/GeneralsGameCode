param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Text)
    $withoutBlocks = [regex]::Replace($Text, '/\*[\s\S]*?\*/', {
        param($Match)
        return ($Match.Value -replace '[^\r\n]', ' ')
    })
    return [regex]::Replace($withoutBlocks, '//[^\r\n]*', '')
}

function Get-BracedBody {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][int]$Start
    )
    $open = $Text.IndexOf('{', $Start)
    if ($open -lt 0) { return $null }
    $depth = 0
    for ($index = $open; $index -lt $Text.Length; ++$index) {
        if ($Text[$index] -eq '{') { ++$depth }
        elseif ($Text[$index] -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $Text.Substring($open + 1, $index - $open - 1)
            }
        }
    }
    return $null
}

function Test-TerrainLodStartupContract {
    param([Parameter(Mandatory = $true)][string]$ImplementationText)

    $text = Remove-CppComments $ImplementationText
    $signature = $text.IndexOf('void W3DDisplay::calculateTerrainLOD()',
        [StringComparison]::Ordinal)
    if ($signature -lt 0) { return $false }
    $body = Get-BracedBody $text $signature
    if ($null -eq $body) { return $false }

    $predicate = $body.IndexOf('if (DX8Wrapper::Is_D3D11_Backend_Active())',
        [StringComparison]::Ordinal)
    $terrainOnly = $body.IndexOf('m_3DScene->drawTerrainOnly(true);',
        [StringComparison]::Ordinal)
    if ($predicate -lt 0 -or $terrainOnly -lt 0 -or $predicate -gt $terrainOnly) {
        return $false
    }
    $d3d11Branch = Get-BracedBody $body $predicate
    if ($null -eq $d3d11Branch) { return $false }
    $maximum = $d3d11Branch.IndexOf(
        'TheWritableGlobalData->m_terrainLOD = TERRAIN_LOD_MAX;',
        [StringComparison]::Ordinal)
    $restore = $d3d11Branch.IndexOf('m_3DScene->drawTerrainOnly(false);',
        [StringComparison]::Ordinal)
    $apply = $d3d11Branch.IndexOf('TheTerrainRenderObject->adjustTerrainLOD(0);',
        [StringComparison]::Ordinal)
    $return = $d3d11Branch.IndexOf('return;', [StringComparison]::Ordinal)
    if ($maximum -lt 0 -or $restore -lt 0 -or $apply -lt 0 -or $return -lt 0 -or
        $maximum -gt $restore -or $restore -gt $apply -or $apply -gt $return) {
        return $false
    }

    $sampleCount = $body.IndexOf('const Int NUM_SAMPLES=20;',
        [StringComparison]::Ordinal)
    $sampleLoop = $body.IndexOf('for (i=0; i<NUM_SAMPLES; i++)',
        [StringComparison]::Ordinal)
    $finalRestore = $body.LastIndexOf('m_3DScene->drawTerrainOnly(false);',
        [StringComparison]::Ordinal)
    return $sampleCount -gt $predicate -and $sampleLoop -gt $terrainOnly -and
        $finalRestore -gt $sampleLoop
}

if ($SelfTest) {
    $valid = @'
void W3DDisplay::calculateTerrainLOD()
{
    if (DX8Wrapper::Is_D3D11_Backend_Active())
    {
        TheWritableGlobalData->m_terrainLOD = TERRAIN_LOD_MAX;
        m_3DScene->drawTerrainOnly(false);
        TheTerrainRenderObject->adjustTerrainLOD(0);
        return;
    }
    const Int NUM_SAMPLES=20;
    m_3DScene->drawTerrainOnly(true);
    for (i=0; i<NUM_SAMPLES; i++) {}
    m_3DScene->drawTerrainOnly(false);
}
'@
    $valid = $valid -replace "`r`n", "`n"
    if (-not (Test-TerrainLodStartupContract $valid)) {
        throw 'Valid D3D11 terrain LOD fixture rejected.'
    }
    $mutations = @(
        $valid.Replace('DX8Wrapper::Is_D3D11_Backend_Active()', 'false'),
        $valid.Replace('TERRAIN_LOD_MAX', 'TERRAIN_LOD_NO_WATER'),
        $valid.Replace("        return;`n", ''),
        $valid.Replace("        m_3DScene->drawTerrainOnly(false);`n", ''),
        $valid.Replace("    m_3DScene->drawTerrainOnly(false);`n}", '}')
    )
    foreach ($mutation in $mutations) {
        if ($mutation -eq $valid -or (Test-TerrainLodStartupContract $mutation)) {
            throw 'Invalid D3D11 terrain LOD fixture accepted.'
        }
    }
    Write-Output 'Terrain LOD startup audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is specified.'
}

$paths = @(
    'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp',
    'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp'
)
foreach ($relativePath in $paths) {
    $implementation = [IO.File]::ReadAllText((Join-Path $SourceRoot $relativePath))
    if (-not (Test-TerrainLodStartupContract $implementation)) {
        throw "D3D11 terrain LOD startup contract failed: $relativePath"
    }
}

Write-Output 'Terrain LOD startup audit passed.'
