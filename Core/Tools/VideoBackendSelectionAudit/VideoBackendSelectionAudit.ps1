param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Assert-TokenCount {
    param(
        [string]$Content,
        [string]$Pattern,
        [int]$ExpectedCount,
        [string]$FailureMessage
    )

    if ([regex]::Matches($Content, $Pattern).Count -ne $ExpectedCount) {
        throw $FailureMessage
    }
}

function Assert-ExactConditionalBlock {
    param(
        [string]$Content,
        [string]$Open,
        [string]$Body,
        [string]$Close,
        [string]$FailureMessage
    )

    $normalized = $Content -replace "`r`n", "`n"
    $pattern = '(?m)^' + [regex]::Escape($Open) + "`n" + [regex]::Escape($Body) + "`n" + [regex]::Escape($Close) + '$'
    if ([regex]::Matches($normalized, $pattern).Count -ne 1) {
        throw $FailureMessage
    }
}

function Assert-EffectiveRuntimeInstall {
    param(
        [string]$Content,
        [string]$EffectivePrefix,
        [string]$FailureMessage
    )

    $normalized = $Content -replace "`r`n", "`n"
    $pattern = '(?ms)^    if\(WIN32 AND RTS_BUILD_OPTION_FFMPEG AND RTS_FFMPEG_RUNTIME_DLLS\)\s*' +
        '^        # Install the exact resolved DLLs, never an unbounded SDK bin directory\.\s*' +
        '^        install\(FILES \$\{RTS_FFMPEG_RUNTIME_DLLS\}\s*' +
        '^            DESTINATION "\$\{' + [regex]::Escape($EffectivePrefix) + '\}"\)\s*' +
        '^    endif\(\)'
    if ([regex]::Matches($normalized, $pattern).Count -ne 1) {
        throw $FailureMessage
    }
}

function Remove-CppCommentsAndLiterals {
    param([string]$Content)

    $pattern = '(?s)R"(?<rawDelimiter>[^\s()\\]{0,16})\(.*?\)\k<rawDelimiter>"|' +
        '/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace($Content, $pattern, ' ')
}

function Get-CppClassBody {
    param(
        [string]$Content,
        [string]$ClassName
    )

    $code = Remove-CppCommentsAndLiterals $Content
    $declaration = [regex]::Match($code,
        '\bclass\s+' + [regex]::Escape($ClassName) + '\b[^;{]*\{')
    if (-not $declaration.Success) {
        throw "Class $ClassName was not found."
    }

    $openBrace = $declaration.Index + $declaration.Value.LastIndexOf('{')
    $depth = 0
    for ($index = $openBrace; $index -lt $code.Length; ++$index) {
        if ($code[$index] -eq '{') {
            ++$depth
        } elseif ($code[$index] -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $code.Substring($openBrace + 1, $index - $openBrace - 1)
            }
        }
    }
    throw "Class $ClassName has no balanced body."
}

function Assert-LegacyVideoAccessor {
    param(
        [string]$ClassBody,
        [string]$ReturnValue,
        [switch]$RequireOverride,
        [string]$FailureMessage
    )

    $override = if ($RequireOverride) { '\s+override' } else { '(?:\s+override)?' }
    $pattern = '\bvirtual\s+LegacyVideoAudioInterface\s*\*\s*' +
        'getLegacyVideoAudioInterface\s*\(\s*\)' + $override +
        '\s*\{\s*return\s+' + [regex]::Escape($ReturnValue) + '\s*;\s*\}'
    if ($ClassBody -cnotmatch $pattern) {
        throw $FailureMessage
    }
}

function Assert-NoLegacyVideoDynamicCast {
    param([string]$Content)

    $code = Remove-CppCommentsAndLiterals $Content
    if ($code -cmatch '\bdynamic_cast\s*<') {
        throw 'Bink player reintroduced an RTTI-dependent legacy video audio bridge.'
    }
}

function Assert-LegacyVideoAccessorCount {
    param(
        [string]$Content,
        [int]$ExpectedCount
    )

    $code = Remove-CppCommentsAndLiterals $Content
    if ([regex]::Matches($code, '\bgetLegacyVideoAudioInterface\s*\(').Count -ne $ExpectedCount) {
        throw 'Bink player does not use exactly the checked legacy video audio capabilities.'
    }
}

function Get-CppIntroBody {
    param([string]$Content)

    $code = Remove-CppCommentsAndLiterals $Content
    $matches = [regex]::Matches($code,
        '\bif\s*\(\s*m_intro\s*!=\s*nullptr\s*\)\s*\{')
    $candidates = @()
    foreach ($match in $matches) {
        $openBrace = $match.Index + $match.Value.LastIndexOf('{')
        $depth = 0
        for ($index = $openBrace; $index -lt $code.Length; ++$index) {
            if ($code[$index] -eq '{') {
                ++$depth
            } elseif ($code[$index] -eq '}') {
                --$depth
                if ($depth -eq 0) {
                    $body = $code.Substring($openBrace + 1, $index - $openBrace - 1)
                    if ($body -match '\bTheDisplay\s*->\s*UPDATE\s*\(' -and
                        $body -match '\bTheDisplay\s*->\s*DRAW\s*\(' -and
                        $body -match '\breturn\s*;') {
                        $candidates += $body
                    }
                    break
                }
            }
        }
    }
    if ($candidates.Count -ne 1) {
        throw 'Game client does not contain exactly one checked intro update block.'
    }
    return $candidates[0]
}

function Assert-IntroVideoService {
    param(
        [string]$Content,
        [string]$FailureMessage
    )

    $body = Get-CppIntroBody $Content
    $videoCalls = [regex]::Matches($body,
        '\bTheVideoPlayer\s*->\s*UPDATE\s*\(\s*\)\s*;')
    $displayUpdate = [regex]::Match($body,
        '\bTheDisplay\s*->\s*UPDATE\s*\(\s*\)\s*;')
    $displayDraw = [regex]::Match($body,
        '\bTheDisplay\s*->\s*DRAW\s*\(\s*\)\s*;')
    $return = [regex]::Match($body, '\breturn\s*;')
    if ($videoCalls.Count -ne 1 -or
        -not $displayUpdate.Success -or
        -not $displayDraw.Success -or
        -not $return.Success -or
        $videoCalls[0].Index -gt $displayUpdate.Index -or
        $displayUpdate.Index -gt $displayDraw.Index -or
        $displayDraw.Index -gt $return.Index) {
        throw $FailureMessage
    }
}

if ($SelfTest) {
    $valid = "    if(`${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)`n        include(cmake/bink.cmake)`n    endif()"
    Assert-ExactConditionalBlock $valid '    if(${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)' '        include(cmake/bink.cmake)' '    endif()' 'Valid conditional block was rejected.'

    $wrongCondition = "    if(RTS_BUILD_OPTION_FFMPEG)`n        include(cmake/bink.cmake)`n    endif()"
    try {
        Assert-ExactConditionalBlock $wrongCondition '    if(NOT RTS_BUILD_OPTION_FFMPEG)' '        include(cmake/bink.cmake)' '    endif()' 'Malformed condition was accepted.'
        throw 'Negative conditional self-test did not reject a wrong condition.'
    } catch {
        if ($_.Exception.Message -ne 'Malformed condition was accepted.') { throw }
    }

    $duplicate = $valid + "`n" + $valid
    try {
        Assert-ExactConditionalBlock $duplicate '    if(${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)' '        include(cmake/bink.cmake)' '    endif()' 'Duplicate conditional block was accepted.'
        throw 'Negative conditional self-test did not reject a duplicate block.'
    } catch {
        if ($_.Exception.Message -ne 'Duplicate conditional block was accepted.') { throw }
    }

    $validInstall = @'
    if(WIN32 AND RTS_BUILD_OPTION_FFMPEG AND RTS_FFMPEG_RUNTIME_DLLS)
        # Install the exact resolved DLLs, never an unbounded SDK bin directory.
        install(FILES ${RTS_FFMPEG_RUNTIME_DLLS}
            DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
    endif()
'@
    Assert-EffectiveRuntimeInstall $validInstall '_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS' 'Valid effective-prefix install was rejected.'
    $rawInstall = $validInstall.Replace('_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS',
        'RTS_INSTALL_PREFIX_GENERALS')
    try {
        Assert-EffectiveRuntimeInstall $rawInstall '_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS' 'Raw-prefix install was accepted.'
        throw 'Negative effective-prefix self-test did not reject a raw prefix.'
    } catch {
        if ($_.Exception.Message -ne 'Raw-prefix install was accepted.') { throw }
    }

    $validAudioManager = @'
class AudioManager : public SubsystemInterface
{
    public:
        // A misleading getLegacyVideoAudioInterface() token must not satisfy the check.
        const char *description = "dynamic_cast<LegacyVideoAudioInterface *>(TheAudio)";
        virtual LegacyVideoAudioInterface *
        getLegacyVideoAudioInterface()
        {
            return nullptr;
        }
};
'@
    $audioManagerBody = Get-CppClassBody $validAudioManager 'AudioManager'
    Assert-LegacyVideoAccessor $audioManagerBody 'nullptr' `
        -FailureMessage 'Valid AudioManager accessor was rejected.'

    $validMilesAudioManager = @'
class MilesAudioManager : public AudioManager, public LegacyVideoAudioInterface
{
    public:
        virtual LegacyVideoAudioInterface *getLegacyVideoAudioInterface() override
        {
            return this;
        }
};
'@
    $milesAudioManagerBody = Get-CppClassBody $validMilesAudioManager 'MilesAudioManager'
    Assert-LegacyVideoAccessor $milesAudioManagerBody 'this' -RequireOverride `
        -FailureMessage 'Valid MilesAudioManager accessor was rejected.'

    $validBinkPlayer = @'
void BinkVideoPlayer::deinit()
{
    TheAudio->getLegacyVideoAudioInterface();
    /* alias->getLegacyVideoAudioInterface(); */
    const char *ignored = "dynamic_cast<LegacyVideoAudioInterface *>(TheAudio)";
    const char *rawIgnored = R"AUDIT(alias->getLegacyVideoAudioInterface();
        dynamic_cast<LegacyVideoAudioInterface const *>(TheAudio))AUDIT";
    TheAudio->getLegacyVideoAudioInterface();
    TheAudio->getLegacyVideoAudioInterface();
}
'@
    Assert-NoLegacyVideoDynamicCast $validBinkPlayer
    Assert-LegacyVideoAccessorCount $validBinkPlayer 3

    foreach ($invalidDynamicCast in @(
        'dynamic_cast<LegacyVideoAudioInterface*>(TheAudio)',
        'dynamic_cast< const LegacyVideoAudioInterface * >(TheAudio)',
        'dynamic_cast<LegacyVideoAudioInterface const*>(TheAudio)',
        'dynamic_cast<::LegacyVideoAudioInterface volatile *>(TheAudio)',
        'using V = LegacyVideoAudioInterface; dynamic_cast<V *>(TheAudio)',
        'dynamic_cast<class LegacyVideoAudioInterface *>(TheAudio)',
        'dynamic_cast<::audio::LegacyVideoAudioInterface *>(TheAudio)'
    )) {
        try {
            Assert-NoLegacyVideoDynamicCast $invalidDynamicCast
            throw 'Negative dynamic-cast self-test accepted an RTTI bridge.'
        } catch {
            if ($_.Exception.Message -ne 'Bink player reintroduced an RTTI-dependent legacy video audio bridge.') { throw }
        }
    }

    $wrongCaseAccessor = $validAudioManager.Replace(
        'getLegacyVideoAudioInterface', 'getlegacyvideoaudiointerface')
    try {
        Assert-LegacyVideoAccessor (Get-CppClassBody $wrongCaseAccessor 'AudioManager') 'nullptr' `
            -FailureMessage 'Wrong-case accessor was accepted.'
        throw 'Negative accessor-case self-test accepted a malformed identifier.'
    } catch {
        if ($_.Exception.Message -ne 'Wrong-case accessor was accepted.') { throw }
    }

    $extraAliasCall = $validBinkPlayer + "`nLegacyVideoAudioInterface *audio = manager->getLegacyVideoAudioInterface();"
    try {
        Assert-LegacyVideoAccessorCount $extraAliasCall 3
        throw 'Negative accessor-count self-test accepted a fourth alias call.'
    } catch {
        if ($_.Exception.Message -ne 'Bink player does not use exactly the checked legacy video audio capabilities.') { throw }
    }

    $validIntroUpdate = @'
if (m_intro != nullptr)
{
    TheVideoPlayer->UPDATE();
    TheDisplay->UPDATE();
    TheDisplay->DRAW();
    return;
}
'@
    Assert-IntroVideoService $validIntroUpdate 'Valid intro video service ordering was rejected.'

    foreach ($invalidIntroUpdate in @(
        $validIntroUpdate.Replace('    TheVideoPlayer->UPDATE();' + "`n", ''),
        $validIntroUpdate.Replace(
            '    TheVideoPlayer->UPDATE();' + "`n" + '    TheDisplay->UPDATE();',
            '    TheDisplay->UPDATE();' + "`n" + '    TheVideoPlayer->UPDATE();'),
        $validIntroUpdate.Replace(
            '    TheVideoPlayer->UPDATE();',
            '    TheVideoPlayer->UPDATE();' + "`n" + '    TheVideoPlayer->UPDATE();')
    )) {
        try {
            Assert-IntroVideoService $invalidIntroUpdate 'Malformed intro video service ordering was accepted.'
            throw 'Negative intro-service self-test accepted a malformed update block.'
        } catch {
            if ($_.Exception.Message -ne 'Malformed intro video service ordering was accepted.') { throw }
        }
    }

    Write-Output 'Video backend selection audit negative self-test passed.'
    exit 0
}

$headers = @(
    'Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h',
    'GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h'
)

foreach ($relativePath in $headers) {
    $content = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    if ($content -notmatch '(?ms)^\s*#ifdef RTS_HAS_FFMPEG\s*^\s*#include "VideoDevice/FFmpeg/FFmpegVideoPlayer\.h"\s*^\s*#else\s*^\s*#include "VideoDevice/Bink/BinkVideoPlayer\.h"\s*^\s*#endif') {
        throw "FFmpeg/Bink video header selection contract is missing in $relativePath"
    }
    Assert-TokenCount $content 'VideoDevice/FFmpeg/FFmpegVideoPlayer\.h' 1 "FFmpeg header ownership is ambiguous in $relativePath"
    Assert-TokenCount $content 'VideoDevice/Bink/BinkVideoPlayer\.h' 1 "Bink header ownership is ambiguous in $relativePath"
    if ($content -notmatch '(?ms)^\s*#ifdef RTS_HAS_FFMPEG\s*^\s*virtual VideoPlayerInterface \*createVideoPlayer\(\) override \{ return NEW FFmpegVideoPlayer; \}\s*^\s*#else\s*^\s*virtual VideoPlayerInterface \*createVideoPlayer\(\) override \{ return NEW BinkVideoPlayer; \}\s*^\s*#endif') {
        throw "FFmpeg/Bink video backend selection contract is missing in $relativePath"
    }
}

$deviceCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/CMakeLists.txt') -Raw
if ($deviceCMake -notmatch '(?ms)^if\(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG\)\s*^    list\(APPEND GAMEENGINEDEVICE_SRC\s*^        Include/VideoDevice/Bink/BinkVideoPlayer\.h\s*^        Source/VideoDevice/Bink/BinkVideoPlayer\.cpp\s*^    \)\s*^endif\(\)') {
    throw 'Bink source ownership is not conditional on the FFmpeg backend option.'
}
Assert-TokenCount $deviceCMake 'BinkVideoPlayer\.h' 1 'Bink header source ownership is ambiguous.'
Assert-TokenCount $deviceCMake 'BinkVideoPlayer\.cpp' 1 'Bink implementation source ownership is ambiguous.'

if ($deviceCMake -match 'install\(FILES \$\{RTS_FFMPEG_RUNTIME_DLLS\}') {
    throw 'FFmpeg runtime installation must be owned by each title effective-prefix boundary.'
}

foreach ($title in @(
    @{ Path = 'Generals/CMakeLists.txt'; Prefix = '_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS' },
    @{ Path = 'GeneralsMD/CMakeLists.txt'; Prefix = '_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR' }
)) {
    $titleCMake = Get-Content -LiteralPath (Join-Path $SourceRoot $title.Path) -Raw
    Assert-EffectiveRuntimeInstall $titleCMake $title.Prefix `
        "FFmpeg runtime DLLs are not installed through the effective prefix in $($title.Path)"
}

$runtimeCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'cmake/legacy-product-runtime.cmake') -Raw
if ($runtimeCMake -notmatch '(?ms)^    if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^        target_link_libraries\(rts_legacy_product_runtime INTERFACE binkstub\)\s*^    endif\(\)') {
    throw 'Bink link ownership is not conditional on the FFmpeg backend option.'
}
Assert-ExactConditionalBlock $runtimeCMake '    if(NOT RTS_BUILD_OPTION_FFMPEG)' '        target_link_libraries(rts_legacy_product_runtime INTERFACE binkstub)' '    endif()' 'Bink link conditional block is not exact.'
Assert-TokenCount $runtimeCMake 'binkstub' 1 'Bink runtime link ownership is ambiguous.'

$rootCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt') -Raw
$configIndex = $rootCMake.IndexOf('include(cmake/config.cmake)', [System.StringComparison]::Ordinal)
$binkIndex = $rootCMake.IndexOf('include(cmake/bink.cmake)', [System.StringComparison]::Ordinal)
if ($configIndex -lt 0 -or $binkIndex -lt 0 -or $configIndex -ge $binkIndex) {
    throw 'Build configuration must be available before selecting video backend dependencies.'
}
if ($rootCMake -notmatch '(?ms)^    if\(\$\{CMAKE_SIZEOF_VOID_P\} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG\)\s*^        include\(cmake/bink\.cmake\)\s*^    endif\(\)') {
    throw 'Bink dependency fetch is not conditional on the FFmpeg backend option.'
}
Assert-ExactConditionalBlock $rootCMake '    if(${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)' '        include(cmake/bink.cmake)' '    endif()' 'Bink fetch conditional block is not exact.'
Assert-TokenCount $rootCMake 'include\(cmake/bink\.cmake\)' 1 'Bink dependency fetch ownership is ambiguous.'

$runtimeTestsCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'GeneralsMD/Code/Tools/RuntimeRegressionTests/CMakeLists.txt') -Raw
if ($runtimeTestsCMake -notmatch '(?ms)^if\(CMAKE_SIZEOF_VOID_P EQUAL 4\).*?^\s*if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^\s*target_link_libraries\(z_runtime_regression_tests PRIVATE binkstub\)\s*^\s*endif\(\)') {
    throw 'Zero Hour runtime regression tests link Bink outside the fallback backend.'
}
Assert-TokenCount $runtimeTestsCMake 'binkstub' 1 'Zero Hour runtime regression test link ownership is ambiguous.'

$audioContractFiles = @(
    'Core/GameEngine/Include/Common/GameAudio.h',
    'Core/GameEngineDevice/Include/MilesAudioDevice/MilesAudioManager.h',
    'Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp',
    'Core/GameEngineDevice/Include/VideoDevice/Bink/BinkVideoPlayer.h',
    'Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp',
    'Core/GameEngineDevice/Include/VideoDevice/FFmpeg/FFmpegVideoPlayer.h',
    'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp'
)

foreach ($relativePath in $audioContractFiles) {
    $content = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    if ($content -match 'getHandleForBink|releaseHandleForBink|initializeBinkWithMiles') {
        throw "Legacy Bink-specific audio contract remains in $relativePath"
    }
}

$gameAudio = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngine/Include/Common/GameAudio.h') -Raw
if ($gameAudio -notmatch 'class LegacyVideoAudioInterface') {
    throw 'AudioManager is missing the explicit legacy video audio capability interface.'
}
$gameAudioBody = Get-CppClassBody $gameAudio 'AudioManager'
Assert-LegacyVideoAccessor $gameAudioBody 'nullptr' `
    -FailureMessage 'AudioManager is missing the checked legacy video audio capability accessor.'
if ($gameAudio -match 'getHandleForVideo') {
    throw 'AudioManager retains a raw video audio acquisition contract.'
}
if ($gameAudio -match 'releaseHandleForVideo') {
    throw 'AudioManager retains a raw video audio release contract.'
}

$milesAudio = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/MilesAudioDevice/MilesAudioManager.h') -Raw
if ($milesAudio -notmatch 'virtual void \*getLegacyVideoDirectSoundHandle\(\)' -or $milesAudio -notmatch 'virtual void releaseLegacyVideoAudioHandle\(\)') {
    throw 'Miles fallback does not own the legacy video audio handle contract.'
}
if ($milesAudio -notmatch 'void\* getLegacyVideoDirectSoundHandle\(\) override' -or $milesAudio -notmatch 'void releaseLegacyVideoAudioHandle\(\) override') {
    throw 'Miles headless audio manager does not override the legacy video audio contract.'
}
$milesAudioBody = Get-CppClassBody $milesAudio 'MilesAudioManager'
Assert-LegacyVideoAccessor $milesAudioBody 'this' -RequireOverride `
    -FailureMessage 'Miles fallback does not publish its checked legacy video audio capability.'

$binkPlayer = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp') -Raw
Assert-NoLegacyVideoDynamicCast $binkPlayer
Assert-LegacyVideoAccessorCount $binkPlayer 3

$ffmpegPlayer = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp') -Raw
if ($ffmpegPlayer -match 'RTS_HAS_OPENAL|RTS_USE_OPENAL|OpenALAudioStream|getHandleForVideo|releaseHandleForVideo') {
    throw 'FFmpeg player retains an unowned legacy or OpenAL audio path.'
}

foreach ($relativePath in @(
    'Generals/Code/GameEngine/Source/GameClient/GameClient.cpp',
    'GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp'
)) {
    $gameClient = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    Assert-IntroVideoService $gameClient `
        "Intro movie audio is not serviced before display update in $relativePath"
}

Write-Output 'Video backend selection audit passed.'
