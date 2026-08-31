param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Remove-CppCommentsAndLiterals {
    param([Parameter(Mandatory = $true)][string]$Content)

    $pattern = '(?s)R"(?<rawDelimiter>[^\s()\\]{0,16})\(.*?\)\k<rawDelimiter>"|' +
        '/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace($Content, $pattern, ' ')
}

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Content)

    $pattern = '(?s)R"(?<rawDelimiter>[^\s()\\]{0,16})\(.*?\)\k<rawDelimiter>"|' +
        '/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace($Content, $pattern, {
        param($match)
        if ($match.Value.StartsWith('/*') -or $match.Value.StartsWith('//')) {
            return ' '
        }
        return $match.Value
    })
}

function Assert-RegistrySourceContract {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Context,
        [int]$ExpectedRetailReads = 2,
        [int]$ExpectedRetailWrites = 2,
        [int]$ExpectedRetailDeletes = 0,
        [int]$ExpectedNativeReads = 0,
        [int]$ExpectedNativeWrites = 0
    )

    $commentsRemoved = Remove-CppComments -Content $Content
    if ($commentsRemoved -notmatch '(?m)^#include\s+"Common/RegistryView\.h"\s*$') {
        throw "$Context does not include the retail registry-view boundary."
    }
    $code = Remove-CppCommentsAndLiterals -Content $Content
    if ($code -match '\bReg(?:Open|Create)Key(?:Ex)?(?:A|W)?\b' -or
        $code -match '\bRegDeleteKey(?:Ex)?(?:A|W)?\b') {
        throw "$Context bypasses the retail registry-view boundary."
    }
    if ([regex]::Matches($code, '\bOpenRetailRegistryKey\s*\(').Count -ne $ExpectedRetailReads) {
        throw "$Context must contain exactly $ExpectedRetailReads retail registry reads."
    }
    if ([regex]::Matches($code, '\bCreateRetailRegistryKey\s*\(').Count -ne $ExpectedRetailWrites) {
        throw "$Context must contain exactly $ExpectedRetailWrites retail registry writes."
    }
    if ([regex]::Matches($code, '\bDeleteRetailRegistryKey\s*\(').Count -ne $ExpectedRetailDeletes) {
        throw "$Context must contain exactly $ExpectedRetailDeletes retail registry deletes."
    }
    if ([regex]::Matches($code, '\bOpenNativeRegistryKey\s*\(').Count -ne $ExpectedNativeReads) {
        throw "$Context must contain exactly $ExpectedNativeReads native registry reads."
    }
    if ([regex]::Matches($code, '\bCreateNativeRegistryKey\s*\(').Count -ne $ExpectedNativeWrites) {
        throw "$Context must contain exactly $ExpectedNativeWrites native registry writes."
    }
}

function Assert-RegistryHeaderContract {
    param([Parameter(Mandatory = $true)][string]$Content)

    $code = Remove-CppCommentsAndLiterals -Content $Content
    foreach ($token in @(
        'GetRetailRegistryAccessMask',
        'KEY_WOW64_32KEY',
        'OpenRetailRegistryKey',
        'CreateRetailRegistryKey',
        'DeleteRetailRegistryKey',
        'GetNativeRegistryAccessMask',
        'KEY_WOW64_64KEY',
        'OpenNativeRegistryKey',
        'CreateNativeRegistryKey'
    )) {
        if ($code -notmatch [regex]::Escape($token)) {
            throw "RegistryView.h is missing $token."
        }
    }
    if ($code -notmatch '(?s)inline\s+REGSAM\s+GetRetailRegistryAccessMask.*?#if\s+defined\(_WIN64\).*?KEY_WOW64_32KEY.*?#else.*?return\s+access\s*;.*?#endif') {
        throw 'RegistryView.h does not preserve Win32 behavior while selecting the retail view on x64.'
    }
    if ($code -notmatch '(?s)inline\s+REGSAM\s+GetNativeRegistryAccessMask.*?#if\s+defined\(_WIN64\).*?KEY_WOW64_64KEY.*?#else.*?return\s+access\s*;.*?#endif') {
        throw 'RegistryView.h does not preserve Win32 behavior while selecting the native view on x64.'
    }
    if ($code -notmatch '(?s)inline\s+LONG\s+DeleteRetailRegistryKey\s*\([^)]*\)\s*\{\s*#if\s+defined\(_WIN64\).*?RegDeleteKeyExA\s*\([^;]*KEY_WOW64_32KEY[^;]*\).*?#else.*?RegDeleteKeyA\s*\(.*?#endif\s*\}') {
        throw 'RegistryView.h does not preserve Win32 deletion while selecting the retail view on x64.'
    }
    foreach ($wrapper in @(
        [pscustomobject]@{ Name = 'OpenRetailRegistryKey'; Mask = 'GetRetailRegistryAccessMask' },
        [pscustomobject]@{ Name = 'CreateRetailRegistryKey'; Mask = 'GetRetailRegistryAccessMask' },
        [pscustomobject]@{ Name = 'OpenNativeRegistryKey'; Mask = 'GetNativeRegistryAccessMask' },
        [pscustomobject]@{ Name = 'CreateNativeRegistryKey'; Mask = 'GetNativeRegistryAccessMask' }
    )) {
        $pattern = '(?s)\b' + [regex]::Escape($wrapper.Name) +
            '\s*\([^)]*\)\s*\{[^{}]*\b' + [regex]::Escape($wrapper.Mask) +
            '\s*\(\s*access\s*\)[^{}]*\}'
        if ($code -notmatch $pattern) {
            throw "RegistryView.h does not bind $($wrapper.Name) to $($wrapper.Mask)."
        }
    }
    if ([regex]::Matches($code, '\bRegOpenKeyExA\s*\(').Count -ne 2 -or
        [regex]::Matches($code, '\bRegCreateKeyExA\s*\(').Count -ne 2 -or
        [regex]::Matches($code, '\bRegDeleteKeyExA\s*\(').Count -ne 1 -or
        [regex]::Matches($code, '\bRegDeleteKeyA\s*\(').Count -ne 1) {
        throw 'RegistryView.h must contain exactly the six audited WinAPI boundary calls.'
    }
}

if ($SelfTest) {
    $valid = @'
#include "Common/RegistryView.h"
OpenRetailRegistryKey(root, path, 0, KEY_READ, &handle);
OpenRetailRegistryKey(root, path, 0, KEY_READ, &handle);
CreateRetailRegistryKey(root, path, 0, name, 0, KEY_WRITE, 0, &handle, 0);
CreateRetailRegistryKey(root, path, 0, name, 0, KEY_WRITE, 0, &handle, 0);
'@
    Assert-RegistrySourceContract -Content $valid -Context 'valid fixture'

    foreach ($api in @(
        'RegOpenKey', 'RegOpenKeyA', 'RegOpenKeyW',
        'RegOpenKeyEx', 'RegOpenKeyExA', 'RegOpenKeyExW',
        'RegCreateKey', 'RegCreateKeyA', 'RegCreateKeyW',
        'RegCreateKeyEx', 'RegCreateKeyExA', 'RegCreateKeyExW',
        'RegDeleteKey', 'RegDeleteKeyA', 'RegDeleteKeyW',
        'RegDeleteKeyEx', 'RegDeleteKeyExA', 'RegDeleteKeyExW'
    )) {
        $caughtExplicitBypass = $false
        try {
            Assert-RegistrySourceContract -Content ($valid + "`nauto bypass = &$api;") -Context "$api bypass fixture"
        }
        catch { $caughtExplicitBypass = $true }
        if (-not $caughtExplicitBypass) { throw "Self-test accepted a direct $api bypass." }
    }

    $decoy = @'
// OpenRetailRegistryKey(root, path, 0, KEY_READ, &handle);
// RegOpenKeyExW(root, path, 0, KEY_READ, &handle);
const char *ignored = "CreateRetailRegistryKey and RegCreateKeyExA are not calls here";
const char *url = "https://example.invalid/a/*not-a-comment*/";
const char *raw = R"marker(RegOpenKeyExA and // CreateRetailRegistryKey)marker";
'@
    Assert-RegistrySourceContract -Content ($valid + $decoy) -Context 'trivia fixture'

    $caughtCommentedInclude = $false
    try {
        Assert-RegistrySourceContract -Content ($valid -replace '#include "Common/RegistryView.h"', '/* #include "Common/RegistryView.h" */') -Context 'commented include fixture'
    }
    catch { $caughtCommentedInclude = $true }
    if (-not $caughtCommentedInclude) { throw 'Self-test accepted an include hidden inside a comment.' }

    Write-Output 'Registry view contract audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless -SelfTest is used.'
}

$sourceFull = [IO.Path]::GetFullPath($SourceRoot)
$headerPath = Join-Path $sourceFull 'Core/Libraries/Include/Common/RegistryView.h'
if (-not (Test-Path -LiteralPath $headerPath -PathType Leaf)) {
    throw "Retail registry-view boundary is missing: $headerPath"
}
Assert-RegistryHeaderContract -Content (Get-Content -LiteralPath $headerPath -Raw)

foreach ($contract in @(
    [pscustomobject]@{
        Path = 'Core/Libraries/Source/WWVegas/WWLib/registry.cpp'
        RetailReads = 6; RetailWrites = 1; RetailDeletes = 2
        NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Core/Libraries/Source/WWVegas/WWDownload/FTP.cpp'
        RetailReads = 1; RetailWrites = 0; RetailDeletes = 0
        NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Core/GameEngine/Source/Common/Audio/urllaunch.cpp'
        RetailReads = 0; RetailWrites = 0; RetailDeletes = 0
        NativeReads = 3; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Core/Libraries/Source/WWVegas/WWDownload/registry.cpp'
        RetailReads = 2; RetailWrites = 2; RetailDeletes = 0; NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Generals/Code/GameEngine/Source/Common/System/registry.cpp'
        RetailReads = 2; RetailWrites = 2; RetailDeletes = 0; NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'GeneralsMD/Code/GameEngine/Source/Common/System/registry.cpp'
        RetailReads = 2; RetailWrites = 2; RetailDeletes = 0; NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Core/Tools/Launcher/findpatch.cpp'
        RetailReads = 1; RetailWrites = 0; RetailDeletes = 0; NativeReads = 0; NativeWrites = 0
    },
    [pscustomobject]@{
        Path = 'Core/Tools/Launcher/patch.cpp'
        RetailReads = 1; RetailWrites = 0; RetailDeletes = 0; NativeReads = 0; NativeWrites = 1
    },
    [pscustomobject]@{
        Path = 'Core/Tools/Launcher/DatGen/DatGen.cpp'
        RetailReads = 4; RetailWrites = 0; RetailDeletes = 0; NativeReads = 1; NativeWrites = 0
    }
)) {
    $path = Join-Path $sourceFull $contract.Path
    Assert-RegistrySourceContract -Content (Get-Content -LiteralPath $path -Raw) `
        -Context $contract.Path `
        -ExpectedRetailReads $contract.RetailReads `
        -ExpectedRetailWrites $contract.RetailWrites `
        -ExpectedRetailDeletes $contract.RetailDeletes `
        -ExpectedNativeReads $contract.NativeReads `
        -ExpectedNativeWrites $contract.NativeWrites
}

Write-Output 'Retail registry view contract audit passed.'
