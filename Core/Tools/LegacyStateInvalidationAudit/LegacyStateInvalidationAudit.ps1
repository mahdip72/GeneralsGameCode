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

function Get-FunctionBody {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Signature
    )

    $start = $Text.IndexOf($Signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { return $null }
    $open = $Text.IndexOf('{', $start)
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

function Get-BraceDepthAt {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][int]$Position
    )

    $depth = 0
    for ($index = 0; $index -lt $Position; ++$index) {
        if ($Text[$index] -eq '{') { ++$depth }
        elseif ($Text[$index] -eq '}') { --$depth }
    }
    return $depth
}

function Test-LegacyStateInvalidationContract {
    param([Parameter(Mandatory = $true)][string]$ImplementationText)

    $text = Remove-CppComments $ImplementationText
    $invalidate = Get-FunctionBody $text 'void DX8Wrapper::Invalidate_Cached_Render_States()'
    $initialize = Get-FunctionBody $text 'bool DX8Wrapper::Init(void * hwnd, bool lite)'
    $create = Get-FunctionBody $text 'bool DX8Wrapper::Create_Device()'
    $reset = Get-FunctionBody $text 'bool DX8Wrapper::Reset_Device(bool reload_assets, bool *reset_requires_reacquire)'
    $setDx8Light = Get-FunctionBody $text 'void DX8Wrapper::Set_DX8_Light(int index, D3DLIGHT8* light)'
    if ($null -eq $invalidate -or $null -eq $initialize -or $null -eq $create -or
        $null -eq $reset -or $null -eq $setDx8Light) {
        return $false
    }

    $nativeCreate = $create.IndexOf('&D3DDevice', [StringComparison]::Ordinal)
    $createReset = $create.IndexOf('ResetTrackedLegacyState();', [StringComparison]::Ordinal)
    $createSeed = $create.IndexOf('SeedTrackedLegacyPipelineState();', [StringComparison]::Ordinal)
    $createInvalidate = $create.IndexOf('Invalidate_Cached_Render_States();', [StringComparison]::Ordinal)
    $dependentInits = $create.IndexOf('Do_Onetime_Device_Dependent_Inits();', [StringComparison]::Ordinal)
    if ($nativeCreate -lt 0 -or $createReset -lt 0 -or $createSeed -lt 0 -or
        $createInvalidate -lt 0 -or $dependentInits -lt 0 -or
        $nativeCreate -gt $createReset -or $createReset -gt $createSeed -or
        $createSeed -gt $createInvalidate -or $createInvalidate -gt $dependentInits -or
        (Get-BraceDepthAt $create $createReset) -ne 0 -or
        (Get-BraceDepthAt $create $createSeed) -ne 0 -or
        (Get-BraceDepthAt $create $createInvalidate) -ne 0 -or
        (Get-BraceDepthAt $create $dependentInits) -ne 0) {
        return $false
    }

    if ($invalidate -match '\b(?:ResetTrackedLegacyState|SeedTrackedLegacyPipelineState)\s*\(') {
        return $false
    }
    $textureLoop = [regex]::Match($invalidate,
        'for\s*\(\s*a\s*=\s*0\s*;\s*a\s*<\s*MAX_TEXTURE_STAGES\s*;\s*\+\+a\s*\)')
    $texturePresence = [regex]::Match($invalidate,
        '\bTrackLegacyTexturePresence\s*\(\s*a\s*,\s*false\s*\)')
    if (-not $textureLoop.Success -or -not $texturePresence.Success -or
        $textureLoop.Index -gt $texturePresence.Index -or
        (Get-BraceDepthAt $invalidate $texturePresence.Index) -ne 1) {
        return $false
    }
    $lightPublish = [regex]::Match($setDx8Light,
        '\bTrackLegacyLight\s*\(\s*index\s*,\s*neutralLight\s*\)')
    $lastLightMutation = $setDx8Light.LastIndexOf('LightEnable(',
        [StringComparison]::Ordinal)
    if (-not $lightPublish.Success -or $lastLightMutation -lt 0 -or
        $lastLightMutation -gt $lightPublish.Index -or
        (Get-BraceDepthAt $setDx8Light $lightPublish.Index) -ne 0) {
        return $false
    }

    $initReset = $initialize.IndexOf('ResetTrackedLegacyState();', [StringComparison]::Ordinal)
    $initSeed = $initialize.IndexOf('SeedTrackedLegacyPipelineState();', [StringComparison]::Ordinal)
    $initInvalidate = $initialize.IndexOf('Invalidate_Cached_Render_States();', [StringComparison]::Ordinal)
    if ($initReset -lt 0 -or $initSeed -lt 0 -or $initInvalidate -lt 0 -or
        $initReset -gt $initSeed -or $initSeed -gt $initInvalidate -or
        (Get-BraceDepthAt $initialize $initReset) -ne 0 -or
        (Get-BraceDepthAt $initialize $initSeed) -ne 0 -or
        (Get-BraceDepthAt $initialize $initInvalidate) -ne 0) {
        return $false
    }

    $deviceReset = $reset.IndexOf('Reset(&_PresentParameters)', [StringComparison]::Ordinal)
    $bridgePrepare = $reset.IndexOf(
        '_D3D11Bridge.Prepare_Legacy_Device_Reset()',
        [StringComparison]::Ordinal)
    $bridgeFailureReturn = if ($bridgePrepare -ge 0) {
        $reset.IndexOf('return false;', $bridgePrepare,
            [StringComparison]::Ordinal)
    } else { -1 }
    $resourceTeardown = $reset.IndexOf('WW3D::_Invalidate_Textures();',
        [StringComparison]::Ordinal)
    $resetFailureCheck = $reset.IndexOf('if (hr != D3D_OK)', $deviceReset, [StringComparison]::Ordinal)
    $resetFailureReturn = $reset.IndexOf('return false;', $resetFailureCheck, [StringComparison]::Ordinal)
    $trackedReset = $reset.IndexOf('ResetTrackedLegacyState();', [StringComparison]::Ordinal)
    $trackedSeed = $reset.IndexOf('SeedTrackedLegacyPipelineState();', [StringComparison]::Ordinal)
    $cacheInvalidate = $reset.IndexOf('Invalidate_Cached_Render_States();', [StringComparison]::Ordinal)
    return $bridgePrepare -ge 0 -and $bridgeFailureReturn -ge 0 -and
        $resourceTeardown -ge 0 -and $deviceReset -ge 0 -and
        $bridgePrepare -lt $bridgeFailureReturn -and
        $bridgeFailureReturn -lt $resourceTeardown -and
        $resourceTeardown -lt $deviceReset -and $resetFailureCheck -ge 0 -and
        $resetFailureReturn -ge 0 -and $trackedReset -ge 0 -and
        $trackedSeed -ge 0 -and $cacheInvalidate -ge 0 -and
        $deviceReset -lt $resetFailureCheck -and
        $resetFailureCheck -lt $resetFailureReturn -and
        $resetFailureReturn -lt $trackedReset -and
        $trackedReset -lt $trackedSeed -and $trackedSeed -lt $cacheInvalidate -and
        (Get-BraceDepthAt $reset $bridgePrepare) -eq
            (Get-BraceDepthAt $reset $deviceReset) -and
        (Get-BraceDepthAt $reset $deviceReset) -eq
            (Get-BraceDepthAt $reset $trackedReset) -and
        (Get-BraceDepthAt $reset $trackedReset) -eq
            (Get-BraceDepthAt $reset $trackedSeed)
}

function Test-LegacyBridgeResetContract {
    param([Parameter(Mandatory = $true)][string]$BridgeImplementationText)

    $text = Remove-CppComments $BridgeImplementationText
    $prepare = Get-FunctionBody $text 'bool D3D11LegacyBridge::Prepare_Legacy_Device_Reset()'
    if ($null -eq $prepare) { return $false }

    $endFrame = $prepare.IndexOf('End_Frame(false', [StringComparison]::Ordinal)
    $advanceGeneration = $prepare.IndexOf('capture_queue.advanceGeneration();',
        [StringComparison]::Ordinal)
    $cancelStale = $prepare.IndexOf('capture_queue.cancelStale(',
        [StringComparison]::Ordinal)
    $clearPending = $prepare.IndexOf('pending_clear = false;',
        [StringComparison]::Ordinal)
    $releaseCaches = $prepare.IndexOf('Release_Caches();',
        [StringComparison]::Ordinal)
    return $endFrame -ge 0 -and $advanceGeneration -ge 0 -and
        $cancelStale -ge 0 -and $clearPending -ge 0 -and
        $releaseCaches -ge 0 -and $endFrame -lt $advanceGeneration -and
        $advanceGeneration -lt $cancelStale -and $cancelStale -lt $clearPending -and
        $clearPending -lt $releaseCaches -and
        (Get-BraceDepthAt $prepare $releaseCaches) -eq 0
}

function Test-PointParticleVertexContract {
    param([Parameter(Mandatory = $true)][string]$PointGroupImplementationText)

    $text = Remove-CppComments $PointGroupImplementationText
    $render = Get-FunctionBody $text 'void PointGroupClass::Render(RenderInfoClass &rinfo)'
    $renderVolume = Get-FunctionBody $text 'void PointGroupClass::RenderVolumeParticle(RenderInfoClass &rinfo, unsigned int depth )'
    if ($null -eq $render -or $null -eq $renderVolume) { return $false }

    foreach ($body in @($render, $renderVolume)) {
        $location = $body.IndexOf('Get_Location_Offset()', [StringComparison]::Ordinal)
        $normal = $body.IndexOf('Get_Normal_Offset()', [StringComparison]::Ordinal)
        $texture0 = $body.IndexOf('Get_Tex_Offset(0)', [StringComparison]::Ordinal)
        $texture1 = $body.IndexOf('Get_Tex_Offset(1)', [StringComparison]::Ordinal)
        if ($location -lt 0 -or $normal -lt 0 -or $texture0 -lt 0 -or
            $texture1 -lt 0 -or $location -gt $normal -or
            $normal -gt $texture0 -or $texture0 -gt $texture1 -or
            (Get-BraceDepthAt $body $location) -ne
                (Get-BraceDepthAt $body $normal) -or
            (Get-BraceDepthAt $body $normal) -ne
                (Get-BraceDepthAt $body $texture0) -or
            (Get-BraceDepthAt $body $texture0) -ne
                (Get-BraceDepthAt $body $texture1)) {
            return $false
        }
    }
    return $true
}

if ($SelfTest) {
    $valid = @'
bool DX8Wrapper::Init(void * hwnd, bool lite)
{
    ResetTrackedLegacyState();
    SeedTrackedLegacyPipelineState();
    Invalidate_Cached_Render_States();
}
void DX8Wrapper::Invalidate_Cached_Render_States()
{
    ClearLegacyWrapperCaches();
    for (a=0; a<MAX_TEXTURE_STAGES; ++a) {
        TrackLegacyTexturePresence(a, false);
    }
}
void DX8Wrapper::Set_DX8_Light(int index, D3DLIGHT8* light)
{
    LegacyLightState neutralLight;
    LightEnable(index, TRUE);
    TrackLegacyLight(index, neutralLight);
}
bool DX8Wrapper::Create_Device()
{
    CreateDevice(&D3DDevice);
    ResetTrackedLegacyState();
    SeedTrackedLegacyPipelineState();
    Invalidate_Cached_Render_States();
    Do_Onetime_Device_Dependent_Inits();
}
bool DX8Wrapper::Reset_Device(bool reload_assets, bool *reset_requires_reacquire)
{
    if (_UseD3D11Backend && !_D3D11Bridge.Prepare_Legacy_Device_Reset()) {
        return false;
    }
    WW3D::_Invalidate_Textures();
    Reset(&_PresentParameters);
    if (hr != D3D_OK)
        return false;
    ResetTrackedLegacyState();
    SeedTrackedLegacyPipelineState();
    Invalidate_Cached_Render_States();
}
'@
    $validBridge = @'
bool D3D11LegacyBridge::Prepare_Legacy_Device_Reset()
{
    if (frame_open) {
        End_Frame(false);
    }
    capture_queue.advanceGeneration();
    capture_queue.cancelStale(RENDER_RESULT_FAILED);
    pending_clear = false;
    Release_Caches();
    return true;
}
'@
    $validPointGroup = @'
void PointGroupClass::Render(RenderInfoClass &rinfo)
{
    for (;;) {
        Write(Get_Location_Offset());
        Write(Get_Normal_Offset());
        Write(Get_Tex_Offset(0));
        Write(Get_Tex_Offset(1));
    }
}
void PointGroupClass::RenderVolumeParticle(RenderInfoClass &rinfo, unsigned int depth )
{
    for (;;) {
        Write(Get_Location_Offset());
        Write(Get_Normal_Offset());
        Write(Get_Tex_Offset(0));
        Write(Get_Tex_Offset(1));
    }
}
'@
    $valid = $valid -replace "`r`n", "`n"
    $validBridge = $validBridge -replace "`r`n", "`n"
    $validPointGroup = $validPointGroup -replace "`r`n", "`n"
    if (-not (Test-LegacyStateInvalidationContract $valid) -or
        -not (Test-LegacyBridgeResetContract $validBridge) -or
        -not (Test-PointParticleVertexContract $validPointGroup)) {
        throw 'Valid cache-invalidation fixture rejected.'
    }
    $ordinaryReset = $valid.Replace(
        '    ClearLegacyWrapperCaches();',
        "    ClearLegacyWrapperCaches();`n    ResetTrackedLegacyState();")
    if (Test-LegacyStateInvalidationContract $ordinaryReset) {
        throw 'Ordinary cache invalidation was allowed to reset tracked device state.'
    }
    $missingTextureClear = $valid.Replace(
        "        TrackLegacyTexturePresence(a, false);`n", '')
    if ($missingTextureClear -eq $valid -or
        (Test-LegacyStateInvalidationContract $missingTextureClear)) {
        throw 'Ordinary invalidation without texture-presence clearing was accepted.'
    }
    $missingLightPublish = $valid.Replace(
        "    TrackLegacyLight(index, neutralLight);`n", '')
    if ($missingLightPublish -eq $valid -or
        (Test-LegacyStateInvalidationContract $missingLightPublish)) {
        throw 'Low-level light restore without D3D11 state publication was accepted.'
    }
    $missingInitReset = $valid.Replace(
        "bool DX8Wrapper::Init(void * hwnd, bool lite)`n{`n    ResetTrackedLegacyState();`n    SeedTrackedLegacyPipelineState();`n    Invalidate_Cached_Render_States();`n}",
        "bool DX8Wrapper::Init(void * hwnd, bool lite)`n{`n    Invalidate_Cached_Render_States();`n}")
    if ($missingInitReset -eq $valid) {
        throw 'Initialization mutation did not change the fixture.'
    }
    if (Test-LegacyStateInvalidationContract $missingInitReset) {
        throw 'Initialization without an explicit tracked-state reset was accepted.'
    }
    $missingCreateReset = $valid.Replace(
        "    CreateDevice(&D3DDevice);`n    ResetTrackedLegacyState();`n    SeedTrackedLegacyPipelineState();`n    Invalidate_Cached_Render_States();`n    Do_Onetime_Device_Dependent_Inits();",
        "    CreateDevice(&D3DDevice);`n    Invalidate_Cached_Render_States();`n    Do_Onetime_Device_Dependent_Inits();")
    if ($missingCreateReset -eq $valid) {
        throw 'Device-creation mutation did not change the fixture.'
    }
    if (Test-LegacyStateInvalidationContract $missingCreateReset) {
        throw 'Device creation without an explicit tracked-state reset was accepted.'
    }
    $missingCreateInvalidate = $valid.Replace(
        "    Invalidate_Cached_Render_States();`n    Do_Onetime_Device_Dependent_Inits();",
        '    Do_Onetime_Device_Dependent_Inits();')
    if ($missingCreateInvalidate -eq $valid -or
        (Test-LegacyStateInvalidationContract $missingCreateInvalidate)) {
        throw 'Device creation without wrapper-cache invalidation was accepted.'
    }
    $missingBridgePrepare = $valid.Replace(
        "    if (_UseD3D11Backend && !_D3D11Bridge.Prepare_Legacy_Device_Reset()) {`n        return false;`n    }`n", '')
    if ($missingBridgePrepare -eq $valid -or
        (Test-LegacyStateInvalidationContract $missingBridgePrepare)) {
        throw 'Native reset without bridge cache release was accepted.'
    }
    $lateDeviceReset = $valid.Replace(
        '    Reset(&_PresentParameters);',
        "    ResetTrackedLegacyState();`n    Reset(&_PresentParameters);")
    if ($lateDeviceReset -eq $valid) {
        throw 'Device-reset ordering mutation did not change the fixture.'
    }
    if (Test-LegacyStateInvalidationContract $lateDeviceReset) {
        throw 'Tracked state reset before the native device reset was accepted.'
    }
    $unreachableCreateReset = $valid.Replace(
        "    ResetTrackedLegacyState();`n    SeedTrackedLegacyPipelineState();`n    Invalidate_Cached_Render_States();`n    Do_Onetime_Device_Dependent_Inits();",
        "    if (false) {`n        ResetTrackedLegacyState();`n        SeedTrackedLegacyPipelineState();`n    }`n    Invalidate_Cached_Render_States();`n    Do_Onetime_Device_Dependent_Inits();")
    if ($unreachableCreateReset -eq $valid -or
        (Test-LegacyStateInvalidationContract $unreachableCreateReset)) {
        throw 'Unreachable device-creation reset was accepted.'
    }
    $unreachableDeviceReset = $valid.Replace(
        "    if (hr != D3D_OK)`n        return false;`n    ResetTrackedLegacyState();`n    SeedTrackedLegacyPipelineState();`n    Invalidate_Cached_Render_States();",
        "    if (hr != D3D_OK)`n        return false;`n    if (false) {`n        ResetTrackedLegacyState();`n        SeedTrackedLegacyPipelineState();`n    }`n    Invalidate_Cached_Render_States();")
    if ($unreachableDeviceReset -eq $valid -or
        (Test-LegacyStateInvalidationContract $unreachableDeviceReset)) {
        throw 'Unreachable device-reset tracking reset was accepted.'
    }
    $unreachableBridgePrepare = $valid.Replace(
        "    if (_UseD3D11Backend && !_D3D11Bridge.Prepare_Legacy_Device_Reset()) {`n        return false;`n    }`n    WW3D::_Invalidate_Textures();",
        "    if (false) {`n        _D3D11Bridge.Prepare_Legacy_Device_Reset();`n    }`n    WW3D::_Invalidate_Textures();")
    if ($unreachableBridgePrepare -eq $valid -or
        (Test-LegacyStateInvalidationContract $unreachableBridgePrepare)) {
        throw 'Unreachable bridge reset preparation was accepted.'
    }
    $unreachableTextureClear = $valid.Replace(
        "    for (a=0; a<MAX_TEXTURE_STAGES; ++a) {`n        TrackLegacyTexturePresence(a, false);`n    }",
        "    TrackLegacyTexturePresence(a, false);`n    for (a=0; a<MAX_TEXTURE_STAGES; ++a) {`n    }")
    if ($unreachableTextureClear -eq $valid -or
        (Test-LegacyStateInvalidationContract $unreachableTextureClear)) {
        throw 'Texture-presence clear outside the stage loop was accepted.'
    }
    $earlyLightPublish = $valid.Replace(
        "    LightEnable(index, TRUE);`n    TrackLegacyLight(index, neutralLight);",
        "    TrackLegacyLight(index, neutralLight);`n    LightEnable(index, TRUE);")
    if ($earlyLightPublish -eq $valid -or
        (Test-LegacyStateInvalidationContract $earlyLightPublish)) {
        throw 'Light publication before the legacy mutation was accepted.'
    }
    $unreachableBridgeRelease = $validBridge.Replace(
        '    Release_Caches();',
        "    if (false) {`n        Release_Caches();`n    }")
    if ($unreachableBridgeRelease -eq $validBridge -or
        (Test-LegacyBridgeResetContract $unreachableBridgeRelease)) {
        throw 'Unreachable bridge cache release was accepted.'
    }
    $firstNormal = $validPointGroup.IndexOf('        Write(Get_Normal_Offset());',
        [StringComparison]::Ordinal)
    $missingRenderNormal = $validPointGroup.Remove($firstNormal,
        '        Write(Get_Normal_Offset());'.Length)
    if ($firstNormal -lt 0 -or
        (Test-PointParticleVertexContract $missingRenderNormal)) {
        throw 'Point Render without a normal initialization was accepted.'
    }
    $lastTexture1 = $validPointGroup.LastIndexOf('        Write(Get_Tex_Offset(1));',
        [StringComparison]::Ordinal)
    $missingVolumeTexture1 = $validPointGroup.Remove($lastTexture1,
        '        Write(Get_Tex_Offset(1));'.Length)
    if ($lastTexture1 -lt 0 -or
        (Test-PointParticleVertexContract $missingVolumeTexture1)) {
        throw 'Volume particle Render without a second UV initialization was accepted.'
    }
    Write-Output 'Legacy state invalidation audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is specified.'
}

$implementationPath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp'
$bridgeImplementationPath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp'
$pointGroupImplementationPath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/pointgr.cpp'
$implementation = [IO.File]::ReadAllText($implementationPath)
$bridgeImplementation = [IO.File]::ReadAllText($bridgeImplementationPath)
$pointGroupImplementation = [IO.File]::ReadAllText($pointGroupImplementationPath)
if (-not (Test-LegacyStateInvalidationContract $implementation) -or
    -not (Test-LegacyBridgeResetContract $bridgeImplementation) -or
    -not (Test-PointParticleVertexContract $pointGroupImplementation)) {
    throw 'Legacy state cache invalidation does not preserve tracked D3D11 device state.'
}

Write-Output 'Legacy state invalidation audit passed.'
