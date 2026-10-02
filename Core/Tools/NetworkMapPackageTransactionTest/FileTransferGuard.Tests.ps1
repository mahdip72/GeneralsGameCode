param([Parameter(Mandatory = $true)][string]$SourceRoot)

$transferPath = Join-Path $SourceRoot 'Core/GameEngine/Source/GameNetwork/FileTransfer.cpp'
$receiverPath = Join-Path $SourceRoot 'Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp'
$transfer = Get-Content -LiteralPath $transferPath -Raw
$receiver = Get-Content -LiteralPath $receiverPath -Raw

function Require([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

$start = $transfer.IndexOf('Bool DoAnyMapTransfers(')
Require ($start -ge 0) 'missing map-transfer entrypoint'
$body = $transfer.Substring($start)
$hostOwner = $body.IndexOf('std::unique_ptr<rts::network_epoch::NetworkMapPackageTransaction::ReadGuard>')
$hostBranch = $body.IndexOf('if (game->amIHost())')
$hostRead = $body.IndexOf('hostRead.reset(new rts::network_epoch::NetworkMapPackageTransaction::ReadGuard(')
$sendMap = $body.IndexOf('doFileTransfer(game->getMap(), ls, mask)')
Require ($hostOwner -ge 0 -and $hostOwner -lt $hostBranch -and
    $hostRead -gt $hostBranch -and $sendMap -gt $hostRead) `
    'host source must be guarded before map streaming'

$open = $body.IndexOf('{', $hostBranch)
Require ($open -ge 0) 'missing host-only lock scope'
$depth = 0
$close = -1
for ($i = $open; $i -lt $body.Length; ++$i) {
    if ($body[$i] -eq '{') { ++$depth }
    elseif ($body[$i] -eq '}') {
        --$depth
        if ($depth -eq 0) { $close = $i; break }
    }
}
Require ($close -gt $hostRead -and $close -lt $sendMap) `
    'receiver must leave the host-only lock branch before network pump'
Require (-not $body.Substring(0, $sendMap).Contains('ReadGuard mapRead(')) `
    'an unconditional map read guard would block the receiver commit'
$noTransfer = $body.IndexOf('if (!mask)')
$localRead = $body.IndexOf('ReadGuard localRead(')
Require ($noTransfer -gt $close -and $localRead -gt $noTransfer) `
    'receiver validation may lock only in a short local-read scope'
Require ($body.Contains('decision == rts::network_epoch::NetworkSidecarTransferDecision::Transfer') -and
    $body.Contains('!TheGameInfo->getConstSlot(i)->hasMap()') -and
    $body.Contains('doFileTransfer(game->getMap(), ls, mask)')) `
    'missing-map and differing-sidecar clients must still reach final-map transfer'

$receiveStart = $receiver.IndexOf('void ConnectionManager::processFile(')
Require ($receiveStart -ge 0) 'missing map-file receiver'
$receive = $receiver.Substring($receiveStart)
Require ($receive.Contains('m_pendingMapPackage.commit(validateCommittedNetworkMap') -and
    $receive.IndexOf('m_pendingMapPackage.commit(validateCommittedNetworkMap') -lt
    $receive.IndexOf('processFileProgress(progressMsg)')) `
    'the receiver must commit its package before acknowledging transfer'

Write-Host 'NET3 host-only transfer guard contract passed.'
