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

function Get-CppFunction([string]$Text, [string]$Pattern, [string]$Path)
{
    $matches = [System.Text.RegularExpressions.Regex]::Matches(
        $Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($matches.Count -ne 1)
    {
        throw "Expected exactly one function matching '$Pattern' in $Path; found $($matches.Count)."
    }
    return Get-CppBlockAt $Text $matches[0].Index
}

function Require-UniqueOrderedTokens([string]$Text, [string[]]$Tokens, [string]$Path)
{
    $previous = -1
    foreach ($token in $Tokens)
    {
        $position = $Text.IndexOf($token, [System.StringComparison]::Ordinal)
        if ($position -lt 0 -or
            $position -ne $Text.LastIndexOf($token, [System.StringComparison]::Ordinal) -or
            $position -le $previous)
        {
            throw "Missing, repeated, or reordered token '$token' in $Path."
        }
        $previous = $position
    }
}

function Require-OptionalUniqueTokenBetween([string]$Text, [string]$Token, [string]$AfterToken, [string]$BeforeToken, [string]$Path)
{
	$matches = [System.Text.RegularExpressions.Regex]::Matches(
		$Text, [System.Text.RegularExpressions.Regex]::Escape($Token),
		[System.Text.RegularExpressions.RegexOptions]::None)
	if ($matches.Count -gt 1)
	{
		throw "Optional token '$Token' is repeated in $Path."
	}
	if ($matches.Count -eq 1)
	{
		$after = $Text.IndexOf($AfterToken, [System.StringComparison]::Ordinal)
		$before = $Text.IndexOf($BeforeToken, [System.StringComparison]::Ordinal)
		$position = $matches[0].Index
		if ($after -lt 0 -or $before -lt 0 -or $position -le $after -or $position -ge $before)
		{
			throw "Optional token '$Token' is outside its required position in $Path."
		}
	}
}

function Get-ResolutionTail([string]$OptionsText, [string]$Path)
{
    $saveOptions = Get-CppFunction $OptionsText `
        '^[\t ]*static[\t ]+void[\t ]+saveOptions[\t ]*\([\t ]*\)[\t ]*\r?\n[\t ]*\{' $Path
    $startMarker = 'GadgetComboBoxGetSelectedPos( comboBoxResolution, &index );'
    $endMarker = '// MUST NEVER ADD ANOTHER OPTION HERE AT THE END !'
    $start = $saveOptions.IndexOf($startMarker, [System.StringComparison]::Ordinal)
    $end = $saveOptions.IndexOf($endMarker, [System.StringComparison]::Ordinal)
    if ($start -lt 0 -or $start -ne $saveOptions.LastIndexOf($startMarker, [System.StringComparison]::Ordinal) -or
        $end -lt 0 -or $end -ne $saveOptions.LastIndexOf($endMarker, [System.StringComparison]::Ordinal) -or
        $end -le $start)
    {
        throw "Resolution-tail markers are absent, duplicated, or reversed in $Path."
    }

    $tail = $saveOptions.Substring($start, $end - $start)
    Require-UniqueOrderedTokens $tail @(
        'GadgetComboBoxGetSelectedPos( comboBoxResolution, &index );',
        'TheDisplay->setDisplayMode(xres,yres,bitDepth,TheDisplay->getWindowed())',
        'TheGlobalLanguageData->onResolutionChanged();',
        'TheHeaderTemplateManager->onResolutionChanged();',
        'TheMouse->onResolutionChanged();',
        'TheShell->recreateWindowLayouts();',
        'TheInGameUI->recreateControlBar();',
        'TheInGameUI->refreshCustomUiResources();',
        'TheInGameUI->refreshLocalizedFontResources();'
    ) $Path
    return $tail
}

$titleRoots = @('Generals', 'GeneralsMD')
$relativeMainMenu = 'Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp'
$relativeOptionsMenu = 'Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp'
$fragments = @{}

foreach ($title in $titleRoots)
{
    $titleRoot = Join-Path $SourceRoot $title
    $mainMenuPath = Join-Path $titleRoot $relativeMainMenu
    $optionsMenuPath = Join-Path $titleRoot $relativeOptionsMenu
    $mainMenuText = [System.IO.File]::ReadAllText($mainMenuPath)
    $optionsMenuText = [System.IO.File]::ReadAllText($optionsMenuPath)

    $decline = Get-CppFunction $mainMenuText `
        '^[\t ]*void[\t ]+DeclineResolution[\t ]*\([\t ]*\)[\t ]*\r?\n[\t ]*\{' $mainMenuPath
    Require-UniqueOrderedTokens $decline @(
        'TheDisplay->setDisplayMode(oldDispSettings.xRes, oldDispSettings.yRes,',
        'TheGlobalLanguageData->onResolutionChanged();',
        'TheShell->recreateWindowLayouts();',
        'TheInGameUI->recreateControlBar();',
        'TheInGameUI->refreshLocalizedFontResources();'
    ) $mainMenuPath
    Require-OptionalUniqueTokenBetween $decline 'TheInGameUI->refreshCustomUiResources();' 'TheInGameUI->recreateControlBar();' 'TheInGameUI->refreshLocalizedFontResources();' $mainMenuPath

    $tail = Get-ResolutionTail $optionsMenuText $optionsMenuPath
    $wrapper = "void ApplyOptionsResolutionTail()`r`n{`r`n    Int index = -1;`r`n$tail`r`n}"
    $mainHash = (Get-FileHash -LiteralPath $mainMenuPath -Algorithm SHA256).Hash
    $optionsHash = (Get-FileHash -LiteralPath $optionsMenuPath -Algorithm SHA256).Hash
    $fragments[$title] = @{
        MainMenu = $decline
        OptionsTail = $wrapper
        MainHash = $mainHash
        OptionsHash = $optionsHash
    }
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
foreach ($title in $titleRoots)
{
    $suffix = if ($title -eq 'Generals') { 'Generals' } else { 'GeneralsMD' }
    $outPath = Join-Path $OutputDirectory ("ResolutionRefreshCallbacks_{0}.inl" -f $suffix)
    $parts = @(
        "// Extracted from $title MainMenu.cpp SHA256 $($fragments[$title].MainHash) and OptionsMenu.cpp SHA256 $($fragments[$title].OptionsHash).",
        $fragments[$title].MainMenu,
        $fragments[$title].OptionsTail
    )
    [System.IO.File]::WriteAllText($outPath, [string]::Join("`r`n`r`n", $parts))
}
