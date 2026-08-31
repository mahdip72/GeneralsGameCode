param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-TokenViolations {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RuntimeCMake,
        [string]$NATSource = ''
    )

    # Audit source structure independently of the caller's checkout line-ending
    # policy. PowerShell here-strings and Windows worktrees commonly use CRLF,
    # while the exact-line expressions below intentionally operate on LF.
    $Source = $Source -replace "`r`n", "`n"
    $RuntimeCMake = $RuntimeCMake -replace "`r`n", "`n"
    $NATSource = $NATSource -replace "`r`n", "`n"

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

    $endpointStart = $Source.IndexOf('Bool ConnectionManager::matchesNetworkPeerEndpoint(',
        [StringComparison]::Ordinal)
    $endpointEnd = if ($endpointStart -ge 0) {
        $Source.IndexOf('Int ConnectionManager::findNetworkPeerEndpoint(', $endpointStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($endpointStart -lt 0 -or $endpointEnd -lt 0 -or
        $Source.Substring($endpointStart, $endpointEnd - $endpointStart).IndexOf(
            'IsMatchingNetworkPeerEndpoint(', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('ordinary gameplay endpoint binding must compare both source address and port')
    }

    $immediateStart = $Source.IndexOf('void ConnectionManager::sendLocalCommandImmediate(',
        [StringComparison]::Ordinal)
    $immediateEnd = if ($immediateStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::sendLocalCommandDirect(', $immediateStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($immediateStart -lt 0 -or $immediateEnd -lt 0) {
        $violations.Add('local command router fallback path is missing')
    } else {
        $immediate = $Source.Substring($immediateStart, $immediateEnd - $immediateStart)
        $routerHelperIndex = $immediate.IndexOf(
            'rts::network_epoch::IsNetworkPacketRouterEligible(', [StringComparison]::Ordinal)
        if ($routerHelperIndex -ge 0) {
            $routerGuardStart = $immediate.LastIndexOf('#if defined(_WIN64)', $routerHelperIndex,
                [StringComparison]::Ordinal)
            $routerGuardEnd = $immediate.IndexOf('#endif', $routerHelperIndex,
                [StringComparison]::Ordinal)
            if ($routerGuardStart -lt 0 -or $routerGuardEnd -lt $routerHelperIndex) {
                $violations.Add('x64 router helper use must be guarded from the Win32/VC6 lane')
            }
        }
        foreach ($required in @(
                'Bool packetRouterEligible = FALSE;',
                'packetRouterEligible = m_packetRouterSlot < MAX_SLOTS',
                'packetRouterHasConnection && !packetRouterIsQuitting')) {
            if ($immediate.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("local command router path is missing legacy-compatible fallback '$required'")
            }
        }
    }

    $transportStart = $Source.IndexOf('void ConnectionManager::processTransportMessage(',
        [StringComparison]::Ordinal)
    $transportEnd = if ($transportStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::doRelay()', $transportStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($transportStart -lt 0 -or $transportEnd -lt 0) {
        $violations.Add('ordinary gameplay source-binding path is missing')
    } else {
        $transport = $Source.Substring($transportStart, $transportEnd - $transportStart)
        if ($transport.IndexOf('findNetworkPeerEndpoint(message)', [StringComparison]::Ordinal) -lt 0 -or
            $transport.IndexOf('isNetworkCommandSourceAuthorized(cmd->getCommand(), sourceSlot)',
                [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('ordinary gameplay packets must bind the source endpoint and claimed origin')
        }
        foreach ($required in @(
                'IsNetworkFrameResendResponseAuthorized(',
                'm_frameResendRequestOutstanding',
                'm_frameResendRequestResponder',
                'm_frameResendRequestFrame',
                'm_frameResendRequestExpectedInfoMask,',
                'm_frameResendRequestExpectedInfoMask',
                'm_frameResendRequestReceivedInfoMask',
                'NETCOMMANDTYPE_GAMECOMMAND',
                'NETCOMMANDTYPE_FRAMEINFO',
                'getExecutionFrame()',
                'getFrameCommandCount(m_frameResendRequestFrame)',
                'getCommandCount(m_frameResendRequestFrame)',
                'IsNetworkFrameResendResponseComplete(',
                'frameResendExpired',
                'timeGetTime()',
                'clearNetworkFrameResendRequest()')) {
            if ($transport.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("frame resend admission is missing bounded provenance '$required'")
            }
        }
        if ($transport.IndexOf('if (!sourceAuthorized && !frameResendResponseAuthorized)',
                [StringComparison]::Ordinal) -lt 0 -or
            $transport.IndexOf('frameResendResponseAccepted', [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('frame resend provenance must remain an exception to strict source binding')
        }
        if ($transport.IndexOf('if (frameResendExpired)', [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('frame resend provenance must expire before admission')
        }
    }

    $resendStart = $Source.IndexOf('void ConnectionManager::requestFrameDataResend(',
        [StringComparison]::Ordinal)
    $resendEnd = if ($resendStart -ge 0) {
        $Source.IndexOf('msg->detach();', $resendStart, [StringComparison]::Ordinal)
    } else { -1 }
    if ($resendStart -lt 0 -or $resendEnd -lt 0) {
        $violations.Add('frame resend request provenance setup is missing')
    } else {
        $resendRequest = $Source.Substring($resendStart, $resendEnd - $resendStart)
        foreach ($required in @(
                'clearNetworkFrameResendRequest();',
                'm_frameResendRequestResponder =',
                'm_frameResendRequestFrame =',
                'm_frameResendRequestStartTime = static_cast<UnsignedInt>(timeGetTime())',
                'm_frameResendRequestExpectedInfoMask |=',
                'm_frameResendRequestOutstanding =',
                'sourceSlot != m_localSlot',
                'sendLocalCommandDirect(msg, 1 << playerID)')) {
            if ($resendRequest.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("frame resend request must establish bounded responder/frame provenance '$required'")
            }
        }
        if ($resendRequest.IndexOf('sourceSlot != playerID', [StringComparison]::Ordinal) -ge 0) {
            $violations.Add('frame resend origin mask must exclude the local requester, not the responder')
        }
    }

    $frameAuthStart = $Source.IndexOf(
        'inline bool IsNetworkFrameResendResponseAuthorized(', [StringComparison]::Ordinal)
    $frameAuthEnd = if ($frameAuthStart -ge 0) {
        $Source.IndexOf('inline bool IsNetworkFrameResendResponseComplete(', $frameAuthStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($frameAuthStart -lt 0 -or $frameAuthEnd -lt 0) {
        $violations.Add('frame resend authorization helper is missing')
    } else {
        $frameAuth = $Source.Substring($frameAuthStart, $frameAuthEnd - $frameAuthStart)
        foreach ($required in @(
                'expectedOriginMask',
                'claimedSlot < maxSlots',
                'expectedOriginMask & (1U << claimedSlot)')) {
            if ($frameAuth.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("frame resend authorization must require the expected origin mask '$required'")
            }
        }
    }

    $authStart = $Source.IndexOf('Bool ConnectionManager::isNetworkCommandSourceAuthorized(',
        [StringComparison]::Ordinal)
    $authEnd = if ($authStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::rejectNetworkHello(', $authStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($authStart -lt 0 -or $authEnd -lt 0) {
        $violations.Add('network command source authorization path is missing')
    } else {
        $auth = $Source.Substring($authStart, $authEnd - $authStart)
        foreach ($required in @(
            'packetRouterSlot = MAX_SLOTS',
            'm_packetRouterSlot < MAX_SLOTS',
            'm_packetRouterSlot != m_localSlot',
            'm_connections[m_packetRouterSlot] != nullptr',
            '!m_connections[m_packetRouterSlot]->isQuitting()')) {
            if ($auth.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("router source authorization must verify active router provenance '$required'")
            }
        }
    }

    if ($processStart -ge 0 -and $processEnd -gt $processStart) {
        $malformedDropIndex = $process.IndexOf('dropInvalidNetworkHelloPacket(candidateSlot',
            [StringComparison]::Ordinal)
        if ($malformedDropIndex -lt 0 -or
            $process.IndexOf('rejectNetworkHello(', [StringComparison]::Ordinal) -ge 0) {
            $violations.Add('malformed NET3 records must use a nonfatal drop-only handler')
        }
    }

    $dropStart = $Source.IndexOf('void ConnectionManager::dropInvalidNetworkHelloPacket(',
        [StringComparison]::Ordinal)
    $dropEnd = if ($dropStart -ge 0) {
        $Source.IndexOf('Bool ConnectionManager::isNetworkHelloCandidate(', $dropStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($dropStart -lt 0 -or $dropEnd -lt 0) {
        $violations.Add('network invalid-packet drop path is missing')
    } else {
        $drop = $Source.Substring($dropStart, $dropEnd - $dropStart)
        foreach ($forbidden in @(
                'm_networkHelloExpectedSlots', 'm_networkHelloValidated',
                'm_networkHelloAckReceived', 'm_networkHelloRemoteToken',
                'm_networkHelloRequired', 'm_networkHelloFailed',
                'm_networkHelloDeferred', 'm_networkHelloPending',
                'm_packetRouterSlot', 'm_packetRouterFallback',
                'm_frameData', 'setFrameData(', 'setQuitFrame(',
                'm_connections[', 'setQuitting()', 'rejectNetworkHello(')) {
            if ($drop.IndexOf($forbidden, [StringComparison]::Ordinal) -ge 0) {
                $violations.Add('invalid NET3 packet handler must not mutate membership or fail the session')
                break
            }
        }
    }

    $serviceStart = $Source.IndexOf('void ConnectionManager::serviceNetworkHello()',
        [StringComparison]::Ordinal)
    $serviceEnd = if ($serviceStart -ge 0) {
        $Source.IndexOf('Bool ConnectionManager::sendNetworkHello(', $serviceStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($serviceStart -lt 0 -or $serviceEnd -lt 0) {
        $violations.Add('network handshake retry/timeout service is missing')
    } else {
        $service = $Source.Substring($serviceStart, $serviceEnd - $serviceStart)
        foreach ($required in @(
                'm_networkHelloExpectedSlots', 'm_networkHelloAttempts',
                'm_networkHelloLastSend', 'IsNetworkHelloTimedOut(',
                'IsNetworkHelloRetryDue(', 'IsNetworkHelloAttemptLimitReached(',
                'rejectNetworkHello(-1, "NET3 handshake timed out")',
                'rejectNetworkHello(-1, "NET3 handshake retry limit reached")')) {
            if ($service.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("NET3 handshake must retain its bounded retry/timeout failure gate '$required'")
            }
        }
    }

    $deferStart = $Source.IndexOf('void ConnectionManager::deferNetworkMessage(',
        [StringComparison]::Ordinal)
    $deferEnd = if ($deferStart -ge 0) {
        $Source.IndexOf('Bool ConnectionManager::queueNetworkHelloCommand(', $deferStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($deferStart -lt 0 -or $deferEnd -lt 0) {
        $violations.Add('network deferred-queue path is missing')
    } else {
        $defer = $Source.Substring($deferStart, $deferEnd - $deferStart)
        if ($defer.IndexOf('findNetworkPeerEndpoint(message)', [StringComparison]::Ordinal) -lt 0 -or
            ($defer.IndexOf('maxPeerDeferredMessages', [StringComparison]::Ordinal) -lt 0 -and
             $defer.IndexOf('IsNetworkHelloDeferredPeerQuotaExceeded(', [StringComparison]::Ordinal) -lt 0) -or
            $defer.IndexOf('matchesNetworkPeerEndpoint(', [StringComparison]::Ordinal) -lt 0 -or
             $defer.IndexOf('dropInvalidNetworkHelloPacket(sourceSlot', [StringComparison]::Ordinal) -lt 0 -or
            $defer.IndexOf('m_networkHelloDeferredCount >= static_cast<UnsignedInt>(MAX_MESSAGES)',
                [StringComparison]::Ordinal) -lt 0 -or
            $defer.IndexOf('rejectNetworkHello(', [StringComparison]::Ordinal) -ge 0) {
            $violations.Add('deferred NET3 traffic must use a per-peer bound and drop-only overflow handling')
        }
    }

    $dropStart = $Source.IndexOf('void ConnectionManager::dropInvalidNetworkHelloPacket(',
        [StringComparison]::Ordinal)
    $dropEnd = if ($dropStart -ge 0) {
        $Source.IndexOf('Bool ConnectionManager::isNetworkHelloCandidate(', $dropStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($dropStart -lt 0 -or $dropEnd -lt 0) {
        $violations.Add('network invalid-packet drop path is missing')
    } else {
        $drop = $Source.Substring($dropStart, $dropEnd - $dropStart)
        if ($drop.IndexOf('DEBUG_LOG_LEVEL', [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('invalid NET3 packet drop path must retain bounded diagnostics')
        }
    }

    $candidateStart = $Source.IndexOf('Bool ConnectionManager::isNetworkHelloCandidate(',
        [StringComparison]::Ordinal)
    $candidateEnd = if ($candidateStart -ge 0) {
        $Source.IndexOf('void ConnectionManager::deferNetworkMessage(', $candidateStart,
            [StringComparison]::Ordinal)
    } else { -1 }
    if ($candidateStart -lt 0 -or $candidateEnd -lt 0) {
        $violations.Add('network Hello endpoint candidate gate is missing')
    } else {
        $candidate = $Source.Substring($candidateStart, $candidateEnd - $candidateStart)
        $identityIndex = $candidate.IndexOf('DecodeNetworkHelloIdentity(',
            [StringComparison]::Ordinal)
        $slotIndex = $candidate.IndexOf('findNetworkHelloSlot(', [StringComparison]::Ordinal)
        $endpointIndex = $candidate.IndexOf('matchesNetworkPeerEndpoint(message, slot)',
            [StringComparison]::Ordinal)
        if ($identityIndex -lt 0 -or $slotIndex -le $identityIndex -or
            $endpointIndex -le $slotIndex) {
            $violations.Add('network Hello candidate must bind its claimed slot to the observed endpoint')
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
            $violations.Add('network relay must route every NET3 prefix through bounded drop handling before gameplay parsing')
        }
        $knownEndpointIndex = $relay.IndexOf('isKnownNetworkPeerEndpoint(message)',
            [StringComparison]::Ordinal)
        if ($knownEndpointIndex -lt 0) {
            $knownEndpointIndex = $relay.IndexOf('endpointKnown', [StringComparison]::Ordinal)
        }
        $deferIndex = $relay.IndexOf('deferNetworkMessage(message)', [StringComparison]::Ordinal)
        if ($knownEndpointIndex -lt 0 -or $deferIndex -le $knownEndpointIndex) {
            $violations.Add('network relay must discard foreign packets before handshake deferral')
        }
        $ordinaryIndex = $relay.IndexOf('processTransportMessage(message)', [StringComparison]::Ordinal)
        if ($ordinaryIndex -lt 0 -or $ordinaryIndex -gt $parseIndex) {
            $violations.Add('network relay must source-bind ordinary gameplay packets after NET3')
        }
        $malformedIndex = $relay.IndexOf('dropInvalidNetworkHelloPacket(sourceSlot',
            [StringComparison]::Ordinal)
        if ($malformedIndex -lt 0) {
            $violations.Add('known NET3 senders must route malformed records to the drop-only handler')
        } elseif ($relay.IndexOf('message.length = 0;', $malformedIndex,
                [StringComparison]::Ordinal) -lt 0) {
            $violations.Add('malformed NET3 packets must be consumed after the drop-only handler')
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($NATSource)) {
        $natProbeStart = $NATSource.IndexOf(
            'IsExpectedProbeSource(m_expectedProbeNodeNumber, fromNode,',
            [StringComparison]::Ordinal)
        if ($natProbeStart -lt 0) {
            $natProbeStart = $NATSource.IndexOf('if (fromNode == m_targetNodeNumber', [StringComparison]::Ordinal)
        }
        $natProbeEnd = if ($natProbeStart -ge 0) {
            $NATSource.IndexOf('notifyUsersOfConnectionDone(m_targetNodeNumber);', $natProbeStart,
                [StringComparison]::Ordinal)
        } else { -1 }
        if ($natProbeStart -lt 0 -or $natProbeEnd -lt 0) {
            $violations.Add('NAT target probes must validate expected identity before updating endpoint state')
        } else {
            $natProbe = $NATSource.Substring($natProbeStart, $natProbeEnd - $natProbeStart)
            foreach ($required in @('IsExpectedProbeSource(', 'IsNewerProbeGeneration(',
                    'm_expectedProbeNodeNumber', 'm_expectedProbeCookie, probeCookie',
                    'm_lastAcceptedProbeGeneration', 'm_numNodes', 'MAX_SLOTS')) {
                if ($natProbe.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                    $violations.Add("NAT target probes must validate expected identity, cookie, and bounds '$required'")
                }
            }
        }
        foreach ($required in @(
                'generateNatProbeCookie(', 'SystemFunction036', 'm_probeCookie',
                'm_expectedProbeCookie', 'm_probeGeneration',
                'm_lastAcceptedProbeGeneration',
                'IsNewProbeEpoch(',
                'str.format("PROBE%d %08X %08X"',
                'options.format("PORT%d %d %08X %08X"',
                'sscanf(probeText + probePrefixLength, "%d %X %X %c"',
                'kNativePortMessageMaxLength',
                'RequireNativePortDelimiter(',
                'TryParseNativePortMessage(c, &parsedPort)',
                'IsValidNatAddress(parsedPort.address)')) {
            if ($NATSource.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
                $violations.Add("NAT probe cookie contract is missing '$required'")
            }
        }

        $modernCookieNames = @(
            'm_expectedProbeNodeNumber', 'm_probeCookie', 'm_expectedProbeCookie',
            'm_probeGeneration', 'm_lastAcceptedProbeGeneration',
            'generateNatProbeCookie(', 'IsExpectedProbeSource(',
            'IsNewerProbeGeneration(', 'IsNewProbeEpoch(')
        $probeSendStart = $NATSource.IndexOf('void NAT::sendAProbe(', [StringComparison]::Ordinal)
        $probeSendEnd = if ($probeSendStart -ge 0) {
            $NATSource.IndexOf('void NAT::sendMangledSourcePort(', $probeSendStart,
                [StringComparison]::Ordinal)
        } else { -1 }
        if ($probeSendStart -lt 0 -or $probeSendEnd -lt 0) {
            $violations.Add('NAT probe sender lane split is missing')
        } else {
            $probeSend = $NATSource.Substring($probeSendStart, $probeSendEnd - $probeSendStart)
            $modernProbeFormat = $probeSend.IndexOf('str.format("PROBE%d %08X %08X"',
                [StringComparison]::Ordinal)
            $legacyProbeFormat = $probeSend.IndexOf('str.format("PROBE%d", fromNode)',
                [StringComparison]::Ordinal)
            $modernProbeGuard = if ($modernProbeFormat -ge 0) {
                $probeSend.LastIndexOf('#if defined(_WIN64)', $modernProbeFormat,
                    [StringComparison]::Ordinal)
            } else { -1 }
            $modernProbeElse = if ($modernProbeFormat -ge 0) {
                $probeSend.IndexOf('#else', $modernProbeFormat, [StringComparison]::Ordinal)
            } else { -1 }
            $legacyProbeElse = if ($legacyProbeFormat -ge 0) {
                $probeSend.LastIndexOf('#else', $legacyProbeFormat, [StringComparison]::Ordinal)
            } else { -1 }
            if ($modernProbeFormat -lt 0 -or $modernProbeGuard -lt 0 -or
                $modernProbeElse -lt $modernProbeFormat) {
                $violations.Add('modern NAT PROBE format must be guarded to the x64 lane')
            }
            if ($legacyProbeFormat -lt 0 -or $legacyProbeElse -lt 0 -or
                $legacyProbeElse -lt $modernProbeFormat) {
                $violations.Add('legacy NAT PROBE format must preserve the original Win32 lane')
            }
            if ($legacyProbeElse -ge 0) {
                $legacyProbeEnd = $probeSend.IndexOf('#endif', $legacyProbeElse,
                    [StringComparison]::Ordinal)
                if ($legacyProbeEnd -lt 0) {
                    $violations.Add('legacy NAT PROBE branch must have a bounded preprocessor guard')
                } else {
                    $legacyProbe = $probeSend.Substring($legacyProbeElse,
                        $legacyProbeEnd - $legacyProbeElse)
                    foreach ($modernName in $modernCookieNames) {
                        if ($legacyProbe.IndexOf($modernName, [StringComparison]::Ordinal) -ge 0) {
                            $violations.Add("legacy NAT PROBE branch references x64-only '$modernName'")
                        }
                    }
                }
            }
        }

        $modernProbeParser = $NATSource.IndexOf(
            'sscanf(probeText + probePrefixLength, "%d %X %X %c"',
            [StringComparison]::Ordinal)
        $legacyProbeParser = $NATSource.IndexOf(
            'Int fromNode = atoi((char *)data + strlen("PROBE"))',
            [StringComparison]::Ordinal)
        $modernParserGuard = if ($modernProbeParser -ge 0) {
            $NATSource.LastIndexOf('#if defined(_WIN64)', $modernProbeParser,
                [StringComparison]::Ordinal)
        } else { -1 }
        $modernParserElse = if ($modernProbeParser -ge 0) {
            $NATSource.IndexOf('#else', $modernProbeParser, [StringComparison]::Ordinal)
        } else { -1 }
        $legacyParserElse = if ($legacyProbeParser -ge 0) {
            $NATSource.LastIndexOf('#else', $legacyProbeParser, [StringComparison]::Ordinal)
        } else { -1 }
        if ($modernProbeParser -lt 0 -or $modernParserGuard -lt 0 -or
            $modernParserElse -lt $modernProbeParser) {
            $violations.Add('modern NAT PROBE parser must be guarded to the x64 lane')
        }
        if ($legacyProbeParser -lt 0 -or $legacyParserElse -lt 0 -or
            $legacyParserElse -lt $modernProbeParser) {
            $violations.Add('legacy NAT PROBE parser must preserve atoi-based Win32 behavior')
        }
        if ($legacyParserElse -ge 0) {
            $legacyParserEnd = $NATSource.IndexOf('#endif', $legacyParserElse,
                [StringComparison]::Ordinal)
            if ($legacyParserEnd -lt 0) {
                $violations.Add('legacy NAT PROBE parser branch must have a bounded preprocessor guard')
            } else {
                $legacyParserBlock = $NATSource.Substring($legacyParserElse,
                    $legacyParserEnd - $legacyParserElse)
                foreach ($modernName in $modernCookieNames) {
                    if ($legacyParserBlock.IndexOf($modernName, [StringComparison]::Ordinal) -ge 0) {
                        $violations.Add("legacy NAT PROBE parser references x64-only '$modernName'")
                    }
                }
            }
        }

        $portSendStart = $NATSource.IndexOf(
            'void NAT::sendMangledPortNumberToTarget(', [StringComparison]::Ordinal)
        $portSendEnd = if ($portSendStart -ge 0) {
            $NATSource.IndexOf('void NAT::processGlobalMessage(', $portSendStart,
                [StringComparison]::Ordinal)
        } else { -1 }
        if ($portSendStart -lt 0 -or $portSendEnd -lt 0) {
            $violations.Add('NAT PORT sender lane split is missing')
        } else {
            $portSend = $NATSource.Substring($portSendStart, $portSendEnd - $portSendStart)
            $modernPortFormat = $portSend.IndexOf(
                'options.format("PORT%d %d %08X %08X"', [StringComparison]::Ordinal)
            $legacyPortFormat = $portSend.IndexOf(
                'options.format("PORT%d %d %08X", m_localNodeNumber, mangledPort, m_localIP)',
                [StringComparison]::Ordinal)
            $modernPortGuard = if ($modernPortFormat -ge 0) {
                $portSend.LastIndexOf('#if defined(_WIN64)', $modernPortFormat,
                    [StringComparison]::Ordinal)
            } else { -1 }
            $modernPortElse = if ($modernPortFormat -ge 0) {
                $portSend.IndexOf('#else', $modernPortFormat, [StringComparison]::Ordinal)
            } else { -1 }
            $legacyPortElse = if ($legacyPortFormat -ge 0) {
                $portSend.LastIndexOf('#else', $legacyPortFormat, [StringComparison]::Ordinal)
            } else { -1 }
            if ($modernPortFormat -lt 0 -or $modernPortGuard -lt 0 -or
                $modernPortElse -lt $modernPortFormat) {
                $violations.Add('modern NAT PORT format must be guarded to the x64 lane')
            }
            if ($legacyPortFormat -lt 0 -or $legacyPortElse -lt 0 -or
                $legacyPortElse -lt $modernPortFormat) {
                $violations.Add('legacy NAT PORT format must preserve the original Win32 lane')
            }
            if ($legacyPortElse -ge 0) {
                $legacyPortEnd = $portSend.IndexOf('#endif', $legacyPortElse,
                    [StringComparison]::Ordinal)
                if ($legacyPortEnd -lt 0) {
                    $violations.Add('legacy NAT PORT branch must have a bounded preprocessor guard')
                } else {
                    $legacyPort = $portSend.Substring($legacyPortElse,
                        $legacyPortEnd - $legacyPortElse)
                    foreach ($modernName in $modernCookieNames) {
                        if ($legacyPort.IndexOf($modernName, [StringComparison]::Ordinal) -ge 0) {
                            $violations.Add("legacy NAT PORT branch references x64-only '$modernName'")
                        }
                    }
                }
            }
        }

        $modernPortParser = $NATSource.IndexOf(
            'TryParseNativePortMessage(c, &parsedPort)',
            [StringComparison]::Ordinal)
        $legacyPortParser = $NATSource.IndexOf(
            'sscanf(c, "%d %X", &intport, &addr)', [StringComparison]::Ordinal)
        $modernPortParserGuard = if ($modernPortParser -ge 0) {
            $NATSource.LastIndexOf('#if defined(_WIN64)', $modernPortParser,
                [StringComparison]::Ordinal)
        } else { -1 }
        $modernPortParserElse = if ($modernPortParser -ge 0) {
            $NATSource.IndexOf('#else', $modernPortParser, [StringComparison]::Ordinal)
        } else { -1 }
        $legacyPortParserElse = if ($legacyPortParser -ge 0) {
            $NATSource.LastIndexOf('#else', $legacyPortParser, [StringComparison]::Ordinal)
        } else { -1 }
        if ($modernPortParser -lt 0 -or $modernPortParserGuard -lt 0 -or
            $modernPortParserElse -lt $modernPortParser) {
            $violations.Add('modern NAT PORT parser must be guarded to the x64 lane')
        }
        if ($legacyPortParser -lt 0 -or $legacyPortParserElse -lt 0 -or
            $legacyPortParserElse -lt $modernPortParser) {
            $violations.Add('legacy NAT PORT parser must preserve the original sscanf behavior')
        }
        if ($legacyPortParserElse -ge 0) {
            $legacyPortParserEnd = $NATSource.IndexOf('#endif', $legacyPortParserElse,
                [StringComparison]::Ordinal)
            if ($legacyPortParserEnd -lt 0) {
                $violations.Add('legacy NAT PORT parser branch must have a bounded preprocessor guard')
            } else {
                $legacyPortParserBlock = $NATSource.Substring($legacyPortParserElse,
                    $legacyPortParserEnd - $legacyPortParserElse)
                foreach ($modernName in $modernCookieNames) {
                    if ($legacyPortParserBlock.IndexOf($modernName, [StringComparison]::Ordinal) -ge 0) {
                        $violations.Add("legacy NAT PORT parser references x64-only '$modernName'")
                    }
                }
            }
        }

        foreach ($modernField in @(
                'm_expectedProbeNodeNumber', 'm_probeCookie', 'm_expectedProbeCookie',
                'm_probeGeneration', 'm_lastAcceptedProbeGeneration')) {
            $fieldIndex = $NATSource.IndexOf($modernField, [StringComparison]::Ordinal)
            if ($fieldIndex -ge 0) {
                $fieldGuard = $NATSource.LastIndexOf('#if defined(_WIN64)', $fieldIndex,
                    [StringComparison]::Ordinal)
                $fieldEnd = $NATSource.IndexOf('#endif', $fieldIndex,
                    [StringComparison]::Ordinal)
                if ($fieldGuard -lt 0 -or $fieldEnd -lt $fieldIndex) {
                    $violations.Add("NAT cookie/generation field '$modernField' is visible outside x64")
                }
            }
        }
        foreach ($modernHelper in @(
                'inline bool IsExpectedProbeSource(',
                'inline bool IsNewerProbeGeneration(',
                'inline bool IsNewProbeEpoch(')) {
            $helperIndex = $NATSource.IndexOf($modernHelper, [StringComparison]::Ordinal)
            if ($helperIndex -ge 0) {
                $helperGuard = $NATSource.LastIndexOf('#if defined(_WIN64)', $helperIndex,
                    [StringComparison]::Ordinal)
                $helperEnd = $NATSource.IndexOf('#endif', $helperIndex,
                    [StringComparison]::Ordinal)
                if ($helperGuard -lt 0 -or $helperEnd -lt $helperIndex) {
                    $violations.Add("NAT modern helper '$modernHelper' is visible outside x64")
                }
            }
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
inline bool IsNetworkFrameResendResponseAuthorized(
    std::uint32_t observedSlot, std::uint32_t requestedResponderSlot,
    std::uint32_t claimedSlot, std::uint32_t expectedOriginMask,
    std::uint32_t maxSlots, bool requestOutstanding, bool requestExpired,
    bool isFrameDataCommand, std::uint32_t responseFrame,
    std::uint32_t requestedFrame) {
    return requestOutstanding && !requestExpired && isFrameDataCommand &&
        observedSlot < maxSlots && requestedResponderSlot < maxSlots &&
        claimedSlot < maxSlots &&
        (expectedOriginMask & (1U << claimedSlot)) != 0U;
}
inline bool IsNetworkFrameResendResponseComplete(...) { return true; }
void ConnectionManager::beginNetworkHello() {
    generateNetworkHelloToken(&m_networkHelloLocalToken);
    sendNetworkHello(i);
}
void ConnectionManager::serviceNetworkHello() {
    if ((m_networkHelloExpectedSlots & (1U << i)) != 0U) {
        if (!m_networkHelloValidated[i] || !m_networkHelloAckReceived[i]) {
            sendNetworkHello(i);
        }
    }
    if (IsNetworkHelloTimedOut(now, m_networkHelloStartTime)) {
        rejectNetworkHello(-1, "NET3 handshake timed out");
        return;
    }
    if (!IsNetworkHelloRetryDue(now, m_networkHelloLastSend)) { return; }
    if (IsNetworkHelloAttemptLimitReached(m_networkHelloAttempts)) {
        rejectNetworkHello(-1, "NET3 handshake retry limit reached");
        return;
    }
    m_networkHelloLastSend = now;
    ++m_networkHelloAttempts;
}
Bool ConnectionManager::sendNetworkHello(Int slot) { return TRUE; }
Bool ConnectionManager::sendNetworkHelloAck() {
    EncodeNetworkHello(m_networkHelloRemoteToken[slot], NetworkHelloKind::Ack);
}
Int ConnectionManager::findNetworkHelloSlot() { return 0; }
Bool ConnectionManager::matchesNetworkPeerEndpoint() {
    return IsMatchingNetworkPeerEndpoint(message.addr, message.port, expected.addr, expected.port);
}
Bool ConnectionManager::isKnownNetworkPeerEndpoint() {
    return matchesNetworkPeerEndpoint(message, slot);
}
Int ConnectionManager::findNetworkPeerEndpoint() { return 0; }
Bool ConnectionManager::isNetworkCommandSourceAuthorized() {
    UnsignedInt packetRouterSlot = MAX_SLOTS;
    if (m_packetRouterSlot < MAX_SLOTS && m_packetRouterSlot != m_localSlot &&
        m_connections[m_packetRouterSlot] != nullptr &&
        !m_connections[m_packetRouterSlot]->isQuitting()) {
        packetRouterSlot = m_packetRouterSlot;
    }
    return IsNetworkCommandSourceAuthorized(sourceSlot, claimedSlot, packetRouterSlot);
}
void ConnectionManager::rejectNetworkHello() {}
void ConnectionManager::dropInvalidNetworkHelloPacket() {
    DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("drop invalid NET3 packet"));
}
Bool ConnectionManager::isNetworkHelloCandidate() {
    DecodeNetworkHelloIdentity(message, &identity);
    findNetworkHelloSlot(identity);
    return matchesNetworkPeerEndpoint(message, slot);
}
void ConnectionManager::deferNetworkMessage() {
    const Int sourceSlot = findNetworkPeerEndpoint(message);
    const UnsignedInt maxPeerDeferredMessages = static_cast<UnsignedInt>(MAX_MESSAGES / MAX_SLOTS);
    UnsignedInt peerDeferredMessages = 0U;
    if (matchesNetworkPeerEndpoint(message, sourceSlot)) { ++peerDeferredMessages; }
    if (peerDeferredMessages >= maxPeerDeferredMessages) {
        dropInvalidNetworkHelloPacket(sourceSlot, "NET3 deferred packet queue peer limit exceeded");
        return;
    }
    if (m_networkHelloDeferredCount >= static_cast<UnsignedInt>(MAX_MESSAGES)) { return; }
}
Bool ConnectionManager::queueNetworkHelloCommand() { return TRUE; }
Bool ConnectionManager::processNetworkHello() {
    DecodeAndValidateNetworkHelloRecord(message);
    findNetworkHelloSlot(identity);
    if (!result.ok()) {
        if (enforceFailure) {
            Int candidateSlot = slot;
            dropInvalidNetworkHelloPacket(candidateSlot, "malformed or incompatible NET3 record");
        }
        return FALSE;
    }
    if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { return FALSE; }
    if (kind == rts::network_epoch::NetworkHelloKind::Hello) {
        m_networkHelloRemoteToken[slot] = receivedSessionToken;
        sendNetworkHelloAck(slot);
    }
}
#endif
void ConnectionManager::sendLocalCommandImmediate(NetCommandMsg *msg, UnsignedByte relay) {
    const Bool packetRouterHasConnection = m_packetRouterSlot < MAX_SLOTS &&
        m_connections[m_packetRouterSlot] != nullptr;
    const Bool packetRouterIsQuitting = packetRouterHasConnection &&
        m_connections[m_packetRouterSlot]->isQuitting();
    Bool packetRouterEligible = FALSE;
#if defined(_WIN64)
    packetRouterEligible = rts::network_epoch::IsNetworkPacketRouterEligible(
        m_packetRouterSlot, m_localSlot, MAX_SLOTS, packetRouterHasConnection, packetRouterIsQuitting);
#else
    packetRouterEligible = m_packetRouterSlot < MAX_SLOTS &&
        (m_packetRouterSlot == m_localSlot ||
            (packetRouterHasConnection && !packetRouterIsQuitting));
#endif
    if (CommandRequiresDirectSend(msg) || !packetRouterEligible) {
        sendLocalCommandDirect(msg, relay);
        return;
    }
}
void ConnectionManager::sendLocalCommandDirect() {}
void ConnectionManager::processTransportMessage() {
    const Int sourceSlot = findNetworkPeerEndpoint(message);
    const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
    const Bool frameResendExpired = m_frameResendRequestOutstanding &&
        static_cast<UnsignedInt>(now - m_frameResendRequestStartTime) >=
        kNetworkFrameResendResponseTimeoutMs;
    if (frameResendExpired) { clearNetworkFrameResendRequest(); }
    Bool frameResendResponseAccepted = FALSE;
    UnsignedInt frameResendInfoMask = 0U;
    for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
        NetCommandMsg *command = cmd->getCommand();
        const Bool sourceAuthorized = isNetworkCommandSourceAuthorized(cmd->getCommand(), sourceSlot);
        const Bool isFrameDataCommand = command->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND ||
            command->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO;
        const Bool frameResendResponseAuthorized =
            IsNetworkFrameResendResponseAuthorized(sourceSlot,
                m_frameResendRequestResponder, command->getPlayerID(),
                m_frameResendRequestExpectedInfoMask, MAX_SLOTS,
                m_frameResendRequestOutstanding, frameResendExpired,
                isFrameDataCommand, command->getExecutionFrame(), m_frameResendRequestFrame);
        if (!sourceAuthorized && !frameResendResponseAuthorized) { continue; }
        if (frameResendResponseAuthorized) {
            frameResendResponseAccepted = TRUE;
            if (command->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
                frameResendInfoMask |= 1U << command->getPlayerID();
            }
        }
    }
    if (frameResendInfoMask != 0U) {
        m_frameResendRequestReceivedInfoMask |= frameResendInfoMask;
    }
    if (frameResendResponseAccepted && m_frameResendRequestOutstanding) {
        UnsignedInt frameResendReadyCommandMask = 0U;
        for (Int sourceSlot = 0; sourceSlot < MAX_SLOTS; ++sourceSlot) {
            const UnsignedInt sourceMask = 1U << sourceSlot;
            if ((m_frameResendRequestExpectedInfoMask & sourceMask) != 0U &&
                m_frameData[sourceSlot] != nullptr &&
                m_frameData[sourceSlot]->getFrameCommandCount(m_frameResendRequestFrame) ==
                m_frameData[sourceSlot]->getCommandCount(m_frameResendRequestFrame)) {
                frameResendReadyCommandMask |= sourceMask;
            }
        }
        if (IsNetworkFrameResendResponseComplete(
                m_frameResendRequestExpectedInfoMask,
                m_frameResendRequestReceivedInfoMask,
                frameResendReadyCommandMask)) {
            clearNetworkFrameResendRequest();
        }
    }
}
void ConnectionManager::clearNetworkFrameResendRequest() {}
void ConnectionManager::requestFrameDataResend(Int playerID, UnsignedInt frame) {
    clearNetworkFrameResendRequest();
    if (playerID < MAX_SLOTS) {
        if (static_cast<UnsignedInt>(playerID) != m_localSlot) {
            for (Int sourceSlot = 0; sourceSlot < MAX_SLOTS; ++sourceSlot) {
                if (sourceSlot != m_localSlot && m_frameData[sourceSlot] != nullptr) {
                    m_frameResendRequestExpectedInfoMask |= 1U << sourceSlot;
                }
            }
            m_frameResendRequestResponder = static_cast<UnsignedInt>(playerID);
            m_frameResendRequestFrame = frame;
            m_frameResendRequestStartTime = static_cast<UnsignedInt>(timeGetTime());
            m_frameResendRequestOutstanding = m_frameResendRequestExpectedInfoMask != 0U;
        }
        sendLocalCommandDirect(msg, 1 << playerID);
    }
    msg->detach();
}
void ConnectionManager::doRelay() {
    if (m_networkHelloFailed) {
        m_transport->m_inBuffer[i].length = 0;
        return;
    }
    if (HasNetworkHelloPrefix(message)) {
        if (!HasNetworkHelloMagic(message)) { return; }
        if (!isNetworkHelloCandidate(message)) {
            const Int sourceSlot = findNetworkPeerEndpoint(message);
            if (sourceSlot >= 0) {
                dropInvalidNetworkHelloPacket(sourceSlot, "malformed or unrelated NET3-sized payload");
            }
            message.length = 0;
            return;
        }
    }
    if (m_networkHelloRequired) {
        if (isKnownNetworkPeerEndpoint(message)) { deferNetworkMessage(message); }
        return;
    }
    processTransportMessage(message);
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
    $goodNAT = @'
struct NativePortMessage {
    int node;
    unsigned int port;
    unsigned int address;
    unsigned int probeCookie;
};
static const unsigned int kNativePortMessageMaxLength = 64U;
inline bool RequireNativePortDelimiter(const char *&cursor, const char *end) {
    return cursor < end;
}
inline bool TryParseNativePortMessage(const char *input, NativePortMessage *message) {
    return input != nullptr && message != nullptr;
}
inline bool IsValidNatAddress(unsigned int address) { return address != 0U; }
#if defined(_WIN64)
class NATModernFields {
    Int m_expectedProbeNodeNumber;
    UnsignedInt m_probeCookie;
    UnsignedInt m_expectedProbeCookie;
    UnsignedInt m_probeGeneration;
    UnsignedInt m_lastAcceptedProbeGeneration;
};
inline bool IsExpectedProbeSource(...) { return true; }
inline bool IsNewerProbeGeneration(...) { return true; }
inline bool IsNewProbeEpoch(...) { return true; }
static Bool generateNatProbeCookie(UnsignedInt *cookie) {
    *cookie = 0U;
    GetProcAddress(advapi32, "SystemFunction036");
    return TRUE;
}
#endif
void NAT::sendAProbe(...) {
#if defined(_WIN64)
    ++m_probeGeneration;
    str.format("PROBE%d %08X %08X", fromNode, m_probeCookie, m_probeGeneration);
#else
    str.format("PROBE%d", fromNode);
#endif
}
void NAT::sendMangledSourcePort(...) {}
void NAT::sendMangledPortNumberToTarget(...) {
#if defined(_WIN64)
    if (m_probeCookie == 0U) return;
    options.format("PORT%d %d %08X %08X", m_localNodeNumber, port, m_localIP, m_probeCookie);
#else
    options.format("PORT%d %d %08X", m_localNodeNumber, mangledPort, m_localIP);
#endif
}
void NAT::processGlobalMessage(...) {
#if defined(_WIN64)
    NativePortMessage parsedPort = {-1, 0U, 0U, 0U};
    if (!TryParseNativePortMessage(c, &parsedPort) ||
        !IsValidNatAddress(parsedPort.address) || parsedPort.probeCookie == 0U) return;
    const Int node = parsedPort.node;
    const UnsignedInt intport = parsedPort.port;
    const UnsignedInt addr = parsedPort.address;
    const UnsignedInt probeCookie = parsedPort.probeCookie;
    if (IsNewProbeEpoch(m_expectedProbeNodeNumber, m_expectedProbeCookie,
            node, probeCookie)) {
        m_lastAcceptedProbeGeneration = 0U;
    }
    m_expectedProbeCookie = probeCookie;
#else
    Int node = atoi(c);
    while (*c != ' ') { ++c; }
    while (*c == ' ') { ++c; }
    UnsignedInt intport = 0;
    UnsignedInt addr = 0;
    sscanf(c, "%d %X", &intport, &addr);
#endif
}
void NAT::connectionUpdate() {
#if defined(_WIN64)
    if (parsedNode && IsExpectedProbeSource(m_expectedProbeNodeNumber, fromNode,
            m_expectedProbeCookie, probeCookie, observedAddress, m_numNodes, MAX_SLOTS) &&
            IsNewerProbeGeneration(probeGeneration, m_lastAcceptedProbeGeneration)) {
        char probeText[64];
        sscanf(probeText + probePrefixLength, "%d %X %X %c",
            &fromNode, &probeCookie, &probeGeneration, &trailingChar);
        m_lastAcceptedProbeGeneration = probeGeneration;
        targetSlot->setIP(observedAddress);
        targetSlot->setPort(m_transport->m_inBuffer[i].port);
        notifyUsersOfConnectionDone(m_targetNodeNumber);
    }
#else
    Int fromNode = atoi((char *)data + strlen("PROBE"));
#endif
}
'@
    $goodSource = $goodSource -replace "`r`n", "`n"
    $goodCMake = $goodCMake -replace "`r`n", "`n"
    $goodNAT = $goodNAT -replace "`r`n", "`n"
    if ((Get-TokenViolations $goodSource $goodCMake $goodNAT).Count -ne 0) {
        throw 'known-good session-token fixture failed'
    }
    $permissiveStatus = $goodSource.Replace('status != 0', 'status < 0')
    if (-not ((Get-TokenViolations $permissiveStatus $goodCMake $goodNAT) -match 'fail-closed')) {
        throw 'permissive BCrypt status fixture was not rejected'
    }
    $fallback = $goodSource.Replace('BCryptGenRandom(nullptr,', 'rand(); BCryptGenRandom(nullptr,')
    if (-not ((Get-TokenViolations $fallback $goodCMake $goodNAT) -match 'forbidden fallback')) {
        throw 'fallback RNG fixture was not rejected'
    }
    $failOpen = $goodSource.Replace('if (m_networkHelloFailed)', 'if (false)')
    if (-not ((Get-TokenViolations $failOpen $goodCMake $goodNAT) -match 'fail closed')) {
        throw 'fail-open relay fixture was not rejected'
    }
    $slotBeforeIntegrity = $goodSource.Replace(
        "    DecodeAndValidateNetworkHelloRecord(message);`n    findNetworkHelloSlot(identity);",
        "    findNetworkHelloSlot(identity);`n    DecodeAndValidateNetworkHelloRecord(message);")
    if (-not ((Get-TokenViolations $slotBeforeIntegrity $goodCMake $goodNAT) -match 'validate integrity')) {
        throw 'identity-before-integrity fixture was not rejected'
    }
    $fatalStale = $goodSource.Replace(
        'if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { return FALSE; }',
        'if (!IsNetworkHelloSessionTokenAccepted(kind, local, received)) { rejectNetworkHello(slot); return FALSE; }')
    if (-not ((Get-TokenViolations $fatalStale $goodCMake $goodNAT) -match 'nonfatal')) {
        throw 'fatal stale-token fixture was not rejected'
    }
    $localAck = $goodSource.Replace(
        'EncodeNetworkHello(m_networkHelloRemoteToken[slot], NetworkHelloKind::Ack);',
        'EncodeNetworkHello(m_networkHelloLocalToken, NetworkHelloKind::Ack);')
    if (-not ((Get-TokenViolations $localAck $goodCMake $goodNAT) -match 'echo')) {
        throw 'non-echoing Ack fixture was not rejected'
    }
    $missingPrefix = $goodSource.Replace('HasNetworkHelloPrefix(message)',
        'HasNetworkHelloMagic(message)')
    if (-not ((Get-TokenViolations $missingPrefix $goodCMake $goodNAT) -match 'drop')) {
        throw 'missing NET3 prefix drop fixture was not rejected'
    }
    $unguardedRouter = $goodSource.Replace(
        "#if defined(_WIN64)`n    packetRouterEligible = rts::network_epoch::IsNetworkPacketRouterEligible(`n        m_packetRouterSlot, m_localSlot, MAX_SLOTS, packetRouterHasConnection, packetRouterIsQuitting);`n#else",
        "    packetRouterEligible = rts::network_epoch::IsNetworkPacketRouterEligible(`n        m_packetRouterSlot, m_localSlot, MAX_SLOTS, packetRouterHasConnection, packetRouterIsQuitting);")
    if (-not ((Get-TokenViolations $unguardedRouter $goodCMake $goodNAT) -match 'guarded from the Win32')) {
        throw 'unguarded Win32 router-helper fixture was not rejected'
    }
    $unboundCandidate = $goodSource.Replace(
        'return matchesNetworkPeerEndpoint(message, slot);',
        'return TRUE;')
    if (-not ((Get-TokenViolations $unboundCandidate $goodCMake $goodNAT) -match 'bind its claimed slot')) {
        throw 'unbound NET3 candidate fixture was not rejected'
    }
    $foreignDeferred = $goodSource.Replace(
        'if (isKnownNetworkPeerEndpoint(message)) { deferNetworkMessage(message); }',
        'deferNetworkMessage(message);')
    if (-not ((Get-TokenViolations $foreignDeferred $goodCMake $goodNAT) -match 'foreign packets')) {
        throw 'foreign deferred-packet fixture was not rejected'
    }
    $globalMalformed = $goodSource.Replace(
        'dropInvalidNetworkHelloPacket(candidateSlot, "malformed or incompatible NET3 record");',
        'rejectNetworkHello(-1, "malformed or incompatible NET3 record");')
    if (-not ((Get-TokenViolations $globalMalformed $goodCMake $goodNAT) -match 'nonfatal drop-only')) {
        throw 'global malformed-NET3 failure fixture was not rejected'
    }
    $globalQueue = $goodSource.Replace(
        'dropInvalidNetworkHelloPacket(sourceSlot, "NET3 deferred packet queue peer limit exceeded");',
        'rejectNetworkHello(-1, "NET3 deferred packet queue peer limit exceeded");')
    if (-not ((Get-TokenViolations $globalQueue $goodCMake $goodNAT) -match 'deferred NET3')) {
        throw 'global deferred-queue failure fixture was not rejected'
    }
    $mutatingInvalidDrop = $goodSource.Replace(
        'void ConnectionManager::dropInvalidNetworkHelloPacket() {',
        'void ConnectionManager::dropInvalidNetworkHelloPacket() { m_networkHelloExpectedSlots &= ~(1U << slot);')
    if ($mutatingInvalidDrop -ceq $goodSource -or
        -not ((Get-TokenViolations $mutatingInvalidDrop $goodCMake $goodNAT) -match 'must not mutate')) {
        throw 'invalid-packet membership mutation fixture was not rejected'
    }
    $missingTimeoutGate = $goodSource.Replace(
        'rejectNetworkHello(-1, "NET3 handshake timed out");',
        'dropInvalidNetworkHelloPacket(-1, "NET3 handshake timed out");')
    if (-not ((Get-TokenViolations $missingTimeoutGate $goodCMake $goodNAT) -match 'retry/timeout failure gate')) {
        throw 'missing handshake timeout gate fixture was not rejected'
    }
    $missingRetryGate = $goodSource.Replace(
        'rejectNetworkHello(-1, "NET3 handshake retry limit reached");',
        'dropInvalidNetworkHelloPacket(-1, "NET3 handshake retry limit reached");')
    if (-not ((Get-TokenViolations $missingRetryGate $goodCMake $goodNAT) -match 'retry/timeout failure gate')) {
        throw 'missing handshake retry gate fixture was not rejected'
    }
    $unboundTransport = $goodSource.Replace(
        'const Int sourceSlot = findNetworkPeerEndpoint(message);',
        'const Int sourceSlot = -1;')
    if (-not ((Get-TokenViolations $unboundTransport $goodCMake $goodNAT) -match 'ordinary gameplay packets')) {
        throw 'unbound ordinary-gameplay fixture was not rejected'
    }
    $unboundedResend = $goodSource.Replace(
        'if (!sourceAuthorized && !frameResendResponseAuthorized) { continue; }',
        'if (false) { continue; }')
    if (-not ((Get-TokenViolations $unboundedResend $goodCMake $goodNAT) -match 'strict source binding')) {
        throw 'unbounded frame-resend exception fixture was not rejected'
    }
    $missingResendFrame = $goodSource.Replace(
        'command->getExecutionFrame(), m_frameResendRequestFrame',
        'command->getPlayerID(), m_frameResendRequestFrame')
    if (-not ((Get-TokenViolations $missingResendFrame $goodCMake $goodNAT) -match 'bounded provenance')) {
        throw 'missing requested-frame binding fixture was not rejected'
    }
    $missingResendExpiry = $goodSource.Replace(
        'if (frameResendExpired) { clearNetworkFrameResendRequest(); }',
        'if (false) { clearNetworkFrameResendRequest(); }')
    if (-not ((Get-TokenViolations $missingResendExpiry $goodCMake $goodNAT) -match 'expire before admission')) {
        throw 'missing frame-resend expiry fixture was not rejected'
    }
    $missingResendResponder = $goodSource.Replace(
        'm_frameResendRequestResponder = static_cast<UnsignedInt>(playerID);',
        'm_frameResendRequestFrame = frame;')
    if (-not ((Get-TokenViolations $missingResendResponder $goodCMake $goodNAT) -match 'responder/frame provenance')) {
        throw 'missing requested-responder binding fixture was not rejected'
    }
    $missingResendReadiness = $goodSource.Replace(
        'IsNetworkFrameResendResponseComplete(',
        'true /* removed actual frame readiness */')
    if (-not ((Get-TokenViolations $missingResendReadiness $goodCMake $goodNAT) -match 'frame resend admission')) {
        throw 'missing frame-resend readiness fixture was not rejected'
    }
    $wrongResendOriginMask = $goodSource.Replace(
        'sourceSlot != m_localSlot', 'sourceSlot != playerID')
    if (-not ((Get-TokenViolations $wrongResendOriginMask $goodCMake $goodNAT) -match 'origin mask')) {
        throw 'resend mask excluding the responder fixture was not rejected'
    }
    $missingResendOriginMask = $goodSource.Replace(
        '(expectedOriginMask & (1U << claimedSlot)) != 0U',
        'true /* missing expected origin mask */')
    if (-not ((Get-TokenViolations $missingResendOriginMask $goodCMake $goodNAT) -match 'expected origin mask')) {
        throw 'missing resend origin-mask admission fixture was not rejected'
    }
    $missingNatProvenance = $goodNAT.Replace(
        'IsExpectedProbeSource(m_expectedProbeNodeNumber, fromNode,',
        'true /* missing expected target provenance */ &&')
    if (-not ((Get-TokenViolations $goodSource $goodCMake $missingNatProvenance) -match 'NAT target probes')) {
        throw 'missing NAT source-provenance fixture was not rejected'
    }
    $missingNatCookie = $goodNAT.Replace('m_expectedProbeCookie, probeCookie',
        'm_expectedProbeNodeNumber, fromNode')
    if (-not ((Get-TokenViolations $goodSource $goodCMake $missingNatCookie) -match 'cookie')) {
        throw 'missing NAT cookie fixture was not rejected'
    }
    $missingNatFreshness = $goodNAT.Replace(
        'IsNewerProbeGeneration(probeGeneration, m_lastAcceptedProbeGeneration)',
        'true /* missing probe generation freshness */ && false')
    if (-not ((Get-TokenViolations $goodSource $goodCMake $missingNatFreshness) -match 'NAT target probes')) {
        throw 'missing NAT generation-freshness fixture was not rejected'
    }
    $legacyNatProbe = $goodNAT.Replace('PROBE%d %08X %08X', 'PROBE%d %08X').Replace(
        'sscanf(probeText + probePrefixLength, "%d %X %X %c"',
        'sscanf(probeText + probePrefixLength, "%d %X %c"').Replace(
        'PORT%d %d %08X %08X', 'PORT%d %d %08X')
    if (-not ((Get-TokenViolations $goodSource $goodCMake $legacyNatProbe) -match 'cookie')) {
        throw 'legacy unbound NAT probe fixture was not rejected'
    }
    $win32Leak = $goodCMake.Replace('target_link_libraries(runtime INTERFACE legacy)',
        "target_link_libraries(runtime INTERFACE legacy)`n        bcrypt")
    if (-not ((Get-TokenViolations $goodSource $win32Leak $goodNAT) -match 'Win32')) {
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
$epochHeader = [IO.File]::ReadAllText((Join-Path $root 'Core/Libraries/Include/Lib/NetworkEpochHandshake.h'))
$source = $source + "`n" + $epochHeader
$natHeader = [IO.File]::ReadAllText((Join-Path $root 'Core/GameEngine/Include/GameNetwork/NAT.h'))
$natPolicy = [IO.File]::ReadAllText((Join-Path $root 'Core/Libraries/Include/Lib/NetworkNatPolicy.h'))
$natSource = $natHeader + "`n" + $natPolicy + "`n" + $epochHeader + "`n" +
    [IO.File]::ReadAllText((Join-Path $root 'Core/GameEngine/Source/GameNetwork/NAT.cpp'))
$violations = @(Get-TokenViolations $source $runtimeCMake $natSource)
if ($violations.Count -ne 0) {
    $violations | Write-Output
    exit 1
}
Write-Output 'Network session-token CSPRNG and dependency audit passed.'
