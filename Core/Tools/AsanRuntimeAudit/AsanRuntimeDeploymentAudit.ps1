param(
    [Parameter(Mandatory = $true)]
    [string] $BuildRoot,
    [Parameter(Mandatory = $true)]
    [string] $RuntimeDll,
    [switch] $ExpectGenerals,
    [switch] $ExpectZeroHour
)

$ErrorActionPreference = 'Stop'

function Get-CanonicalPath([string] $Path, [string] $Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description is required."
    }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}

$resolvedBuildRoot = Get-CanonicalPath $BuildRoot 'BuildRoot'
$resolvedRuntimeDll = Get-CanonicalPath $RuntimeDll 'RuntimeDll'
$runtimeName = 'clang_rt.asan_dynamic-x86_64.dll'

if ([IO.Path]::GetFileName($resolvedRuntimeDll) -cne $runtimeName) {
    throw "RuntimeDll must name $runtimeName."
}
if (-not (Test-Path -LiteralPath $resolvedRuntimeDll -PathType Leaf)) {
    throw "Resolved ASan runtime is missing: $resolvedRuntimeDll"
}

$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $sourceHash = [BitConverter]::ToString(
        $sha256.ComputeHash([IO.File]::ReadAllBytes($resolvedRuntimeDll))).Replace('-', '')
}
finally {
    $sha256.Dispose()
}

$runtimeRoots = @('Core', 'Generals', 'GeneralsMD') | ForEach-Object {
    Join-Path $resolvedBuildRoot $_
} | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
$builtExecutables = @(foreach ($runtimeRoot in $runtimeRoots) {
    Get-ChildItem -LiteralPath $runtimeRoot -Recurse -File -Filter '*.exe' -ErrorAction SilentlyContinue
})
if ($builtExecutables.Count -eq 0) {
    throw "No ASan executable was found below '$resolvedBuildRoot'."
}

foreach ($executable in $builtExecutables) {
    $stagedRuntime = Join-Path $executable.DirectoryName $runtimeName
    if (-not (Test-Path -LiteralPath $stagedRuntime -PathType Leaf)) {
        throw "ASan runtime is not staged beside $($executable.FullName)."
    }

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $stagedHash = [BitConverter]::ToString(
            $sha256.ComputeHash([IO.File]::ReadAllBytes($stagedRuntime))).Replace('-', '')
    }
    finally {
        $sha256.Dispose()
    }
    if ($sourceHash -cne $stagedHash) {
        throw "Staged ASan runtime beside $($executable.FullName) differs from the resolved compiler runtime."
    }
}

$installScripts = @(Get-ChildItem -LiteralPath $resolvedBuildRoot -Recurse -File -Filter 'cmake_install.cmake' -ErrorAction SilentlyContinue)
foreach ($expectation in @(
    @{ Title = 'Generals'; Required = $ExpectGenerals.IsPresent },
    @{ Title = 'GeneralsMD'; Required = $ExpectZeroHour.IsPresent }
)) {
    $title = $expectation.Title
    $installScript = $installScripts | Where-Object { $_.DirectoryName -eq (Join-Path $resolvedBuildRoot $title) } | Select-Object -First 1
    if ($null -eq $installScript) {
        if ($expectation.Required) {
            throw "$title install script is missing from the ASan product graph."
        }
        continue
    }
    $installText = Get-Content -LiteralPath $installScript.FullName -Raw
    if ($installText -notmatch [regex]::Escape($runtimeName)) {
        throw "$title install script does not stage $runtimeName."
    }
}

Write-Output "ASan runtime deployment audit passed for $($builtExecutables.Count) executable(s)."
