param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [switch]$SelfTest,
    [switch]$RequireNativeD3D8Free,
    [string]$NativeClosurePath
)

$ErrorActionPreference = 'Stop'

function Test-DependencyToken([string]$Content, [string]$Dependency)
{
    return $Content -match ("\b" + [regex]::Escape($Dependency) + "\b")
}

function Remove-CMakeComments([string]$Content)
{
    $normalized = $Content.Replace("`r`n", "`n").Replace("`r", "`n")
    return [regex]::Replace($normalized, '(?m)#.*$', '')
}

function Test-ExactCMakeLine([string]$Content, [string]$Line)
{
    return $Content -match ('(?m)^' + [regex]::Escape($Line) + '\r?$')
}

function Test-CMakeTargetLinkDependency(
    [string]$Content,
    [string]$Target,
    [string]$Dependency,
    [string]$Visibility = '')
{
    $withoutComments = Remove-CMakeComments $Content
    $commands = [regex]::Matches($withoutComments,
        '(?is)(?<![A-Za-z0-9_])target_link_libraries\s*\((?<Arguments>.*?)\)')
    foreach ($command in $commands) {
        $arguments = $command.Groups['Arguments'].Value.Trim()
        $targetPattern = '^' + [regex]::Escape($Target) + '(?:\s|$)'
        if ($Target -ne '*' -and $arguments -notmatch $targetPattern) {
            continue
        }
        if (-not [string]::IsNullOrWhiteSpace($Visibility)) {
            $visibilityPattern = '(?is)^' + [regex]::Escape($Target) +
                '\s+' + [regex]::Escape($Visibility) + '(?:\s|$)'
            if ($arguments -notmatch $visibilityPattern) {
                continue
            }
        }
        if (Test-DependencyToken $arguments $Dependency) {
            return $true
        }
    }
    return $false
}

$nativeCutoverForbiddenDependencies = @(
    'rts_d3d8_headers',
    'rts_native_d3d8_compat_boundary',
    'd3d8to9',
    'rts_d3d8lib',
    'd3d8',
    'd3dx8',
    'milesstub',
    'binkstub'
)

function Assert-NativeD3D8FreeClosure([string]$Content)
{
    foreach ($dependency in $nativeCutoverForbiddenDependencies) {
        if (Test-DependencyToken $Content $dependency) {
            throw "Native cutover closure retains forbidden dependency '$dependency'."
        }
    }
}

function Assert-ProductRuntimeSelector([string]$Content)
{
    $withoutComments = Remove-CMakeComments $Content
    $selection = [regex]::Match($withoutComments, @'
(?is)if\s*\(\s*WIN32\s+AND\s+CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s*\)
(?<Native>.*?)
elseif\s*\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+4\s*\)
(?<Legacy>.*?)
elseif\s*\(
'@)
    if (-not $selection.Success) {
        throw 'Product runtime selector does not expose distinct native x64 and 32-bit branches.'
    }

    $nativeBranch = $selection.Groups['Native'].Value
    $legacyBranch = $selection.Groups['Legacy'].Value
    if ($nativeBranch -notmatch '(?i)include\s*\([^\)]*native-product-runtime\.cmake' -or
        $nativeBranch -notmatch '(?is)target_link_libraries\s*\(\s*rts_product_runtime\s+INTERFACE\s+rts_native_product_runtime\s*\)' -or
        (Test-DependencyToken $nativeBranch 'rts_legacy_product_runtime')) {
        throw 'Native x64 product selection does not resolve exclusively through rts_native_product_runtime.'
    }
    if ($legacyBranch -notmatch '(?i)include\s*\([^\)]*legacy-product-runtime\.cmake' -or
        $legacyBranch -notmatch '(?is)target_link_libraries\s*\(\s*rts_product_runtime\s+INTERFACE\s+rts_legacy_product_runtime\s*\)' -or
        (Test-DependencyToken $legacyBranch 'rts_native_product_runtime')) {
        throw 'The 32-bit product selection does not resolve exclusively through rts_legacy_product_runtime.'
    }

    $outsideSelection = $withoutComments.Remove($selection.Index,
        $selection.Length)
    foreach ($architectureSpecificToken in @(
        'native-product-runtime.cmake',
        'legacy-product-runtime.cmake',
        'rts_native_product_runtime',
        'rts_legacy_product_runtime'
    )) {
        if (Test-DependencyToken $outsideSelection $architectureSpecificToken) {
            throw "Architecture-specific product runtime token '$architectureSpecificToken' appears outside the selected architecture branches."
        }
    }
}

function Get-NativeVisibleSource([string]$Content)
{
    $lines = @($Content -split "`r?`n")
    $conditions = New-Object 'System.Collections.Generic.List[object]'
    $output = New-Object 'System.Collections.Generic.List[string]'
    $active = $true

    foreach ($line in $lines) {
        if ($line -match '^\s*#\s*(?:if\s+!defined\s*\(\s*_WIN64\s*\)|ifndef\s+_WIN64)') {
            $conditions.Add([pscustomobject]@{ Kind = 'legacy'; Visible = $false })
            $active = $false
            continue
        }
        if ($line -match '^\s*#\s*(?:if\s+defined\s*\(\s*_WIN64\s*\)|ifdef\s+_WIN64)') {
            $conditions.Add([pscustomobject]@{ Kind = 'native'; Visible = $true })
            continue
        }
        if ($line -match '^\s*#\s*(?:if|ifdef|ifndef)\b') {
            # Unknown feature/compiler conditions can expose either branch to
            # x64. Retain both for this conservative authority scan.
            $conditions.Add([pscustomobject]@{ Kind = 'other'; Visible = $true })
            continue
        }
        if ($line -match '^\s*#\s*elif\b') {
            if ($conditions.Count -gt 0) {
                $last = $conditions.Count - 1
                if ($line -match '^\s*#\s*elif\s+!defined\s*\(\s*_WIN64\s*\)') {
                    $conditions[$last].Kind = 'legacy'
                    $conditions[$last].Visible = $false
                }
                elseif ($line -match '^\s*#\s*elif\s+defined\s*\(\s*_WIN64\s*\)') {
                    $conditions[$last].Kind = 'native'
                    $conditions[$last].Visible = $true
                }
                else {
                    $conditions[$last].Kind = 'other'
                    $conditions[$last].Visible = $true
                }
                $active = @($conditions | Where-Object { -not $_.Visible }).Count -eq 0
            }
            continue
        }
        if ($line -match '^\s*#\s*else\b') {
            if ($conditions.Count -gt 0) {
                $last = $conditions.Count - 1
                if ($conditions[$last].Kind -ne 'other') {
                    $conditions[$last].Visible = -not $conditions[$last].Visible
                }
                $active = @($conditions | Where-Object { -not $_.Visible }).Count -eq 0
            }
            continue
        }
        if ($line -match '^\s*#\s*endif\b') {
            if ($conditions.Count -gt 0) {
                $conditions.RemoveAt($conditions.Count - 1)
                $active = @($conditions | Where-Object { -not $_.Visible }).Count -eq 0
            }
            continue
        }
        if ($active) {
            $output.Add($line)
        }
    }
    return $output -join "`n"
}

function Assert-NativeDeviceAuthorityRetired([string]$Content)
{
    $nativeVisible = Get-NativeVisibleSource $Content
    foreach ($authority in @(
        @{ Pattern = '\bD3D8Lib\s*=\s*Load_D3D8_Runtime\s*\('; Name = 'Load_D3D8_Runtime' },
        @{ Pattern = '\bDirect3DCreate8Ptr\s*\(\s*D3D_SDK_VERSION\s*\)'; Name = 'Direct3DCreate8' },
        @{ Pattern = '\bD3DInterface\s*->\s*CreateDevice\s*\('; Name = 'IDirect3D8::CreateDevice' }
    )) {
        if ($nativeVisible -match $authority.Pattern) {
            throw "Native cutover still exposes $($authority.Name) device authority to x64."
        }
    }
}

function Assert-NativeDeviceBootstrapContract([string]$Content)
{
    Assert-NativeDeviceAuthorityRetired $Content
    $nativeVisible = Get-NativeVisibleSource $Content
    if ($nativeVisible -notmatch '\b_NativeProductDeviceLifecycle\.create\s*\(' -or
        $nativeVisible -notmatch '\b_D3D11Bridge\.Initialize\s*\(\s*_Hwnd\s*,\s*nullptr\s*,') {
        throw 'Native x64 device creation does not bootstrap the direct D3D11 lifecycle without a legacy device.'
    }
}

function Assert-NativeRendererAbiPropagation([string]$Content, [string]$Target)
{
    if (-not (Test-CMakeTargetLinkDependency $Content $Target `
            'core_renderer' 'PUBLIC')) {
        throw "Target $Target does not publicly propagate the native renderer ABI."
    }
}

function Assert-LegacyRenderHeaderAbiPropagation([string]$Content,
    [string]$Target)
{
    if (-not (Test-CMakeTargetLinkDependency $Content $Target `
            'rts_d3d8_headers' 'PUBLIC')) {
        throw "Target $Target does not publicly propagate its compile-only legacy render declarations."
    }
}

function Assert-WW3DPublicAbiPropagation([string]$Content, [string]$Target)
{
    if (-not (Test-CMakeTargetLinkDependency $Content $Target `
            'core_renderer' 'PUBLIC')) {
        throw "Target $Target does not expose a public WW3D ABI boundary."
    }
    foreach ($dependency in @('core_config', 'core_renderer')) {
        if (-not (Test-CMakeTargetLinkDependency $Content $Target `
                $dependency 'PUBLIC')) {
            throw "Target $Target does not publicly propagate $dependency as part of its WW3D ABI."
        }
    }
}

function Assert-NativeColorRenderTargetContract([string]$Content)
{
    $start = $Content.IndexOf(
        'DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)',
        [StringComparison]::Ordinal)
    $end = $Content.IndexOf(
        '//! Create render target with associated depth stencil buffer',
        $start, [StringComparison]::Ordinal)
    if ($start -lt 0 -or $end -le $start) {
        throw 'Color-only render-target factory body is missing or ambiguous.'
    }
    $nativeBody = Get-NativeVisibleSource $Content.Substring($start,
        $end - $start)
    foreach ($required in @(
        'NEW_REF\s*\(\s*TextureClass',
        '\bIs_Initialized\s*\(',
        '\bAcquire_Native_Surface\s*\(\s*0\s*,\s*0\s*,\s*true',
        '\bisValid\s*\(\s*\)',
        '\bREF_PTR_RELEASE\s*\(\s*texture\s*\)'
    )) {
        if ($nativeBody -notmatch $required) {
            throw 'Native color-only render-target creation is not fail-closed through typed texture ownership.'
        }
    }
}

function Assert-ProjectedShadowRenderTargetContract([string]$Content,
    [string]$Title)
{
    $start = $Content.IndexOf(
        'Bool W3DProjectedShadowManager::ReAcquireResources()',
        [StringComparison]::Ordinal)
    $end = $Content.IndexOf(
        'void W3DProjectedShadowManager::ReleaseResources()',
        $start, [StringComparison]::Ordinal)
    if ($start -lt 0 -or $end -le $start) {
        throw "$Title projected-shadow ReAcquireResources body is missing or ambiguous."
    }
    $body = $Content.Substring($start, $end - $start)
    $createCalls = [regex]::Matches($body,
        '\bDX8Wrapper::Create_Render_Target\s*\(').Count
    if ($createCalls -lt 2 -or
        $body -notmatch '!\s*m_dynamicRenderTarget->Is_Initialized\s*\(\s*\)' -or
        $body -notmatch '(?s)REF_PTR_RELEASE\s*\(\s*m_dynamicRenderTarget\s*\).*?return\s+FALSE\s*;') {
        throw "$Title projected-shadow reset path does not reject and release an invalid color target."
    }
    foreach ($handleFactory in @('Get_Shadow_Index_Buffer_Handle',
            'Get_Shadow_Vertex_Buffer_Handle')) {
        $handleStart = $body.IndexOf($handleFactory,
            [StringComparison]::Ordinal)
        $failureEnd = if ($handleStart -ge 0) {
            $body.IndexOf('return FALSE;', $handleStart,
                [StringComparison]::Ordinal)
        } else { -1 }
        if ($handleStart -lt 0 -or $failureEnd -le $handleStart -or
            $body.Substring($handleStart, $failureEnd - $handleStart) -notmatch
                '\bReleaseResources\s*\(\s*\)') {
            throw "$Title projected-shadow buffer acquisition failure does not release the complete resource set."
        }
    }
}

function Assert-ShadowManagerFailureContract([string]$Content, [string]$Title)
{
    $start = $Content.IndexOf('Bool W3DShadowManager::init()',
        [StringComparison]::Ordinal)
    $end = $Content.IndexOf('void W3DShadowManager::Reset()', $start,
        [StringComparison]::Ordinal)
    if ($start -lt 0 -or $end -le $start) {
        throw "$Title shadow-manager init body is missing or ambiguous."
    }
    $body = $Content.Substring($start, $end - $start)
    foreach ($manager in @('TheW3DVolumetricShadowManager',
            'TheW3DProjectedShadowManager')) {
        $escaped = [regex]::Escape($manager)
        if ($body -notmatch ('!\s*' + $escaped + '->init\s*\(\s*\)') -or
            $body -notmatch ('!\s*' + $escaped +
                '->ReAcquireResources\s*\(\s*\)')) {
            throw "$Title shadow startup does not propagate $manager initialization failure."
        }
    }
    if ($body -notmatch '\bresult\s*=\s*FALSE\s*;') {
        throw "$Title shadow startup does not return a failed resource acquisition."
    }
}

function Assert-ShroudRenderTargetContract([string]$Content, [string]$Title)
{
    $start = $Content.IndexOf('Bool W3DShroud::ReAcquireResources()',
        [StringComparison]::Ordinal)
    $end = $Content.IndexOf('Bool W3DShroud::syncSourceTexture()', $start,
        [StringComparison]::Ordinal)
    if ($start -lt 0 -or $end -le $start) {
        throw "$Title shroud ReAcquireResources body is missing or ambiguous."
    }
    $body = $Content.Substring($start, $end - $start)
    if ($body -notmatch '!\s*m_pDstTexture->Is_Initialized\s*\(\s*\)' -or
        $body -notmatch '(?s)REF_PTR_RELEASE\s*\(\s*m_pDstTexture\s*\).*?return\s+FALSE\s*;') {
        throw "$Title shroud reset path does not reject and release an invalid native texture."
    }
}

function Assert-NativeDeviceShutdownOwnershipContract([string]$Content)
{
    $shutdownStart = $Content.IndexOf('void DX8Wrapper::Shutdown()',
        [StringComparison]::Ordinal)
    $shutdownEnd = $Content.IndexOf(
        'bool DX8Wrapper::Do_Onetime_Device_Dependent_Inits()',
        $shutdownStart, [StringComparison]::Ordinal)
    if ($shutdownStart -lt 0 -or $shutdownEnd -le $shutdownStart) {
        throw 'Native device shutdown body is missing or ambiguous.'
    }
    $shutdownBody = $Content.Substring($shutdownStart,
        $shutdownEnd - $shutdownStart)
    $release = $shutdownBody.IndexOf('Release_Device();',
        [StringComparison]::Ordinal)
    $hostShutdown = $shutdownBody.IndexOf(
        '_NativeProductDeviceLifecycle.shutdown();',
        [StringComparison]::Ordinal)
    if ($shutdownBody -notmatch '\.ownsDeviceResources\s*\(\s*\)' -or
        $release -lt 0 -or $hostShutdown -lt 0 -or $release -ge $hostShutdown) {
        throw 'Native shutdown must release owners before the active-or-lost device host.'
    }
}

function Assert-NativeProductPackagingIsLegacyFree([string]$Content)
{
    foreach ($payload in @(
        'native-d3d8-compat',
        'native_d3d8_compat',
        'd3d8to9',
        'd3d8.dll',
        'd3dx8.dll',
        'D3DX9_43.dll',
        'D3DCompiler_43.dll',
        'mss32.dll',
        'binkw32.dll'
    )) {
        if ($Content -match [regex]::Escape($payload)) {
            throw "Native product packaging retains forbidden compatibility payload '$payload'."
        }
    }
}

function Assert-NativeProductImportsAreLegacyFree([string]$Content)
{
    if ($Content -match '(?im)^\s*(?:d3d8|d3d9|d3dx8|d3dx9_43|mss32|binkw32)\.dll\s*$') {
        throw 'Native product import table retains a forbidden legacy D3D8/D3DX/Miles/Bink runtime.'
    }
}

function Assert-PresetCacheContract($Presets, [string]$Name, [hashtable]$Expected)
{
    $preset = @($Presets.configurePresets | Where-Object { $_.name -eq $Name })
    if ($preset.Count -ne 1) {
        throw "Build graph preset '$Name' is missing or ambiguous."
    }
    foreach ($optionName in $Expected.Keys) {
        $property = $preset[0].cacheVariables.PSObject.Properties[$optionName]
        if ($null -eq $property -or $property.Value -ne $Expected[$optionName]) {
            throw "Build graph preset '$Name' must set $optionName=$($Expected[$optionName])."
        }
    }
}

function Assert-AuthoringWorkflowProductContract([string]$CIWorkflow, [string]$BuildWorkflow)
{
    foreach ($authoringPreset in @(
        'win32-generals-authoring',
        'win32-zerohour-authoring'
    )) {
        $entryPattern = '(?ms)^          - preset: "' +
            [regex]::Escape($authoringPreset) +
            '"\r?\n(?<entry>(?:(?!^          - preset: ).)*?)(?=^          - preset: |^      fail-fast:)'
        $entry = [regex]::Match($CIWorkflow, $entryPattern)
        if (-not $entry.Success -or
            $entry.Groups['entry'].Value -notmatch '(?m)^            product: false\r?$') {
            throw "CI authoring preset '$authoringPreset' must pass product:false to the reusable build workflow."
        }
    }
    if ($CIWorkflow -notmatch '(?m)^      product: \$\{\{ matrix\.product \}\}\r?$') {
        throw 'The CI matrix product flag is not propagated to the reusable build workflow.'
    }
    if ($BuildWorkflow -notmatch '(?ms)^      product:\r?\n        required: false\r?\n        default: true\r?\n        type: boolean') {
        throw 'The reusable build workflow does not expose its explicit product input contract.'
    }
}

function Test-ProductMainGuard([string]$Content, [string]$ProductOption)
{
    # CMake blocks are nested, so a simple "if ... add_subdirectory(Main) ...
    # endif" regex can accidentally join two sibling if blocks.  Strip
    # comments and walk the relevant commands while retaining the active
    # branch stack.  This intentionally checks that Main is inside the
    # product option's block, including any nested conditions.
    $withoutComments = [regex]::Replace($Content, '(?m)#.*$', '')
    $commands = [regex]::Matches(
        $withoutComments,
        '(?is)\b(if|elseif|else|endif|add_subdirectory)\s*\((.*?)\)')
    $stack = New-Object 'System.Collections.Generic.List[object]'
    $sawMain = $false
    $sawUnguardedMain = $false

    foreach ($commandMatch in $commands) {
        $command = $commandMatch.Groups[1].Value.ToLowerInvariant()
        $arguments = $commandMatch.Groups[2].Value.Trim()

        switch ($command) {
            'if' {
                $stack.Add([pscustomobject]@{
                    ProductBranch = $arguments.Equals($ProductOption, [StringComparison]::OrdinalIgnoreCase)
                })
                break
            }
            'elseif' {
                if ($stack.Count -gt 0) {
                    $stack[$stack.Count - 1].ProductBranch =
                        $arguments.Equals($ProductOption, [StringComparison]::OrdinalIgnoreCase)
                }
                break
            }
            'else' {
                if ($stack.Count -gt 0) {
                    $stack[$stack.Count - 1].ProductBranch = $false
                }
                break
            }
            'endif' {
                if ($stack.Count -gt 0) {
                    $stack.RemoveAt($stack.Count - 1)
                }
                break
            }
            'add_subdirectory' {
                $subdirectory = ($arguments -split '\s+')[0] -replace '^[''\"]|[''\"]$',''
                if ($subdirectory.Equals('Main', [StringComparison]::OrdinalIgnoreCase)) {
                    $sawMain = $true
                    if (@($stack | Where-Object { $_.ProductBranch }).Count -eq 0) {
                        $sawUnguardedMain = $true
                    }
                }
                break
            }
        }
    }

    return $sawMain -and -not $sawUnguardedMain
}

function Test-AuthoringTitleSubtreeGuard(
    [string]$Content,
    [string]$TitleDirectory,
    [string]$TitleOption)
{
    # A product-disabled authoring preset still needs the selected title
    # subtree. Walk the CMake block structure so a sibling title guard cannot
    # hide a product-gated (or otherwise unguarded) add_subdirectory call.
    $withoutComments = [regex]::Replace($Content, '(?m)#.*$', '')
    $commands = [regex]::Matches(
        $withoutComments,
        '(?is)\b(if|elseif|else|endif|add_subdirectory)\s*\((.*?)\)')
    $stack = New-Object 'System.Collections.Generic.List[object]'
    $sawTitle = $false
    $sawInvalidTitle = $false
    $productToken = '(?i)(?<![A-Za-z0-9_])RTS_BUILD_PRODUCT(?![A-Za-z0-9_])'

    foreach ($commandMatch in $commands) {
        $command = $commandMatch.Groups[1].Value.ToLowerInvariant()
        $arguments = $commandMatch.Groups[2].Value.Trim()

        switch ($command) {
            'if' {
                $stack.Add([pscustomobject]@{
                    TitleBranch = $arguments.Equals($TitleOption,
                        [StringComparison]::OrdinalIgnoreCase)
                    ProductBranch = $arguments -match $productToken
                })
                break
            }
            'elseif' {
                if ($stack.Count -gt 0) {
                    $stack[$stack.Count - 1].TitleBranch = $arguments.Equals(
                        $TitleOption, [StringComparison]::OrdinalIgnoreCase)
                    $stack[$stack.Count - 1].ProductBranch = $arguments -match $productToken
                }
                break
            }
            'else' {
                if ($stack.Count -gt 0) {
                    $stack[$stack.Count - 1].TitleBranch = $false
                    $stack[$stack.Count - 1].ProductBranch = $false
                }
                break
            }
            'endif' {
                if ($stack.Count -gt 0) {
                    $stack.RemoveAt($stack.Count - 1)
                }
                break
            }
            'add_subdirectory' {
                $subdirectory = ($arguments -split '\s+')[0] -replace '^[''\"]|[''\"]$',''
                if ($subdirectory.Equals($TitleDirectory,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    $sawTitle = $true
                    $hasTitleGuard = @($stack | Where-Object { $_.TitleBranch }).Count -gt 0
                    $hasProductGuard = @($stack | Where-Object { $_.ProductBranch }).Count -gt 0
                    if (-not $hasTitleGuard -or $hasProductGuard) {
                        $sawInvalidTitle = $true
                    }
                }
                break
            }
        }
    }

    return $sawTitle -and -not $sawInvalidTitle
}

if ($SelfTest) {
    if (-not (Test-DependencyToken 'target_link_libraries(example PRIVATE binkstub)' 'binkstub')) {
        throw 'Legacy product runtime audit self-test did not detect an exact dependency token.'
    }
    if (Test-DependencyToken 'target_link_libraries(example PRIVATE binkstub_extra)' 'binkstub') {
        throw 'Legacy product runtime audit self-test matched a dependency prefix.'
    }
    Assert-NativeD3D8FreeClosure 'links=rts_xaudio2|bcrypt|d3d11|dxgi|dinput8|dxguid'
    foreach ($dependency in $nativeCutoverForbiddenDependencies) {
        $caught = $false
        try {
            Assert-NativeD3D8FreeClosure "links=rts_xaudio2|$dependency|d3d11"
        }
        catch {
            $caught = $_.Exception.Message -match [regex]::Escape($dependency)
        }
        if (-not $caught) {
            throw "Legacy product runtime audit self-test did not reject native graph leakage '$dependency'."
        }
    }
    $goodSelector = @'
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include("${CMAKE_CURRENT_LIST_DIR}/native-product-runtime.cmake")
    target_link_libraries(rts_product_runtime INTERFACE rts_native_product_runtime)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    include("${CMAKE_CURRENT_LIST_DIR}/legacy-product-runtime.cmake")
    target_link_libraries(rts_product_runtime INTERFACE rts_legacy_product_runtime)
elseif(RTS_BUILD_PRODUCT)
endif()
'@
    Assert-ProductRuntimeSelector $goodSelector
    Assert-ProductRuntimeSelector ($goodSelector -replace "`n", "`r`n")
    $selectorCaught = $false
    try {
        Assert-ProductRuntimeSelector $goodSelector.Replace(
            'rts_native_product_runtime)', 'rts_legacy_product_runtime)')
    }
    catch {
        $selectorCaught = $true
    }
    if (-not $selectorCaught) {
        throw 'Legacy product runtime audit self-test accepted legacy selection in the x64 branch.'
    }
    $outsideSelectorCaught = $false
    $outsideSelector = $goodSelector + @'

target_link_libraries(rts_product_runtime INTERFACE rts_legacy_product_runtime)
'@
    try {
        Assert-ProductRuntimeSelector $outsideSelector
    }
    catch {
        $outsideSelectorCaught = $_.Exception.Message -match 'outside the selected architecture branches'
    }
    if (-not $outsideSelectorCaught) {
        throw 'Legacy product runtime audit self-test accepted an architecture-specific link outside the selector branches.'
    }
    if (-not (Test-ExactCMakeLine "include(cmake/product-runtime.cmake)`r`n" `
            'include(cmake/product-runtime.cmake)')) {
        throw 'Legacy product runtime audit self-test rejected a valid CRLF CMake declaration.'
    }
    if (Test-CMakeTargetLinkDependency `
            '# target_link_libraries(example PUBLIC rts_product_runtime)' `
            'example' 'rts_product_runtime' 'PUBLIC') {
        throw 'Legacy product runtime audit self-test accepted a comment-only product runtime link.'
    }
    if (-not (Test-CMakeTargetLinkDependency `
            'target_link_libraries(example PUBLIC rts_product_runtime)' `
            'example' 'rts_product_runtime' 'PUBLIC')) {
        throw 'Legacy product runtime audit self-test rejected a real product runtime link.'
    }
    Assert-NativeDeviceAuthorityRetired @'
#if !defined(_WIN64)
D3D8Lib = Load_D3D8_Runtime();
D3DInterface = Direct3DCreate8Ptr(D3D_SDK_VERSION);
D3DInterface->CreateDevice();
#endif
'@
    $authorityCaught = $false
    try {
        Assert-NativeDeviceAuthorityRetired @'
D3D8Lib = Load_D3D8_Runtime();
D3DInterface = Direct3DCreate8Ptr(D3D_SDK_VERSION);
D3DInterface->CreateDevice();
'@
    }
    catch {
        $authorityCaught = $_.Exception.Message -match 'device authority'
    }
    if (-not $authorityCaught) {
        throw 'Legacy product runtime audit self-test accepted x64-visible D3D8 device authority.'
    }
    Assert-NativeDeviceBootstrapContract @'
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
bool InitializeNative() { return _D3D11Bridge.Initialize(_Hwnd, nullptr, width, height); }
bool CreateNative() { return _NativeProductDeviceLifecycle.create(width, height); }
#else
D3D8Lib = Load_D3D8_Runtime();
D3DInterface->CreateDevice();
#endif
'@
    Assert-NativeRendererAbiPropagation @'
target_link_libraries(g_gameenginedevice PUBLIC core_renderer)
'@ 'g_gameenginedevice'
    $abiPropagationCaught = $false
    try {
        Assert-NativeRendererAbiPropagation @'
target_link_libraries(g_gameenginedevice PRIVATE core_renderer)
'@ 'g_gameenginedevice'
    }
    catch {
        $abiPropagationCaught = $_.Exception.Message -match 'renderer ABI'
    }
    if (-not $abiPropagationCaught) {
        throw 'Legacy product runtime audit self-test accepted private native renderer ABI propagation.'
    }
    $commentedAbiCaught = $false
    try {
        Assert-NativeRendererAbiPropagation @'
# target_link_libraries(g_gameenginedevice PUBLIC core_renderer)
'@ 'g_gameenginedevice'
    }
    catch {
        $commentedAbiCaught = $_.Exception.Message -match 'renderer ABI'
    }
    if (-not $commentedAbiCaught) {
        throw 'Legacy product runtime audit self-test accepted comment-only renderer ABI propagation.'
    }
    Assert-LegacyRenderHeaderAbiPropagation @'
target_link_libraries(z_gameenginedevice PUBLIC rts_d3d8_headers)
'@ 'z_gameenginedevice'
    $headerPropagationCaught = $false
    try {
        Assert-LegacyRenderHeaderAbiPropagation @'
target_link_libraries(z_gameenginedevice PRIVATE rts_d3d8_headers)
'@ 'z_gameenginedevice'
    }
    catch {
        $headerPropagationCaught = $_.Exception.Message -match 'compile-only legacy render declarations'
    }
    if (-not $headerPropagationCaught) {
        throw 'Legacy product runtime audit self-test accepted private legacy render-header ABI propagation.'
    }
    Assert-WW3DPublicAbiPropagation @'
target_link_libraries(z_ww3d2 PUBLIC core_config core_renderer)
'@ 'z_ww3d2'
    $ww3dAbiPropagationCaught = $false
    try {
        Assert-WW3DPublicAbiPropagation @'
target_link_libraries(z_ww3d2 PUBLIC core_renderer)
target_link_libraries(z_ww3d2 PRIVATE core_config)
'@ 'z_ww3d2'
    }
    catch {
        $ww3dAbiPropagationCaught = $_.Exception.Message -match 'WW3D ABI|core_config'
    }
    if (-not $ww3dAbiPropagationCaught) {
        throw 'Legacy product runtime audit self-test accepted split WW3D build-config ABI propagation.'
    }
    Assert-NativeColorRenderTargetContract @'
TextureClass *
DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
    TextureClass *texture = NEW_REF(TextureClass,(width, height, format));
    NativeW3DSurfaceHandle surface;
    if (texture == nullptr || !texture->Is_Initialized() ||
        !texture->Acquire_Native_Surface(0, 0, true, &surface) ||
        !surface.isValid()) { REF_PTR_RELEASE(texture); }
    return texture;
#else
    return legacy;
#endif
}
//! Create render target with associated depth stencil buffer
'@
    $colorTargetCaught = $false
    try {
        Assert-NativeColorRenderTargetContract @'
TextureClass *
DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
    return nullptr;
#else
    return legacy;
#endif
}
//! Create render target with associated depth stencil buffer
'@
    }
    catch {
        $colorTargetCaught = $_.Exception.Message -match 'typed texture ownership'
    }
    if (-not $colorTargetCaught) {
        throw 'Legacy product runtime audit self-test accepted a null native color-target factory.'
    }
    Assert-ProjectedShadowRenderTargetContract @'
Bool W3DProjectedShadowManager::ReAcquireResources()
{
    m_dynamicRenderTarget = DX8Wrapper::Create_Render_Target(1, 1, alpha);
    if (m_dynamicRenderTarget == nullptr)
        m_dynamicRenderTarget = DX8Wrapper::Create_Render_Target(1, 1);
    if (m_dynamicRenderTarget == nullptr ||
        !m_dynamicRenderTarget->Is_Initialized()) {
        REF_PTR_RELEASE(m_dynamicRenderTarget);
        return FALSE;
    }
    if (Get_Shadow_Index_Buffer_Handle(owner) == nullptr) {
        ReleaseResources();
        return FALSE;
    }
    if (Get_Shadow_Vertex_Buffer_Handle(owner) == nullptr) {
        ReleaseResources();
        return FALSE;
    }
    return TRUE;
}
void W3DProjectedShadowManager::ReleaseResources()
'@ 'fixture'
    $shadowTargetCaught = $false
    try {
        Assert-ProjectedShadowRenderTargetContract @'
Bool W3DProjectedShadowManager::ReAcquireResources()
{
    m_dynamicRenderTarget = DX8Wrapper::Create_Render_Target(1, 1, alpha);
    if (m_dynamicRenderTarget == nullptr)
        m_dynamicRenderTarget = DX8Wrapper::Create_Render_Target(1, 1);
    return TRUE;
}
void W3DProjectedShadowManager::ReleaseResources()
'@ 'fixture'
    }
    catch {
        $shadowTargetCaught = $_.Exception.Message -match 'invalid color target'
    }
    if (-not $shadowTargetCaught) {
        throw 'Legacy product runtime audit self-test accepted an unchecked projected-shadow target.'
    }
    Assert-ShadowManagerFailureContract @'
Bool W3DShadowManager::init()
{
    Bool result = TRUE;
    if (TheW3DVolumetricShadowManager &&
        (!TheW3DVolumetricShadowManager->init() ||
         !TheW3DVolumetricShadowManager->ReAcquireResources())) result = FALSE;
    if (TheW3DProjectedShadowManager &&
        (!TheW3DProjectedShadowManager->init() ||
         !TheW3DProjectedShadowManager->ReAcquireResources())) result = FALSE;
    return result;
}
void W3DShadowManager::Reset()
'@ 'fixture'
    $shadowManagerCaught = $false
    try {
        Assert-ShadowManagerFailureContract @'
Bool W3DShadowManager::init()
{
    if (TheW3DVolumetricShadowManager->init())
        TheW3DVolumetricShadowManager->ReAcquireResources();
    if (TheW3DProjectedShadowManager->init())
        TheW3DProjectedShadowManager->ReAcquireResources();
    return TRUE;
}
void W3DShadowManager::Reset()
'@ 'fixture'
    }
    catch {
        $shadowManagerCaught = $_.Exception.Message -match 'initialization failure|failed resource acquisition'
    }
    if (-not $shadowManagerCaught) {
        throw 'Legacy product runtime audit self-test accepted ignored shadow startup failures.'
    }
    Assert-ShroudRenderTargetContract @'
Bool W3DShroud::ReAcquireResources()
{
    if (!m_pDstTexture || !m_pDstTexture->Is_Initialized()) {
        REF_PTR_RELEASE(m_pDstTexture);
        return FALSE;
    }
    return TRUE;
}
Bool W3DShroud::syncSourceTexture()
'@ 'fixture'
    $shroudTargetCaught = $false
    try {
        Assert-ShroudRenderTargetContract @'
Bool W3DShroud::ReAcquireResources()
{
    return m_pDstTexture != nullptr;
}
Bool W3DShroud::syncSourceTexture()
'@ 'fixture'
    }
    catch {
        $shroudTargetCaught = $_.Exception.Message -match 'invalid native texture'
    }
    if (-not $shroudTargetCaught) {
        throw 'Legacy product runtime audit self-test accepted an unchecked shroud texture.'
    }
    Assert-NativeProductPackagingIsLegacyFree 'install(FILES xaudio2.dll DESTINATION Generals)'
    $packagingCaught = $false
    try {
        Assert-NativeProductPackagingIsLegacyFree 'install(FILES d3d8.dll D3DX9_43.dll DESTINATION Generals)'
    }
    catch {
        $packagingCaught = $_.Exception.Message -match 'forbidden compatibility payload'
    }
    if (-not $packagingCaught) {
        throw 'Legacy product runtime audit self-test accepted an x64 D3D8/D3DX packaging leak.'
    }
    Assert-NativeProductImportsAreLegacyFree "d3d11.dll`ndxgi.dll`nxaudio2_9.dll"
    $importCaught = $false
    try {
        Assert-NativeProductImportsAreLegacyFree "d3d11.dll`n  mss32.dll`ndxgi.dll"
    }
    catch {
        $importCaught = $_.Exception.Message -match 'import table'
    }
    if (-not $importCaught) {
        throw 'Legacy product runtime audit self-test accepted an x64 Miles import leak.'
    }
    $presetFixture = '{"configurePresets":[{"name":"fixture","cacheVariables":{"PRODUCT":"OFF","TOOLS":"ON"}}]}' |
        ConvertFrom-Json
    Assert-PresetCacheContract $presetFixture 'fixture' @{ PRODUCT = 'OFF'; TOOLS = 'ON' }
    $guardFixture = @'
if(RTS_BUILD_GENERALS_PRODUCT)
    add_subdirectory(Main)
endif()
'@
    if (-not (Test-ProductMainGuard $guardFixture 'RTS_BUILD_GENERALS_PRODUCT')) {
        throw 'Legacy product runtime audit self-test did not recognize a block-bounded product Main guard.'
    }
    $siblingGuardFixture = @'
if(RTS_BUILD_GENERALS_PRODUCT)
endif()
if(RTS_BUILD_GENERALS_TOOLS)
    add_subdirectory(Main)
endif()
'@
    if (Test-ProductMainGuard $siblingGuardFixture 'RTS_BUILD_GENERALS_PRODUCT') {
        throw 'Legacy product runtime audit self-test crossed a sibling if block while locating Main.'
    }
    $duplicateGuardFixture = @'
if(RTS_BUILD_GENERALS_PRODUCT)
    add_subdirectory(Main)
endif()
if(RTS_BUILD_GENERALS_TOOLS)
    add_subdirectory(Main)
endif()
'@
    if (Test-ProductMainGuard $duplicateGuardFixture 'RTS_BUILD_GENERALS_PRODUCT') {
        throw 'Legacy product runtime audit self-test ignored an unguarded duplicate Main occurrence.'
    }
    $authoringTitleFixture = @'
if(RTS_BUILD_GENERALS)
    add_subdirectory(Generals)
endif()
'@
    if (-not (Test-AuthoringTitleSubtreeGuard $authoringTitleFixture `
            'Generals' 'RTS_BUILD_GENERALS')) {
        throw 'Legacy product runtime audit self-test did not accept a title-selected authoring subtree.'
    }
    $productGatedTitleFixture = @'
if(RTS_BUILD_PRODUCT AND RTS_BUILD_GENERALS)
    add_subdirectory(Generals)
endif()
'@
    if (Test-AuthoringTitleSubtreeGuard $productGatedTitleFixture `
            'Generals' 'RTS_BUILD_GENERALS') {
        throw 'Legacy product runtime audit self-test accepted a product-gated authoring subtree.'
    }
    Write-Output 'Legacy product runtime audit self-test passed.'
    exit 0
}

$runtimeModules = @{
    Selector = Join-Path $SourceRoot 'cmake/product-runtime.cmake'
    Native = Join-Path $SourceRoot 'cmake/native-product-runtime.cmake'
    Legacy = Join-Path $SourceRoot 'cmake/legacy-product-runtime.cmake'
}
foreach ($runtimeModule in $runtimeModules.Values) {
    if (-not (Test-Path -LiteralPath $runtimeModule)) {
        throw "Product runtime module is missing: $runtimeModule"
    }
}

$selectorModule = Get-Content -LiteralPath $runtimeModules.Selector -Raw
$nativeModule = Get-Content -LiteralPath $runtimeModules.Native -Raw
$legacyModule = Get-Content -LiteralPath $runtimeModules.Legacy -Raw
Assert-ProductRuntimeSelector $selectorModule

$rootCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt') -Raw
if (-not (Test-ExactCMakeLine $rootCMake 'include(cmake/product-runtime.cmake)') -or
    (Test-ExactCMakeLine $rootCMake 'include(cmake/native-product-runtime.cmake)') -or
    (Test-ExactCMakeLine $rootCMake 'include(cmake/legacy-product-runtime.cmake)') -or
    $rootCMake -match '(?i)native-d3d8-compat\.cmake') {
    throw 'The root graph must register only the architecture-selected product runtime module.'
}
foreach ($titleSubtree in @(
    @{ Directory = 'Generals'; Option = 'RTS_BUILD_GENERALS' },
    @{ Directory = 'GeneralsMD'; Option = 'RTS_BUILD_ZEROHOUR' }
)) {
    if (-not (Test-AuthoringTitleSubtreeGuard $rootCMake `
            $titleSubtree.Directory $titleSubtree.Option)) {
        throw "The root graph must select $($titleSubtree.Directory) from $($titleSubtree.Option) independently of RTS_BUILD_PRODUCT."
    }
}
if ($legacyModule -notmatch 'CMAKE_SIZEOF_VOID_P EQUAL 4') {
    throw 'The legacy product runtime module is not restricted to 32-bit builds.'
}
foreach ($forbidden in @('rts_native_product_runtime', 'rts_xaudio2', 'bcrypt', 'd3d11', 'dxgi')) {
    if (Test-DependencyToken $legacyModule $forbidden) {
        throw "The 32-bit legacy runtime module leaks native dependency '$forbidden'."
    }
}
foreach ($required in @('rts_xaudio2', 'bcrypt', 'd3d11', 'dxgi')) {
    if (-not (Test-DependencyToken $nativeModule $required)) {
        throw "The native x64 runtime module is missing '$required'."
    }
}
Assert-NativeD3D8FreeClosure $nativeModule
if (-not (Test-ExactCMakeLine $nativeModule `
        'set(RTS_NATIVE_PRODUCT_REQUIRES_LEGACY_D3D8 OFF CACHE INTERNAL')) {
    throw 'The native x64 runtime module does not explicitly retire the legacy D3D8 runtime requirement.'
}
if (-not (Test-ExactCMakeLine $nativeModule `
        'set(RTS_NATIVE_PRODUCT_RESOURCE_CLOSURE_COMPLETE ON CACHE INTERNAL')) {
    throw 'The native x64 runtime module does not declare the completed sampled-texture and surface ownership cutover.'
}

$presets = Get-Content -LiteralPath (Join-Path $SourceRoot 'CMakePresets.json') -Raw |
    ConvertFrom-Json
Assert-PresetCacheContract $presets 'vc6-generals-oracle' @{
    RTS_BUILD_CORE_TOOLS = 'OFF'
    RTS_BUILD_CORE_EXTRAS = 'OFF'
    RTS_BUILD_PRODUCT = 'ON'
    RTS_BUILD_GENERALS = 'ON'
    RTS_BUILD_ZEROHOUR = 'OFF'
    RTS_BUILD_GENERALS_PRODUCT = 'ON'
    RTS_BUILD_GENERALS_TOOLS = 'OFF'
    RTS_BUILD_OPTION_FFMPEG = 'OFF'
}
Assert-PresetCacheContract $presets 'vc6-zerohour-oracle' @{
    RTS_BUILD_CORE_TOOLS = 'OFF'
    RTS_BUILD_CORE_EXTRAS = 'OFF'
    RTS_BUILD_PRODUCT = 'ON'
    RTS_BUILD_GENERALS = 'OFF'
    RTS_BUILD_ZEROHOUR = 'ON'
    RTS_BUILD_ZEROHOUR_PRODUCT = 'ON'
    RTS_BUILD_ZEROHOUR_TOOLS = 'OFF'
    RTS_BUILD_OPTION_FFMPEG = 'OFF'
}
Assert-PresetCacheContract $presets 'win32-generals-authoring' @{
    RTS_BUILD_PRODUCT = 'OFF'
    RTS_BUILD_GENERALS = 'ON'
    RTS_BUILD_ZEROHOUR = 'OFF'
    RTS_BUILD_GENERALS_PRODUCT = 'OFF'
    RTS_BUILD_GENERALS_TOOLS = 'ON'
}
Assert-PresetCacheContract $presets 'win32-zerohour-authoring' @{
    RTS_BUILD_PRODUCT = 'OFF'
    RTS_BUILD_GENERALS = 'OFF'
    RTS_BUILD_ZEROHOUR = 'ON'
    RTS_BUILD_ZEROHOUR_PRODUCT = 'OFF'
    RTS_BUILD_ZEROHOUR_TOOLS = 'ON'
}

$ciWorkflowPath = Join-Path $SourceRoot '.github/workflows/ci.yml'
$buildWorkflowPath = Join-Path $SourceRoot '.github/workflows/build-toolchain.yml'
if (-not (Test-Path -LiteralPath $ciWorkflowPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $buildWorkflowPath -PathType Leaf)) {
    throw 'The authoring product contract requires both CI workflow sources.'
}
Assert-AuthoringWorkflowProductContract `
    (Get-Content -LiteralPath $ciWorkflowPath -Raw) `
    (Get-Content -LiteralPath $buildWorkflowPath -Raw)

foreach ($title in @(
    @{ Path = 'Generals/Code/CMakeLists.txt'; ProductOption = 'RTS_BUILD_GENERALS_PRODUCT' },
    @{ Path = 'GeneralsMD/Code/CMakeLists.txt'; ProductOption = 'RTS_BUILD_ZEROHOUR_PRODUCT' }
)) {
    $titleCMake = Get-Content -LiteralPath (Join-Path $SourceRoot $title.Path) -Raw
    if (-not (Test-ProductMainGuard $titleCMake $title.ProductOption)) {
        throw "$($title.Path) does not isolate the product executable from the authoring graph."
    }
}

foreach ($productPackaging in @('Generals/CMakeLists.txt', 'GeneralsMD/CMakeLists.txt')) {
    Assert-NativeProductPackagingIsLegacyFree (Get-Content -LiteralPath (
        Join-Path $SourceRoot $productPackaging) -Raw)
}

$requiredConsumers = @(
    'Core/GameEngine/CMakeLists.txt',
    'Core/GameEngineDevice/CMakeLists.txt',
    'Core/Libraries/Source/WWVegas/CMakeLists.txt',
    'Generals/Code/Libraries/Source/WWVegas/CMakeLists.txt',
    'GeneralsMD/Code/Libraries/Source/WWVegas/CMakeLists.txt',
    'Generals/Code/Main/CMakeLists.txt',
    'GeneralsMD/Code/Main/CMakeLists.txt',
    'Generals/Code/Tools/RuntimeRegressionTests/CMakeLists.txt',
    'GeneralsMD/Code/Tools/RuntimeRegressionTests/CMakeLists.txt'
)

$legacyDependencies = @(
    'binkstub',
    'milesstub',
    'rts_d3d8lib',
    'd3d8',
    'd3dx8'
)
foreach ($relativePath in $requiredConsumers) {
    $path = Join-Path $SourceRoot $relativePath
    $content = Get-Content -LiteralPath $path -Raw
    $executableContent = [regex]::Replace($content, '(?m)#.*$', '')
    if (-not (Test-CMakeTargetLinkDependency $content '*' `
            'rts_product_runtime')) {
        throw "Missing architecture-selected rts_product_runtime boundary in $relativePath"
    }
    if ($executableContent -match '\brts_(?:legacy|native)_product_runtime\b') {
        throw "Architecture-specific product runtime bypasses rts_product_runtime in $relativePath"
    }

    foreach ($dependency in $legacyDependencies) {
        if (Test-DependencyToken $executableContent $dependency) {
            throw "Legacy dependency $dependency bypasses rts_product_runtime in $relativePath"
        }
    }
}

$dx8WrapperSource = Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp') -Raw
Assert-NativeDeviceBootstrapContract $dx8WrapperSource
Assert-NativeDeviceShutdownOwnershipContract $dx8WrapperSource
Assert-NativeColorRenderTargetContract $dx8WrapperSource
Assert-NativeRendererAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/CMakeLists.txt') -Raw) `
    'g_gameenginedevice'
Assert-LegacyRenderHeaderAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/CMakeLists.txt') -Raw) `
    'g_gameenginedevice'
Assert-NativeRendererAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/CMakeLists.txt') -Raw) `
    'z_gameenginedevice'
Assert-LegacyRenderHeaderAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/CMakeLists.txt') -Raw) `
    'z_gameenginedevice'
Assert-WW3DPublicAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt') -Raw) `
    'g_ww3d2'
Assert-WW3DPublicAbiPropagation (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt') -Raw) `
    'z_ww3d2'
Assert-ProjectedShadowRenderTargetContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp') -Raw) `
    'Generals'
Assert-ProjectedShadowRenderTargetContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp') -Raw) `
    'Zero Hour'
Assert-ShadowManagerFailureContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp') -Raw) `
    'Generals'
Assert-ShadowManagerFailureContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp') -Raw) `
    'Zero Hour'
Assert-ShroudRenderTargetContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp') -Raw) `
    'Generals'
Assert-ShroudRenderTargetContract (Get-Content -LiteralPath (
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp') -Raw) `
    'Zero Hour'

if ($RequireNativeD3D8Free) {
    if ([string]::IsNullOrWhiteSpace($NativeClosurePath) -or
        -not (Test-Path -LiteralPath $NativeClosurePath -PathType Leaf)) {
        throw 'NativeClosurePath must name a generated native product link closure when RequireNativeD3D8Free is enabled.'
    }
    Assert-NativeD3D8FreeClosure (Get-Content -LiteralPath $NativeClosurePath -Raw)
}

Write-Output 'Product runtime architecture selection and dependency audit passed.'
