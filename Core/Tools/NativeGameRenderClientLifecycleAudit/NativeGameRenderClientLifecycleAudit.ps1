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

function Assert-InitialRender2DResolutionContract {
    param(
        [string] $TitleRoot
    )

    $displayPath = Join-Path $SourceRoot "$TitleRoot/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    if (-not (Test-Path -LiteralPath $displayPath -PathType Leaf)) {
        throw "W3D display source is missing: $displayPath"
    }

    $displaySource = Get-Content -LiteralPath $displayPath -Raw
    $initStart = $displaySource.IndexOf('void W3DDisplay::init()')
    $initEnd = $displaySource.IndexOf('void W3DDisplay::reset()', $initStart)
    Assert-SourceContract ($initStart -ge 0 -and $initEnd -gt $initStart) `
        "could not isolate W3DDisplay::init for $TitleRoot"

    $init = $displaySource.Substring($initStart, $initEnd - $initStart)
    $resolutionQuery = $init.IndexOf('WW3D::Get_Device_Resolution(actualWidth, actualHeight,')
    $screenResolutionSetter = -1
    $displayWidthUpdate = -1
    if ($resolutionQuery -ge 0) {
        $screenResolutionSetter = $init.IndexOf(
            'Render2DClass::Set_Screen_Resolution', $resolutionQuery)
        $displayWidthUpdate = $init.IndexOf('setWidth(actualWidth)', $resolutionQuery)
    }
    Assert-SourceContract ($resolutionQuery -ge 0) `
        "W3DDisplay::init for $TitleRoot must query the final device resolution"
    Assert-SourceContract ($screenResolutionSetter -gt $resolutionQuery -and
        $screenResolutionSetter -lt $displayWidthUpdate) `
        "W3DDisplay::init for $TitleRoot must publish the final device resolution to Render2D before display updates"

    $initialResolution = $init.Substring($resolutionQuery, $screenResolutionSetter - $resolutionQuery)
    $initialResolution += $init.Substring($screenResolutionSetter,
        [Math]::Min(256, $init.Length - $screenResolutionSetter))
    Assert-SourceContract ($initialResolution -match
        'Render2DClass::Set_Screen_Resolution\s*\(\s*RectClass\s*\(\s*0\s*,\s*0\s*,\s*actualWidth\s*,\s*actualHeight\s*\)\s*\)\s*;') `
        "W3DDisplay::init for $TitleRoot must publish actualWidth/actualHeight to Render2D"
}

foreach ($titleRoot in @('Generals', 'GeneralsMD')) {
    Assert-InitialRender2DResolutionContract $titleRoot
}

Write-Output 'Native GameRenderClient lifecycle and startup 2D viewport source audit passed.'
