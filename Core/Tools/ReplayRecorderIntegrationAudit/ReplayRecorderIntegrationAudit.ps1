param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-FunctionBody {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Signature
    )

    $startIndex = $Content.IndexOf($Signature, [StringComparison]::Ordinal)
    if ($startIndex -lt 0) {
        throw "missing recorder function '$Signature'"
    }
    $openIndex = $Content.IndexOf('{', $startIndex + $Signature.Length)
    if ($openIndex -lt 0) {
        throw "missing opening brace for recorder function '$Signature'"
    }

    $depth = 0
    $state = 'code'
    $escaped = $false
    for ($index = $openIndex; $index -lt $Content.Length; ++$index) {
        $character = $Content[$index]
        $next = if ($index + 1 -lt $Content.Length) { $Content[$index + 1] } else { [char]0 }
        if ($state -eq 'line-comment') {
            if ($character -eq "`n") { $state = 'code' }
            continue
        }
        if ($state -eq 'block-comment') {
            if ($character -eq '*' -and $next -eq '/') { $state = 'code'; ++$index }
            continue
        }
        if ($state -eq 'string' -or $state -eq 'character') {
            if ($escaped) { $escaped = $false; continue }
            if ($character -eq '\') { $escaped = $true; continue }
            if (($state -eq 'string' -and $character -eq '"') -or
                ($state -eq 'character' -and $character -eq "'")) { $state = 'code' }
            continue
        }
        if ($character -eq '/' -and $next -eq '/') { $state = 'line-comment'; ++$index; continue }
        if ($character -eq '/' -and $next -eq '*') { $state = 'block-comment'; ++$index; continue }
        if ($character -eq '"') { $state = 'string'; continue }
        if ($character -eq "'") { $state = 'character'; continue }
        if ($character -eq '{') { ++$depth; continue }
        if ($character -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $Content.Substring($startIndex, $index - $startIndex + 1)
            }
        }
    }
    throw "missing closing brace for recorder function '$Signature'"
}

function Test-RecorderContent {
    param([Parameter(Mandatory = $true)][string]$Content)

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $start = Get-FunctionBody $Content 'void RecorderClass::startRecording('
    $stop = Get-FunctionBody $Content 'void RecorderClass::stopRecording()'
    $validator = Get-FunctionBody $Content 'static Bool validateNativeReplayContainer('
    $header = Get-FunctionBody $Content 'Bool RecorderClass::readReplayHeader('
    $playback = Get-FunctionBody $Content 'Bool RecorderClass::playbackFile('
    $readers = (Get-FunctionBody $Content 'Bool RecorderClass::readExact(') +
        (Get-FunctionBody $Content 'Bool RecorderClass::readUnicodeString(') +
        (Get-FunctionBody $Content 'Bool RecorderClass::readAsciiString(')
    $writer = Get-FunctionBody $Content 'void RecorderClass::writeToFile(GameMessage * msg)'
    $failure = Get-FunctionBody $Content 'void RecorderClass::failNativeReplayRead('
    $nativeCommand = Get-FunctionBody $Content 'Bool RecorderClass::appendNativeReplayCommand()'
    $frameReader = Get-FunctionBody $Content 'void RecorderClass::readNextFrame()'
    $commandReader = Get-FunctionBody $Content 'void RecorderClass::appendNextCommand()'

    $beginIndex = $start.IndexOf('beginNativeReplayContainer(m_file)', [StringComparison]::Ordinal)
    $modeIndex = $start.IndexOf('m_mode = RECORDERMODETYPE_RECORD;', [StringComparison]::Ordinal)
    if ($start.IndexOf('if (!beginNativeReplayContainer(m_file))', [StringComparison]::Ordinal) -lt 0 -or
        $beginIndex -lt 0 -or $modeIndex -le $beginIndex) {
        $violations.Add('record mode must be published only after native replay startup succeeds')
    }
    if (([regex]::Matches($start, [regex]::Escape('m_fileName.clear();'))).Count -lt 2) {
        $violations.Add('both recording startup failures must clear the pending replay name')
    }
    if ($start.IndexOf('DeleteFile(filepath.str())', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('native replay startup failure must remove its truncated LastReplay file')
    }
    foreach ($required in @(
        'writeNativeReplayExact(',
        'writeNativeReplayU32Field(',
        'writeNativeReplayBoolField(',
        'writeNativeReplayWideString(',
        'writeNativeReplaySystemTime(',
        'writeNativeReplayAsciiString(',
        'm_replayWriteError = TRUE;')) {
        if ($start.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native replay header writing is missing '$required'")
        }
    }

    foreach ($required in @(
        'Bool replayFinalized = !m_replayWriteError;',
        'replayFinalized = finalizeNativeReplayContainer(m_file);',
        'if (replayFinalized && m_archiveReplays)',
        'else if (!replayFinalized)',
        'm_mode = RECORDERMODETYPE_NONE;',
        'DeleteFile(invalidReplayPath.str())')) {
        if ($stop.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("missing finalization policy '$required'")
        }
    }
    foreach ($required in @(
        'buildNativeReplayCommand(msg, record, &recordBytes)',
        'm_replayWriteError = TRUE;',
        '#else',
        '#endif')) {
        if ($writer.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native recording must fail closed for '$required'")
        }
    }
    if ($stop.IndexOf('m_file->close();', [StringComparison]::Ordinal) -lt 0 -or
        $stop.IndexOf('m_file = nullptr;', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('recording stop must release the replay file on every finalization result')
    }

    if ($header.IndexOf('m_file->read(', [StringComparison]::Ordinal) -ge 0 -or
        $playback.IndexOf('m_file->read(', [StringComparison]::Ordinal) -ge 0) {
        $violations.Add('replay headers and launch fields must not bypass exact-read helpers')
    }
    foreach ($required in @(
        'rts::replay::ReadExact(*m_file',
        'rts::replay::ReadWideString(*m_file',
        'rts::replay::ReadAsciiString(*m_file',
        'readNativeReplayU16Field(m_file')) {
        if ($readers.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("missing bounded replay reader '$required'")
        }
    }
    if ($validator.IndexOf('options.expectedSchemaVersion = rts::runtime_epoch::kCurrentReplaySchemaVersion;',
            [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('native replay container validation must require the current replay schema')
    }
    foreach ($required in @(
        'validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd)',
        'm_nativeReplayContainer = TRUE;')) {
        if ($header.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native replay header integration is missing '$required'")
        }
    }
    $validationGuardIndex = $header.IndexOf(
        'if (!validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd))',
        [StringComparison]::Ordinal)
    $containerPublishIndex = $header.IndexOf('m_nativeReplayContainer = TRUE;',
        [StringComparison]::Ordinal)
    if ($validationGuardIndex -lt 0 -or $containerPublishIndex -le $validationGuardIndex) {
        $violations.Add('native replay mode must be published only after container validation succeeds')
    }
    foreach ($required in @(
        'readNativeReplayU32Field(m_file',
        'readNativeReplayBoolField(m_file',
        'readNativeReplaySystemTime(m_file')) {
        if ($header.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native replay header reading is missing '$required'")
        }
    }
    if ($playback.IndexOf('readNativeReplayU32Field(m_file', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('native replay launch fields must use explicit little-endian reads')
    }
    foreach ($required in @(
        'ParseCanonicalReplayCommand(',
        'kMaxReplayCommandBytes',
        'ReplayCommandError::Truncated')) {
        if ($frameReader.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native frame reader is missing '$required'")
        }
    }
    $readErrorIndex = $failure.IndexOf('m_replayReadError = TRUE;', [StringComparison]::Ordinal)
    $stopPlaybackIndex = $failure.IndexOf('stopPlayback();', [StringComparison]::Ordinal)
    if ($readErrorIndex -lt 0 -or $stopPlaybackIndex -le $readErrorIndex) {
        $violations.Add('native replay failure handling must publish the read-error state')
    }
    $parseIndex = $frameReader.IndexOf('m_nativeReplayParsed = rts::replay_command::ParseCanonicalReplayCommand(',
        [StringComparison]::Ordinal)
    $parseGuardIndex = $frameReader.IndexOf('if (!m_nativeReplayParsed.ok())', [StringComparison]::Ordinal)
    $parseFailureIndex = $frameReader.IndexOf('failNativeReplayRead(m_nativeReplayParsed.error, start);',
        [StringComparison]::Ordinal)
    if ($parseIndex -lt 0 -or $parseGuardIndex -le $parseIndex -or $parseFailureIndex -le $parseGuardIndex) {
        $violations.Add('native frame parsing must fail closed before publishing a frame')
    }
    foreach ($required in @(
        'ReplayCommandError::PayloadSizeMismatch',
        'friend_setPlayerIndex')) {
        if ($nativeCommand.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("native command reader is missing '$required'")
        }
    }
    if ($commandReader.IndexOf('appendNativeReplayCommand();', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add("native command reader is missing 'appendNativeReplayCommand();'")
    }
    return @($violations)
}

if ($SelfTest) {
    $good = @'
void RecorderClass::startRecording() {
  if (!beginNativeReplayContainer(m_file)) { return; }
  writeNativeReplayExact(m_file, data, size);
  writeNativeReplayU32Field(m_file, value);
  writeNativeReplayBoolField(m_file, value);
  writeNativeReplayWideString(m_file, value);
  writeNativeReplaySystemTime(m_file, value);
  writeNativeReplayAsciiString(m_file, value);
  m_replayWriteError = TRUE;
  m_fileName.clear();
  m_fileName.clear();
  DeleteFile(filepath.str());
  m_mode = RECORDERMODETYPE_RECORD;
}
void RecorderClass::stopRecording() {
  Bool replayFinalized = !m_replayWriteError;
  replayFinalized = finalizeNativeReplayContainer(m_file);
  m_file->close();
  m_file = nullptr;
  if (replayFinalized && m_archiveReplays) archiveReplay(m_fileName);
  else if (!replayFinalized) {
    DeleteFile(invalidReplayPath.str());
    m_mode = RECORDERMODETYPE_NONE;
  }
}
void RecorderClass::archiveReplay() {}
static Bool validateNativeReplayContainer() {
  options.expectedSchemaVersion = rts::runtime_epoch::kCurrentReplaySchemaVersion;
}
static void writeNativeReplayU16() {}
Bool RecorderClass::readReplayHeader() {
  if (!validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd)) { return FALSE; }
  m_nativeReplayContainer = TRUE;
  readNativeReplayU32Field(m_file, value);
  readNativeReplayBoolField(m_file, value);
  readNativeReplaySystemTime(m_file, value);
  return readExact();
}
Bool RecorderClass::simulateReplay() { return FALSE; }
Bool RecorderClass::playbackFile() { readNativeReplayU32Field(m_file, value); return readExact(); }
Bool RecorderClass::readExact() { return rts::replay::ReadExact(*m_file, p, n); }
Bool RecorderClass::readUnicodeString() { readNativeReplayU16Field(m_file, value); return rts::replay::ReadWideString(*m_file, p, n); }
Bool RecorderClass::readAsciiString() { return rts::replay::ReadAsciiString(*m_file, p, n); }
void RecorderClass::writeToFile(GameMessage * msg) {
  buildNativeReplayCommand(msg, record, &recordBytes);
  m_replayWriteError = TRUE;
#else
#endif
}
void RecorderClass::writeArgument() {}
void RecorderClass::failNativeReplayRead() {
  m_replayReadError = TRUE;
  stopPlayback();
}
Bool RecorderClass::readNativeReplayArgument() {}
Bool RecorderClass::appendNativeReplayCommand() {
  ReplayCommandError::PayloadSizeMismatch;
  friend_setPlayerIndex;
}
void RecorderClass::readNextFrame() {
  m_nativeReplayParsed = rts::replay_command::ParseCanonicalReplayCommand(record, bytes);
  kMaxReplayCommandBytes;
  ReplayCommandError::Truncated;
  if (!m_nativeReplayParsed.ok()) {
    failNativeReplayRead(m_nativeReplayParsed.error, start);
  }
}
void RecorderClass::appendNextCommand() {
  appendNativeReplayCommand();
}
void RecorderClass::readArgument() {}
'@
    if ((Test-RecorderContent $good).Count -ne 0) {
        throw 'known-good recorder fixture failed'
    }
    $unsafeArchive = $good.Replace('if (replayFinalized && m_archiveReplays)',
        'if (m_archiveReplays)')
    if (-not ((Test-RecorderContent $unsafeArchive) -match 'finalization policy')) {
        throw 'unsafe archive fixture was not rejected'
    }
    $directRead = $good.Replace('  return readExact();',
        '  m_file->read(p, n); return TRUE;')
    if (-not ((Test-RecorderContent $directRead) -match 'bypass exact-read')) {
        throw 'direct-read fixture was not rejected'
    }
    $schemaOutsideValidator = $good.Replace(
        '  options.expectedSchemaVersion = rts::runtime_epoch::kCurrentReplaySchemaVersion;', '')
    $schemaOutsideValidator += "`noptions.expectedSchemaVersion = rts::runtime_epoch::kCurrentReplaySchemaVersion;"
    if (-not ((Test-RecorderContent $schemaOutsideValidator) -match 'current replay schema')) {
        throw 'schema token outside the native validator was not rejected'
    }
    $errorOutsideFailure = $good.Replace('  m_replayReadError = TRUE;', '')
    $errorOutsideFailure += "`nm_replayReadError = TRUE;"
    if (-not ((Test-RecorderContent $errorOutsideFailure) -match 'read-error state')) {
        throw 'read-error token outside the native failure helper was not rejected'
    }
    $payloadCheckOutsideCommand = $good.Replace('  ReplayCommandError::PayloadSizeMismatch;', '')
    $payloadCheckOutsideCommand += "`nReplayCommandError::PayloadSizeMismatch;"
    if (-not ((Test-RecorderContent $payloadCheckOutsideCommand) -match 'PayloadSizeMismatch')) {
        throw 'payload-size token outside the native command helper was not rejected'
    }
    $unguardedStartup = $good.Replace('if (!beginNativeReplayContainer(m_file)) { return; }',
        'beginNativeReplayContainer(m_file);')
    if (-not ((Test-RecorderContent $unguardedStartup) -match 'record mode')) {
        throw 'unguarded native replay startup was not rejected'
    }
    $unguardedHeader = $good.Replace(
        'if (!validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd)) { return FALSE; }',
        'validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd);')
    if (-not ((Test-RecorderContent $unguardedHeader) -match 'container validation')) {
        throw 'unguarded native replay validation was not rejected'
    }
    $missingStop = $good.Replace('  stopPlayback();', '')
    if (-not ((Test-RecorderContent $missingStop) -match 'read-error state')) {
        throw 'native replay failure without playback shutdown was not rejected'
    }
    $ignoredParseFailure = $good.Replace('  if (!m_nativeReplayParsed.ok()) {', '  if (FALSE) {')
    if (-not ((Test-RecorderContent $ignoredParseFailure) -match 'fail closed')) {
        throw 'ignored native parse failure was not rejected'
    }
    Write-Output 'Replay recorder integration audit self-tests passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw '-SourceRoot is required unless -SelfTest is used.'
}
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$paths = @(
    'Generals/Code/GameEngine/Source/Common/Recorder.cpp',
    'GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp'
)
$allViolations = New-Object 'System.Collections.Generic.List[string]'
foreach ($relativePath in $paths) {
    $path = Join-Path $root ($relativePath -replace '/', '\')
    $content = [IO.File]::ReadAllText($path)
    foreach ($violation in (Test-RecorderContent $content)) {
        $allViolations.Add("${relativePath}: $violation")
    }
}
if ($allViolations.Count -ne 0) {
    $allViolations | Write-Output
    exit 1
}
Write-Output 'Replay recorder integration audit passed for both titles.'
