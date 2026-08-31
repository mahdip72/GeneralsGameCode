param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Text)

    return [regex]::Replace($Text, '(?s)/\*.*?\*/|//[^\r\n]*', '')
}

function Get-FunctionBody {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Signature
    )

    $start = $Text.IndexOf($Signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { return $null }
    $open = $Text.IndexOf('{', $start + $Signature.Length)
    if ($open -lt 0) { return $null }

    $depth = 0
    for ($index = $open; $index -lt $Text.Length; ++$index) {
        if ($Text[$index] -eq '{') {
            ++$depth
        } elseif ($Text[$index] -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $Text.Substring($open + 1, $index - $open - 1)
            }
        }
    }
    return $null
}

function Test-NATOwnershipContract {
    param(
        [Parameter(Mandatory = $true)][string]$HeaderText,
        [Parameter(Mandatory = $true)][string]$ImplementationText,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Callers
    )

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $header = Remove-CppComments $HeaderText
    $implementation = Remove-CppComments $ImplementationText

    if ($header -notmatch '\bTransport\s*\*\s*takeTransport\s*\(\s*\)\s*;') {
        $violations.Add('NAT must expose an explicit transport ownership handoff')
    }
    if ($header -match '\bgetTransport\s*\(') {
        $violations.Add('NAT must not expose a borrowing getTransport accessor')
    }

    $destructor = Get-FunctionBody $implementation 'NAT::~NAT()'
    if ($null -eq $destructor) {
        $violations.Add('NAT destructor is missing')
    } else {
        $deleteIndex = $destructor.IndexOf('delete m_transport;', [StringComparison]::Ordinal)
        $clearIndex = $destructor.IndexOf('m_transport = nullptr;', [StringComparison]::Ordinal)
        if ($deleteIndex -lt 0 -or $clearIndex -lt $deleteIndex) {
            $violations.Add('NAT destructor must delete and clear its still-owned transport')
        }
    }

    $take = Get-FunctionBody $implementation 'Transport * NAT::takeTransport()'
    if ($null -eq $take) {
        $violations.Add('NAT transport handoff implementation is missing')
    } else {
        $captureIndex = $take.IndexOf('Transport *transport = m_transport;', [StringComparison]::Ordinal)
        $clearIndex = $take.IndexOf('m_transport = nullptr;', [StringComparison]::Ordinal)
        $returnIndex = $take.IndexOf('return transport;', [StringComparison]::Ordinal)
        if ($captureIndex -lt 0 -or $clearIndex -le $captureIndex -or $returnIndex -le $clearIndex) {
            $violations.Add('NAT transport handoff must clear NAT ownership before returning the pointer')
        }
    }

    foreach ($caller in $Callers.GetEnumerator()) {
        $callerText = Remove-CppComments $caller.Value
        if ($callerText -match '\bgetTransport\s*\(') {
            $violations.Add("$($caller.Key) still borrows the NAT transport")
        }
        if ($callerText -notmatch 'attachTransport\s*\(\s*TheNAT\s*->\s*takeTransport\s*\(\s*\)\s*\)') {
            $violations.Add("$($caller.Key) does not attach an explicitly transferred NAT transport")
        }
    }

    return $violations.ToArray()
}

function Test-GameEngineTeardownContract {
    param(
        [Parameter(Mandatory = $true)][string]$ImplementationText,
        [Parameter(Mandatory = $true)][string]$Title
    )

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $implementation = Remove-CppComments $ImplementationText
    $destructor = Get-FunctionBody $implementation 'GameEngine::~GameEngine()'
    if ($null -eq $destructor) {
        $violations.Add("$Title GameEngine destructor is missing")
        return $violations.ToArray()
    }

    $requiredOrder = @(
        'delete TheLAN;',
        'TheLAN = nullptr;',
        'delete TheNAT;',
        'TheNAT = nullptr;',
        'delete TheMapCache;',
        'TheGameResultsQueue->endThreads();',
        'rts::JobSystem::instance().shutdown();'
    )
    $previous = -1
    foreach ($required in $requiredOrder) {
        $position = $destructor.IndexOf($required, [StringComparison]::Ordinal)
        if ($position -lt 0) {
            $violations.Add("$Title teardown is missing '$required'")
        } elseif ($position -le $previous) {
            $violations.Add("$Title teardown releases network globals after '$required' or out of order")
        } else {
            $previous = $position
        }
    }

    return $violations.ToArray()
}

function Get-OwnershipLifecycleViolations {
    param(
        [Parameter(Mandatory = $true)][string]$NATHeader,
        [Parameter(Mandatory = $true)][string]$NATImplementation,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Callers,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$GameEngines
    )

    $violations = New-Object 'System.Collections.Generic.List[string]'
    foreach ($violation in @(Test-NATOwnershipContract $NATHeader $NATImplementation $Callers)) {
        $violations.Add($violation)
    }
    foreach ($engine in $GameEngines.GetEnumerator()) {
        foreach ($violation in @(Test-GameEngineTeardownContract $engine.Value $engine.Key)) {
            $violations.Add($violation)
        }
    }
    return $violations.ToArray()
}

if ($SelfTest) {
    $natHeader = 'class NAT { Transport * takeTransport(); };'
    $natImplementation = @'
NAT::~NAT()
{
    delete m_transport;
    m_transport = nullptr;
}
Transport * NAT::takeTransport()
{
    Transport *transport = m_transport;
    m_transport = nullptr;
    return transport;
}
'@
    $callers = @{
        CoreStaging = 'TheNetwork->attachTransport(TheNAT->takeTransport());'
        Generals = 'TheNetwork->attachTransport(TheNAT->takeTransport());'
        GeneralsMD = 'TheNetwork->attachTransport(TheNAT->takeTransport());'
    }
    $gameEngines = @{
        Generals = 'GameEngine::~GameEngine() { delete TheLAN; TheLAN = nullptr; delete TheNAT; TheNAT = nullptr; delete TheMapCache; TheGameResultsQueue->endThreads(); rts::JobSystem::instance().shutdown(); }'
        GeneralsMD = 'GameEngine::~GameEngine() { delete TheLAN; TheLAN = nullptr; delete TheNAT; TheNAT = nullptr; delete TheMapCache; TheGameResultsQueue->endThreads(); rts::JobSystem::instance().shutdown(); }'
    }

    if (@(Get-OwnershipLifecycleViolations $natHeader $natImplementation $callers $gameEngines).Count -ne 0) {
        throw 'Known-good network ownership fixture failed.'
    }
    $missingClear = $natImplementation.Replace('m_transport = nullptr;', '')
    if (@(Get-OwnershipLifecycleViolations $natHeader $missingClear $callers $gameEngines).Count -eq 0) {
        throw 'Missing NAT ownership clear fixture was accepted.'
    }
    $borrowingCaller = $callers.Clone()
    $borrowingCaller['Generals'] = 'TheNetwork->attachTransport(TheNAT->getTransport());'
    if (@(Get-OwnershipLifecycleViolations $natHeader $natImplementation $borrowingCaller $gameEngines).Count -eq 0) {
        throw 'Borrowing NAT transport fixture was accepted.'
    }
    $lateTeardown = $gameEngines.Clone()
    $lateTeardown['GeneralsMD'] = 'GameEngine::~GameEngine() { delete TheMapCache; delete TheLAN; TheLAN = nullptr; delete TheNAT; TheNAT = nullptr; TheGameResultsQueue->endThreads(); rts::JobSystem::instance().shutdown(); }'
    if (@(Get-OwnershipLifecycleViolations $natHeader $natImplementation $callers $lateTeardown).Count -eq 0) {
        throw 'Late network teardown fixture was accepted.'
    }
    Write-Output 'Network ownership lifecycle audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is specified.'
}

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$natHeader = Get-Content -LiteralPath (Join-Path $root 'Core/GameEngine/Include/GameNetwork/NAT.h') -Raw
$natImplementation = Get-Content -LiteralPath (Join-Path $root 'Core/GameEngine/Source/GameNetwork/NAT.cpp') -Raw
$callers = [ordered]@{
    CoreStaging = Get-Content -LiteralPath (Join-Path $root 'Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp') -Raw
    Generals = Get-Content -LiteralPath (Join-Path $root 'Generals/Code/GameEngine/Source/GameNetwork/GameSpyGameInfo.cpp') -Raw
    GeneralsMD = Get-Content -LiteralPath (Join-Path $root 'GeneralsMD/Code/GameEngine/Source/GameNetwork/GameSpyGameInfo.cpp') -Raw
}
$gameEngines = [ordered]@{
    Generals = Get-Content -LiteralPath (Join-Path $root 'Generals/Code/GameEngine/Source/Common/GameEngine.cpp') -Raw
    GeneralsMD = Get-Content -LiteralPath (Join-Path $root 'GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp') -Raw
}

$violations = @(Get-OwnershipLifecycleViolations $natHeader $natImplementation $callers $gameEngines)
if ($violations.Count -ne 0) {
    $violations | Write-Output
    exit 1
}
Write-Output 'Network ownership lifecycle audit passed.'
