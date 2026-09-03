param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$sourceRootPath = (Resolve-Path -LiteralPath $SourceRoot).Path
$sharedAdapter = Join-Path $sourceRootPath 'Core\Libraries\Include\Lib\HordeObjectComputationIslandAdapter.inl'
$islandSource = Join-Path $sourceRootPath 'Core\Libraries\Source\TaskRuntime\ObjectComputationIsland.cpp'
foreach ($sharedSource in @($sharedAdapter, $islandSource)) {
    if (-not (Test-Path -LiteralPath $sharedSource -PathType Leaf)) {
        throw "Horde shared source was not found: $sharedSource"
    }
}
$adapterText = Get-Content -LiteralPath $sharedAdapter -Raw
$islandText = Get-Content -LiteralPath $islandSource -Raw
foreach ($marker in @(
    'void ResetHordeObjectComputationIslandForMatch()',
    's_hordeObjectComputationCircuitBreaker = FALSE;',
    'PreflightObjectComputationIsland(options, &preflightMetrics)',
    'captureSpatialCells(*state, nonemptyCellCount, membershipCount)',
    'admitSpatialCapture(captures, moduleCount, objectCount, cellCountX,',
    'captureCount < HORDE_ISLAND_MINIMUM_DUE_MODULES',
    'legacyCost.queryCellVisits = legacyCellVisits;',
    'legacyCost.queryMemberVisits = legacyMemberVisits;',
    'workerCost.queryCellVisits = workerCellVisits;',
    'workerCost.queryMemberVisits = workerMemberVisits;',
    'EvaluateImmutableSpatialQueryAdmission(legacyCost,',
    'parallelTransactionCost < legacyTransactionCost',
    'ObjectComputationCandidateIndexAt(*state->view, *merged,',
    'state->matchEpoch != s_hordeObjectComputationMatchEpoch',
    's_reusableHordeState.releaseStorage();'
)) {
    if ($adapterText.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
        throw "Horde owner adapter is missing reset contract marker: $marker"
    }
}

$consumeStart = $adapterText.IndexOf(
    'Bool ConsumeHordeObjectComputationIsland(GameLogic *logic,',
    [StringComparison]::Ordinal)
$legacyBoundary = $adapterText.IndexOf('#else', $consumeStart,
    [StringComparison]::Ordinal)
if ($consumeStart -lt 0 -or $legacyBoundary -lt 0) {
    throw 'Horde owner consume boundary was not found.'
}
$consumeBody = $adapterText.Substring($consumeStart,
    $legacyBoundary - $consumeStart)
if ($consumeBody.IndexOf(
        'objectIndex != state->objectCount',
        [StringComparison]::Ordinal) -ge 0) {
    throw 'Horde owner commit retains the removed second full-world scan.'
}

foreach ($spatialMarker in @(
    'collectSpatialScan(',
    'view.findSpatialCellSpan(',
    'broadCandidateLegacy(',
    'ObjectComputationCandidateIndexAt('
)) {
    if ($islandText.IndexOf($spatialMarker,
            [StringComparison]::Ordinal) -lt 0) {
        throw "Object computation island is missing compact spatial marker: $spatialMarker"
    }
}
$legacyPredicateMarkers = @(
    'const float actualDistanceSquared = deltaX * deltaX + deltaY * deltaY +',
    'const float totalRadius = owner.boundingSphereRadius +',
    'const float actualDistance = sqrtf(actualDistanceSquared);',
    'const float boundaryDistance = actualDistance - totalRadius;',
    'boundaryDistance * boundaryDistance;',
    'return boundaryDistanceSquared < maximumDistance * maximumDistance;'
)
$predicatePosition = -1
foreach ($marker in $legacyPredicateMarkers) {
    $next = $islandText.IndexOf($marker, [StringComparison]::Ordinal)
    if ($next -lt 0 -or $next -le $predicatePosition) {
        throw "Object computation sphere predicate changed legacy operation order: $marker"
    }
    $predicatePosition = $next
}
if ($islandText.IndexOf('const double reach',
        [StringComparison]::Ordinal) -ge 0) {
    throw 'Object computation sphere predicate retains non-legacy double equivalence.'
}

foreach ($requiredFence in @(
    'waitWithoutOwnerHelp(group,',
    '++metrics->physicalWaitTimeouts;',
    'jobs->cancel(group);',
    'jobs->wait(group);'
)) {
    if ($islandText.IndexOf($requiredFence,
            [StringComparison]::Ordinal) -lt 0) {
        throw "Object computation island is missing bounded fence marker: $requiredFence"
    }
}
foreach ($activeWait in @(
    'std::this_thread::yield',
    'std::chrono::steady_clock'
)) {
    if ($islandText.IndexOf($activeWait,
            [StringComparison]::Ordinal) -ge 0) {
        throw "Object computation island retains active owner wait: $activeWait"
    }
}

$prepareStart = $adapterText.IndexOf(
    'Bool PrepareHordeObjectComputationIsland(GameLogic *logic,',
    [StringComparison]::Ordinal)
$prepareEnd = $adapterText.IndexOf(
    'Bool ConsumeHordeObjectComputationIsland(GameLogic *logic,',
    $prepareStart, [StringComparison]::Ordinal)
if ($prepareStart -lt 0 -or $prepareEnd -lt 0) {
    throw 'Horde owner adapter prepare boundary was not found.'
}
$prepareBody = $adapterText.Substring($prepareStart,
    $prepareEnd - $prepareStart)
$admission = $prepareBody.IndexOf('admitSpatialCapture(captures, moduleCount,',
    [StringComparison]::Ordinal)
$worldCapture = $prepareBody.IndexOf('captureObjects(logic, *state)',
    [StringComparison]::Ordinal)
$arenaAcquire = $prepareBody.IndexOf('acquireReusableState()',
    [StringComparison]::Ordinal)
$tinyReject = $prepareBody.IndexOf(
    'moduleCount < HORDE_ISLAND_MINIMUM_DUE_MODULES',
    [StringComparison]::Ordinal)
$cellTotals = $prepareBody.IndexOf('countSpatialCapture(logic,',
    [StringComparison]::Ordinal)
if ($admission -lt 0 -or $worldCapture -lt 0 -or $arenaAcquire -lt 0 -or
    $tinyReject -lt 0 -or $cellTotals -lt 0 -or
    $tinyReject -gt $cellTotals -or $admission -gt $worldCapture -or
    $admission -gt $arenaAcquire) {
    throw 'Horde exact due/spatial-cost admission must precede world capture, sort, and arena acquisition.'
}
$policy = $prepareBody.IndexOf(
    'options.parallel = rts::PrepareSimulationCommandsOffThread();',
    [StringComparison]::Ordinal)
$preflight = $prepareBody.IndexOf(
    'PreflightObjectComputationIsland(options, &preflightMetrics)',
    [StringComparison]::Ordinal)
if ($policy -lt 0 -or $preflight -lt 0 -or $policy -gt $preflight) {
    throw 'Horde policy must feed the allocation-free scheduler preflight.'
}
foreach ($expensiveMarker in @(
    'new (std::nothrow)',
    'captureContiguousHordePrefix(',
    'countSpatialCapture(logic,',
    'captureObjects(logic, *state)',
    'new (std::nothrow) rts::SimulationReadView'
)) {
    $expensive = $prepareBody.IndexOf($expensiveMarker,
        [StringComparison]::Ordinal)
    if ($expensive -lt 0 -or $preflight -gt $expensive) {
        throw "Horde preflight must precede live scan/capture/allocation marker: $expensiveMarker"
    }
}

foreach ($title in @('Generals', 'GeneralsMD')) {
    $header = Join-Path $sourceRootPath "$title\Code\GameEngine\Include\GameLogic\HordeObjectComputationIsland.h"
    $gameLogic = Join-Path $sourceRootPath "$title\Code\GameEngine\Source\GameLogic\System\GameLogic.cpp"
    foreach ($file in @($header, $gameLogic)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "$title Horde reset contract source was not found: $file"
        }
    }
    $headerText = Get-Content -LiteralPath $header -Raw
    $gameLogicText = Get-Content -LiteralPath $gameLogic -Raw
    if ($headerText.IndexOf('void ResetHordeObjectComputationIslandForMatch();',
            [StringComparison]::Ordinal) -lt 0) {
        throw "$title public owner adapter does not declare the match reset hook."
    }
    $resetStart = $gameLogicText.IndexOf('void GameLogic::reset()',
        [StringComparison]::Ordinal)
    if ($resetStart -lt 0) {
        throw "$title GameLogic reset implementation was not found."
    }
    $resetEnd = $gameLogicText.IndexOf('m_thingTemplateBuildableOverrides.clear();',
        $resetStart, [StringComparison]::Ordinal)
    if ($resetEnd -lt 0) {
        throw "$title GameLogic reset boundary was not found."
    }
    $resetBody = $gameLogicText.Substring($resetStart, $resetEnd - $resetStart)
    if ($resetBody.IndexOf('ResetHordeObjectComputationIslandForMatch();',
            [StringComparison]::Ordinal) -lt 0) {
        throw "$title GameLogic reset does not clear the Horde circuit breaker."
    }
}

Write-Output 'Generals and Zero Hour Horde exact-float, cost-admission, bounded-fence, and match-reset contracts are present.'
