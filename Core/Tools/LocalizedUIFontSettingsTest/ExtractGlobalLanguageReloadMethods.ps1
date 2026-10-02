<#
Copyright 2026 TheSuperHackers
SPDX-License-Identifier: GPL-3.0-or-later
#>

param(
	[Parameter(Mandatory = $true)][string]$SourceRoot,
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[switch]$AllowMissingReset
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

function Get-UniqueCppBlock([string]$Text, [string]$Signature, [switch]$Optional)
{
	$start = $Text.IndexOf($Signature, [System.StringComparison]::Ordinal)
	if ($start -lt 0)
	{
		if ($Optional) { return $null }
		throw "Missing C++ source signature '$Signature'."
	}
	if ($Text.IndexOf($Signature, $start + $Signature.Length, [System.StringComparison]::Ordinal) -ge 0)
		{ throw "C++ source signature is ambiguous: '$Signature'." }
	return Get-CppBlockAt $Text $start
}

function Get-UniqueSourceLine([string]$Text, [string]$Pattern)
{
	$matches = [regex]::Matches($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
	if ($matches.Count -ne 1) { throw "Expected one source line matching '$Pattern'; found $($matches.Count)." }
	return $matches[0].Value
}

function Normalize-SourceNewlines([string]$Text)
{
	$lineFeed = [string][char]10
	$carriageReturn = [string][char]13
	return $Text.Replace($carriageReturn + $lineFeed, $lineFeed)
}

function Assert-SameOrderedItems([string[]]$Expected, [string[]]$Observed, [string]$Description)
{
	if ($Expected.Count -ne $Observed.Count -or
		[string]::Join('|', $Expected) -cne [string]::Join('|', $Observed))
	{
		throw "$Description changed; update the source-linked fixture before relying on it."
	}
}

$globalLanguagePath = Join-Path $SourceRoot 'Core/GameEngine/Source/GameClient/GlobalLanguage.cpp'
$globalLanguageHeader = Join-Path $SourceRoot 'Core/GameEngine/Include/GameClient/GlobalLanguage.h'
if (!(Test-Path -LiteralPath $globalLanguagePath -PathType Leaf)) { throw "Missing source file: $globalLanguagePath" }
if (!(Test-Path -LiteralPath $globalLanguageHeader -PathType Leaf)) { throw "Missing source header: $globalLanguageHeader" }

# Normalize text in memory so exact source-line checks work with LF and CRLF.
# Hashes below remain over the original on-disk bytes.
$sourceText = Normalize-SourceNewlines ([System.IO.File]::ReadAllText($globalLanguagePath))
$headerText = Normalize-SourceNewlines ([System.IO.File]::ReadAllText($globalLanguageHeader))
$headerHash = (Get-FileHash -LiteralPath $globalLanguageHeader -Algorithm SHA256).Hash
$sourceHash = (Get-FileHash -LiteralPath $globalLanguagePath -Algorithm SHA256).Hash
$globalPointer = Get-UniqueSourceLine $sourceText '^GlobalLanguage \*TheGlobalLanguageData = nullptr;$'
$methodList = New-Object object[] 13
$methodList[0] = (Get-UniqueCppBlock $sourceText 'static const LookupListRec ResolutionFontSizeMethodNames[]') + ';'
$methodList[1] = (Get-UniqueCppBlock $sourceText 'static const FieldParse TheGlobalLanguageDataFieldParseTable[]') + ';'
$methodList[2] = Get-UniqueCppBlock $sourceText 'void INI::parseLanguageDefinition( INI *ini )'
$methodList[3] = Get-UniqueCppBlock $sourceText 'static void resetLanguageDefinitionDefaults(GlobalLanguage *globalLanguage)' -Optional:$AllowMissingReset
$methodList[4] = Get-UniqueCppBlock $sourceText 'GlobalLanguage::GlobalLanguage()'
$methodList[5] = Get-UniqueCppBlock $sourceText 'GlobalLanguage::~GlobalLanguage()'
$methodList[6] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::reset()'
$methodList[7] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::init()'
$methodList[8] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::onResolutionChanged()'
$methodList[9] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::parseFontDesc('
$methodList[10] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::parseFontFileName('
$methodList[11] = Get-UniqueCppBlock $sourceText 'void GlobalLanguage::parseCustomDefinition()'
$methodList[12] = Get-UniqueCppBlock $sourceText 'FontDesc::FontDesc()'

$resetMethod = $methodList[3]
if (!$AllowMissingReset -and !$resetMethod) { throw 'Candidate source must provide the shared language-default reset helper.' }
$onResolutionChanged = $methodList[8]
$initMethod = $methodList[7]

$expectedFontPairs = @(
	'CopyrightFont:m_copyrightFont',
	'MessageFont:m_messageFont',
	'MilitaryCaptionTitleFont:m_militaryCaptionTitleFont',
	'MilitaryCaptionFont:m_militaryCaptionFont',
	'SuperweaponCountdownNormalFont:m_superweaponCountdownNormalFont',
	'SuperweaponCountdownReadyFont:m_superweaponCountdownReadyFont',
	'NamedTimerCountdownNormalFont:m_namedTimerCountdownNormalFont',
	'NamedTimerCountdownReadyFont:m_namedTimerCountdownReadyFont',
	'DrawableCaptionFont:m_drawableCaptionFont',
	'DefaultWindowFont:m_defaultWindowFont',
	'DefaultDisplayStringFont:m_defaultDisplayStringFont',
	'TooltipFontName:m_tooltipFontName',
	'NativeDebugDisplay:m_nativeDebugDisplay',
	'DrawGroupInfoFont:m_drawGroupInfoFont',
	'CreditsTitleFont:m_creditsTitleFont',
	'CreditsMinorTitleFont:m_creditsPositionFont',
	'CreditsNormalFont:m_creditsNormalFont')
$tableFontPairs = @([regex]::Matches($methodList[1],
	'\{\s*"(?<token>[^"]+)"\s*,\s*GlobalLanguage::parseFontDesc\s*,\s*nullptr\s*,\s*offsetof\(\s*GlobalLanguage\s*,\s*(?<member>m_[A-Za-z0-9_]+)\s*\)\s*\}') |
	ForEach-Object { "$($_.Groups['token'].Value):$($_.Groups['member'].Value)" })
Assert-SameOrderedItems $expectedFontPairs $tableFontPairs 'The 17 Language FontDesc parse fields'

$mockPath = Join-Path $PSScriptRoot 'GlobalLanguageReloadMockTypes.inl'
$assertionPath = Join-Path $PSScriptRoot 'GlobalLanguageReloadAssertions.inl'
$mockText = [System.IO.File]::ReadAllText($mockPath)
$assertionText = [System.IO.File]::ReadAllText($assertionPath)
$mockClass = Get-UniqueCppBlock $mockText 'class GlobalLanguage : public SubsystemInterface'
$fixtureFontPairs = @([regex]::Matches($assertionText,
	'\{\s*"(?<token>[^"]+)"\s*,\s*&GlobalLanguage::(?<member>m_[A-Za-z0-9_]+)\s*\}') |
	ForEach-Object { "$($_.Groups['token'].Value):$($_.Groups['member'].Value)" })
Assert-SameOrderedItems $expectedFontPairs $fixtureFontPairs 'The fixture FontDesc expectation list'
$requiredMembers = @($expectedFontPairs | ForEach-Object { $_.Substring($_.IndexOf(':') + 1) }) + @(
	'm_unicodeFontName', 'm_unicodeFontFileName', 'm_useHardWrap', 'm_militaryCaptionSpeed',
	'm_militaryCaptionDelayMS', 'm_resolutionFontSizeAdjustment', 'm_userResolutionFontSizeAdjustment',
	'm_resolutionFontSizeMethod', 'm_localFonts')
foreach ($member in $requiredMembers)
{
	if ($headerText -notmatch "\b$member\s*;" -or $mockClass -notmatch "\b$member\s*;")
		{ throw "Production/mock GlobalLanguage field '$member' is missing or renamed." }
}

if ($resetMethod)
{
	$resetFontMembers = @([regex]::Matches($resetMethod, 'globalLanguage->(?<member>m_[A-Za-z0-9_]+)\s*=\s*defaultFont\s*;') |
		ForEach-Object { $_.Groups['member'].Value })
	$expectedFontMembers = @($expectedFontPairs | ForEach-Object { $_.Substring($_.IndexOf(':') + 1) })
	Assert-SameOrderedItems $expectedFontMembers $resetFontMembers 'The reset helper 17-font default assignments'
	foreach ($member in @('m_unicodeFontName', 'm_unicodeFontFileName', 'm_militaryCaptionSpeed',
		'm_useHardWrap', 'm_resolutionFontSizeAdjustment', 'm_resolutionFontSizeMethod', 'm_militaryCaptionDelayMS'))
	{
		if ($resetMethod -notmatch "globalLanguage->$member(?:\.clear\(\)|\s*=)")
			{ throw "The reset helper no longer restores parsed scalar/string '$member'." }
	}
}

if ($resetMethod)
{
	if ($onResolutionChanged.IndexOf('RemoveFontResource', [System.StringComparison]::Ordinal) -lt 0 -or
		$onResolutionChanged.IndexOf('m_localFonts.clear()', [System.StringComparison]::Ordinal) -lt 0 -or
		$onResolutionChanged.IndexOf('resetLanguageDefinitionDefaults(this)', [System.StringComparison]::Ordinal) -lt 0 -or
		$onResolutionChanged.IndexOf('init();', [System.StringComparison]::Ordinal) -lt 0 -or
		$onResolutionChanged.IndexOf('parseCustomDefinition();', [System.StringComparison]::Ordinal) -lt 0 -or
		!($onResolutionChanged.IndexOf('RemoveFontResource', [System.StringComparison]::Ordinal) -lt
		  $onResolutionChanged.IndexOf('m_localFonts.clear()', [System.StringComparison]::Ordinal) -and
		  $onResolutionChanged.IndexOf('m_localFonts.clear()', [System.StringComparison]::Ordinal) -lt
		  $onResolutionChanged.IndexOf('resetLanguageDefinitionDefaults(this)', [System.StringComparison]::Ordinal) -and
		  $onResolutionChanged.IndexOf('resetLanguageDefinitionDefaults(this)', [System.StringComparison]::Ordinal) -lt
		  $onResolutionChanged.IndexOf('init();', [System.StringComparison]::Ordinal) -and
		  $onResolutionChanged.IndexOf('init();', [System.StringComparison]::Ordinal) -lt
		  $onResolutionChanged.IndexOf('parseCustomDefinition();', [System.StringComparison]::Ordinal)))
		{ throw 'Reload ordering changed; expected unregister/clear, defaults, INI/init, then addon override.' }
}
if (!($initMethod.IndexOf('loadFileDirectory', [System.StringComparison]::Ordinal) -lt
	  $initMethod.IndexOf('AddFontResource', [System.StringComparison]::Ordinal) -and
	  $initMethod.IndexOf('AddFontResource', [System.StringComparison]::Ordinal) -lt
	  $initMethod.IndexOf('getResolutionFontAdjustment', [System.StringComparison]::Ordinal)))
	{ throw 'Init ordering changed; expected parse, local font registration, then saved user preference.' }

$relativeFontHeader = 'Code/GameEngine/Include/GameClient/FontDesc.h'
$fontHeaderShape = '(?s)AsciiString\s+name\s*;.*?Int\s+size\s*;.*?Bool\s+bold\s*;'
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
foreach ($title in @('Generals', 'GeneralsMD'))
{
	$fontHeaderPath = Join-Path (Join-Path $SourceRoot $title) $relativeFontHeader
	if (!(Test-Path -LiteralPath $fontHeaderPath -PathType Leaf)) { throw "Missing $title FontDesc header: $fontHeaderPath" }
	$fontText = [System.IO.File]::ReadAllText($fontHeaderPath)
	if ($fontText -notmatch $fontHeaderShape) { throw "$title FontDesc field layout no longer matches this fixture." }
	$fontHash = (Get-FileHash -LiteralPath $fontHeaderPath -Algorithm SHA256).Hash
	$parts = @($globalPointer) + @($methodList | Where-Object { $_ })
	$suffix = if ($title -eq 'Generals') { 'Generals' } else { 'GeneralsMD' }
	$outPath = Join-Path $OutputDirectory ("GlobalLanguageReload_{0}.inl" -f $suffix)
	$header = "// Extracted from shared GlobalLanguage.cpp SHA256 $sourceHash; shared GlobalLanguage.h SHA256 $headerHash; $title FontDesc.h SHA256 $fontHash.`r`n"
	[System.IO.File]::WriteAllText($outPath, $header + [string]::Join("`r`n`r`n", $parts))
}
