param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Test-RendererShutdownLifecycleContract {
    param(
        [Parameter(Mandatory = $true)][string]$HeaderText,
        [Parameter(Mandatory = $true)][string]$ImplementationText
    )

    if ($HeaderText -notmatch '(?s)class\s+W3DParticleSystemManager.*?virtual\s+void\s+reset\s*\(\s*\)\s+override\s*;') {
        return $false
    }

    $match = [regex]::Match($ImplementationText,
        '(?s)void\s+W3DParticleSystemManager::reset\s*\(\s*\)\s*\{(?<body>.*?)\n\}')
    if (-not $match.Success) {
        return $false
    }

    $body = $match.Groups['body'].Value
    $pointRelease = $body.IndexOf('m_pointGroup->Set_Texture(nullptr);', [StringComparison]::Ordinal)
    $streakRelease = $body.IndexOf('m_streakLine->Set_Texture(nullptr);', [StringComparison]::Ordinal)
    $baseReset = $body.IndexOf('ParticleSystemManager::reset();', [StringComparison]::Ordinal)
    return $pointRelease -ge 0 -and $streakRelease -ge 0 -and $baseReset -ge 0 -and
        $pointRelease -lt $baseReset -and $streakRelease -lt $baseReset
}

function Test-DisplayShutdownLifecycleContract {
    param(
        [Parameter(Mandatory = $true)][string]$BaseManagerHeader,
        [Parameter(Mandatory = $true)][string]$ManagerHeader,
        [Parameter(Mandatory = $true)][string]$ManagerImplementation,
        [Parameter(Mandatory = $true)][string]$DisplayImplementation,
        [Parameter(Mandatory = $true)][string]$GameClientImplementation,
        [Parameter(Mandatory = $true)][bool]$RequireSnow
    )

    if ($BaseManagerHeader -notmatch 'virtual\s+void\s+releaseGraphicsResources\s*\(\s*\)' -or
        $ManagerHeader -notmatch 'virtual\s+void\s+releaseGraphicsResources\s*\(\s*\)\s+override\s*;') {
        return $false
    }
    if ($ManagerImplementation -notmatch '(?s)W3DDisplayStringManager::~W3DDisplayStringManager\s*\(\s*\).*?releaseGraphicsResources\s*\(\s*\)\s*;' -or
        $ManagerImplementation -notmatch '(?s)void\s+W3DDisplayStringManager::releaseGraphicsResources\s*\(\s*\).*?m_groupNumeralStrings.*?m_formationLetterDisplayString') {
        return $false
    }

    $displayStart = $DisplayImplementation.IndexOf('W3DDisplay::~W3DDisplay()', [StringComparison]::Ordinal)
    $displayEnd = $DisplayImplementation.IndexOf('inline Bool isResolutionSupported', $displayStart, [StringComparison]::Ordinal)
    if ($displayStart -lt 0 -or $displayEnd -lt 0) {
        return $false
    }
    $displayDestructor = $DisplayImplementation.Substring($displayStart, $displayEnd - $displayStart)
    $movieStop = $displayDestructor.IndexOf('stopMovie();', [StringComparison]::Ordinal)
    $stringRelease = $displayDestructor.IndexOf('TheDisplayStringManager->releaseGraphicsResources();', [StringComparison]::Ordinal)
    $rendererShutdown = $displayDestructor.IndexOf('WW3D::Shutdown();', [StringComparison]::Ordinal)
    if ($movieStop -lt 0 -or $stringRelease -lt 0 -or $rendererShutdown -lt 0 -or
        $movieStop -gt $rendererShutdown -or $stringRelease -gt $rendererShutdown) {
        return $false
    }

    $clientStart = $GameClientImplementation.IndexOf('GameClient::~GameClient()', [StringComparison]::Ordinal)
    $clientEnd = $GameClientImplementation.IndexOf('void GameClient::init()', $clientStart, [StringComparison]::Ordinal)
    if ($clientStart -lt 0 -or $clientEnd -lt 0) {
        return $false
    }
    $clientDestructor = $GameClientImplementation.Substring($clientStart, $clientEnd - $clientStart)
    $managerDelete = $clientDestructor.IndexOf('delete TheDisplayStringManager;', [StringComparison]::Ordinal)
    $fontDelete = $clientDestructor.IndexOf('delete TheFontLibrary;', [StringComparison]::Ordinal)
    if ($managerDelete -lt 0 -or $fontDelete -lt 0 -or $managerDelete -gt $fontDelete) {
        return $false
    }
    if ($RequireSnow) {
        $snowDelete = $clientDestructor.IndexOf('delete TheSnowManager;', [StringComparison]::Ordinal)
        $displayDelete = $clientDestructor.IndexOf('delete TheDisplay;', [StringComparison]::Ordinal)
        if ($snowDelete -lt 0 -or $displayDelete -lt 0 -or $snowDelete -gt $displayDelete) {
            return $false
        }
    }
    return $true
}

if ($SelfTest) {
    $header = 'class W3DParticleSystemManager { public: virtual void reset() override; };'
    $valid = @'
void W3DParticleSystemManager::reset()
{
    if (m_pointGroup != nullptr) m_pointGroup->Set_Texture(nullptr);
    if (m_streakLine != nullptr) m_streakLine->Set_Texture(nullptr);
    ParticleSystemManager::reset();
}
'@
    $missingStreak = $valid.Replace(
        '    if (m_streakLine != nullptr) m_streakLine->Set_Texture(nullptr);', '')
    $wrongOrder = @'
void W3DParticleSystemManager::reset()
{
    ParticleSystemManager::reset();
    if (m_pointGroup != nullptr) m_pointGroup->Set_Texture(nullptr);
    if (m_streakLine != nullptr) m_streakLine->Set_Texture(nullptr);
}
'@

    if (-not (Test-RendererShutdownLifecycleContract $header $valid)) {
        throw 'The valid shutdown-lifecycle fixture was rejected.'
    }
    if (Test-RendererShutdownLifecycleContract 'class W3DParticleSystemManager {};' $valid) {
        throw 'A missing reset override was accepted.'
    }
    if (Test-RendererShutdownLifecycleContract $header $missingStreak) {
        throw 'A missing streak release was accepted.'
    }
    if (Test-RendererShutdownLifecycleContract $header $wrongOrder) {
        throw 'Renderer resources released after the base reset were accepted.'
    }

    $baseManager = 'virtual void releaseGraphicsResources() {}'
    $managerHeader = 'virtual void releaseGraphicsResources() override;'
    $managerImplementation = @'
W3DDisplayStringManager::~W3DDisplayStringManager() { releaseGraphicsResources(); }
void W3DDisplayStringManager::releaseGraphicsResources()
{
    freeDisplayString(m_groupNumeralStrings[0]);
    freeDisplayString(m_formationLetterDisplayString);
}
'@
    $displayImplementation = @'
W3DDisplay::~W3DDisplay()
{
    stopMovie();
    TheDisplayStringManager->releaseGraphicsResources();
    WW3D::Shutdown();
}
inline Bool isResolutionSupported() { return true; }
'@
    $gameClientImplementation = @'
GameClient::~GameClient()
{
    delete TheSnowManager;
    delete TheDisplay;
    delete TheDisplayStringManager;
    delete TheFontLibrary;
}
void GameClient::init() {}
'@
    if (-not (Test-DisplayShutdownLifecycleContract $baseManager $managerHeader $managerImplementation $displayImplementation $gameClientImplementation $true)) {
        throw 'The valid display shutdown-lifecycle fixture was rejected.'
    }
    $lateMovie = $displayImplementation.Replace('    stopMovie();', '').Replace(
        '    WW3D::Shutdown();', "    WW3D::Shutdown();`n    stopMovie();")
    if (Test-DisplayShutdownLifecycleContract $baseManager $managerHeader $managerImplementation $lateMovie $gameClientImplementation $true) {
        throw 'A movie texture released after renderer shutdown was accepted.'
    }
    $lateSnow = @'
GameClient::~GameClient()
{
    delete TheDisplay;
    delete TheSnowManager;
    delete TheDisplayStringManager;
    delete TheFontLibrary;
}
void GameClient::init() {}
'@
    if (Test-DisplayShutdownLifecycleContract $baseManager $managerHeader $managerImplementation $displayImplementation $lateSnow $true) {
        throw 'Snow resources released after display shutdown were accepted.'
    }
    Write-Output 'Renderer shutdown lifecycle audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is specified.'
}

$headers = @(
    Join-Path $SourceRoot 'Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DParticleSys.h'
    Join-Path $SourceRoot 'GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DParticleSys.h'
)
$implementationPath = Join-Path $SourceRoot 'Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DParticleSys.cpp'
$implementation = Get-Content -LiteralPath $implementationPath -Raw

foreach ($headerPath in $headers) {
    $header = Get-Content -LiteralPath $headerPath -Raw
    if (-not (Test-RendererShutdownLifecycleContract $header $implementation)) {
        throw "Particle renderer resources are not released by reset before display shutdown: $headerPath"
    }
}

$baseManagerHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngine/Include/GameClient/DisplayStringManager.h') -Raw
$titleContracts = @(
    @{
        ManagerHeader = 'Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DDisplayStringManager.h'
        ManagerImplementation = 'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp'
        DisplayImplementation = 'Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp'
        GameClientImplementation = 'Generals/Code/GameEngine/Source/GameClient/GameClient.cpp'
        RequireSnow = $false
    },
    @{
        ManagerHeader = 'GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DDisplayStringManager.h'
        ManagerImplementation = 'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp'
        DisplayImplementation = 'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp'
        GameClientImplementation = 'GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp'
        RequireSnow = $true
    }
)
foreach ($contract in $titleContracts) {
    $managerHeader = Get-Content -LiteralPath (Join-Path $SourceRoot $contract.ManagerHeader) -Raw
    $managerImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot $contract.ManagerImplementation) -Raw
    $displayImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot $contract.DisplayImplementation) -Raw
    $gameClientImplementation = Get-Content -LiteralPath (Join-Path $SourceRoot $contract.GameClientImplementation) -Raw
    if (-not (Test-DisplayShutdownLifecycleContract $baseManagerHeader $managerHeader $managerImplementation `
        $displayImplementation $gameClientImplementation $contract.RequireSnow)) {
        throw "Display graphics resources are not quiesced before renderer shutdown: $($contract.DisplayImplementation)"
    }
}

Write-Output 'Renderer shutdown lifecycle audit passed.'
