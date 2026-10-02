<#
Copyright 2026 TheSuperHackers
SPDX-License-Identifier: GPL-3.0-or-later
#>

param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$BaselineRevision
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

function Get-BaselineSource([string]$RelativePath)
{
    $result = & git -C $SourceRoot show ("{0}:{1}" -f $BaselineRevision, $RelativePath)
    if ($LASTEXITCODE -ne 0) { throw "Could not read baseline source $RelativePath at $BaselineRevision." }
    return [string]::Join("`n", $result)
}

function Get-OriginalFontOverride([string]$Text, [string]$Path)
{
    $marker = $Text.IndexOf('override INI values with language localized values:',
        [System.StringComparison]::Ordinal)
    if ($marker -lt 0) { throw "Missing original localized-font block marker in $Path." }
    $start = $Text.IndexOf('if (TheGlobalLanguageData)', $marker,
        [System.StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing original language-data block in $Path." }
    return Get-CppBlockAt $Text $start
}

$titleRoots = @('Generals', 'GeneralsMD')
$fragments = @{}
$sourceHashes = @{}
foreach ($title in $titleRoots)
{
    $relativePath = "$title/Code/GameEngine/Source/GameClient/InGameUI.cpp"
    $baselineSource = Get-BaselineSource $relativePath
    $originalOverride = Get-OriginalFontOverride $baselineSource $relativePath
    $fragments[$title] = "void InGameUI::applyOriginalLocalizedFontOverride()`r`n{`r`n$originalOverride`r`n}"
    $baselineBytes = [System.Text.Encoding]::UTF8.GetBytes($baselineSource)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $sourceHashes[$title] = [System.BitConverter]::ToString($sha256.ComputeHash($baselineBytes)).Replace('-', '')
    $sha256.Dispose()
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
foreach ($title in $titleRoots)
{
    $suffix = if ($title -eq 'Generals') { 'Generals' } else { 'GeneralsMD' }
    $outPath = Join-Path $OutputDirectory ("LocalizedUIFontStaleBaseline_{0}.inl" -f $suffix)
    $header = "// Historical stale-baseline qualification extracted from $BaselineRevision for $title; baseline InGameUI SHA256 $($sourceHashes[$title]).`r`n"
    [System.IO.File]::WriteAllText($outPath, $header + $fragments[$title])
}
