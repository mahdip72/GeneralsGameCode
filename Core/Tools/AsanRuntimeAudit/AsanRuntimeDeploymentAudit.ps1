param(
    [Parameter(Mandatory = $true)]
    [string] $BuildRoot,
    [Parameter(Mandatory = $true)]
    [string] $RuntimeDll
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
$configurationDirectories = @(foreach ($runtimeRoot in $runtimeRoots) {
    Get-ChildItem -LiteralPath $runtimeRoot -Directory -ErrorAction SilentlyContinue | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName $runtimeName) -PathType Leaf
    }
})
$builtExecutables = @($configurationDirectories | ForEach-Object {
    Get-ChildItem -LiteralPath $_.FullName -File -Filter '*.exe' -ErrorAction SilentlyContinue
})
if ($builtExecutables.Count -eq 0) {
    throw "No ASan executable was found in a staged runtime output directory below '$resolvedBuildRoot'."
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
foreach ($title in @('Generals', 'GeneralsMD')) {
    $installScript = $installScripts | Where-Object { $_.DirectoryName -eq (Join-Path $resolvedBuildRoot $title) } | Select-Object -First 1
    if ($null -eq $installScript) {
        continue
    }
    $installText = Get-Content -LiteralPath $installScript.FullName -Raw
    if ($installText -notmatch [regex]::Escape($runtimeName)) {
        throw "$title install script does not stage $runtimeName."
    }
}

Write-Output "ASan runtime deployment audit passed for $($builtExecutables.Count) executable(s)."
