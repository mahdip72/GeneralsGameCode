Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Local diagnostic-only host data contract.  This module never loads or writes
# the game source, never claims archive provenance, and never returns canonical
# qualification/promotion authority.  The entrypoint owns process execution.
# Artifact-set/runtime checks bind to the supplied manifests; the shared session
# boundary remains responsible for any full runtime-closure validation.
$script:SchemaVersion = 1
$script:EvidenceKind = 'stage5-local-lockstep-diagnostic-data'
$script:Producer = 'stage5-local-lockstep-diagnostic-data-v1'
$script:MapName = 'Maps\Twilight Flame\Twilight Flame.map'
$script:MapCrcAlgorithm = 'Common/crc.h rotate-left-one then byte-add modulo 2^32'
$script:MapEntryEncoding = 'EAR-refpack-raw'

$script:RequiredFilesByTitle = [ordered]@{
    Generals = @(
        'English.big',
        'INI.big',
        'maps.big',
        'W3D.big',
        'Data/Scripts/MultiplayerScripts.scb',
        'Data/Scripts/SkirmishScripts.scb'
    )
    ZeroHour = @(
        'INIZH.big',
        'MapsZH.big',
        'W3DZH.big',
        'Data/Scripts/MultiplayerScripts.scb',
        'Data/Scripts/Scripts.ini',
        'Data/Scripts/SkirmishScripts.scb'
    )
}

# These are independent observations of the actual Twilight Flame BIG entries
# supplied by the delivery manager.  New/Read reparse the selected BIG and
# compare the observed entry bytes to these values.  They are map-byte
# bindings, not reviewed archive identities.
$script:ExpectedMapEntries = [ordered]@{
    Generals = [ordered]@{
        archivePath = 'GeneralsRuntime/maps.big'
        entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
        sha256 = 'A6811E2F16BAA0ED1E47839C71347CCB51F1F088BDE8EA696E1750687F65E115'
        byteLength = [uint64]402471
        mapCrc = [uint32]739101722
    }
    ZeroHour = [ordered]@{
        archivePath = 'ZeroHourRuntime/MapsZH.big'
        entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
        sha256 = '3B8B47E1AF6E5D8D479F282EF6B6E542B5C2DBBD700A91FCF55AA7DA209B1100'
        byteLength = [uint64]400412
        mapCrc = [uint32]4042777579
    }
}

function Assert-Stage5LocalCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Stage5LocalProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-Stage5LocalCondition ($null -ne $Object) "$Context is null."
    $propertyMatches = @(if ($Object -is [Collections.IDictionary]) {
        @($Object.Keys | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_, $Name)
        })
    }
    else {
        @($Object.PSObject.Properties | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_.Name, $Name)
        })
    })
    Assert-Stage5LocalCondition ($propertyMatches.Count -eq 1) `
        "$Context is missing '$Name'."
    if ($Object -is [Collections.IDictionary]) {
        return $Object[$propertyMatches[0]]
    }
    return $propertyMatches[0].Value
}

function Test-Stage5LocalHasExactProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $false }
    if ($Object -is [Collections.IDictionary]) {
        foreach ($key in $Object.Keys) {
            if ([StringComparer]::Ordinal.Equals([string]$key, $Name)) {
                return $true
            }
        }
        return $false
    }
    foreach ($property in $Object.PSObject.Properties) {
        if ([StringComparer]::Ordinal.Equals([string]$property.Name, $Name)) {
            return $true
        }
    }
    return $false
}

function Test-Stage5LocalOrdinalMember {
    param([object]$Value, [object[]]$Allowed)
    foreach ($candidate in $Allowed) {
        if ([StringComparer]::Ordinal.Equals([string]$Value, [string]$candidate)) {
            return $true
        }
    }
    return $false
}

function Assert-Stage5LocalExactProperties {
    param([object]$Object, [string[]]$Names, [string]$Context)
    Assert-Stage5LocalCondition ($null -ne $Object) "$Context is null."
    $actual = @(if ($Object -is [Collections.IDictionary]) {
        @($Object.Keys | ForEach-Object { [string]$_ })
    }
    else { @($Object.PSObject.Properties.Name) })
    $expected = @($Names)
    Assert-Stage5LocalCondition ($actual.Count -eq $expected.Count) `
        "$Context has unexpected property count."
    foreach ($name in $expected) {
        Assert-Stage5LocalCondition (@($actual | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_, [string]$name)
        }).Count -eq 1) "$Context is missing '$name'."
    }
    foreach ($name in $actual) {
        Assert-Stage5LocalCondition (@($expected | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_, [string]$name)
        }).Count -eq 1) `
            "$Context has unexpected property '$name'."
    }
}

function Assert-Stage5LocalJsonString {
    param([object]$Value, [string]$Context = 'JSON string')
    Assert-Stage5LocalCondition ($Value -is [string]) `
        "$Context must be a JSON string."
    return [string]$Value
}

function Test-Stage5LocalJsonInteger {
    param([object]$Value)
    if ($null -eq $Value -or $Value -is [bool]) { return $false }
    if ($Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]) {
        return $true
    }
    if ($Value -is [decimal]) {
        return [decimal]::Truncate([decimal]$Value) -eq [decimal]$Value
    }
    if ($Value -is [single] -or $Value -is [double]) {
        [double]$number = $Value
        return -not [double]::IsNaN($number) -and
            -not [double]::IsInfinity($number) -and
            [Math]::Truncate($number) -eq $number
    }
    return $false
}

function Assert-Stage5LocalJsonInteger {
    param([object]$Value, [string]$Context = 'JSON integer')
    Assert-Stage5LocalCondition (Test-Stage5LocalJsonInteger $Value) `
        "$Context must be a JSON integer."
    return $Value
}

function Get-Stage5LocalFullPath {
    param([string]$Path, [string]$Context = 'Path')
    Assert-Stage5LocalCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context is empty."
    try { $full = [IO.Path]::GetFullPath($Path) }
    catch { throw "$Context is not a valid filesystem path." }
    Assert-Stage5LocalCondition ($full.Length -lt 32768) `
        "$Context exceeds the bounded Windows path length."
    return $full
}

function Assert-Stage5LocalNoReparsePath {
    param(
        [string]$Path,
        [ValidateSet('Leaf', 'Container')][string]$PathType = 'Leaf',
        [string]$Context = 'Path'
    )
    $full = Get-Stage5LocalFullPath $Path $Context
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    $cursor = $item
    while ($null -ne $cursor) {
        Assert-Stage5LocalCondition (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context traverses a reparse point: $($cursor.FullName)"
        $parentPath = Split-Path -Parent $cursor.FullName
        if ([string]::IsNullOrEmpty($parentPath) -or $parentPath -ceq $cursor.FullName) { break }
        $cursor = Get-Item -LiteralPath $parentPath -Force -ErrorAction Stop
    }
    if ($PathType -ceq 'Leaf') {
        Assert-Stage5LocalCondition (-not $item.PSIsContainer) "$Context is not a regular file."
    }
    else {
        Assert-Stage5LocalCondition $item.PSIsContainer "$Context is not a directory."
    }
    return $full
}

function Assert-Stage5LocalPathBelow {
    param([string]$Root, [string]$Path, [string]$Context = 'Path')
    $rootFull = (Get-Stage5LocalFullPath $Root "$Context root").TrimEnd('\', '/')
    $pathFull = Get-Stage5LocalFullPath $Path $Context
    $comparison = [StringComparison]::OrdinalIgnoreCase
    Assert-Stage5LocalCondition (
        $pathFull.Length -gt $rootFull.Length -and
        $pathFull.StartsWith($rootFull + '\', $comparison)
    ) "$Context escapes its runtime root."
    return $pathFull
}

function Assert-Stage5LocalSafeRelativePath {
    param([object]$Path, [string]$Context = 'Relative path')
    Assert-Stage5LocalCondition ($Path -is [string] -and $Path.Length -gt 0 -and $Path.Length -lt 1024) `
        "$Context is empty or oversized."
    Assert-Stage5LocalCondition ($Path -cmatch '^[^\\/:]+(?:/[^\\/:]+)*$') `
        "$Context must use bounded slash-separated relative components."
    Assert-Stage5LocalCondition ($Path -cnotmatch '[\x00-\x1F\x7F]') `
        "$Context contains a control character."
    $segments = @($Path -split '/')
    foreach ($segment in $segments) {
        Assert-Stage5LocalCondition ($segment -cne '.' -and $segment -cne '..' -and
            -not $segment.EndsWith('.') -and -not $segment.EndsWith(' ')) `
            "$Context contains an unsafe component."
        $base = ($segment -split '\.')[0].ToUpperInvariant()
        Assert-Stage5LocalCondition ($base -notin @('CON', 'PRN', 'AUX', 'NUL') -and
            $base -notmatch '^(COM|LPT)[1-9]$') "$Context contains a reserved device name."
    }
    return $Path
}

function Assert-Stage5LocalSha256 {
    param([object]$Value, [string]$Context = 'SHA-256')
    Assert-Stage5LocalCondition ($Value -is [string] -and $Value -cmatch '^[0-9A-F]{64}$') `
        "$Context must be uppercase hexadecimal SHA-256."
    return $Value
}

function Assert-Stage5LocalSourceCommit {
    param([object]$Value)
    Assert-Stage5LocalCondition ($Value -is [string] -and $Value -cmatch '^[0-9a-f]{40}$') `
        'SourceCommit must be lowercase 40-hex.'
    return $Value
}

function Assert-Stage5LocalUuidV4 {
    param([object]$Value, [string]$Context = 'Cohort nonce')
    Assert-Stage5LocalCondition ($Value -is [string] -and
        $Value -cmatch '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
        "$Context must be a lowercase UUID v4."
    return $Value
}

function Assert-Stage5LocalUtc {
    param([object]$Value, [string]$Context = 'UTC timestamp')
    Assert-Stage5LocalCondition ($Value -is [string] -and
        $Value -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$') `
        "$Context must use canonical seven-digit UTC precision."
    [DateTimeOffset]$parsed = [DateTimeOffset]::MinValue
    Assert-Stage5LocalCondition ([DateTimeOffset]::TryParseExact(
        $Value, 'yyyy-MM-ddTHH:mm:ss.fffffffZ',
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal,
        [ref]$parsed)) "$Context is not a valid calendar timestamp."
    return $Value
}

function Get-Stage5LocalStreamSha256 {
    param([IO.Stream]$Stream)
    Assert-Stage5LocalCondition ($null -ne $Stream -and $Stream.CanRead -and $Stream.CanSeek) `
        'SHA-256 stream is not readable and seekable.'
    $origin = $Stream.Position
    try {
        $Stream.Position = 0
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return (($algorithm.ComputeHash($Stream) | ForEach-Object {
                $_.ToString('X2')
            }) -join '')
        }
        finally { $algorithm.Dispose() }
    }
    finally { $Stream.Position = $origin }
}

function Get-Stage5LocalFileBinding {
    param([string]$Path, [string]$Context = 'File')
    $full = Assert-Stage5LocalNoReparsePath $Path Leaf $Context
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Assert-Stage5LocalCondition ($stream.Length -gt 0) "$Context is empty."
        $length = [uint64]$stream.Length
        $hash = Get-Stage5LocalStreamSha256 $stream
    }
    finally { $stream.Dispose() }
    return [pscustomobject]@{
        fullPath = $full
        sha256 = $hash
        byteLength = $length
    }
}

function Get-Stage5LocalTextSha256 {
    param([string]$Text)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('X2')
        }) -join '')
    }
    finally { $algorithm.Dispose() }
}

function ConvertFrom-Stage5LocalJson {
    param(
        [Parameter(Mandatory = $true)][string]$Json,
        [string]$Context = 'JSON document'
    )
    try {
        $converter = Get-Command ConvertFrom-Json -ErrorAction Stop
        if ($converter.Parameters.ContainsKey('DateKind')) {
            return $Json | ConvertFrom-Json -DateKind String
        }
        return $Json | ConvertFrom-Json
    }
    catch {
        throw "$Context is not valid JSON."
    }
}

function Get-Stage5LocalRuntimeRoot {
    param([string]$Executable, [string]$Title)
    $full = Assert-Stage5LocalNoReparsePath $Executable Leaf "$Title executable"
    $leaf = [IO.Path]::GetFileName($full)
    $expectedLeaf = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    Assert-Stage5LocalCondition ($leaf -ieq $expectedLeaf) `
        "$Title executable must be $expectedLeaf."
    $root = Split-Path -Parent $full
    [void](Assert-Stage5LocalNoReparsePath $root Container "$Title runtime root")
    return $root
}

function Get-Stage5LocalRotateAddChecksum {
    param([byte[]]$Bytes)
    Assert-Stage5LocalCondition ($null -ne $Bytes -and $Bytes.Length -gt 0) `
        'Map entry bytes are empty.'
    [uint64]$value = 0
    foreach ($byte in $Bytes) {
        $value = (($value -shl 1) + [uint64]$byte + ($value -shr 31)) -band [uint64]4294967295
    }
    return [uint32]$value
}

function Read-Stage5LocalBigEndianUInt32 {
    param([IO.BinaryReader]$Reader)
    $bytes = $Reader.ReadBytes(4)
    Assert-Stage5LocalCondition ($bytes.Length -eq 4) 'BIGF table integer is truncated.'
    [array]::Reverse($bytes)
    return [uint32][BitConverter]::ToUInt32($bytes, 0)
}

function Get-Stage5LocalBigEntryBinding {
    param(
        [string]$ArchivePath,
        [string]$Title,
        [string]$MapName,
        [Collections.IDictionary]$ExpectedMap
    )
    $expectedArchivePath = Assert-Stage5LocalJsonString $ExpectedMap.archivePath `
        "$Title expected BIG archive path"
    $expectedEntryPath = Assert-Stage5LocalJsonString $ExpectedMap.entryPath `
        "$Title expected BIG entry path"
    $expectedSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $ExpectedMap.sha256 "$Title expected map SHA-256") `
        "$Title expected map SHA-256"
    $expectedByteLength = Assert-Stage5LocalJsonInteger $ExpectedMap.byteLength `
        "$Title expected map byteLength"
    $expectedMapCrc = Assert-Stage5LocalJsonInteger $ExpectedMap.mapCrc `
        "$Title expected map CRC"
    [decimal]$expectedByteLengthValue = $expectedByteLength
    [decimal]$expectedMapCrcValue = $expectedMapCrc
    Assert-Stage5LocalCondition ($expectedByteLengthValue -ge 4 -and
        $expectedByteLengthValue -le 16777216 -and
        $expectedMapCrcValue -gt 0 -and
        $expectedMapCrcValue -le [decimal]4294967295) `
        "$Title expected map binding is outside the bounded range."
    $archive = Get-Stage5LocalFileBinding $ArchivePath "$Title BIG archive"
    Assert-Stage5LocalCondition ($archive.byteLength -ge 16) "$Title BIG archive is truncated."
    $stream = [IO.File]::Open($archive.fullPath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
        Assert-Stage5LocalCondition ($magic -ceq 'BIGF') "$Title archive is not a BIGF archive."
        [void]$reader.ReadUInt32()
        $count = Read-Stage5LocalBigEndianUInt32 $reader
        # BIGF stores a fourth 32-bit header field before the directory.  The
        # game seeks to 0x10 before reading entries; consume that field rather
        # than treating it as an entry offset.
        [void]$reader.ReadUInt32()
        Assert-Stage5LocalCondition ($count -gt 0 -and $count -le 100000) `
            "$Title BIGF entry count is outside the bounded range."
        $found = $null
        $target = $MapName.Replace('/', '\')
        for ($index = 0; $index -lt $count; ++$index) {
            $offset = [uint64](Read-Stage5LocalBigEndianUInt32 $reader)
            $size = [uint64](Read-Stage5LocalBigEndianUInt32 $reader)
            $nameBytes = New-Object Collections.Generic.List[byte]
            do {
                $character = $reader.ReadByte()
                if ($character -eq 0) { break }
                [void]$nameBytes.Add($character)
                Assert-Stage5LocalCondition ($nameBytes.Count -le 1024) `
                    'BIGF entry name exceeds its fixed bound.'
            } while ($true)
            Assert-Stage5LocalCondition ($nameBytes.Count -gt 0) `
                'BIGF entry name is empty.'
            $name = [Text.Encoding]::ASCII.GetString($nameBytes.ToArray())
            $normalized = $name.Replace('/', '\')
            if ($normalized -ieq $target) {
                Assert-Stage5LocalCondition ($null -eq $found) `
                    "$Title BIGF contains duplicate map entries."
                $found = [pscustomobject]@{ offset = $offset; size = $size; name = $name }
            }
        }
        Assert-Stage5LocalCondition ($null -ne $found) "$Title map entry was not found in its BIG archive."
        $tableEnd = [uint64]$stream.Position
        Assert-Stage5LocalCondition ($found.size -ge 4 -and $found.size -le 16777216 -and
            $found.offset -ge $tableEnd -and $found.offset -le [uint64]$stream.Length -and
            $found.size -le ([uint64]$stream.Length - $found.offset)) `
            "$Title map entry span is outside the bounded BIG archive."
        $stream.Position = [int64]$found.offset
        $mapBytes = $reader.ReadBytes([int]$found.size)
        Assert-Stage5LocalCondition ($mapBytes.Length -eq [int]$found.size) "$Title map entry is truncated."
        $header = [Text.Encoding]::ASCII.GetString($mapBytes, 0, 4)
        $encoding = if ([Text.Encoding]::ASCII.GetString($mapBytes, 0, 3) -ceq 'EAR' -and $mapBytes[3] -eq 0) {
            $script:MapEntryEncoding
        }
        elseif ($header -ceq 'CkMp') { 'CkMp' }
        else { '' }
        Assert-Stage5LocalCondition (-not [string]::IsNullOrEmpty($encoding)) `
            "$Title map entry has an unsupported map header."
        Assert-Stage5LocalCondition ($encoding -ceq $script:MapEntryEncoding) `
            "$Title map entry is not the required raw EAR refpack payload."
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            $mapSha = (($algorithm.ComputeHash($mapBytes) | ForEach-Object {
                $_.ToString('X2')
            }) -join '')
        }
        finally { $algorithm.Dispose() }
        $mapCrc = Get-Stage5LocalRotateAddChecksum $mapBytes
        Assert-Stage5LocalCondition ($mapSha -ceq $expectedSha256 -and
            [uint64]$mapBytes.Length -eq [uint64]$expectedByteLengthValue -and
            [uint32]$mapCrc -eq [uint32]$expectedMapCrcValue) `
            "$Title BIG map bytes do not match the independently verified entry binding."
        return [ordered]@{
            title = $Title
            archivePath = $expectedArchivePath
            entryPath = $expectedEntryPath
            sha256 = $mapSha
            byteLength = [uint64]$mapBytes.Length
            mapCrc = [uint32]$mapCrc
            crcAlgorithm = $script:MapCrcAlgorithm
            encoding = $encoding
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-Stage5LocalRuntimeFiles {
    param([string]$Executable, [string]$Title)
    $root = Get-Stage5LocalRuntimeRoot $Executable $Title
    $runtimeLeaf = if ($Title -ceq 'Generals') { 'GeneralsRuntime' } else { 'ZeroHourRuntime' }
    $records = New-Object 'Collections.Generic.List[object]'
    foreach ($relative in $script:RequiredFilesByTitle[$Title]) {
        [void](Assert-Stage5LocalSafeRelativePath $relative "$Title runtime file")
        $full = Assert-Stage5LocalPathBelow $root (Join-Path $root ($relative -replace '/', '\')) "$Title runtime file"
        $binding = Get-Stage5LocalFileBinding $full "$Title runtime file $relative"
        $records.Add([ordered]@{
            title = $Title
            path = "$runtimeLeaf/$relative"
            sha256 = $binding.sha256
            byteLength = [uint64]$binding.byteLength
        })
    }
    return [pscustomobject]@{ root = $root; runtimeLeaf = $runtimeLeaf; files = @($records.ToArray()) }
}

function Get-Stage5LocalExpectedRuntimeClosure {
    param([object]$ExpectedRuntimeClosure)
    Assert-Stage5LocalExactProperties $ExpectedRuntimeClosure `
        @('dependencyManifestSha256', 'closureSha256') 'Expected runtime closure'
    $dependencyManifestSha256 = Assert-Stage5LocalJsonString `
        (Get-Stage5LocalProperty $ExpectedRuntimeClosure 'dependencyManifestSha256' 'Expected runtime closure') `
        'Expected runtime dependency-manifest SHA-256'
    $closureSha256 = Assert-Stage5LocalJsonString `
        (Get-Stage5LocalProperty $ExpectedRuntimeClosure 'closureSha256' 'Expected runtime closure') `
        'Expected runtime closure SHA-256'
    return [ordered]@{
        dependencyManifestSha256 = Assert-Stage5LocalSha256 $dependencyManifestSha256 `
            'Expected runtime dependency-manifest SHA-256'
        closureSha256 = Assert-Stage5LocalSha256 $closureSha256 `
            'Expected runtime closure SHA-256'
    }
}

function Get-Stage5LocalExpectedMapCrcs {
    param([object]$ExpectedMapCrcs)
    Assert-Stage5LocalCondition ($null -ne $ExpectedMapCrcs) `
        'Expected map CRCs must contain exactly Generals and ZeroHour.'
    $keys = @(if ($ExpectedMapCrcs -is [Collections.IDictionary]) {
        @($ExpectedMapCrcs.Keys | ForEach-Object { [string]$_ })
    }
    else { @($ExpectedMapCrcs.PSObject.Properties.Name) })
    Assert-Stage5LocalCondition ($keys.Count -eq 2 -and
        @($keys | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_, 'Generals')
        }).Count -eq 1 -and
        @($keys | Where-Object {
            [StringComparer]::Ordinal.Equals([string]$_, 'ZeroHour')
        }).Count -eq 1) `
        'Expected map CRCs must contain exactly Generals and ZeroHour.'
    $result = [ordered]@{}
    foreach ($title in @('Generals', 'ZeroHour')) {
        $rawValue = Get-Stage5LocalProperty $ExpectedMapCrcs $title `
            'Expected map CRCs'
        [void](Assert-Stage5LocalJsonInteger $rawValue "$title map CRC")
        [decimal]$value = $rawValue
        Assert-Stage5LocalCondition ($value -gt 0 -and $value -le [decimal]4294967295) `
            "$title map CRC is outside UInt32 range."
        Assert-Stage5LocalCondition ($value -eq [decimal]$script:ExpectedMapEntries[$title].mapCrc) `
            "$title map CRC is not the independently verified map binding."
        $result[$title] = [uint32]$value
    }
    return $result
}

function Get-Stage5LocalArtifactBinding {
    param(
        [string]$ManifestPath,
        [string]$GeneralsExecutable,
        [string]$ZeroHourExecutable,
        [object]$ExpectedSourceCommit,
        [Collections.IDictionary]$ExpectedRuntimeClosure
    )
    $expectedSourceCommit = Assert-Stage5LocalSourceCommit `
        (Assert-Stage5LocalJsonString $ExpectedSourceCommit `
            'Expected artifact-set sourceCommit')
    $manifest = Get-Stage5LocalFileBinding $ManifestPath 'Artifact-set manifest'
    $artifactRoot = Split-Path -Parent $manifest.fullPath
    $json = [IO.File]::ReadAllText($manifest.fullPath)
    $document = ConvertFrom-Stage5LocalJson $json 'Artifact-set manifest'
    Assert-Stage5LocalExactProperties $document `
        @('schemaVersion', 'sourceCommit', 'productSet', 'architecture', 'artifacts', 'runtimeClosure') `
        'Artifact-set manifest'
    [void](Assert-Stage5LocalJsonInteger $document.schemaVersion 'Artifact-set manifest schemaVersion')
    Assert-Stage5LocalCondition ($document.schemaVersion -eq 1) `
        'Artifact-set manifest schemaVersion is unsupported.'
    $sourceCommit = Assert-Stage5LocalJsonString $document.sourceCommit `
        'Artifact-set manifest sourceCommit'
    Assert-Stage5LocalCondition ($sourceCommit -ceq $expectedSourceCommit) `
        'Artifact-set manifest sourceCommit is stale or substituted.'
    $architecture = Assert-Stage5LocalJsonString $document.architecture `
        'Artifact-set manifest architecture'
    Assert-Stage5LocalCondition ($architecture -ceq 'x64') `
        'Artifact-set manifest architecture is not x64.'
    Assert-Stage5LocalCondition ($document.productSet -is [Array] -and
        $document.productSet.Count -eq 2 -and
        (Assert-Stage5LocalJsonString $document.productSet[0] 'Artifact-set productSet[0]') -ceq 'Generals' -and
        (Assert-Stage5LocalJsonString $document.productSet[1] 'Artifact-set productSet[1]') -ceq 'ZeroHour') `
        'Artifact-set manifest productSet is not the exact two-title set.'
    Assert-Stage5LocalExactProperties $document.runtimeClosure `
        @('dependencyManifest', 'closureSha256') 'Artifact-set runtimeClosure'
    Assert-Stage5LocalExactProperties $document.runtimeClosure.dependencyManifest `
        @('path', 'sha256') 'Artifact-set dependency manifest'
    $runtimeClosure = Get-Stage5LocalExpectedRuntimeClosure $ExpectedRuntimeClosure
    $declaredClosure = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $document.runtimeClosure.closureSha256 `
            'Artifact-set closure SHA-256') `
        'Artifact-set closure SHA-256'
    Assert-Stage5LocalCondition ($declaredClosure -ceq $runtimeClosure.closureSha256) `
        'Artifact-set closure SHA-256 does not match the expected runtime closure.'
    $dependencyRelativePath = Assert-Stage5LocalJsonString `
        $document.runtimeClosure.dependencyManifest.path `
        'Artifact-set dependency manifest path'
    [void](Assert-Stage5LocalSafeRelativePath $dependencyRelativePath `
        'Artifact-set dependency manifest path')
    $dependencyPath = Assert-Stage5LocalPathBelow $artifactRoot `
        (Join-Path $artifactRoot $dependencyRelativePath) `
        'Artifact-set dependency manifest'
    $dependency = Get-Stage5LocalFileBinding $dependencyPath 'Artifact-set dependency manifest'
    $declaredDependencySha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $document.runtimeClosure.dependencyManifest.sha256 `
            'Artifact-set dependency manifest SHA-256') `
        'Artifact-set dependency manifest SHA-256'
    Assert-Stage5LocalCondition ($dependency.sha256 -ceq
        $declaredDependencySha256) `
        'Artifact-set dependency manifest bytes do not match their declared hash.'
    Assert-Stage5LocalCondition ($dependency.sha256 -ceq $runtimeClosure.dependencyManifestSha256) `
        'Artifact-set dependency-manifest SHA-256 does not match the expected runtime closure.'

    $roles = @('generals-executable', 'generals-launcher', 'generals-launcher-config',
        'zerohour-executable', 'zerohour-launcher', 'zerohour-launcher-config')
    Assert-Stage5LocalCondition ($document.artifacts -is [Array] -and $document.artifacts.Count -eq 6) `
        'Artifact-set manifest must contain exactly six artifact records.'
    $seenRoles = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($artifact in $document.artifacts) {
        Assert-Stage5LocalExactProperties $artifact @('role', 'path', 'sha256') 'Artifact-set artifact'
        $role = Assert-Stage5LocalJsonString $artifact.role 'Artifact-set artifact role'
        Assert-Stage5LocalCondition ((Test-Stage5LocalOrdinalMember $role $roles) -and
            $seenRoles.Add($role)) `
            'Artifact-set artifact roles are duplicated or unknown.'
        $artifactRelativePath = Assert-Stage5LocalJsonString $artifact.path `
            "Artifact-set artifact $role path"
        [void](Assert-Stage5LocalSafeRelativePath $artifactRelativePath `
            'Artifact-set artifact path')
        $artifactPath = Assert-Stage5LocalPathBelow $artifactRoot `
            (Join-Path $artifactRoot $artifactRelativePath) 'Artifact-set artifact'
        $actual = Get-Stage5LocalFileBinding $artifactPath "Artifact-set artifact $role"
        $declaredArtifactSha256 = Assert-Stage5LocalSha256 `
            (Assert-Stage5LocalJsonString $artifact.sha256 `
                "Artifact-set artifact $role SHA-256") `
            "Artifact-set artifact $role SHA-256"
        Assert-Stage5LocalCondition ($actual.sha256 -ceq
            $declaredArtifactSha256) `
            "Artifact-set artifact $role bytes do not match their declared hash."
    }
    foreach ($role in $roles) {
        Assert-Stage5LocalCondition ($seenRoles.Contains($role)) "Artifact-set artifact $role is missing."
    }
    $actualExecutables = [ordered]@{
        'generals-executable' = (Get-Stage5LocalFileBinding $GeneralsExecutable 'Generals executable').sha256
        'zerohour-executable' = (Get-Stage5LocalFileBinding $ZeroHourExecutable 'ZeroHour executable').sha256
    }
    foreach ($artifact in $document.artifacts) {
        if ($actualExecutables.Contains($artifact.role)) {
            Assert-Stage5LocalCondition ($artifact.sha256 -ceq $actualExecutables[$artifact.role]) `
                "Artifact-set $($artifact.role) does not bind the supplied executable."
        }
    }
    return [pscustomobject]@{
        path = $manifest.fullPath
        sha256 = $manifest.sha256
        runtimeClosure = $runtimeClosure
        sourceCommit = $sourceCommit
    }
}

function Get-Stage5LocalCanonicalFiles {
    param([object[]]$Files)
    # Evidence closure order must not depend on the host's culture or the
    # case-insensitive ordering used by Sort-Object on Windows PowerShell.
    $byKey = New-Object 'Collections.Generic.SortedDictionary[string,object]' ([StringComparer]::Ordinal)
    foreach ($file in $Files) {
        $key = '{0}|{1}' -f $file.title, $file.path
        Assert-Stage5LocalCondition (-not $byKey.ContainsKey($key)) `
            'Local file bindings contain a duplicate title/path.'
        $byKey.Add($key, $file)
    }
    $ordered = @($byKey.Values)
    $text = (@($ordered | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.path, $_.sha256, $_.byteLength
    }) -join "`n") + "`n"
    return [pscustomobject]@{
        files = $ordered
        text = $text
        sha256 = Get-Stage5LocalTextSha256 $text
    }
}

function Assert-Stage5LocalManifestDocument {
    param(
        [object]$Document,
        [object]$ExpectedSourceCommit,
        [Collections.IDictionary]$ExpectedRuntimeClosure,
        [object]$ExpectedMapName,
        [object]$ExpectedMapCrcs,
        [object[]]$ActualFiles,
        [object[]]$ActualMapEntries,
        [object]$ExpectedArtifactSetSha256
    )
    $expectedSourceCommit = Assert-Stage5LocalSourceCommit `
        (Assert-Stage5LocalJsonString $ExpectedSourceCommit 'Expected sourceCommit')
    $expectedMapName = Assert-Stage5LocalJsonString $ExpectedMapName 'Expected mapName'
    $expectedArtifactSetSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $ExpectedArtifactSetSha256 'Expected artifact-set SHA-256') `
        'Expected artifact-set SHA-256'
    Assert-Stage5LocalExactProperties $Document @(
        'schemaVersion', 'evidenceKind', 'producer', 'status',
        'finalAcceptanceClaim', 'canonicalQualification', 'promotionGrant',
        'externalQualificationSkipped', 'manualTestingDeferred',
        'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'cohortNonce', 'cohortCreatedUtc', 'mapName', 'mapCrcs',
        'mapEntries', 'files', 'closureSha256'
    ) 'Local lockstep diagnostic-data manifest'
    $schemaVersion = Assert-Stage5LocalJsonInteger $Document.schemaVersion `
        'Local lockstep diagnostic-data schemaVersion'
    $evidenceKind = Assert-Stage5LocalJsonString $Document.evidenceKind `
        'Local lockstep diagnostic-data evidenceKind'
    $producer = Assert-Stage5LocalJsonString $Document.producer `
        'Local lockstep diagnostic-data producer'
    $status = Assert-Stage5LocalJsonString $Document.status `
        'Local lockstep diagnostic-data status'
    Assert-Stage5LocalCondition ($schemaVersion -eq $script:SchemaVersion -and
        $evidenceKind -ceq $script:EvidenceKind -and
        $producer -ceq $script:Producer -and
        $status -ceq 'ready') `
        'Local lockstep diagnostic-data schema, producer, or status is invalid.'
    Assert-Stage5LocalCondition ($Document.finalAcceptanceClaim -is [bool] -and -not $Document.finalAcceptanceClaim -and
        $Document.canonicalQualification -is [bool] -and -not $Document.canonicalQualification -and
        $Document.promotionGrant -is [bool] -and -not $Document.promotionGrant -and
        $Document.externalQualificationSkipped -is [bool] -and $Document.externalQualificationSkipped -and
        $Document.manualTestingDeferred -is [bool] -and $Document.manualTestingDeferred) `
        'Local lockstep diagnostic-data disposition is not explicitly non-final.'
    $sourceCommit = Assert-Stage5LocalSourceCommit `
        (Assert-Stage5LocalJsonString $Document.sourceCommit `
            'Local lockstep diagnostic-data sourceCommit')
    Assert-Stage5LocalCondition ($sourceCommit -ceq $expectedSourceCommit) `
        'Local lockstep diagnostic-data sourceCommit is stale or substituted.'
    $artifactSetSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $Document.artifactSetSha256 `
            'Local lockstep diagnostic-data artifact-set SHA-256') `
        'Local artifact-set SHA-256'
    Assert-Stage5LocalCondition ($artifactSetSha256 -ceq $expectedArtifactSetSha256) `
        'Local lockstep diagnostic-data artifactSetSha256 is stale or substituted.'
    $cohortNonce = Assert-Stage5LocalUuidV4 `
        (Assert-Stage5LocalJsonString $Document.cohortNonce `
            'Local cohort nonce')
    $cohortCreatedUtc = Assert-Stage5LocalUtc `
        (Assert-Stage5LocalJsonString $Document.cohortCreatedUtc `
            'Local cohort created UTC')
    $mapName = Assert-Stage5LocalJsonString $Document.mapName `
        'Local lockstep diagnostic-data mapName'
    Assert-Stage5LocalCondition ($mapName -ceq $expectedMapName -and
        $mapName -ceq $script:MapName) `
        'Local lockstep diagnostic-data mapName is stale, substituted, or unsupported.'
    $runtimeClosure = Get-Stage5LocalExpectedRuntimeClosure $ExpectedRuntimeClosure
    Assert-Stage5LocalExactProperties $Document.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') 'Local runtime closure'
    $documentDependencyManifestSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $Document.runtimeClosure.dependencyManifestSha256 `
            'Local runtime dependency-manifest SHA-256') `
        'Local runtime dependency-manifest SHA-256'
    $documentClosureSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $Document.runtimeClosure.closureSha256 `
            'Local runtime closure SHA-256') `
        'Local runtime closure SHA-256'
    Assert-Stage5LocalCondition ($documentDependencyManifestSha256 -ceq
        $runtimeClosure.dependencyManifestSha256 -and
        $documentClosureSha256 -ceq $runtimeClosure.closureSha256) `
        'Local runtime closure binding is stale or substituted.'
    $mapCrcs = Get-Stage5LocalExpectedMapCrcs $ExpectedMapCrcs
    Assert-Stage5LocalExactProperties $Document.mapCrcs @('Generals', 'ZeroHour') 'Local map CRCs'
    foreach ($title in @('Generals', 'ZeroHour')) {
        $documentMapCrc = Assert-Stage5LocalJsonInteger $Document.mapCrcs.$title `
            "$title local map CRC"
        [decimal]$documentMapCrcValue = $documentMapCrc
        Assert-Stage5LocalCondition ($documentMapCrcValue -gt 0 -and
            $documentMapCrcValue -le [decimal]4294967295 -and
            $documentMapCrcValue -eq [decimal]$mapCrcs[$title]) `
            "$title local map CRC binding differs."
    }
    Assert-Stage5LocalCondition ($Document.mapEntries -is [Array] -and $Document.mapEntries.Count -eq 2) `
        'Local map-entry bindings must contain exactly two title records.'
    foreach ($entry in $Document.mapEntries) {
        Assert-Stage5LocalExactProperties $entry `
            @('title', 'archivePath', 'entryPath', 'sha256', 'byteLength', 'mapCrc', 'crcAlgorithm', 'encoding') `
            'Local BIG map-entry binding'
        $entryTitle = Assert-Stage5LocalJsonString $entry.title 'Local BIG map-entry title'
        $entryArchivePath = Assert-Stage5LocalJsonString $entry.archivePath `
            'Local BIG map-entry archivePath'
        $entryPath = Assert-Stage5LocalJsonString $entry.entryPath `
            'Local BIG map-entry entryPath'
        $entrySha256 = Assert-Stage5LocalSha256 `
            (Assert-Stage5LocalJsonString $entry.sha256 'Local BIG map-entry SHA-256') `
            'Local BIG map-entry SHA-256'
        $entryByteLength = Assert-Stage5LocalJsonInteger $entry.byteLength `
            'Local BIG map-entry byteLength'
        $entryMapCrc = Assert-Stage5LocalJsonInteger $entry.mapCrc `
            'Local BIG map-entry mapCrc'
        $entryCrcAlgorithm = Assert-Stage5LocalJsonString $entry.crcAlgorithm `
            'Local BIG map-entry crcAlgorithm'
        $entryEncoding = Assert-Stage5LocalJsonString $entry.encoding `
            'Local BIG map-entry encoding'
        Assert-Stage5LocalCondition (Test-Stage5LocalOrdinalMember $entryTitle `
            @('Generals', 'ZeroHour')) `
            'Local BIG map-entry binding has an unknown title.'
        [void](Assert-Stage5LocalSafeRelativePath $entryArchivePath `
            'Local BIG archive runtime-relative path')
        [void](Assert-Stage5LocalSafeRelativePath $entryPath `
            'Local BIG entry relative path')
        [decimal]$entryByteLengthValue = $entryByteLength
        [decimal]$entryMapCrcValue = $entryMapCrc
        Assert-Stage5LocalCondition ($entryByteLengthValue -ge 4 -and
            $entryByteLengthValue -le 16777216 -and
            $entryMapCrcValue -gt 0 -and $entryMapCrcValue -le [decimal]4294967295 -and
            $entryCrcAlgorithm -ceq $script:MapCrcAlgorithm -and
            $entryEncoding -ceq $script:MapEntryEncoding) `
            'Local BIG map-entry metadata is invalid.'
    }
    Assert-Stage5LocalCondition ($Document.files -is [Array] -and $Document.files.Count -eq 12) `
        'Local lockstep data must contain exactly twelve files.'
    foreach ($title in @('Generals', 'ZeroHour')) {
        $titleEntries = @($Document.files | Where-Object { [string]$_.title -ceq $title })
        Assert-Stage5LocalCondition ($titleEntries.Count -eq 6) `
            "$title local lockstep data must contain exactly six files."
        $runtimeLeaf = if ($title -ceq 'Generals') { 'GeneralsRuntime' } else { 'ZeroHourRuntime' }
        foreach ($relative in $script:RequiredFilesByTitle[$title]) {
            $expectedPath = "$runtimeLeaf/$relative"
            Assert-Stage5LocalCondition (@($titleEntries | Where-Object {
                [string]$_.path -ceq $expectedPath
            }).Count -eq 1) "$title local lockstep data omits required file $relative."
        }
    }
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($entry in $Document.files) {
        Assert-Stage5LocalExactProperties $entry @('title', 'path', 'sha256', 'byteLength') 'Local file binding'
        $entryTitle = Assert-Stage5LocalJsonString $entry.title 'Local file binding title'
        $entryPath = Assert-Stage5LocalJsonString $entry.path 'Local file binding path'
        $entrySha256 = Assert-Stage5LocalSha256 `
            (Assert-Stage5LocalJsonString $entry.sha256 'Local file binding SHA-256') `
            'Local file binding SHA-256'
        $entryByteLength = Assert-Stage5LocalJsonInteger $entry.byteLength `
            'Local file binding byteLength'
        Assert-Stage5LocalCondition (Test-Stage5LocalOrdinalMember $entryTitle `
            @('Generals', 'ZeroHour')) `
            'Local file binding has an unknown title.'
        [void](Assert-Stage5LocalSafeRelativePath $entryPath 'Local runtime-relative file path')
        [decimal]$entryByteLengthValue = $entryByteLength
        Assert-Stage5LocalCondition ($entryByteLengthValue -gt 0 -and
            $entryByteLengthValue -le [decimal]9007199254740991) `
            'Local file byteLength is outside the bounded integer range.'
        Assert-Stage5LocalCondition ($seen.Add(('{0}|{1}' -f $entryTitle, $entryPath)) `
            -and ($entryPath -match '^(GeneralsRuntime|ZeroHourRuntime)/')) `
            'Local file bindings contain duplicate or non-runtime paths.'
    }
    $canonical = Get-Stage5LocalCanonicalFiles $Document.files
    for ($index = 0; $index -lt $Document.files.Count; ++$index) {
        Assert-Stage5LocalCondition ($Document.files[$index].title -ceq
            $canonical.files[$index].title -and
            $Document.files[$index].path -ceq
            $canonical.files[$index].path) `
            'Local file bindings must use canonical title/path order.'
    }
    $documentFileClosureSha256 = Assert-Stage5LocalSha256 `
        (Assert-Stage5LocalJsonString $Document.closureSha256 `
            'Local file closure SHA-256') `
        'Local file closure SHA-256'
    Assert-Stage5LocalCondition ($documentFileClosureSha256 -ceq $canonical.sha256) `
        'Local file closure SHA-256 is stale or substituted.'
    Assert-Stage5LocalCondition ($null -ne $ActualFiles -and $ActualFiles.Count -eq 12) `
        'Actual local file observations are incomplete.'
    foreach ($expected in $Document.files) {
        $actual = @($ActualFiles | Where-Object {
            [string]$_.title -ceq [string]$expected.title -and
            [string]$_.path -ceq [string]$expected.path
        })
        Assert-Stage5LocalCondition ($actual.Count -eq 1 -and
            [string]$actual[0].sha256 -ceq [string]$expected.sha256 -and
            [uint64]$actual[0].byteLength -eq [uint64]$expected.byteLength) `
            "Actual $($expected.title) runtime file differs from the retained binding."
    }
    Assert-Stage5LocalCondition ($null -ne $ActualMapEntries -and $ActualMapEntries.Count -eq 2) `
        'Actual local BIG map observations are incomplete.'
    for ($index = 0; $index -lt 2; ++$index) {
        $expected = $Document.mapEntries[$index]
        $actual = $ActualMapEntries[$index]
        $expectedTitle = @('Generals', 'ZeroHour')[$index]
        $known = $script:ExpectedMapEntries[$expectedTitle]
        Assert-Stage5LocalCondition ($null -ne $known -and
            [string]$expected.title -ceq $expectedTitle -and
            [string]$actual.title -ceq $expectedTitle -and
            [string]$expected.archivePath -ceq [string]$known.archivePath -and
            [string]$expected.entryPath -ceq [string]$known.entryPath -and
            [string]$expected.sha256 -ceq [string]$known.sha256 -and
            [uint64]$expected.byteLength -eq [uint64]$known.byteLength -and
            [uint64]$expected.mapCrc -eq [uint64]$known.mapCrc -and
            [string]$expected.archivePath -ceq [string]$actual.archivePath -and
            [string]$expected.entryPath -ceq [string]$actual.entryPath -and
            [string]$expected.sha256 -ceq [string]$actual.sha256 -and
            [uint64]$expected.byteLength -eq [uint64]$actual.byteLength -and
            [uint64]$expected.mapCrc -eq [uint64]$actual.mapCrc -and
            [string]$expected.crcAlgorithm -ceq $script:MapCrcAlgorithm -and
            [string]$expected.encoding -ceq $script:MapEntryEncoding -and
            [string]$actual.crcAlgorithm -ceq $script:MapCrcAlgorithm -and
            [string]$actual.encoding -ceq $script:MapEntryEncoding) `
            "Actual $($actual.title) BIG map-entry binding differs."
    }
    return $canonical
}

function New-Stage5LocalLockstepDiagnosticDataManifest {
    param(
        [Parameter(Mandatory = $true)][string]$GeneralsExecutable,
        [Parameter(Mandatory = $true)][string]$ZeroHourExecutable,
        [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][object]$RuntimeClosure,
        [Parameter(Mandatory = $true)][string]$CohortNonce,
        [Parameter(Mandatory = $true)][string]$CohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$MapName,
        [Parameter(Mandatory = $true)][object]$MapCrcs
    )
    [void](Assert-Stage5LocalSourceCommit $SourceCommit)
    $closure = Get-Stage5LocalExpectedRuntimeClosure $RuntimeClosure
    [void](Assert-Stage5LocalUuidV4 $CohortNonce)
    [void](Assert-Stage5LocalUtc $CohortCreatedUtc)
    $mapCrcBinding = Get-Stage5LocalExpectedMapCrcs $MapCrcs
    Assert-Stage5LocalCondition ($MapName -ceq $script:MapName) `
        'Only the independently verified Twilight Flame map is supported by this local diagnostic contract.'
    $generals = Get-Stage5LocalRuntimeFiles $GeneralsExecutable 'Generals'
    $zeroHour = Get-Stage5LocalRuntimeFiles $ZeroHourExecutable 'ZeroHour'
    Assert-Stage5LocalCondition ($generals.root -ine $zeroHour.root) `
        'Generals and ZeroHour runtime roots must be distinct.'
    $artifact = Get-Stage5LocalArtifactBinding $ArtifactSetManifestPath `
        $GeneralsExecutable $ZeroHourExecutable $SourceCommit $closure
    Assert-Stage5LocalCondition ($artifact.runtimeClosure.closureSha256 -ceq $closure.closureSha256) `
        'Artifact-set/runtime closure binding differs.'
    $mapEntries = @(
        Get-Stage5LocalBigEntryBinding `
            (Join-Path $generals.root 'maps.big') 'Generals' $MapName $script:ExpectedMapEntries['Generals']
        Get-Stage5LocalBigEntryBinding `
            (Join-Path $zeroHour.root 'MapsZH.big') 'ZeroHour' $MapName $script:ExpectedMapEntries['ZeroHour']
    )
    Assert-Stage5LocalCondition ([uint32]$mapEntries[0].mapCrc -eq $mapCrcBinding.Generals -and
        [uint32]$mapEntries[1].mapCrc -eq $mapCrcBinding.ZeroHour) `
        'Observed BIG map CRCs differ from the requested map CRC bindings.'
    $allFiles = @($generals.files + $zeroHour.files)
    $canonical = Get-Stage5LocalCanonicalFiles $allFiles
    $document = [ordered]@{
        schemaVersion = $script:SchemaVersion
        evidenceKind = $script:EvidenceKind
        producer = $script:Producer
        status = 'ready'
        finalAcceptanceClaim = $false
        canonicalQualification = $false
        promotionGrant = $false
        externalQualificationSkipped = $true
        manualTestingDeferred = $true
        sourceCommit = $SourceCommit
        artifactSetSha256 = $artifact.sha256
        runtimeClosure = $closure
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        mapName = $MapName
        mapCrcs = $mapCrcBinding
        mapEntries = $mapEntries
        files = @($canonical.files)
        closureSha256 = $canonical.sha256
    }
    [void](Assert-Stage5LocalManifestDocument $document $SourceCommit $closure $MapName `
        $mapCrcBinding $allFiles $mapEntries $artifact.sha256)
    return [pscustomobject]@{
        document = $document
        path = $null
        manifestSha256 = $null
        closureSha256 = $canonical.sha256
        fileCount = 12
        sourceCommit = $SourceCommit
        artifactSetSha256 = $artifact.sha256
        runtimeClosure = $closure
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        mapName = $MapName
        mapCrcs = $mapCrcBinding
        mapEntries = @($mapEntries)
        files = @($canonical.files)
    }
}

function Write-Stage5LocalLockstepDiagnosticDataManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Manifest
    )
    $hasDocument = Test-Stage5LocalHasExactProperty $Manifest 'document'
    if ($hasDocument) {
        # New/Read return a documented binding wrapper. Only its document is
        # serialized; derived wrapper metadata is recomputed below, never used
        # as byte-provenance authority. Keep the document-only input supported
        # for callers that deliberately supply just the manifest document.
        $wrapperNames = @($Manifest.PSObject.Properties.Name)
        if ($Manifest -is [Collections.IDictionary]) { $wrapperNames = @($Manifest.Keys) }
        if ($wrapperNames.Count -eq 1) {
            Assert-Stage5LocalExactProperties $Manifest @('document') `
                'Local lockstep diagnostic-data wrapper'
        }
        else {
            Assert-Stage5LocalExactProperties $Manifest @('document', 'path',
                'manifestSha256', 'closureSha256', 'fileCount', 'sourceCommit',
                'artifactSetSha256', 'runtimeClosure', 'cohortNonce',
                'cohortCreatedUtc', 'mapName', 'mapCrcs', 'mapEntries', 'files') `
                'Local lockstep diagnostic-data binding wrapper'
        }
        $document = Get-Stage5LocalProperty $Manifest 'document' `
            'Local lockstep diagnostic-data wrapper'
    }
    else { $document = $Manifest }
    $full = Get-Stage5LocalFullPath $Path 'Local lockstep diagnostic-data manifest output'
    Assert-Stage5LocalCondition (-not (Test-Path -LiteralPath $full)) `
        "Local lockstep diagnostic-data output must be fresh: $full"
    $parent = Split-Path -Parent $full
    [void](Assert-Stage5LocalNoReparsePath $parent Container 'Local lockstep diagnostic-data output directory')
    Assert-Stage5LocalExactProperties $document @(
        'schemaVersion', 'evidenceKind', 'producer', 'status',
        'finalAcceptanceClaim', 'canonicalQualification', 'promotionGrant',
        'externalQualificationSkipped', 'manualTestingDeferred',
        'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'cohortNonce', 'cohortCreatedUtc', 'mapName', 'mapCrcs',
        'mapEntries', 'files', 'closureSha256'
    ) 'Local lockstep diagnostic-data manifest before write'
    $writeClosure = Get-Stage5LocalExpectedRuntimeClosure $document.runtimeClosure
    [void](Assert-Stage5LocalManifestDocument $document $document.sourceCommit `
        $writeClosure $document.mapName $document.mapCrcs $document.files `
        $document.mapEntries $document.artifactSetSha256)
    $json = $document | ConvertTo-Json -Depth 20
    $stream = [IO.File]::Open($full, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
        try {
            $writer.WriteLine($json)
            $writer.Flush()
            $stream.Flush($true)
        }
        finally { $writer.Dispose() }
    }
    finally { $stream.Dispose() }
    $written = Get-Stage5LocalFileBinding $full 'Written local lockstep diagnostic-data manifest'
    return [pscustomobject]@{
        path = $full
        manifestSha256 = $written.sha256
        closureSha256 = $document.closureSha256
        fileCount = @($document.files).Count
        sourceCommit = $document.sourceCommit
        artifactSetSha256 = $document.artifactSetSha256
        runtimeClosure = $document.runtimeClosure
        cohortNonce = $document.cohortNonce
        cohortCreatedUtc = $document.cohortCreatedUtc
        mapName = $document.mapName
        mapCrcs = $document.mapCrcs
        mapEntries = $document.mapEntries
        files = $document.files
        document = $document
    }
}

function Read-Stage5LocalLockstepDiagnosticData {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
        [Parameter(Mandatory = $true)][string]$GeneralsExecutable,
        [Parameter(Mandatory = $true)][string]$ZeroHourExecutable,
        [Parameter(Mandatory = $true)][object]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][object]$ExpectedRuntimeClosure,
        [Parameter(Mandatory = $true)][object]$ExpectedMapName,
        [Parameter(Mandatory = $true)][object]$ExpectedMapCrcs,
        [object]$ExpectedCohortNonce = '',
        [object]$ExpectedCohortCreatedUtc = ''
    )
    $expectedSourceCommit = Assert-Stage5LocalSourceCommit `
        (Assert-Stage5LocalJsonString $ExpectedSourceCommit 'Expected sourceCommit')
    $closure = Get-Stage5LocalExpectedRuntimeClosure $ExpectedRuntimeClosure
    $mapCrcs = Get-Stage5LocalExpectedMapCrcs $ExpectedMapCrcs
    $expectedMapName = Assert-Stage5LocalJsonString $ExpectedMapName 'Expected mapName'
    Assert-Stage5LocalCondition ($expectedMapName -ceq $script:MapName) `
        'Expected map name is unsupported by this local diagnostic contract.'
    $full = Assert-Stage5LocalNoReparsePath $ManifestPath Leaf 'Local lockstep diagnostic-data manifest'
    $manifestFile = Get-Stage5LocalFileBinding $full 'Local lockstep diagnostic-data manifest'
    $document = ConvertFrom-Stage5LocalJson `
        ([IO.File]::ReadAllText($full)) 'Local lockstep diagnostic-data manifest'
    $expectedCohortNonce = Assert-Stage5LocalJsonString $ExpectedCohortNonce `
        'Expected cohort nonce'
    $expectedCohortCreatedUtc = Assert-Stage5LocalJsonString $ExpectedCohortCreatedUtc `
        'Expected cohort created UTC'
    Assert-Stage5LocalCondition (-not [string]::IsNullOrWhiteSpace($expectedCohortNonce)) `
        'ExpectedCohortNonce is required for a bound local diagnostic read.'
    Assert-Stage5LocalCondition (-not [string]::IsNullOrWhiteSpace($expectedCohortCreatedUtc)) `
        'ExpectedCohortCreatedUtc is required for a bound local diagnostic read.'
    [void](Assert-Stage5LocalUuidV4 $expectedCohortNonce 'Expected cohort nonce')
    [void](Assert-Stage5LocalUtc $expectedCohortCreatedUtc 'Expected cohort created UTC')
    $artifact = Get-Stage5LocalArtifactBinding $ArtifactSetManifestPath `
        $GeneralsExecutable $ZeroHourExecutable $expectedSourceCommit $closure
    Assert-Stage5LocalCondition ($manifestFile.sha256 -ne '') 'Manifest SHA-256 could not be retained.'
    $generals = Get-Stage5LocalRuntimeFiles $GeneralsExecutable 'Generals'
    $zeroHour = Get-Stage5LocalRuntimeFiles $ZeroHourExecutable 'ZeroHour'
    Assert-Stage5LocalCondition ($generals.root -ine $zeroHour.root) `
        'Generals and ZeroHour runtime roots must be distinct.'
    $actualFiles = @($generals.files + $zeroHour.files)
    $actualMapEntries = @(
        Get-Stage5LocalBigEntryBinding `
            (Join-Path $generals.root 'maps.big') 'Generals' $expectedMapName $script:ExpectedMapEntries['Generals']
        Get-Stage5LocalBigEntryBinding `
            (Join-Path $zeroHour.root 'MapsZH.big') 'ZeroHour' $expectedMapName $script:ExpectedMapEntries['ZeroHour']
    )
    $canonical = Assert-Stage5LocalManifestDocument $document $expectedSourceCommit $closure `
        $expectedMapName $mapCrcs $actualFiles $actualMapEntries $artifact.sha256
    Assert-Stage5LocalCondition ($document.cohortNonce -ceq $expectedCohortNonce -and
        $document.cohortCreatedUtc -ceq $expectedCohortCreatedUtc) `
        'Local diagnostic-data cohort binding is stale or substituted.'
    return [pscustomobject]@{
        path = $full
        manifestSha256 = $manifestFile.sha256
        closureSha256 = $canonical.sha256
        fileCount = 12
        sourceCommit = $document.sourceCommit
        artifactSetSha256 = $document.artifactSetSha256
        runtimeClosure = $document.runtimeClosure
        cohortNonce = $document.cohortNonce
        cohortCreatedUtc = $document.cohortCreatedUtc
        mapName = $document.mapName
        mapCrcs = $document.mapCrcs
        mapEntries = $document.mapEntries
        files = $document.files
        document = $document
    }
}

Export-ModuleMember -Function `
    Read-Stage5LocalLockstepDiagnosticData, `
    New-Stage5LocalLockstepDiagnosticDataManifest, `
    Write-Stage5LocalLockstepDiagnosticDataManifest
