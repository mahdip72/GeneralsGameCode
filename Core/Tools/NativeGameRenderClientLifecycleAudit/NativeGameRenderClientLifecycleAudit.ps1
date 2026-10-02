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

function Assert-InitialMSAAContract {
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
    $savedMSAA = $init.IndexOf('WW3D::Set_MSAA_Mode(')
    $rendererInitialization = $init.IndexOf('WW3D::Init( ApplicationHWnd )')
    $deviceSelection = $init.IndexOf('WW3D::Set_Render_Device(')
    $effectiveMSAA = $init.IndexOf('WW3D::Get_MSAA_Mode()', $deviceSelection)
    Assert-SourceContract ($savedMSAA -ge 0 -and
        $savedMSAA -lt $rendererInitialization -and
        $rendererInitialization -lt $deviceSelection -and
        $deviceSelection -lt $effectiveMSAA) `
        "W3DDisplay::init for $TitleRoot must apply saved MSAA before native renderer initialization, then publish the effective mode after device selection"
    Assert-SourceContract ($init.LastIndexOf('WW3D::Set_MSAA_Mode(') -eq $savedMSAA) `
        "W3DDisplay::init for $TitleRoot must apply saved MSAA exactly once before native renderer initialization"
}

function Assert-RendererCaptureContract {
    param(
        [string] $TitleRoot
    )

    $displayPath = Join-Path $SourceRoot "$TitleRoot/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    $displaySource = Get-Content -LiteralPath $displayPath -Raw
    Assert-SourceContract ($displaySource.Contains('ConsumeGameBackBufferCaptureSuccess()')) `
        "W3DDisplay for $TitleRoot must acknowledge a capture only from the native completion result"
    Assert-SourceContract (-not ($displaySource -match '(?s)rendererCaptureFrameGate.*?Get_Frame_Count\(\)')) `
        "W3DDisplay for $TitleRoot must not consume capture requests from WW3D FrameCount advancement"
    Assert-SourceContract ($displaySource -match '(?s)captureArmed.*?End_Render\(\).*?ConsumeGameBackBufferCaptureSuccess\(\)') `
        "W3DDisplay for $TitleRoot must evaluate native capture completion after End_Render"
}

function Assert-EndRenderFailureReportingContract {
    param(
        [string] $TitleRoot
    )

    $displayPath = Join-Path $SourceRoot "$TitleRoot/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    $displaySource = Get-Content -LiteralPath $displayPath -Raw
    $helperStart = $displaySource.IndexOf('static void reportEndRenderFailure(WW3DErrorType result)')
    $helperEnd = $displaySource.IndexOf('#ifdef SAMPLE_DYNAMIC_LIGHT', $helperStart)
    Assert-SourceContract ($helperStart -ge 0 -and $helperEnd -gt $helperStart) `
        "could not isolate End_Render failure reporting for $TitleRoot"

    $helper = $displaySource.Substring($helperStart, $helperEnd - $helperStart)
    Assert-SourceContract ($helper.Contains('static bool failureAlreadyReported = false;') -and
        $helper.Contains('if (result == WW3D_ERROR_OK)') -and
        $helper.Contains('failureAlreadyReported = false;')) `
        "End_Render failure reporting for $TitleRoot must reset its once-per-streak latch after a successful frame"
    Assert-SourceContract ($helper.Contains('if (failureAlreadyReported)') -and
        $helper.Contains('::OutputDebugString(message);')) `
        "End_Render failure reporting for $TitleRoot must suppress repeated errors and emit through the Release-safe debugger channel"

    $endRenderCount = [regex]::Matches($displaySource, 'WW3D::End_Render\(\)').Count
    $reportedResultCount = [regex]::Matches(
        $displaySource,
        'const WW3DErrorType endRenderResult = WW3D::End_Render\(\);\s*reportEndRenderFailure\(endRenderResult\);'
    ).Count
    Assert-SourceContract ($endRenderCount -eq 3 -and $reportedResultCount -eq $endRenderCount) `
        "every End_Render path for $TitleRoot must report its result"
}

function Assert-NativeCaptureAndTeardownContract {
    $nativePath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp'
    $rendererPath = Join-Path $SourceRoot 'Core/Libraries/Source/Renderer/NativeW3DRenderer.cpp'
    $nativeSource = Get-Content -LiteralPath $nativePath -Raw
    $rendererSource = Get-Content -LiteralPath $rendererPath -Raw
    Assert-SourceContract ($nativeSource.Contains('D3D11RendererCapture.tga')) `
        'native one-shot capture must retain the documented deterministic TGA path'
    Assert-SourceContract ($nativeSource.Contains('RenderCaptureRequestDescriptor')) `
        'native one-shot capture must enqueue a real descriptor and completion/cancellation callbacks'
    Assert-SourceContract ($nativeSource.Contains('WriteNativeGameCaptureTga')) `
        'native one-shot capture must use an owner-independent D3D11 pixel-to-TGA writer'
    Assert-SourceContract ($nativeSource.Contains('CompleteGameCaptureFile') -and
        $nativeSource.Contains('CancelGameCaptureFile')) `
        'native one-shot capture must distinguish successful completion from cancellation'
    Assert-SourceContract ($nativeSource.Contains('m_gameCaptureResult ==') -and
        $nativeSource.Contains('m_gameCaptureResult !=')) `
        'native capture acknowledgement must inspect both success and failure results'
    Assert-SourceContract ($nativeSource.Contains('const bool ownerThread = m_renderer.HasBackendState()') -and
        $nativeSource.Contains('m_resources.IsOwnerThread()')) `
        'native aggregate teardown must preserve owner affinity after renderer detachment'
    Assert-SourceContract ($nativeSource.Contains('ClearGameRendererStateForDestroyedOwner(this)')) `
        'off-owner native aggregate teardown must clear stale bootstrap publication metadata'
    Assert-SourceContract (-not $nativeSource.Contains('assert(shutdownResult ==')) `
        'native aggregate destruction must not terminate when owner-thread shutdown is rejected'
    Assert-SourceContract ($rendererSource.Contains('DeferShutdownOnOwner')) `
        'off-owner renderer destruction must defer owned backend shutdown to its producer'
    Assert-SourceContract ($rendererSource.Contains('EnqueueFallbackCleanup')) `
        'deferred renderer teardown must use the owner cleanup queue'
    Assert-SourceContract ($rendererSource.Contains('ThreadedRenderDevice::shutdown') -and
        $rendererSource.Contains('!IsOwnerThread()')) `
        'renderer teardown must retain an explicit owner-thread guard around threaded shutdown'
}

foreach ($titleRoot in @('Generals', 'GeneralsMD')) {
    Assert-InitialRender2DResolutionContract $titleRoot
    Assert-InitialMSAAContract $titleRoot
    Assert-RendererCaptureContract $titleRoot
    Assert-EndRenderFailureReportingContract $titleRoot
}

Assert-NativeCaptureAndTeardownContract

Write-Output 'Native GameRenderClient lifecycle, capture acknowledgement, End_Render failure reporting, teardown, startup MSAA, and 2D viewport source audit passed.'
