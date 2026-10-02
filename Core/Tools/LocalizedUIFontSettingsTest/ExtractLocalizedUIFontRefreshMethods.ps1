<#
Copyright 2026 TheSuperHackers
SPDX-License-Identifier: GPL-3.0-or-later
#>

param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

function Get-CppBlockAt([string]$Text, [int]$Start)
{
    $open = $Text.IndexOf('{', $Start)
    if ($open -lt 0) { throw "No opening brace after source offset $Start." }

    $depth = 0
    $state = 'code'
    $escaped = $false
    for ($i = $open; $i -lt $Text.Length; ++$i)
    {
        $ch = $Text[$i]
        $next = if ($i + 1 -lt $Text.Length) { $Text[$i + 1] } else { [char]0 }

        if ($state -eq 'line-comment')
        {
            if ($ch -eq "`n") { $state = 'code' }
            continue
        }
        if ($state -eq 'block-comment')
        {
            if ($ch -eq '*' -and $next -eq '/') { $state = 'code'; ++$i }
            continue
        }
        if ($state -eq 'string' -or $state -eq 'char')
        {
            if ($escaped) { $escaped = $false; continue }
        if ($ch -eq [char]92) { $escaped = $true; continue }
            if (($state -eq 'string' -and $ch -eq '"') -or
                ($state -eq 'char' -and $ch -eq "'")) { $state = 'code' }
            continue
        }

        if ($ch -eq '/' -and $next -eq '/') { $state = 'line-comment'; ++$i; continue }
        if ($ch -eq '/' -and $next -eq '*') { $state = 'block-comment'; ++$i; continue }
        if ($ch -eq '"') { $state = 'string'; continue }
        if ($ch -eq "'") { $state = 'char'; continue }
        if ($ch -eq '{') { ++$depth; continue }
        if ($ch -eq '}')
        {
            --$depth
            if ($depth -eq 0) { return $Text.Substring($Start, $i - $Start + 1) }
        }
    }
    throw "Unbalanced C++ braces after source offset $Start."
}

function Get-CppMethod([string]$Path, [string]$Signature)
{
    $text = [System.IO.File]::ReadAllText($Path)
    $start = $text.IndexOf($Signature, [System.StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing method signature '$Signature' in $Path." }
    return Get-CppBlockAt $text $start
}

$pairedMethods = @(
    'void InGameUI::captureIniFontSettings()',
    'void InGameUI::applyLocalizedFontSetting(',
    'void InGameUI::applyLocalizedFontSettings()',
    'void InGameUI::refreshLocalizedFontResources()',
    'void SuperweaponInfo::setFont(',
    'void Drawable::refreshCaptionFont()'
)

$relativeInGameUI = 'Code/GameEngine/Source/GameClient/InGameUI.cpp'
$relativeDrawable = 'Code/GameEngine/Source/GameClient/Drawable.cpp'
$titleRoots = @('Generals', 'GeneralsMD')
$fragments = @{}
$sourceHashes = @{}

foreach ($title in $titleRoots)
{
    $root = Join-Path $SourceRoot $title
    $inGameUIPath = Join-Path $root $relativeInGameUI
    $drawablePath = Join-Path $root $relativeDrawable

    $parts = @()
    foreach ($signature in $pairedMethods)
    {
        $path = if ($signature.StartsWith('void Drawable::')) { $drawablePath } else { $inGameUIPath }
        $parts += Get-CppMethod $path $signature
    }

    $fragments[$title] = [string]::Join("`r`n`r`n", $parts)
    $sourceHashes[$title] = @{
        InGameUI = (Get-FileHash -LiteralPath $inGameUIPath -Algorithm SHA256).Hash
        Drawable = (Get-FileHash -LiteralPath $drawablePath -Algorithm SHA256).Hash
    }
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
foreach ($title in $titleRoots)
{
    $suffix = if ($title -eq 'Generals') { 'Generals' } else { 'GeneralsMD' }
    $outPath = Join-Path $OutputDirectory ("LocalizedUIFontRefresh_{0}.inl" -f $suffix)
    $hashes = $sourceHashes[$title]
    $header = "// Generated from $title production sources (InGameUI SHA256 $($hashes.InGameUI); Drawable SHA256 $($hashes.Drawable)).`r`n"
    [System.IO.File]::WriteAllText($outPath, $header + $fragments[$title])
}
