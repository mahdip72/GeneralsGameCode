param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot
)

$ErrorActionPreference = 'Stop'

function Assert-SourceContract {
    param(
        [bool] $Condition,
        [string] $Message
    )

    if (-not $Condition) {
        throw "native GameRenderClient lifecycle source contract failed: $Message"
    }
}

$sourcePath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/nativew3dgameclient.cpp'
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "native GameRenderClient lifecycle source is missing: $sourcePath"
}

$source = Get-Content -LiteralPath $sourcePath -Raw
$initializeStart = $source.IndexOf('RenderResult InitializeGameRenderer(')
$shutdownStart = $source.IndexOf('RenderResult ShutdownGameRenderer()', $initializeStart)
Assert-SourceContract ($initializeStart -ge 0 -and $shutdownStart -gt $initializeStart) `
    'could not isolate InitializeGameRenderer'

$initialize = $source.Substring($initializeStart, $shutdownStart - $initializeStart)
$transitionGuard = $initialize.IndexOf('if (g_renderer_state.transitionInProgress)')
$aggregateBranch = $initialize.IndexOf('if (g_renderer_state.aggregate != 0)')
Assert-SourceContract ($transitionGuard -ge 0 -and $aggregateBranch -gt $transitionGuard) `
    'transition guard must precede the aggregate idempotence branch'

$tryBlock = $initialize.IndexOf('try', $aggregateBranch)
Assert-SourceContract ($tryBlock -gt $aggregateBranch) `
    'aggregate idempotence branch is incomplete'
$idempotenceBranch = $initialize.Substring($aggregateBranch, $tryBlock - $aggregateBranch)

foreach ($predicate in @(
    '!IsRendererOwnerThreadLocked()',
    '!g_renderer_state.aggregate->IsInitialized()',
    '!g_renderer_state.aggregate->IsOperational()',
    'GetGameRenderClientNativeOwner() != g_renderer_state.aggregate'
)) {
    Assert-SourceContract ($idempotenceBranch.Contains($predicate)) `
        "same-parameter initialization is missing the $predicate guard"
}

Assert-SourceContract ($idempotenceBranch.Contains('return same ? RENDER_RESULT_OK :')) `
    'same-parameter initialization must retain explicit idempotence'

Write-Output 'Native GameRenderClient lifecycle source audit passed.'
