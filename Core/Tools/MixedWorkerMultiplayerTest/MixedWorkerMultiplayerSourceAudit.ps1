param(
    [Parameter(Mandatory = $false)]
    [string]$SourceRoot = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()
$lockstepV2NamespaceDeclarationPattern =
    '(?m)^[ \t]*namespace[ \t]+lockstep_v2[ \t]*$'

function Read-Source([string]$relativePath) {
    $path = Join-Path $SourceRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $failures.Add("missing source: $relativePath")
        return ''
    }
    return [IO.File]::ReadAllText($path)
}

function Require-Count(
    [string]$source,
    [string]$token,
    [int]$expected,
    [string]$description
) {
    $actual = ([regex]::Matches($source, [regex]::Escape($token))).Count
    if ($actual -ne $expected) {
        $failures.Add("$description (expected $expected, found $actual)")
    }
}

function Assert-SelfTestAcceptsCount(
    [string]$source,
    [string]$token,
    [int]$expected,
    [string]$description
) {
    $failures.Clear()
    Require-Count $source $token $expected $description
    if ($failures.Count -ne 0) {
        $messages = $failures -join '; '
        $failures.Clear()
        throw "Self-test rejected a valid fixture: $messages"
    }
    $failures.Clear()
}

function Assert-SelfTestRejectsCount(
    [string]$source,
    [string]$token,
    [int]$expected,
    [string]$description
) {
    $failures.Clear()
    Require-Count $source $token $expected $description
    if ($failures.Count -eq 0) {
        $failures.Clear()
        throw "Self-test accepted a malformed fixture: $description"
    }
    $failures.Clear()
}

function Invoke-SelfTest {
    $validTitle = @'
#include "GameNetwork/MultiplayerSimulationRuntimePolicy.h"
ShouldPrepareSimulationKernelOffThread(
ShouldPrepareSimulationKernelOffThread(
ShouldPrepareSimulationKernelOffThread(
MULTIPLAYER_SIMULATION_KERNEL_PHYSICS
MULTIPLAYER_SIMULATION_KERNEL_SPATIAL
MULTIPLAYER_SIMULATION_KERNEL_STATUS
TheNetwork == nullptr &&
'@
    Assert-SelfTestAcceptsCount $validTitle `
        'ShouldPrepareSimulationKernelOffThread(' 3 `
        'title fixture has the reviewed policy predicates'
    Assert-SelfTestRejectsCount ($validTitle.Replace(
            'MULTIPLAYER_SIMULATION_KERNEL_STATUS', 'removed_status_route')) `
        'MULTIPLAYER_SIMULATION_KERNEL_STATUS' 1 `
        'title fixture missing the status policy route'

    $validCollision = @'
#include "GameNetwork/MultiplayerSimulationRuntimePolicy.h"
MULTIPLAYER_SIMULATION_KERNEL_COLLISION
const Bool multiplayerPolicyBlocked =
jobs.isCurrentThread(rts::JOB_OWNER_GAME)
'@
    Assert-SelfTestAcceptsCount $validCollision `
        'const Bool multiplayerPolicyBlocked =' 1 `
        'collision fixture keeps the dedicated admission gate'
    Assert-SelfTestRejectsCount ($validCollision.Replace(
            'jobs.isCurrentThread(rts::JOB_OWNER_GAME)', 'removed_owner_gate')) `
        'jobs.isCurrentThread(rts::JOB_OWNER_GAME)' 1 `
        'collision fixture missing owner-thread admission'

    $validAdapter = @'
GetSimulationExecutionMode() == SIMULATION_EXECUTION_PARALLEL
isMultiplayerSimulationKernelEnabled(kernel)
isMultiplayerSimulationKernelEnabled(kernel)
'@
    Assert-SelfTestAcceptsCount $validAdapter `
        'isMultiplayerSimulationKernelEnabled(kernel)' 2 `
        'adapter fixture delegates both policy checks'
    Assert-SelfTestRejectsCount ($validAdapter.Replace(
            'isMultiplayerSimulationKernelEnabled(kernel)', 'removed_policy_check')) `
        'isMultiplayerSimulationKernelEnabled(kernel)' 2 `
        'adapter fixture missing one policy check'

    $validPolicyHeader = @'
MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK = MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK
MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK = MULTIPLAYER_SIMULATION_KERNEL_NONE
'@
    if ($validPolicyHeader -notmatch '(?s)MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK\s*=\s*MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK') {
        throw 'Self-test rejected the valid integrated-kernel mask fixture.'
    }
    if ($validPolicyHeader -notmatch '(?s)MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK\s*=\s*MULTIPLAYER_SIMULATION_KERNEL_NONE') {
        throw 'Self-test rejected the valid release-proven default fixture.'
    }
    $malformedPolicyHeader = $validPolicyHeader.Replace(
        'MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK', 'MULTIPLAYER_SIMULATION_KERNEL_NONE')
    if ($malformedPolicyHeader -match '(?s)MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK\s*=\s*MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK') {
        throw 'Self-test accepted a malformed integrated-kernel mask fixture.'
    }

    $validPromotionBoundary = @'
unsigned candidateKernelMask =
    rts::lockstep_v2::ResolveEmbeddedProductPromotionKernelMask(
        rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK);
if (candidateKernelMask ==
    rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK)
{
    candidateKernelMask =
        getRuntimeMultiplayerSimulationReleaseProvenKernelMask(
            TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC);
}
'@
    $promotionBoundaryPattern = '(?s)candidateKernelMask\s*=\s*rts::lockstep_v2::ResolveEmbeddedProductPromotionKernelMask\(.*?MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK\).*?if \(candidateKernelMask ==\s*rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK\).*?candidateKernelMask\s*=\s*getRuntimeMultiplayerSimulationReleaseProvenKernelMask\('
    if ($validPromotionBoundary -notmatch $promotionBoundaryPattern) {
        throw 'Self-test rejected the valid lockstep-v2 promotion boundary fixture.'
    }
    $malformedPromotionBoundary = $validPromotionBoundary.Replace(
        'ResolveEmbeddedProductPromotionKernelMask(',
        'removed_v2_promotion_boundary(')
    if ($malformedPromotionBoundary -match $promotionBoundaryPattern) {
        throw 'Self-test accepted an ordinary policy path without lockstep-v2 promotion.'
    }

    $validPromotionHeader = @'
namespace lockstep_v2
{
if (authority.promotedKernelMask != liveIntegratedKernelMask)
{
    return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
}
} // namespace lockstep_v2
'@
    if ([regex]::Matches($validPromotionHeader,
            $lockstepV2NamespaceDeclarationPattern).Count -ne 1) {
        throw 'Self-test rejected one lockstep-v2 namespace declaration with a closing comment.'
    }
    $duplicatePromotionNamespace = $validPromotionHeader.Replace(
        'namespace lockstep_v2',
        "namespace lockstep_v2`nnamespace lockstep_v2")
    if ([regex]::Matches($duplicatePromotionNamespace,
            $lockstepV2NamespaceDeclarationPattern).Count -eq 1) {
        throw 'Self-test accepted duplicate lockstep-v2 namespace declarations.'
    }
    $promotionFailClosedPattern = '(?s)authority\.promotedKernelMask != liveIntegratedKernelMask.*?return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;'
    if ($validPromotionHeader -notmatch $promotionFailClosedPattern) {
        throw 'Self-test rejected the valid complete-mask promotion guard.'
    }
    $malformedPromotionHeader = $validPromotionHeader.Replace(
        '!= liveIntegratedKernelMask', '& ~liveIntegratedKernelMask')
    if ($malformedPromotionHeader -match $promotionFailClosedPattern) {
        throw 'Self-test accepted a partial-mask promotion guard.'
    }

    $validInstalledMapParser = @'
if (std::iscntrl(value) || (std::isspace(value) && value != ' '))
    return false;
if (!GetFieldIndex(name, &fieldIndex) || seen[fieldIndex] ||
    (fieldIndex != FIELD_MAP && !IsSafeValue(value)))
    return false;
'@
    foreach ($token in @(
            "std::iscntrl(value) || (std::isspace(value) && value != ' ')",
            '(fieldIndex != FIELD_MAP && !IsSafeValue(value))')) {
        Assert-SelfTestAcceptsCount $validInstalledMapParser $token 1 `
            "installed lockstep-v2 map parser fixture contains '$token'"
        Assert-SelfTestRejectsCount ($validInstalledMapParser.Replace(
                $token, 'removed_map_parser_guard')) $token 1 `
            "installed lockstep-v2 map parser fixture missing '$token'"
    }

    $validWrapper = 'IsWrappedNetworkCommandOriginAuthorized(temp->getPlayerID(), msg->getCommand()->getPlayerID(), MAX_SLOTS) retlist->addMessage'
    if ($validWrapper -notmatch '(?s)IsWrappedNetworkCommandOriginAuthorized\(temp->getPlayerID\(\),.*?msg->getCommand\(\)->getPlayerID\(\), MAX_SLOTS\).*?retlist->addMessage') {
        throw 'Self-test rejected the valid wrapper-origin fixture.'
    }
    $malformedWrapper = $validWrapper.Replace(
        'IsWrappedNetworkCommandOriginAuthorized(temp->getPlayerID(),', 'removed_wrapper_check(')
    if ($malformedWrapper -match '(?s)IsWrappedNetworkCommandOriginAuthorized\(temp->getPlayerID\(\),.*?msg->getCommand\(\)->getPlayerID\(\), MAX_SLOTS\).*?retlist->addMessage') {
        throw 'Self-test accepted a wrapper-origin fixture without the origin check.'
    }

    $validLoopback = @'
physical != submitted
distinctWorkers <= 1
record.physicalKernelMask == expectedPhysicalKernelMask
'@
    foreach ($token in @(
            'physical != submitted',
            'distinctWorkers <= 1',
            'record.physicalKernelMask == expectedPhysicalKernelMask')) {
        Assert-SelfTestAcceptsCount $validLoopback $token 1 `
            "loopback fixture contains '$token'"
        Assert-SelfTestRejectsCount ($validLoopback.Replace($token, 'removed_loopback_guard')) `
            $token 1 "loopback fixture missing '$token'"
    }

    $validPath = @'
#include "GameNetwork/MultiplayerSimulationRuntimePolicy.h"
MULTIPLAYER_SIMULATION_KERNEL_PATH
policy.multiplayerPolicyEnabled = policy.networkGame &&
!policy.networkGame || !policy.multiplayerPolicyEnabled
'@
    Assert-SelfTestAcceptsCount $validPath `
        'MULTIPLAYER_SIMULATION_KERNEL_PATH' 1 `
        'path fixture routes through the live policy'
    Assert-SelfTestRejectsCount ($validPath.Replace(
            'policy.multiplayerPolicyEnabled = policy.networkGame &&',
            'removed_network_fallback')) `
        'policy.multiplayerPolicyEnabled = policy.networkGame &&' 1 `
        'path fixture missing the non-network fallback'

    Write-Output 'Mixed-worker multiplayer source audit self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is selected.'
}

$titlePaths = @(
    'Generals/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp',
    'GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp'
)
foreach ($titlePath in $titlePaths) {
    $source = Read-Source $titlePath
    Require-Count $source 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
        "$titlePath includes the runtime policy adapter exactly once"
    Require-Count $source 'ShouldPrepareLiveSimulationKernelOffThread(' 3 `
        "$titlePath has exactly physics/spatial/status policy predicates"
    Require-Count $source 'MULTIPLAYER_SIMULATION_KERNEL_PHYSICS' 1 `
        "$titlePath has one physics policy route"
    Require-Count $source 'MULTIPLAYER_SIMULATION_KERNEL_SPATIAL' 1 `
        "$titlePath has one spatial policy route"
    Require-Count $source 'MULTIPLAYER_SIMULATION_KERNEL_STATUS' 1 `
        "$titlePath has one status policy route"
    Require-Count $source 'TheNetwork == nullptr &&' 1 `
        "$titlePath keeps only the unproven Horde lane network-serial"
    Require-Count $source 'TheNetwork != nullptr || !rts::PrepareSimulationCommandsOffThread()' 0 `
        "$titlePath has no blanket network status fallback"
}

$collisionPaths = @(
    'Generals/Code/GameEngine/Source/GameLogic/Object/PartitionManager.cpp',
    'GeneralsMD/Code/GameEngine/Source/GameLogic/Object/PartitionManager.cpp'
)
foreach ($collisionPath in $collisionPaths) {
    $collision = Read-Source $collisionPath
    Require-Count $collision 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
        "$collisionPath includes the runtime policy adapter exactly once"
    Require-Count $collision 'MULTIPLAYER_SIMULATION_KERNEL_COLLISION' 1 `
        "$collisionPath has one collision policy route"
    Require-Count $collision 'const Bool multiplayerPolicyBlocked =' 1 `
        "$collisionPath preserves a dedicated multiplayer admission gate"
    Require-Count $collision 'if (TheGameLogic->isInMultiplayerGame() || !schedulerReady)' 0 `
        "$collisionPath has no blanket multiplayer serial fallback"
    Require-Count $collision 'jobs.isCurrentThread(rts::JOB_OWNER_GAME)' 1 `
        "$collisionPath preserves collision owner-thread admission"
}

$generalsAIPath = 'Generals/Code/GameEngine/Source/GameLogic/AI/AI.cpp'
$generalsAI = Read-Source $generalsAIPath
Require-Count $generalsAI 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
    "$generalsAIPath includes the runtime policy adapter exactly once"
Require-Count $generalsAI 'MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING' 1 `
    "$generalsAIPath has one AI planning policy route"
Require-Count $generalsAI 'IsEnemyPlanningMultiplayerPolicyBlocked()' 4 `
    "$generalsAIPath applies one policy decision to admission, execution, and epoch gates"
Require-Count $generalsAI 'owners[i]->applyEnemyPlanningCommit(resolved[i]);' 1 `
    "$generalsAIPath preserves the canonical owner commit"

$zeroHourAIPath = 'GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AI.cpp'
$zeroHourAI = Read-Source $zeroHourAIPath
Require-Count $zeroHourAI 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
    "$zeroHourAIPath includes the centralized runtime policy adapter exactly once"
Require-Count $zeroHourAI 'MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING' 1 `
    "$zeroHourAIPath has one centralized AI planning policy route"
Require-Count $zeroHourAI `
    'owners[i]->commitEnemyPlanningResult(' 1 `
    'Zero Hour enemy planning preserves its owner commit loop'
$zeroHourProductionPath = 'GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AISkirmishPlayer.cpp'
Require-Count (Read-Source $zeroHourProductionPath) `
    'commitProductionPlanningResult(' 2 `
    'Zero Hour production planning preserves its owner-only commit implementation and call'

$spatialPath = 'Core/GameEngine/Source/GameLogic/System/ImmutableSpatialQueryRuntime.cpp'
$spatial = Read-Source $spatialPath
Require-Count $spatial 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
    "$spatialPath includes the runtime policy adapter exactly once"
Require-Count $spatial 'ShouldPrepareLiveSimulationKernelOffThread(' 3 `
    "$spatialPath policy-gates public admission, collection, and query consumption"
Require-Count $spatial 'TheNetwork != nullptr' 0 `
    "$spatialPath has no hidden blanket network serial fallback"
Require-Count $spatial 'jobs.workerCount() <= 1' 1 `
    "$spatialPath preserves the explicit one-worker fallback"
Require-Count $spatial 'jobs.isCurrentThread(rts::JOB_OWNER_GAME)' 3 `
    "$spatialPath preserves all owner-thread gates"

$adapterPath = 'Core/GameEngine/Include/GameNetwork/MultiplayerSimulationRuntimePolicy.h'
$adapter = Read-Source $adapterPath
Require-Count $adapter 'GetSimulationExecutionMode() == SIMULATION_EXECUTION_PARALLEL' 1 `
    "$adapterPath allows network authority only in explicit parallel mode"
Require-Count $adapter 'isMultiplayerSimulationKernelEnabled(kernel)' 2 `
    "$adapterPath delegates only to the resolved session policy"

$networkPath = 'Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp'
$network = Read-Source $networkPath
Require-Count $network 'IsNetworkSimulationRosterIdentityValid(' 1 `
    "$networkPath validates the exact connection roster"
Require-Count $network 'ResolveMultiplayerSimulationSessionPolicy(' 1 `
    "$networkPath persists one resolved session policy"
Require-Count $network 'm_networkSimulationPolicyResolved = TRUE;' 1 `
    "$networkPath publishes resolution only after the complete handshake"
Require-Count $network 'getRuntimeMultiplayerSimulationReleaseProvenKernelMask(' 2 `
	"$networkPath retains the isolated diagnostic-v1 resolver and fallback"
Require-Count $network 'ResolveEmbeddedProductPromotionKernelMask(' 1 `
	"$networkPath derives ordinary product authority from the embedded lockstep-v2 trust root"
if ($network -notmatch '(?s)candidateKernelMask\s*=\s*rts::lockstep_v2::ResolveEmbeddedProductPromotionKernelMask\(.*?MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK\).*?if \(candidateKernelMask ==\s*rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK\).*?candidateKernelMask\s*=\s*getRuntimeMultiplayerSimulationReleaseProvenKernelMask\(') {
    $failures.Add("$networkPath must select lockstep-v2 promotion before the diagnostic-v1 serial fallback")
}
Require-Count $network 'ResolveMultiplayerSimulationRuntimeProofMask(' 1 `
	"$networkPath keeps the diagnostic-v1 proof resolver intact"
Require-Count $network 'calculateFileSha256(' 5 `
	"$networkPath independently hashes the executable, lockstep-v2 origin, and every external proof-bundle layer"
Require-Count $network 'MultiplayerSimulationRuntimeProof.txt' 1 `
    "$networkPath loads one external proof beside the installed executable"
Require-Count $network 'verifyRuntimeProofEvidenceBundle(' 2 `
    "$networkPath validates the proof plus its adjacent evidence bundle before advertising"
Require-Count $network 'MultiplayerSimulationRawEvidence.index' 1 `
    "$networkPath requires the canonical raw 40-peer evidence index"
Require-Count $network 'Net3LoopbackEvidence.json' 1 `
    "$networkPath independently hashes the strict 16-match evidence manifest"
Require-Count $network 'Stage5ArtifactSet.json' 1 `
    "$networkPath independently hashes the exact installed artifact-set manifest"
Require-Count $network 'MultiplayerSimulationReleaseProof.generated.h' 0 `
    "$networkPath never consumes a source-relative generated header"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_GENERATED_RELEASE_PROOF_AVAILABLE' 0 `
	"$networkPath has no mutable generated-header proof authority"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_BUILD_SOURCE_REVISION' 0 `
	"$networkPath has no free-form build source authority"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_PROMOTED_KERNEL_MASK' 0 `
	"$networkPath has no v1 build-defined authority override"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_SOURCE_REVISION' 0 `
	"$networkPath has no v1 build-defined source trust override"
if ($network -notmatch '(?s)MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK\),\s*""\);') {
    $failures.Add("$networkPath must pass the serial mask and empty source trust to the v1 resolver")
}
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_EVIDENCE_SOURCE_REVISION' 0 `
    "$networkPath has no free-form evidence source authority"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_INSTALLED_LOOPBACK_EVIDENCE_SHA256' 0 `
    "$networkPath has no free-form loopback digest authority"
Require-Count $network 'RTS_MULTIPLAYER_SIMULATION_RELEASE_PROVEN_KERNEL_MASK' 0 `
    "$networkPath has no free-form product kernel-mask authority"
Require-Count $network 'MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK) &' 0 `
    "$networkPath never advertises a kernel merely because it is known"
Require-Count $network 'Bool ConnectionManager::isNetworkSimulationPolicyUsable() const' 1 `
    "$networkPath has one central live policy-usability predicate"
Require-Count $network 'isNetworkSimulationPolicyUsable()' 6 `
    "$networkPath routes bit, mask, lockstep-v2 proof, and lifecycle revocation through the central predicate"
Require-Count $network 'IsMultiplayerSimulationPolicyLifecycleUsable(' 1 `
    "$networkPath requires resolved hello state and the exact live roster"
Require-Count $network 'void ConnectionManager::revokeNetworkSimulationPolicy()' 1 `
    "$networkPath has one fail-closed policy revocation implementation"
Require-Count $network 'revokeNetworkSimulationPolicy();' 4 `
    "$networkPath revokes on hello failure, leave, observed quitting, and disconnect"
Require-Count $network 'MULTIPLAYER_SIMULATION_POLICY_SERIAL_NETWORK_UNAVAILABLE' 2 `
    "$networkPath reserves unavailable status for unresolved native and legacy sessions"
Require-Count $network 'm_networkSimulationPolicyResolved ?' 1 `
    "$networkPath preserves a resolved policy's precise rejection status for diagnostics"
Require-Count $network 'm_networkHelloExpectedSlots &=' 0 `
    "$networkPath never renegotiates against a reduced remote roster"

$installedLockstepPath = 'Core/GameEngine/Source/GameNetwork/InstalledLockstepV2Validation.cpp'
$installedLockstep = Read-Source $installedLockstepPath
Require-Count $installedLockstep `
    "std::iscntrl(value) || (std::isspace(value) && value != ' ')" 1 `
    "$installedLockstepPath permits ordinary spaces in reviewed map names while rejecting other whitespace"
Require-Count $installedLockstep `
    '(fieldIndex != FIELD_MAP && !IsSafeValue(value))' 1 `
    "$installedLockstepPath limits the whitespace exception to FIELD_MAP"

$runtimeProofPath = 'Core/Libraries/Include/Lib/MultiplayerSimulationRuntimeProof.h'
$runtimeProof = Read-Source $runtimeProofPath
Require-Count $runtimeProof 'proof.executableSha256 != actualExecutableSha256' 1 `
    "$runtimeProofPath binds the proof to the independently hashed current executable"
Require-Count $runtimeProof 'proof.buildCompatibilityCrc != expectedBuildCompatibilityCrc' 1 `
    "$runtimeProofPath binds the proof to the live build compatibility identity"
Require-Count $runtimeProof 'proof.contentCrc != expectedContentCrc' 1 `
    "$runtimeProofPath binds the proof to the live content identity"
Require-Count $runtimeProof 'proof.producer != "installed-runtime-runner-v1"' 1 `
    "$runtimeProofPath accepts only the installed-runner proof contract"
Require-Count $runtimeProof 'proof.peerProcessCount !=' 1 `
    "$runtimeProofPath requires the complete 40-peer evidence set"
Require-Count $runtimeProof 'proof.rawEvidenceIndexSha256' 1 `
	"$runtimeProofPath binds the external proof to the raw peer-output index"
Require-Count $runtimeProof 'proof.provenKernelMask != trustedBuildKernelMask' 1 `
	"$runtimeProofPath prevents sibling runtime evidence from exceeding build authority"
Require-Count $runtimeProof 'proof.sourceRevision.c_str(), trustedBuildSourceRevision' 1 `
	"$runtimeProofPath binds runtime evidence to the embedded source revision"
Require-Count $runtimeProof 'proof.producer == "installed-runtime-runner-v1"' 1 `
	"$runtimeProofPath rejects the diagnostic v1 proof before any authority check"
Require-Count $runtimeProof 'proof.validationMode == "scoped-net3-loopback-release-proof"' 1 `
	"$runtimeProofPath identifies the diagnostic v1 validation mode explicitly"

$promotionHeaderPath = 'Core/Libraries/Include/Lib/LockstepV2Promotion.h'
$promotionHeader = Read-Source $promotionHeaderPath
$promotionNamespaceDeclarationCount = [regex]::Matches($promotionHeader,
    $lockstepV2NamespaceDeclarationPattern).Count
if ($promotionNamespaceDeclarationCount -ne 1) {
    $failures.Add("$promotionHeaderPath owns exactly one namespace declaration separate from diagnostic v1 (expected 1, found $promotionNamespaceDeclarationCount)")
}
Require-Count $promotionHeader 'struct ProductPromotionAuthority' 1 `
	"$promotionHeaderPath models the embedded v2 release trust root"
Require-Count $promotionHeader '#include "Lib/LockstepV2Contract.h"' 1 `
	"$promotionHeaderPath binds promotion metadata to the reviewed lockstep-v2 contract"
Require-Count $promotionHeader 'promotionSchemaVersion' 3 `
	"$promotionHeaderPath binds construction, storage, and validation to the promotion schema"
Require-Count $promotionHeader 'lockstepSchemaVersion' 3 `
	"$promotionHeaderPath binds construction, storage, and validation to lockstep-v2"
Require-Count $promotionHeader 'protocolEpoch' 3 `
	"$promotionHeaderPath binds construction, storage, and validation to protocol epoch two"
Require-Count $promotionHeader 'promotedKernelMask' 4 `
	"$promotionHeaderPath stores, constructs, validates, and returns the exact promoted mask"
Require-Count $promotionHeader 'ResolveProductPromotionKernelMask(' 2 `
	"$promotionHeaderPath has one resolver and one embedded-authority call"
Require-Count $promotionHeader 'ResolveEmbeddedProductPromotionKernelMask(' 1 `
	"$promotionHeaderPath exposes one ordinary-product authority entry point"
Require-Count $promotionHeader 'RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK' 3 `
	"$promotionHeaderPath defaults and consumes only the dedicated v2 build definition"
Require-Count $promotionHeader 'kSchemaVersion' 2 `
	"$promotionHeaderPath constructs and validates the current lockstep-v2 schema"
Require-Count $promotionHeader 'kProtocolEpoch' 2 `
	"$promotionHeaderPath constructs and validates the current lockstep-v2 protocol epoch"
if ($promotionHeader -notmatch '(?s)authority\.promotedKernelMask != liveIntegratedKernelMask.*?return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;') {
    $failures.Add("$promotionHeaderPath must fail closed unless the complete live kernel mask was promoted")
}

$configBuildPath = 'cmake/config-build.cmake'
$configBuild = Read-Source $configBuildPath
Require-Count $configBuild 'option(RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY' 1 `
	"$configBuildPath exposes one boolean post-gate promotion configuration"
Require-Count $configBuild 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_PROMOTED_KERNEL_MASK' 0 `
	"$configBuildPath does not reuse the v1 promotion namespace"
Require-Count $configBuild 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_SOURCE_REVISION' 0 `
	"$configBuildPath does not reuse the v1 source-trust namespace"
Require-Count $configBuild 'set(RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK 0)' 1 `
	"$configBuildPath defaults the dedicated v2 product authority to serial"
Require-Count $configBuild 'set(RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK 63)' 1 `
	"$configBuildPath promotes exactly the six lockstep-v2-qualified kernels"
Require-Count $configBuild 'RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK=' 1 `
	"$configBuildPath embeds the dedicated v2 authority in the product compilation"
Require-Count $configBuild 'CMAKE_SIZEOF_VOID_P EQUAL 8' 2 `
	"$configBuildPath requires and records the native x64 promotion boundary"
Require-Count $configBuild 'RTS_BUILD_OPTION_DEBUG OR RTS_BUILD_OPTION_PROFILE OR' 1 `
	"$configBuildPath rejects debug, profile, and sanitizer promotion lanes"
Require-Count $configBuild 'RTS_BUILD_OPTION_PROFILE_TRACY OR RTS_BUILD_OPTION_ASAN' 1 `
	"$configBuildPath rejects Tracy profiling promotion lanes"
Require-Count $configBuild 'status --porcelain --untracked-files=all' 0 `
	"$configBuildPath does not derive product authority from mutable worktree state"

$presetsPath = 'CMakePresets.json'
$presets = Read-Source $presetsPath
$presetDocument = $presets | ConvertFrom-Json
$x64ReleasePreset = @($presetDocument.configurePresets | Where-Object {
        [string]$_.name -ceq 'x64'
    })
if ($x64ReleasePreset.Count -ne 1 -or
    [string]$x64ReleasePreset[0].cacheVariables.RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY -cne 'ON') {
    $failures.Add("$presetsPath must enable lockstep-v2 promotion in the native x64 Release product preset")
}
foreach ($nonReleasePreset in @('x64-debug', 'x64-profile')) {
    $matchingPreset = @($presetDocument.configurePresets | Where-Object {
            [string]$_.name -ceq $nonReleasePreset
        })
    if ($matchingPreset.Count -ne 1 -or
        [string]$matchingPreset[0].cacheVariables.RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY -cne 'OFF') {
        $failures.Add("$presetsPath must keep $nonReleasePreset outside the promoted product boundary")
    }
}

$gameEngineCmakePath = 'Core/GameEngine/CMakeLists.txt'
$gameEngineCmake = Read-Source $gameEngineCmakePath
Require-Count $gameEngineCmake 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_PROMOTED_KERNEL_MASK=' 0 `
	"$gameEngineCmakePath has no live v1 authority embedding"
Require-Count $gameEngineCmake 'RTS_MULTIPLAYER_SIMULATION_TRUSTED_SOURCE_REVISION=' 0 `
	"$gameEngineCmakePath has no v1 source trust embedding"

$installedPeerPath = 'Core/GameEngine/Source/GameNetwork/InstalledNet3Validation.cpp'
$installedPeer = Read-Source $installedPeerPath
Require-Count $installedPeer 'EncodeNetworkHello(' 2 `
    "$installedPeerPath exchanges one actual NET3 Hello and Ack record"
Require-Count $installedPeer 'DecodeAndValidateNetworkHelloRecord(' 2 `
    "$installedPeerPath independently validates every peer Hello and Ack record"
Require-Count $installedPeer 'VALIDATION_TIMEOUT_MILLISECONDS = 120000' 1 `
    "$installedPeerPath bounds every local-only session wait"
Require-Count $installedPeer 'SelectMultiplayerSimulationNonProductTestOverrideMask(' 1 `
    "$installedPeerPath confines positive policy bits to the explicit non-product lane"
Require-Count $installedPeer 'CalculateCurrentExecutableSha256(' 2 `
    "$installedPeerPath self-hashes the exact installed process before participation"

$installedRunnerPath = 'Core/Tools/DeterministicSimulationValidation/Invoke-InstalledNet3LoopbackValidation.ps1'
$installedRunner = Read-Source $installedRunnerPath
Require-Count $installedRunner 'Start-Process -FilePath $executable' 1 `
    "$installedRunnerPath starts only the exact supplied product executable"
Require-Count $installedRunner 'Get-Process -Id' 0 `
    "$installedRunnerPath never reacquires a live peer through a reusable PID"
Require-Count $installedRunner '$Process.Kill()' 1 `
    "$installedRunnerPath terminates a timed-out peer through its retained process handle"
Require-Count $installedRunner '$Process.WaitForExit($PostKillWaitMilliseconds)' 1 `
    "$installedRunnerPath bounds retained-handle cleanup after termination"
Require-Count $installedRunner '$Process.Dispose()' 1 `
    "$installedRunnerPath releases every retained peer process handle"
Require-Count $installedRunner '-WindowStyle Hidden' 1 `
    "$installedRunnerPath keeps the scoped headless peer mode noninteractive"
Require-Count $installedRunner '-installedNet3Validation' 1 `
    "$installedRunnerPath invokes only the explicit installed-product validation mode"
Require-Count $installedRunner 'WaitForExit($remaining)' 1 `
    "$installedRunnerPath uses a bounded process completion wait"
Require-Count $installedRunner '$matches.Count -ne 16' 1 `
    "$installedRunnerPath refuses an incomplete match matrix"
Require-Count $installedRunner '$rawIndexEntries.Count -ne 40' 1 `
    "$installedRunnerPath refuses an incomplete peer-process matrix"

$wrapperListPath = 'Core/GameEngine/Source/GameNetwork/NetCommandWrapperList.cpp'
$wrapperList = Read-Source $wrapperListPath
Require-Count $wrapperList 'IsWrappedNetworkCommandOriginAuthorized(temp->getPlayerID(),' 1 `
    "$wrapperListPath binds every reconstructed command to its authenticated wrapper origin"
if ($wrapperList -notmatch '(?s)IsWrappedNetworkCommandOriginAuthorized\(temp->getPlayerID\(\),.*?msg->getCommand\(\)->getPlayerID\(\), MAX_SLOTS\).*?retlist->addMessage') {
    $failures.Add("$wrapperListPath must reject an inner origin mismatch before ready-list publication")
}

$networkFacadePath = 'Core/GameEngine/Source/GameNetwork/Network.cpp'
$networkFacade = Read-Source $networkFacadePath
Require-Count $networkFacade 'Bool Network::isNetworkSimulationPolicyUsable()' 1 `
    "$networkFacadePath exposes one central policy-usability route"
Require-Count $networkFacade 'm_conMgr->isNetworkSimulationPolicyUsable();' 1 `
    "$networkFacadePath delegates usability to the connection roster owner"
Require-Count $networkFacade 'MULTIPLAYER_SIMULATION_POLICY_SERIAL_NETWORK_UNAVAILABLE' 1 `
    "$networkFacadePath fails closed when the connection manager is absent"

$networkInterfacePath = 'Core/GameEngine/Include/GameNetwork/NetworkInterface.h'
$networkInterface = Read-Source $networkInterfacePath
Require-Count $networkInterface 'virtual Bool isNetworkSimulationPolicyUsable() = 0;' 1 `
    "$networkInterfacePath publishes one central live policy-usability contract"

$policyHeaderPath = 'Core/Libraries/Include/Lib/MultiplayerSimulationPolicy.h'
$policyHeader = Read-Source $policyHeaderPath
Require-Count $policyHeader 'MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK =' 1 `
    "$policyHeaderPath separates live integration from known kernels"
Require-Count $policyHeader 'MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK =' 1 `
    "$policyHeaderPath declares a distinct fail-closed release-proven default"
Require-Count $policyHeader 'struct MultiplayerSimulationGeneratedReleaseProof' 1 `
    "$policyHeaderPath models only generated release-proof authority"
Require-Count $policyHeader 'ResolveMultiplayerSimulationGeneratedReleaseProofMask(' 1 `
    "$policyHeaderPath validates exact generated proof fields before release advertisement"
Require-Count $policyHeader 'ResolveMultiplayerSimulationReleaseProvenKernelMask(' 0 `
    "$policyHeaderPath removes the old free-form proof resolver"
Require-Count $policyHeader 'SelectMultiplayerSimulationNonProductTestOverrideMask(' 1 `
    "$policyHeaderPath names the positive harness override as non-product"
Require-Count $policyHeader 'IsMultiplayerSimulationPolicyLifecycleUsable(' 1 `
    "$policyHeaderPath centralizes resolved hello and exact-roster lifecycle validation"
Require-Count $policyHeader 'presentRemoteMask == expectedRemoteMask' 1 `
    "$policyHeaderPath requires every and only the original remote roster to remain present"
Require-Count $policyHeader 'quittingRemoteMask == 0' 1 `
    "$policyHeaderPath rejects the first quitting remote"
if ($policyHeader -notmatch '(?s)MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK\s*=\s*MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK') {
    $failures.Add("$policyHeaderPath includes every reviewed live kernel, including PATH, in the candidate mask")
}
if ($policyHeader -notmatch '(?s)MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK\s*=\s*MULTIPLAYER_SIMULATION_KERNEL_NONE') {
    $failures.Add("$policyHeaderPath keeps the default release-proven mask serial")
}

$policyTestPath = 'Core/Tools/MixedWorkerMultiplayerTest/MixedWorkerMultiplayerTest.cpp'
$policyTest = Read-Source $policyTestPath
Require-Count $policyTest 'wrong generated release-proof schema is rejected' 1 `
    "$policyTestPath rejects unsupported generated proof schema"
Require-Count $policyTest 'missing generated source revision is rejected' 1 `
    "$policyTestPath rejects a missing exact source identity"
Require-Count $policyTest 'missing generated title executable digest is rejected' 1 `
    "$policyTestPath rejects a missing title artifact identity"
Require-Count $policyTest 'missing generated second-title executable digest is rejected' 1 `
    "$policyTestPath requires evidence for both installed title artifacts"
Require-Count $policyTest 'missing generated artifact-set digest is rejected' 1 `
    "$policyTestPath rejects a missing exact artifact-set identity"
Require-Count $policyTest 'all-zero generated evidence-manifest digest is rejected' 1 `
    "$policyTestPath rejects an empty-value manifest identity"
Require-Count $policyTest 'unknown generated release-proof kernel bits are rejected' 1 `
    "$policyTestPath rejects generated proof bits outside live integrations"
Require-Count $policyTest 'empty generated release-proof kernel mask stays serial' 1 `
    "$policyTestPath keeps an empty generated proof fail-closed"
Require-Count $policyTest 'a partial generated proof unlocks only its separately proven kernel' 1 `
    "$policyTestPath permits only separately evidenced release bits"
Require-Count $policyTest 'policy soak uses an explicit non-product positive override' 1 `
    "$policyTestPath labels its positive release-proof override"
Require-Count $policyTest 'resolved policy is usable only with the exact live roster' 1 `
    "$policyTestPath covers the central positive lifecycle state"
Require-Count $policyTest 'reset or revoked policy is unavailable' 1 `
    "$policyTestPath covers reset and explicit revocation"
Require-Count $policyTest 'rejected session policy is unavailable' 1 `
    "$policyTestPath covers resolved policy failure"
Require-Count $policyTest 'first missing peer revokes policy usability' 1 `
    "$policyTestPath covers immediate disconnect fail-closed behavior"
Require-Count $policyTest 'first quitting peer revokes policy usability' 1 `
    "$policyTestPath covers immediate quitting fail-closed behavior"
Require-Count $policyTest 'policy cannot renegotiate a reduced roster after resolution' 1 `
    "$policyTestPath rejects reduced-roster renegotiation"

$wireTestPath = 'Core/Tools/NetworkWireContractTest/NetworkWireContractTest.cpp'
$wireTest = Read-Source $wireTestPath
Require-Count $wireTest 'reviewed lockstep-v2 promotion grants ordinary product authority' 1 `
	"$wireTestPath covers positive ordinary product promotion"
Require-Count $wireTest 'ordinary NET3 hello advertises the reviewed lockstep-v2 product mask' 1 `
	"$wireTestPath covers positive ordinary NET3 advertisement"
Require-Count $wireTest 'matching ordinary promoted peers resolve all six worker kernels' 1 `
	"$wireTestPath covers positive ordinary session-policy authority"
Require-Count $wireTest 'InstalledNet3Validation v1 remains diagnostic even with a nonzero caller mask' 1 `
	"$wireTestPath preserves the diagnostic-v1 negative authority proof"

$wireTestCMakePath = 'Core/Tools/NetworkWireContractTest/CMakeLists.txt'
$wireTestCMake = Read-Source $wireTestCMakePath
Require-Count $wireTestCMake 'core_config' 1 `
	"$wireTestCMakePath validates the same embedded promotion definition as the product graph"
Require-Count $wireTestCMake 'core_task_runtime' 1 `
	"$wireTestCMakePath links the ordinary session-policy resolver exercised by the positive promotion test"

$loopbackHeaderPath = 'Core/Tools/MixedWorkerMultiplayerTest/MixedWorkerMultiplayerLoopbackContract.h'
$loopbackHeader = Read-Source $loopbackHeaderPath
Require-Count $loopbackHeader 'unsigned seedValue;' 1 `
    "$loopbackHeaderPath binds each peer record to the exact nonzero seed"
Require-Count $loopbackHeader 'MixedWorkerLoopbackTitle title;' 1 `
    "$loopbackHeaderPath binds each peer record to the exact title"
Require-Count $loopbackHeader 'char sourceRevision[' 1 `
    "$loopbackHeaderPath binds each peer record to the exact source revision"
Require-Count $loopbackHeader 'char executableSha256[' 1 `
    "$loopbackHeaderPath binds each peer record to the exact title executable"
Require-Count $loopbackHeader 'char artifactSetSha256[' 1 `
    "$loopbackHeaderPath binds each peer record to the exact artifact set"
Require-Count $loopbackHeader 'unsigned physicalKernelMask;' 1 `
    "$loopbackHeaderPath records physical execution separately for every kernel bit"
Require-Count $loopbackHeader 'unsigned kernelSubmittedJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];' 1 `
    "$loopbackHeaderPath records per-kernel submitted work"
Require-Count $loopbackHeader 'unsigned kernelCompletedJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];' 1 `
    "$loopbackHeaderPath records per-kernel completed work"
Require-Count $loopbackHeader 'unsigned kernelPhysicalJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];' 1 `
    "$loopbackHeaderPath records per-kernel physical work"
Require-Count $loopbackHeader 'unsigned kernelPhysicalWorkerMask[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];' 1 `
    "$loopbackHeaderPath records per-kernel physical worker identity"

$loopbackContractPath = 'Core/Tools/MixedWorkerMultiplayerTest/MixedWorkerMultiplayerLoopbackContract.cpp'
$loopbackContract = Read-Source $loopbackContractPath
$loopbackKernelNames = @(
    'MULTIPLAYER_SIMULATION_KERNEL_PHYSICS',
    'MULTIPLAYER_SIMULATION_KERNEL_STATUS',
    'MULTIPLAYER_SIMULATION_KERNEL_COLLISION',
    'MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING',
    'MULTIPLAYER_SIMULATION_KERNEL_SPATIAL',
    'MULTIPLAYER_SIMULATION_KERNEL_PATH'
)
foreach ($loopbackKernelName in $loopbackKernelNames) {
    Require-Count $loopbackContract $loopbackKernelName 1 `
        "$loopbackContractPath accounts for physical evidence from $loopbackKernelName exactly once"
}
Require-Count $loopbackContract 'physical != submitted' 1 `
    "$loopbackContractPath requires balanced submitted, completed, and physical work"
Require-Count $loopbackContract 'distinctWorkers <= 1' 1 `
    "$loopbackContractPath requires more than one physical worker for multi-worker peers"
Require-Count $loopbackContract 'record.physicalKernelMask == expectedPhysicalKernelMask' 1 `
    "$loopbackContractPath requires physical evidence for every enabled bit and no others"
Require-Count $loopbackContract '0x00005a17u' 1 `
    "$loopbackContractPath fixes the first nonzero canonical seed"
Require-Count $loopbackContract '0x0000c0deu' 1 `
    "$loopbackContractPath fixes the second nonzero canonical seed"

Require-Count $policyTest 'zero per-kernel submitted work is rejected' 1 `
    "$policyTestPath rejects missing physical submission evidence"
Require-Count $policyTest 'unbalanced per-kernel completion evidence is rejected' 1 `
    "$policyTestPath rejects unbalanced completion evidence"
Require-Count $policyTest 'unbalanced per-kernel physical work evidence is rejected' 1 `
    "$policyTestPath rejects nonphysical claimed work"
Require-Count $policyTest 'single-worker per-kernel evidence is rejected for a multi-worker peer' 1 `
    "$policyTestPath rejects insufficient physical worker diversity"
Require-Count $policyTest 'forced-one-worker peer must report zero physical kernel work' 1 `
    "$policyTestPath rejects physical execution claimed by a forced-one peer"

$pathRuntimePath = 'Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp'
$pathRuntime = Read-Source $pathRuntimePath
Require-Count $pathRuntime 'GameNetwork/MultiplayerSimulationRuntimePolicy.h' 1 `
    "$pathRuntimePath includes the live session policy adapter exactly once"
Require-Count $pathRuntime 'MULTIPLAYER_SIMULATION_KERNEL_PATH' 1 `
    "$pathRuntimePath routes path authority through one live policy predicate"
Require-Count $pathRuntime 'policy.multiplayerPolicyEnabled = policy.networkGame &&' 1 `
    "$pathRuntimePath preserves non-network multiplayer serial fallback"

$pathPolicyPath = 'Core/Libraries/Source/TaskRuntime/DeterministicPathSearch.cpp'
$pathPolicy = Read-Source $pathPolicyPath
Require-Count $pathPolicy '!policy.networkGame || !policy.multiplayerPolicyEnabled' 1 `
    "$pathPolicyPath requires explicit network session permission"

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) {
        Write-Error $failure
    }
    exit 1
}

Write-Output 'Mixed-worker multiplayer live source audit passed.'
