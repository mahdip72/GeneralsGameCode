param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

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
            if ($depth -eq 0) { return $Text.Substring($open + 1, $index - $open - 1) }
        }
    }
    return $null
}

function Remove-CppComments {
    param([string]$Text)
    return [regex]::Replace($Text, '(?s)/\*.*?\*/|//[^\r\n]*', '')
}

function Test-ParticleResetContract {
    param([string]$HeaderText, [string]$ImplementationText)

	$HeaderText = Remove-CppComments $HeaderText
	$ImplementationText = Remove-CppComments $ImplementationText
    if ($HeaderText -notmatch 'virtual\s+void\s+reset\s*\(\s*\)\s+override\s*;') { return $false }
    $body = Get-FunctionBody $ImplementationText 'void W3DParticleSystemManager::reset()'
    if ($null -eq $body) { return $false }
    $point = $body.IndexOf('m_pointGroup->Set_Texture(nullptr);', [StringComparison]::Ordinal)
    $streak = $body.IndexOf('m_streakLine->Set_Texture(nullptr);', [StringComparison]::Ordinal)
    $base = $body.IndexOf('ParticleSystemManager::reset();', [StringComparison]::Ordinal)
    return $point -ge 0 -and $streak -ge 0 -and $base -ge 0 -and
        $point -lt $base -and $streak -lt $base
}

function Test-DisplayContract {
    param(
        [string]$BaseManagerHeader,
        [string]$ManagerHeader,
        [string]$ManagerImplementation,
        [string]$DisplayImplementation,
        [string]$GameClientImplementation,
        [bool]$RequireSnow
    )

	$BaseManagerHeader = Remove-CppComments $BaseManagerHeader
	$ManagerHeader = Remove-CppComments $ManagerHeader
	$ManagerImplementation = Remove-CppComments $ManagerImplementation
	$DisplayImplementation = Remove-CppComments $DisplayImplementation
	$GameClientImplementation = Remove-CppComments $GameClientImplementation

    if ($BaseManagerHeader -notmatch 'virtual\s+void\s+releaseGraphicsResources\s*\(\s*\)' -or
        $ManagerHeader -notmatch 'virtual\s+void\s+releaseGraphicsResources\s*\(\s*\)\s+override\s*;') {
        return $false
    }
    $managerBody = Get-FunctionBody $ManagerImplementation 'void W3DDisplayStringManager::releaseGraphicsResources()'
    $displayBody = Get-FunctionBody $DisplayImplementation 'W3DDisplay::~W3DDisplay()'
    $clientBody = Get-FunctionBody $GameClientImplementation 'GameClient::~GameClient()'
    if ($null -eq $managerBody -or $null -eq $displayBody -or $null -eq $clientBody) { return $false }
    if ($managerBody -notmatch 'freeDisplayString\s*\(\s*m_groupNumeralStrings\s*\[' -or
        $managerBody -notmatch 'freeDisplayString\s*\(\s*m_formationLetterDisplayString\s*\)') { return $false }

    $movie = $displayBody.IndexOf('stopMovie();', [StringComparison]::Ordinal)
    $strings = $displayBody.IndexOf('TheDisplayStringManager->releaseGraphicsResources();', [StringComparison]::Ordinal)
    $shutdown = $displayBody.IndexOf('WW3D::Shutdown();', [StringComparison]::Ordinal)
    if ($movie -lt 0 -or $strings -lt 0 -or $shutdown -lt 0 -or
        $movie -gt $shutdown -or $strings -gt $shutdown) { return $false }

    $managerDelete = $clientBody.IndexOf('delete TheDisplayStringManager;', [StringComparison]::Ordinal)
	$fontReset = $clientBody.IndexOf('TheFontLibrary->reset();', [StringComparison]::Ordinal)
    $fontDelete = $clientBody.IndexOf('delete TheFontLibrary;', [StringComparison]::Ordinal)
    if ($managerDelete -lt 0 -or $fontReset -lt 0 -or $fontDelete -lt 0 -or
		$managerDelete -gt $fontReset -or $fontReset -gt $fontDelete) { return $false }
    if ($RequireSnow) {
        $snowDelete = $clientBody.IndexOf('delete TheSnowManager;', [StringComparison]::Ordinal)
        $displayDelete = $clientBody.IndexOf('delete TheDisplay;', [StringComparison]::Ordinal)
        if ($snowDelete -lt 0 -or $displayDelete -lt 0 -or $snowDelete -gt $displayDelete) { return $false }
    }
    return $true
}

if ($SelfTest) {
    $header = 'virtual void reset() override;'
    $validParticle = @'
void W3DParticleSystemManager::reset()
{
    m_pointGroup->Set_Texture(nullptr);
    m_streakLine->Set_Texture(nullptr);
    ParticleSystemManager::reset();
}
'@
    if (-not (Test-ParticleResetContract $header $validParticle)) { throw 'Valid particle fixture rejected.' }
    if (Test-ParticleResetContract $header $validParticle.Replace('m_streakLine->Set_Texture(nullptr);', '')) {
        throw 'Missing streak release accepted.'
    }
    $lateParticle = $validParticle.Replace(
        '    ParticleSystemManager::reset();',
        "    ParticleSystemManager::reset();`n    m_pointGroup->Set_Texture(nullptr);").Replace(
        '    m_pointGroup->Set_Texture(nullptr);' + "`n", '')
    if (Test-ParticleResetContract $header $lateParticle) { throw 'Late particle release accepted.' }
	$commentedParticle = $validParticle.Replace(
		'm_pointGroup->Set_Texture(nullptr);',
		'// m_pointGroup->Set_Texture(nullptr);')
	if (Test-ParticleResetContract $header $commentedParticle) { throw 'Commented particle release accepted.' }

    $base = 'virtual void releaseGraphicsResources() {}'
    $managerHeader = 'virtual void releaseGraphicsResources() override;'
	$manager = 'void W3DDisplayStringManager::releaseGraphicsResources() { freeDisplayString(m_groupNumeralStrings[0]); freeDisplayString(m_formationLetterDisplayString); }'
    $display = 'W3DDisplay::~W3DDisplay() { stopMovie(); TheDisplayStringManager->releaseGraphicsResources(); WW3D::Shutdown(); }'
	$client = 'GameClient::~GameClient() { delete TheSnowManager; delete TheDisplay; delete TheDisplayStringManager; TheFontLibrary->reset(); delete TheFontLibrary; }'
    if (-not (Test-DisplayContract $base $managerHeader $manager $display $client $true)) {
        throw 'Valid display fixture rejected.'
    }
    $lateDisplay = 'W3DDisplay::~W3DDisplay() { WW3D::Shutdown(); stopMovie(); TheDisplayStringManager->releaseGraphicsResources(); }'
    if (Test-DisplayContract $base $managerHeader $manager $lateDisplay $client $true) {
        throw 'Late display release accepted.'
    }
	$commentedManager = 'void W3DDisplayStringManager::releaseGraphicsResources() { // freeDisplayString(m_groupNumeralStrings[0]);' + "`n" + '// freeDisplayString(m_formationLetterDisplayString);' + "`n}"
	if (Test-DisplayContract $base $managerHeader $commentedManager $display $client $true) {
		throw 'Commented display-string release accepted.'
	}
	$lateFont = 'GameClient::~GameClient() { delete TheSnowManager; delete TheDisplay; TheFontLibrary->reset(); delete TheFontLibrary; delete TheDisplayStringManager; }'
	if (Test-DisplayContract $base $managerHeader $manager $display $lateFont $true) {
		throw 'Font deletion before display strings accepted.'
	}
	$earlyFontReset = 'GameClient::~GameClient() { delete TheSnowManager; delete TheDisplay; TheFontLibrary->reset(); delete TheDisplayStringManager; delete TheFontLibrary; }'
	if (Test-DisplayContract $base $managerHeader $manager $display $earlyFontReset $true) {
		throw 'Font reset before display strings accepted.'
	}
	$lateSnow = 'GameClient::~GameClient() { delete TheDisplay; delete TheSnowManager; delete TheDisplayStringManager; TheFontLibrary->reset(); delete TheFontLibrary; }'
	if (Test-DisplayContract $base $managerHeader $manager $display $lateSnow $true) {
		throw 'Snow deletion after display accepted.'
	}
    Write-Output 'Renderer shutdown lifecycle audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) { throw 'SourceRoot is required unless SelfTest is specified.' }

$particleHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DParticleSys.h') -Raw
$particleImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DParticleSys.cpp') -Raw
if (-not (Test-ParticleResetContract $particleHeader $particleImplementation)) {
    throw 'Particle renderer resources are not released before base reset and display shutdown.'
}

$baseManagerHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngine/Include/GameClient/DisplayStringManager.h') -Raw
$titles = @(
    @{ Root = 'Generals'; RequireSnow = $false },
    @{ Root = 'GeneralsMD'; RequireSnow = $true }
)
foreach ($title in $titles) {
    $root = $title.Root
    $managerHeader = Get-Content -LiteralPath (Join-Path $SourceRoot "$root/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DDisplayStringManager.h") -Raw
    $managerImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot "$root/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp") -Raw
    $displayImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot "$root/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp") -Raw
    $gameClientImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot "$root/Code/GameEngine/Source/GameClient/GameClient.cpp") -Raw
    if (-not (Test-DisplayContract $baseManagerHeader $managerHeader $managerImplementation $displayImplementation $gameClientImplementation $title.RequireSnow)) {
        throw "Display graphics resources are not quiesced before renderer shutdown: $root"
    }
}

Write-Output 'Renderer shutdown lifecycle audit passed.'
