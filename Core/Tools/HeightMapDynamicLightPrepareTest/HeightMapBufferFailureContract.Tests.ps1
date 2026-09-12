param([string]$SourceRoot = (Join-Path $PSScriptRoot '../../..'))
$ErrorActionPreference = 'Stop'

# Check the actual title consumer, including the serial fallback that runs
# when prepared publication cannot lock its vertex buffer.
$sourcePath = Join-Path $SourceRoot 'Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp'
$source = [IO.File]::ReadAllText($sourcePath)
$functions = @(
    'updateVBWithTerrainPreparation', 'updateVBSerial',
    'updateVBForLight', 'updateVBForLightOptimized',
    'updateVBForLightWithPreparation'
)

function Get-ConsumerBody([string]$text, [string]$name) {
    $start = [regex]::Match($text, 'Int HeightMapRenderObjClass::' + $name + '\s*\(')
    if (!$start.Success) { throw "Missing consumer: $name" }
    $next = [regex]::Match($text.Substring($start.Index + $start.Length),
        '\n(?:Int|Bool|void) HeightMapRenderObjClass::')
    if (!$next.Success) { throw "Missing consumer boundary: $name" }
    return $text.Substring($start.Index, $start.Length + $next.Index)
}

function Assert-FailureContract([string]$body, [string]$name) {
    # Require the failure exit immediately after the acquired pointer, before
    # offset arithmetic, backup mutation, row scatter, or lighting writes.
    $guard = [regex]::Match($body,
        'Get_Vertex_Array\(\);\s*if\s*\(!(?<pointer>\w+)\)\s*\{\s*m_needFullUpdate\s*=\s*true;\s*return\s+-1;\s*\}')
    if (!$guard.Success) { throw "$name loses the failed-lock retry contract" }
    if ($body -notmatch 'lockVtxBuffer\.Commit\(\)') {
        throw "$name does not observe upload failure"
    }
    if ($name -eq 'updateVBForLightWithPreparation') {
        if ($body -notmatch 'published\s*=\s*lockVtxBuffer\.Commit\(\);' -or
            $body -notmatch 'if\s*\(!published\)\s*\{\s*m_needFullUpdate\s*=\s*true;\s*return\s+-1;') {
            throw "$name hides failed publication behind serial fallback"
        }
    } elseif ($body -notmatch 'if\s*\((?:hardwareReady\s*&&\s*)?!lockVtxBuffer\.Commit\(\)\)\s*\{\s*m_needFullUpdate\s*=\s*true;\s*return\s+-1;') {
        throw "$name discards upload failure or its pending rebuild"
    }
}

$rejected = 0
foreach ($name in $functions) {
    $body = Get-ConsumerBody $source $name
    Assert-FailureContract $body $name
    # Every negative fixture is derived from the current production consumer.
    # The historical crash (no guard), lost retry, and hidden upload failure
    # must each be rejected, not just an unrelated synthetic source snippet.
    $mutations = @(
        [regex]::Replace($body, 'if\s*\(!\w+\)\s*\{\s*m_needFullUpdate\s*=\s*true;\s*return\s+-1;\s*\}', ''),
        $body.Replace('m_needFullUpdate = true;', 'm_needFullUpdate = false;'),
        $body.Replace('lockVtxBuffer.Commit()', 'true')
    )
    foreach ($mutation in $mutations) {
        $didReject = $false
        try { Assert-FailureContract $mutation $name } catch { $didReject = $true }
        if (!$didReject) { throw "Negative fixture was accepted for $name" }
        ++$rejected
    }
}
Write-Output "Height-map buffer failure contract passed: $($functions.Count) consumers, $rejected rejected regressions."
