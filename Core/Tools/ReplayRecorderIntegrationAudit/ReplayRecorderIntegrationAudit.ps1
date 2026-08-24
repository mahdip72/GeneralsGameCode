param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-Slice {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Start,
        [Parameter(Mandatory = $true)][string]$End
    )

    $startIndex = $Content.IndexOf($Start, [StringComparison]::Ordinal)
    if ($startIndex -lt 0) {
        throw "missing recorder marker '$Start'"
    }
    $endIndex = $Content.IndexOf($End, $startIndex + $Start.Length,
        [StringComparison]::Ordinal)
    if ($endIndex -lt 0) {
        throw "missing recorder marker '$End'"
    }
    return $Content.Substring($startIndex, $endIndex - $startIndex)
}

function Test-RecorderContent {
    param([Parameter(Mandatory = $true)][string]$Content)

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $start = Get-Slice $Content 'void RecorderClass::startRecording(' 'void RecorderClass::stopRecording()'
    $stop = Get-Slice $Content 'void RecorderClass::stopRecording()' 'void RecorderClass::archiveReplay('
    $header = Get-Slice $Content 'Bool RecorderClass::readReplayHeader(' 'Bool RecorderClass::simulateReplay('
    $playback = Get-Slice $Content 'Bool RecorderClass::playbackFile(' 'Bool RecorderClass::readExact('
    $readers = Get-Slice $Content 'Bool RecorderClass::readExact(' 'void RecorderClass::readNextFrame()'

    $beginIndex = $start.IndexOf('beginNativeReplayContainer(m_file)', [StringComparison]::Ordinal)
    $modeIndex = $start.IndexOf('m_mode = RECORDERMODETYPE_RECORD;', [StringComparison]::Ordinal)
    if ($beginIndex -lt 0 -or $modeIndex -le $beginIndex) {
        $violations.Add('record mode must be published only after native replay startup succeeds')
    }
    if (([regex]::Matches($start, [regex]::Escape('m_fileName.clear();'))).Count -lt 2) {
        $violations.Add('both recording startup failures must clear the pending replay name')
    }
    if ($start.IndexOf('DeleteFile(filepath.str())', [StringComparison]::Ordinal) -lt 0) {
        $violations.Add('native replay startup failure must remove its truncated LastReplay file')
    }

    foreach ($required in @(
        'Bool replayFinalized = TRUE;',
        'replayFinalized = finalizeNativeReplayContainer(m_file);',
        'if (replayFinalized && m_archiveReplays)',
        'else if (!replayFinalized)',
        'm_mode = RECORDERMODETYPE_NONE;',
        'DeleteFile(invalidReplayPath.str())')) {
        if ($stop.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("missing finalization policy '$required'")
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
        'rts::replay::ReadAsciiString(*m_file')) {
        if ($readers.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
            $violations.Add("missing bounded replay reader '$required'")
        }
    }
    return @($violations)
}

if ($SelfTest) {
    $good = @'
void RecorderClass::startRecording() {
  beginNativeReplayContainer(m_file);
  m_fileName.clear();
  m_fileName.clear();
  DeleteFile(filepath.str());
  m_mode = RECORDERMODETYPE_RECORD;
}
void RecorderClass::stopRecording() {
  Bool replayFinalized = TRUE;
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
Bool RecorderClass::readReplayHeader() { return readExact(); }
Bool RecorderClass::simulateReplay() { return FALSE; }
Bool RecorderClass::playbackFile() { return readExact(); }
Bool RecorderClass::readExact() { return rts::replay::ReadExact(*m_file, p, n); }
Bool RecorderClass::readUnicodeString() { return rts::replay::ReadWideString(*m_file, p, n); }
Bool RecorderClass::readAsciiString() { return rts::replay::ReadAsciiString(*m_file, p, n); }
void RecorderClass::readNextFrame() {}
'@
    if ((Test-RecorderContent $good).Count -ne 0) {
        throw 'known-good recorder fixture failed'
    }
    $unsafeArchive = $good.Replace('if (replayFinalized && m_archiveReplays)',
        'if (m_archiveReplays)')
    if (-not ((Test-RecorderContent $unsafeArchive) -match 'finalization policy')) {
        throw 'unsafe archive fixture was not rejected'
    }
    $directRead = $good.Replace('Bool RecorderClass::readReplayHeader() { return readExact(); }',
        'Bool RecorderClass::readReplayHeader() { m_file->read(p, n); return TRUE; }')
    if (-not ((Test-RecorderContent $directRead) -match 'bypass exact-read')) {
        throw 'direct-read fixture was not rejected'
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
