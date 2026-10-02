param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

function Get-FunctionSlice {
    param(
        [string]$Text,
        [string]$BeginMarker,
        [string]$EndMarker,
        [string]$Description
    )
    $begin = $Text.IndexOf($BeginMarker, [StringComparison]::Ordinal)
    if ($begin -lt 0) {
        throw "$Description is missing begin marker '$BeginMarker'."
    }
    $end = $Text.IndexOf($EndMarker, $begin + $BeginMarker.Length,
        [StringComparison]::Ordinal)
    if ($end -lt 0) {
        throw "$Description is missing end marker '$EndMarker'."
    }
    return $Text.Substring($begin, $end - $begin)
}

function Assert-OrderedMarker {
    param(
        [string]$Text,
        [string]$Earlier,
        [string]$Later,
        [string]$Description
    )
    $earlierIndex = $Text.IndexOf($Earlier, [StringComparison]::Ordinal)
    $laterIndex = $Text.IndexOf($Later, [StringComparison]::Ordinal)
    if ($earlierIndex -lt 0 -or $laterIndex -lt 0 -or $earlierIndex -ge $laterIndex) {
        throw "$Description must order '$Earlier' before '$Later'."
    }
}

function Assert-TitleTransactions {
    param([string]$RelativePath, [string]$Title)

    $path = Join-Path $SourceRoot $RelativePath
    $text = [IO.File]::ReadAllText($path)
    $enemy = Get-FunctionSlice $text 'Bool RunSkirmishEnemyPlanningBatch()' `
        'Bool RunSkirmishProductionPlanningBatch()' "$Title enemy planning"
    Assert-OrderedMarker $enemy `
        'referenceClosed = FinishLiveAIPlanningReferenceAttempt(' `
        'if (!referenceClosed)' "$Title enemy receipt closure"
    Assert-OrderedMarker $enemy 'if (!referenceClosed)' `
        'applyEnemyPlanningCommit(' "$Title enemy fail-closed publication"
    $enemyFailureStart = $enemy.IndexOf('if (!referenceClosed)',
        [StringComparison]::Ordinal)
    $enemyPublish = $enemy.IndexOf('applyEnemyPlanningCommit(',
        [StringComparison]::Ordinal)
    $enemyFailure = $enemy.Substring($enemyFailureStart,
        $enemyPublish - $enemyFailureStart)
    if (-not $enemyFailure.Contains('RecordAIPlanningOwnerCommit(false)') -or
        -not $enemyFailure.Contains('return false;')) {
        throw "$Title enemy closure failure no longer rejects before publication."
    }

    $production = Get-FunctionSlice $text `
        'Bool RunSkirmishProductionPlanningBatch()' '#endif' `
        "$Title production planning"
    Assert-OrderedMarker $production `
        'referenceClosed = FinishLiveAIPlanningReferenceAttempt(' `
        'if (!referenceClosed)' "$Title production receipt closure"
    $productionFailureStart = $production.IndexOf('if (!referenceClosed)',
        [StringComparison]::Ordinal)
    $productionFailureEnd = $production.IndexOf('RecordAIPlanningOwnerCommit(true',
        $productionFailureStart, [StringComparison]::Ordinal)
    if ($productionFailureEnd -lt 0) {
        throw "$Title production transaction lost its post-closure commit marker."
    }
    $productionFailure = $production.Substring($productionFailureStart,
        $productionFailureEnd - $productionFailureStart)
    if (-not $productionFailure.Contains('discardStagedProductionPlanningResult()') -or
        -not $productionFailure.Contains('RecordAIPlanningOwnerCommit(false)') -or
        -not $productionFailure.Contains('return false;')) {
        throw "$Title production closure failure can leak a staged result."
    }
}

Assert-TitleTransactions `
    'Generals\Code\GameEngine\Source\GameLogic\AI\AI.cpp' 'Generals'
Assert-TitleTransactions `
    'GeneralsMD\Code\GameEngine\Source\GameLogic\AI\AI.cpp' 'Zero Hour'

Write-Output 'AI planning publication transaction tests passed.'
