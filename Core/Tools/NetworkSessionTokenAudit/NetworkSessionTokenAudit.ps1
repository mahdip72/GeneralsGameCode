param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-TokenViolations {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RuntimeCMake
    )

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $start = $Source.IndexOf('static Bool generateNetworkHelloToken(', [StringComparison]::Ordinal)
    $end = if ($start -ge 0) {
        $Source.IndexOf("#endif", $start, [StringComparison]::Ordinal)
    } else { -1 }
    if ($start -lt 0 -or $end -lt 0) {
        $violations.Add('native session-token generator is missing or unguarded')
        return @($violations)
    }
    $generator = $Source.Substring($start, $end - $start)
    foreach ($required in @(
        'BCryptGenRandom(nullptr,',
        'reinterpret_cast<PUCHAR>(token)',
        'static_cast<ULONG>(sizeof(*token))',
        'BCRYPT_USE_SYSTEM_PREFERRED_RNG',
        'status != 0 || *token == 0U',
        '*token = 0U;')) {
        if ($generator.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("missing fail-closed CSPRNG contract '$required'")
        }
    }
    foreach ($forbidden in @('rand(', 'srand(', 'timeGetTime(', 'QueryPerformanceCounter(')) {
        if ($generator.IndexOf($forbidden, [StringComparison]::Ordinal) -ge 0) {
            $violations.Add("session-token generator uses forbidden fallback '$forbidden'")
        }
    }

    $beginStart = $Source.IndexOf('void ConnectionManager::beginNetworkHello()', [StringComparison]::Ordinal)
    $beginEnd = if ($beginStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::serviceNetworkHello()', $beginStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($beginStart -lt 0 -or $beginEnd -lt 0) {
        $violations.Add('network handshake startup is missing')
    } else {
        $begin = $Source.Substring($beginStart, $beginEnd - $beginStart)
        $generateIndex = $begin.IndexOf('generateNetworkHelloToken(&m_networkHelloLocalToken)',
            [StringComparison]::Ordinal)
        $sendIndex = $begin.IndexOf('sendNetworkHello(i);', [StringComparison]::Ordinal)
        if ($generateIndex -lt 0 -or $sendIndex -lt 0 -or $generateIndex -gt $sendIndex) {
            $violations.Add('network handshake must generate one token before its initial Hello sends')
        }
    }

    $ackStart = $Source.IndexOf('Bool ConnectionManager::sendNetworkHelloAck(',
        [StringComparison]::Ordinal)
    $ackEnd = if ($ackStart -ge 0) {
        $Source.IndexOf('Int ConnectionManager::findNetworkHelloSlot(', $ackStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($ackStart -lt 0 -or $ackEnd -lt 0) {
        $violations.Add('network Hello acknowledgement path is missing')
    } else {
        $ack = $Source.Substring($ackStart, $ackEnd - $ackStart)
        if ($ack.IndexOf('m_networkHelloRemoteToken[slot]', [StringComparison]::Ordinal) -lt 0 -or
            $ack.IndexOf('NetworkHelloKind::Ack', [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('network Ack must echo the validated peer challenge')
        }
    }

    $processStart = $Source.IndexOf('Bool ConnectionManager::processNetworkHello(',
        [StringComparison]::Ordinal)
    $processEnd = if ($processStart -ge 0) {
        $Source.IndexOf("#endif", $processStart, [StringComparison]::Ordinal)
    } else { -1 }
    if ($processStart -lt 0 -or $processEnd -lt 0) {
        $violations.Add('network Hello processing path is missing')
    } else {
        $process = $Source.Substring($processStart, $processEnd - $processStart)
        $decodeIndex = $process.IndexOf('DecodeAndValidateNetworkHelloRecord(',
            [StringComparison]::Ordinal)
        $slotIndex = $process.IndexOf('findNetworkHelloSlot(', [StringComparison]::Ordinal)
        $tokenIndex = $process.IndexOf('IsNetworkHelloSessionTokenAccepted(',
            [StringComparison]::Ordinal)
        $replaceIndex = $process.IndexOf('m_networkHelloRemoteToken[slot] = receivedSessionToken;',
            [StringComparison]::Ordinal)
        $ackIndex = $process.IndexOf('sendNetworkHelloAck(slot);', [StringComparison]::Ordinal)
        if ($decodeIndex -lt 0 -or $slotIndex -le $decodeIndex -or
            $tokenIndex -le $slotIndex -or $replaceIndex -le $tokenIndex -or
            $ackIndex -le $replaceIndex) {
            $violations.Add('network Hello state must validate integrity before slot and token commit')
        }
        $helloCommitIndex = $process.IndexOf('if (kind == rts::network_epoch::NetworkHelloKind::Hello)',
            [StringComparison]::Ordinal)
        if ($tokenIndex -ge 0 -and $helloCommitIndex -gt $tokenIndex) {
            $staleBranch = $process.Substring($tokenIndex, $helloCommitIndex - $tokenIndex)
            if ($staleBranch.IndexOf('rejectNetworkHello(', [StringComparison]::Ordinal) -ge 0) {
                $violations.Add('stale session tokens must remain nonfatal')
            }
        }
    }

    $relayStart = $Source.IndexOf('void ConnectionManager::doRelay()', [StringComparison]::Ordinal)
    $relayEnd = if ($relayStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::update(', $relayStart, [StringComparison]::Ordinal)
    } else { -1 }
    if ($relayStart -lt 0 -or $relayEnd -lt 0) {
        $violations.Add('network relay gate is missing')
    } else {
        $relay = $Source.Substring($relayStart, $relayEnd - $relayStart)
        $failureIndex = $relay.IndexOf('if (m_networkHelloFailed)', [StringComparison]::Ordinal)
        $parseIndex = $relay.IndexOf('NetPacket packet(', [StringComparison]::Ordinal)
        if ($failureIndex -lt 0 -or $parseIndex -lt 0 -or $failureIndex -gt $parseIndex -or
            $relay.IndexOf('m_transport->m_inBuffer[i].length = 0;', $failureIndex,
                [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('network relay does not fail closed before gameplay packet parsing')
        }
        $prefixIndex = $relay.IndexOf('HasNetworkHelloPrefix(', [StringComparison]::Ordinal)
        $magicIndex = $relay.IndexOf('HasNetworkHelloMagic(', [StringComparison]::Ordinal)
        if ($prefixIndex -lt 0 -or $magicIndex -le $prefixIndex -or $parseIndex -le $magicIndex) {
            $violations.Add('network relay must quarantine every NET3 prefix before gameplay parsing')
        }
    }

    $nativeStart = $RuntimeCMake.IndexOf('elseif(RTS_BUILD_PRODUCT', [StringComparison]::Ordinal)
    if ($nativeStart -lt 0) {
        $violations.Add('native runtime dependency block is missing')
    } else {
        $nativeBlock = $RuntimeCMake.Substring($nativeStart)
        if ($nativeBlock -notmatch '(?m)^        bcrypt$') {
            $violations.Add('native runtime dependency block does not link bcrypt')
        }
        $win32Block = $RuntimeCMake.Substring(0, $nativeStart)
        if ($win32Block -match '(?m)^\s*bcrypt$') {
            $violations.Add('Win32 legacy runtime must not link bcrypt')
        }
    }
    return @($violations)
}

if ($SelfTest) {
    $goodSource = @'
#if defined(_WIN64)
static Bool generateNetworkHelloToken(std::uint64_t *token)
{
    *token = 0U;
    const NTSTATUS status = BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(token), static_cast<ULONG>(sizeof(*token)),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0 || *token == 0U) { *token = 0U; return FALSE; }
    return TRUE;
}
#endif
void ConnectionManager::beginNetworkHello() {
    generateNetworkHelloToken(&m_networkHelloLocalToken);
    sendNetworkHello(i);
}
void ConnectionManager::serviceNetworkHello() {}
Bool ConnectionManager::sendNetworkHelloAck() {
    EncodeNetworkHello(m_networkHelloRemoteToken[slot], NetworkHelloKind::Ack);
}
Int ConnectionManager::findNetworkHelloSlot() { return 0; }
Bool ConnectionManager::processNetworkHello() {
    DecodeAndValidateNetworkHelloRecord(message);
    findNetworkHelloSlot(identity);
    if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { return FALSE; }
    if (kind == rts::network_epoch::NetworkHelloKind::Hello) {
        m_networkHelloRemoteToken[slot] = receivedSessionToken;
        sendNetworkHelloAck(slot);
    }
}
#endif
void ConnectionManager::doRelay() {
    if (m_networkHelloFailed) {
        m_transport->m_inBuffer[i].length = 0;
        return;
    }
    if (HasNetworkHelloPrefix(message)) {
        if (!HasNetworkHelloMagic(message)) { return; }
    }
    NetPacket packet(message);
}
void ConnectionManager::update() {}
'@
    $goodCMake = @'
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(runtime INTERFACE legacy)
elseif(RTS_BUILD_PRODUCT)
    target_link_libraries(runtime INTERFACE
        bcrypt
    )
endif()
'@
    if ((Get-TokenViolations $goodSource $goodCMake).Count -ne 0) {
        throw 'known-good session-token fixture failed'
    }
    $permissiveStatus = $goodSource.Replace('status != 0', 'status < 0')
    if (-not ((Get-TokenViolations $permissiveStatus $goodCMake) -match 'fail-closed')) {
        throw 'permissive BCrypt status fixture was not rejected'
    }
    $fallback = $goodSource.Replace('BCryptGenRandom(nullptr,', 'rand(); BCryptGenRandom(nullptr,')
    if (-not ((Get-TokenViolations $fallback $goodCMake) -match 'forbidden fallback')) {
        throw 'fallback RNG fixture was not rejected'
    }
    $failOpen = $goodSource.Replace('if (m_networkHelloFailed)', 'if (false)')
    if (-not ((Get-TokenViolations $failOpen $goodCMake) -match 'fail closed')) {
        throw 'fail-open relay fixture was not rejected'
    }
    $slotBeforeIntegrity = $goodSource.Replace(
        "    DecodeAndValidateNetworkHelloRecord(message);`n    findNetworkHelloSlot(identity);",
        "    findNetworkHelloSlot(identity);`n    DecodeAndValidateNetworkHelloRecord(message);")
    if (-not ((Get-TokenViolations $slotBeforeIntegrity $goodCMake) -match 'validate integrity')) {
        throw 'identity-before-integrity fixture was not rejected'
    }
    $fatalStale = $goodSource.Replace(
        'if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { return FALSE; }',
        'if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { rejectNetworkHello(slot); return FALSE; }')
    if (-not ((Get-TokenViolations $fatalStale $goodCMake) -match 'nonfatal')) {
        throw 'fatal stale-token fixture was not rejected'
    }
    $localAck = $goodSource.Replace(
        'EncodeNetworkHello(m_networkHelloRemoteToken[slot], NetworkHelloKind::Ack);',
        'EncodeNetworkHello(m_networkHelloLocalToken, NetworkHelloKind::Ack);')
    if (-not ((Get-TokenViolations $localAck $goodCMake) -match 'echo')) {
        throw 'non-echoing Ack fixture was not rejected'
    }
    $missingPrefix = $goodSource.Replace('HasNetworkHelloPrefix(message)',
        'HasNetworkHelloMagic(message)')
    if (-not ((Get-TokenViolations $missingPrefix $goodCMake) -match 'quarantine')) {
        throw 'missing NET3 prefix quarantine fixture was not rejected'
    }
    $win32Leak = $goodCMake.Replace('target_link_libraries(runtime INTERFACE legacy)',
        "target_link_libraries(runtime INTERFACE legacy)`n        bcrypt")
    if (-not ((Get-TokenViolations $goodSource $win32Leak) -match 'Win32')) {
        throw 'Win32 bcrypt leak fixture was not rejected'
    }
    Write-Output 'Network session-token audit self-tests passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw '-SourceRoot is required unless -SelfTest is used.'
}
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$source = [IO.File]::ReadAllText((Join-Path $root 'Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp'))
$runtimeCMake = [IO.File]::ReadAllText((Join-Path $root 'cmake/legacy-product-runtime.cmake'))
$violations = @(Get-TokenViolations $source $runtimeCMake)
if ($violations.Count -ne 0) {
    $violations | Write-Output
    exit 1
}
Write-Output 'Network session-token CSPRNG and dependency audit passed.'
