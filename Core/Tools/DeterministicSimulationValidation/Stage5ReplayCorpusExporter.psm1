Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1')

$script:Stage5ExporterMaximumReplayBytes = [Int64](256 * 1024 * 1024)
$script:Stage5ExporterMaximumJsonBytes = [Int64](64 * 1024 * 1024)
$script:Stage5ExporterCommitObserver = $null

if ($null -eq ('Stage5ReplayExporterNative.FileIdentityNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Stage5ReplayExporterNative
{
    public static class FileIdentityNative
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct ByHandleFileInformation
        {
            public UInt32 FileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
            public UInt32 VolumeSerialNumber;
            public UInt32 FileSizeHigh;
            public UInt32 FileSizeLow;
            public UInt32 NumberOfLinks;
            public UInt32 FileIndexHigh;
            public UInt32 FileIndexLow;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafeFileHandle CreateFileW(
            string path, UInt32 desiredAccess, UInt32 shareMode, IntPtr securityAttributes,
            UInt32 creationDisposition, UInt32 flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFileInformationByHandle(
            SafeFileHandle handle, out ByHandleFileInformation information);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern UInt32 GetFinalPathNameByHandleW(
            SafeFileHandle handle, StringBuilder path, UInt32 pathLength, UInt32 flags);

        [StructLayout(LayoutKind.Sequential)]
        private struct FileDispositionInformation
        {
            [MarshalAs(UnmanagedType.Bool)]
            public bool DeleteFile;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle handle, Int32 informationClass,
            ref FileDispositionInformation information, UInt32 bufferSize);

        public static bool MarkFileForDeletion(SafeFileHandle handle)
        {
            FileDispositionInformation information = new FileDispositionInformation();
            information.DeleteFile = true;
            return SetFileInformationByHandle(handle, 4, ref information,
                (UInt32)Marshal.SizeOf(information));
        }
    }
}
'@
}

function Assert-Stage5ExporterCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Test-Stage5ExporterInteger {
    param([object]$Value)
    if ($null -eq $Value) { return $false }
    return $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}

function Get-Stage5ExporterMetadataInteger {
    param(
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Metadata,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][Int64]$Minimum,
        [Parameter(Mandatory = $true)][Int64]$Maximum
    )
    $raw = $Metadata[$Name]
    Assert-Stage5ExporterCondition ($null -ne $raw) `
        "Fresh replay metadata is missing '$Name'."
    if ($raw -isnot [string]) {
        Assert-Stage5ExporterCondition (Test-Stage5ExporterInteger $raw) `
            "Fresh replay metadata '$Name' must be an integer."
    }
    [Int64]$value = 0
    Assert-Stage5ExporterCondition ([Int64]::TryParse([string]$raw, [ref]$value) -and
        $value -ge $Minimum -and $value -le $Maximum) `
        "Fresh replay metadata '$Name' must be an integer between $Minimum and $Maximum."
    return $value
}

function Get-Stage5ExporterRecordPropertyValue {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )
    if ($Record -is [Collections.IDictionary]) {
        Assert-Stage5ExporterCondition ($Record.Contains($Name)) `
            "$Context is missing '$Name'."
        return $Record[$Name]
    }
    $property = $Record.PSObject.Properties[$Name]
    Assert-Stage5ExporterCondition ($null -ne $property) `
        "$Context is missing '$Name'."
    return $property.Value
}

function Get-Stage5ExporterRecordInteger {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][Int64]$Minimum,
        [Parameter(Mandatory = $true)][Int64]$Maximum,
        [switch]$Optional
    )
    $hasProperty = $false
    $value = $null
    if ($Record -is [Collections.IDictionary]) {
        $hasProperty = $Record.Contains($Name)
        if ($hasProperty) { $value = $Record[$Name] }
    } else {
        $property = $Record.PSObject.Properties[$Name]
        $hasProperty = $null -ne $property
        if ($hasProperty) { $value = $property.Value }
    }
    if (-not $hasProperty) {
        if ($Optional) { return $null }
        throw "Corpus manifest record is missing '$Name'."
    }
    Assert-Stage5ExporterCondition (Test-Stage5ExporterInteger $value) `
        "Corpus manifest record '$Name' must be an integer."
    $withinRange = $false
    try {
        $withinRange = $value -ge $Minimum -and $value -le $Maximum
    }
    catch {
        $withinRange = $false
    }
    Assert-Stage5ExporterCondition $withinRange `
        "Corpus manifest record '$Name' must be between $Minimum and $Maximum."
    return [Int64]$value
}

function Get-Stage5ExporterAiContract {
    param(
        [Parameter(Mandatory = $true)][string]$Scenario,
        [string]$Context = 'AI scenario'
    )
    switch ($Scenario) {
        '4v2' {
            return [pscustomobject]@{ actualAi = 6; actualTeams = '4v2' }
        }
        '4v3' {
            return [pscustomobject]@{ actualAi = 7; actualTeams = '4v3' }
        }
        'hard-ai-2v6' {
            return [pscustomobject]@{ actualAi = 8; actualTeams = '2v6' }
        }
        default {
            throw "$Context '$Scenario' is unsupported."
        }
    }
}

function Get-Stage5ReplayTitleContract {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [string]$Context = 'Replay title'
    )
    Assert-Stage5ExporterCondition ($ExpectedTitle -ceq 'Generals' -or
        $ExpectedTitle -ceq 'ZeroHour') `
        "$Context must be Generals or ZeroHour."
    if ($ExpectedTitle -ceq 'Generals') {
        return [pscustomobject]@{
            title = 'Generals'
            epoch = 1
            marker = '[GeneralsAIPlanningEpoch=1]'
            markerSuffix = ' [GeneralsAIPlanningEpoch=1]'
            pathfindingEpoch = 1
            pathfindingMarker = '[GeneralsPathfindingEpoch=1]'
            pathfindingMarkerSuffix = ' [GeneralsPathfindingEpoch=1]'
            qualificationVersion = 2
            qualification = 'current-path-qualified'
        }
    }
    return [pscustomobject]@{
        title = 'ZeroHour'
        epoch = 3
        marker = '[SkirmishAIEpoch=3]'
        markerSuffix = ' [SkirmishAIEpoch=3]'
        pathfindingEpoch = $null
        pathfindingMarker = $null
        pathfindingMarkerSuffix = $null
        qualificationVersion = 2
        qualification = 'current-ai-qualified'
    }
}

function Get-Stage5ExporterFullPath {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is required."
    try {
        return [IO.Path]::GetFullPath($Path)
    }
    catch {
        throw "$Context path is invalid: $($_.Exception.Message)"
    }
}

function ConvertTo-Stage5ExporterNativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context native path is required."
    if ($Path.StartsWith('\\?\', [StringComparison]::OrdinalIgnoreCase)) {
        return $Path
    }
    if ($Path.StartsWith('\\', [StringComparison]::Ordinal)) {
        return '\\?\UNC\' + $Path.Substring(2)
    }
    Assert-Stage5ExporterCondition ($Path -match '^[A-Za-z]:\\') `
        "$Context native path must be drive-rooted or UNC."
    return '\\?\' + $Path
}

function ConvertFrom-Stage5ExporterHandlePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ($Path.StartsWith('\\?\UNC\', [StringComparison]::OrdinalIgnoreCase)) {
        return '\\' + $Path.Substring(8)
    }
    if ($Path.StartsWith('\\?\', [StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring(4)
    }
    return $Path
}

function Get-Stage5ExporterHandleIdentity {
    param(
        [Parameter(Mandatory = $true)][Microsoft.Win32.SafeHandles.SafeFileHandle]$Handle,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition (-not $Handle.IsInvalid -and -not $Handle.IsClosed) `
        "$Context handle is invalid or closed."
    $pathBuilder = New-Object Text.StringBuilder 32768
    $pathLength = [Stage5ReplayExporterNative.FileIdentityNative]::GetFinalPathNameByHandleW(
        $Handle, $pathBuilder, [UInt32]$pathBuilder.Capacity, 0)
    Assert-Stage5ExporterCondition ($pathLength -gt 0 -and
        $pathLength -lt [UInt32]$pathBuilder.Capacity) `
        "$Context opened-handle canonical path is unavailable."
    $information = New-Object `
        'Stage5ReplayExporterNative.FileIdentityNative+ByHandleFileInformation'
    Assert-Stage5ExporterCondition (
        [Stage5ReplayExporterNative.FileIdentityNative]::GetFileInformationByHandle(
            $Handle, [ref]$information)) `
        "$Context opened-handle file identity is unavailable."
    $fileSize = ([UInt64]$information.FileSizeHigh * [UInt64]4294967296) +
        [UInt64]$information.FileSizeLow
    return [pscustomobject]@{
        canonicalPath = [IO.Path]::GetFullPath(
            (ConvertFrom-Stage5ExporterHandlePath $pathBuilder.ToString()))
        volumeSerialNumber = [UInt32]$information.VolumeSerialNumber
        fileIndexHigh = [UInt32]$information.FileIndexHigh
        fileIndexLow = [UInt32]$information.FileIndexLow
        fileId = ('{0:X8}:{1:X8}{2:X8}' -f $information.VolumeSerialNumber,
            $information.FileIndexHigh, $information.FileIndexLow)
        linkCount = [UInt32]$information.NumberOfLinks
        attributes = [UInt32]$information.FileAttributes
        length = [UInt64]$fileSize
    }
}

function Open-Stage5ExporterAncestorHandles {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $full = Get-Stage5ExporterFullPath $FilePath $Context
    $directory = Split-Path -Parent $full
    $root = [IO.Path]::GetPathRoot($directory)
    $paths = New-Object 'Collections.Generic.List[string]'
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($root)) `
        "$Context path must have a local filesystem root."
    $paths.Add([IO.Path]::GetFullPath($root)) | Out-Null
    $current = [IO.Path]::GetFullPath($root)
    $relativeDirectory = $directory.Substring($root.Length)
    foreach ($segment in @($relativeDirectory.Split(
        [char[]]@('\', '/'), [StringSplitOptions]::RemoveEmptyEntries))) {
        $current = [IO.Path]::GetFullPath((Join-Path $current $segment))
        $paths.Add($current) | Out-Null
    }
    $handles = New-Object 'Collections.Generic.List[object]'
    try {
        for ($index = 0; $index -lt $paths.Count; ++$index) {
            $ancestorPath = [IO.Path]::GetFullPath($paths[$index])
            $nativeAncestorPath = ConvertTo-Stage5ExporterNativePath $ancestorPath `
                "$Context ancestor"
            $handle = [Stage5ReplayExporterNative.FileIdentityNative]::CreateFileW(
                $nativeAncestorPath, 0, 3, [IntPtr]::Zero, 3, 0x02200000,
                [IntPtr]::Zero)
            $openError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Assert-Stage5ExporterCondition (-not $handle.IsInvalid) `
                ("$Context ancestor handle could not be opened: " +
                    "$ancestorPath (Win32 error $openError).")
            $identity = Get-Stage5ExporterHandleIdentity $handle `
                "$Context ancestor '$ancestorPath'"
            Assert-Stage5ExporterCondition (
                [String]::Equals($identity.canonicalPath, $ancestorPath,
                    [StringComparison]::OrdinalIgnoreCase) -and
                ($identity.attributes -band [UInt32][IO.FileAttributes]::Directory) -ne 0 -and
                ($identity.attributes -band [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
                "$Context ancestor is redirected, replaced, or a reparse point: $ancestorPath"
            $handles.Add([pscustomobject]@{
                handle = $handle
                path = $ancestorPath
                identity = $identity
            }) | Out-Null
        }
        return $handles.ToArray()
    }
    catch {
        foreach ($heldAncestor in @($handles.ToArray())) {
            $heldAncestor.handle.Dispose()
        }
        throw
    }
}

function Assert-Stage5ExporterAncestorHandlesUnchanged {
    param(
        [Parameter(Mandatory = $true)][object[]]$HeldAncestors,
        [Parameter(Mandatory = $true)][string]$Context
    )
    foreach ($heldAncestor in @($HeldAncestors)) {
        $after = Get-Stage5ExporterHandleIdentity $heldAncestor.handle `
            "$Context ancestor '$($heldAncestor.path)'"
        Assert-Stage5ExporterCondition ($after.fileId -ceq
            $heldAncestor.identity.fileId -and
            $after.volumeSerialNumber -eq $heldAncestor.identity.volumeSerialNumber -and
            [String]::Equals($after.canonicalPath, $heldAncestor.path,
                [StringComparison]::OrdinalIgnoreCase) -and
            ($after.attributes -band [UInt32][IO.FileAttributes]::Directory) -ne 0 -and
            ($after.attributes -band [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context ancestor changed identity, canonical path, or type while held."
    }
}

function Close-Stage5ExporterHeldFile {
    param([object]$HeldFile)
    if ($null -eq $HeldFile) { return }
    if ($null -ne $HeldFile.stream) { $HeldFile.stream.Dispose() }
    foreach ($heldAncestor in @($HeldFile.ancestorHandles)) {
        $heldAncestor.handle.Dispose()
    }
}

function New-Stage5ExporterHeldCommitFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $full = Get-Stage5ExporterFullPath $Path $Context
    $ancestorHandles = @(Open-Stage5ExporterAncestorHandles $full $Context)
    $nativeHandle = $null
    $stream = $null
    try {
        # GENERIC_READ | GENERIC_WRITE | DELETE, share-delete, CREATE_NEW,
        # FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT.  DELETE access
        # lets failure cleanup target the exact opened object, even if its path
        # is concurrently replaced.
        $nativeFull = ConvertTo-Stage5ExporterNativePath $full $Context
        $nativeHandle = [Stage5ReplayExporterNative.FileIdentityNative]::CreateFileW(
            $nativeFull, [UInt32]3221291008, [UInt32]4, [IntPtr]::Zero,
            [UInt32]1, [UInt32]0x00200080, [IntPtr]::Zero)
        $createError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Assert-Stage5ExporterCondition (-not $nativeHandle.IsInvalid) `
            ("$Context could not create its identity-bound temporary file " +
                "'$full' (Win32 error $createError).")
        $stream = [IO.FileStream]::new($nativeHandle, [IO.FileAccess]::ReadWrite,
            1048576, $false)
        $identity = Get-Stage5ExporterHandleIdentity $stream.SafeFileHandle $Context
        Assert-Stage5ExporterCondition (
            [String]::Equals($identity.canonicalPath, $full,
                [StringComparison]::OrdinalIgnoreCase) -and
            $identity.linkCount -eq 1 -and $identity.length -eq 0 -and
            ($identity.attributes -band
                [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context is redirected, hard-linked, or replaced."
        return [pscustomobject]@{
            path = $full
            stream = $stream
            ancestorHandles = $ancestorHandles
            identity = $identity
            length = [Int64]0
        }
    }
    catch {
        if ($null -ne $stream) {
            try {
                [void][Stage5ReplayExporterNative.FileIdentityNative]::MarkFileForDeletion(
                    $stream.SafeFileHandle)
            }
            finally { $stream.Dispose() }
        }
        elseif ($null -ne $nativeHandle) {
            try {
                if (-not $nativeHandle.IsInvalid -and -not $nativeHandle.IsClosed) {
                    [void][Stage5ReplayExporterNative.FileIdentityNative]::MarkFileForDeletion(
                        $nativeHandle)
                }
            }
            finally { $nativeHandle.Dispose() }
        }
        foreach ($heldAncestor in @($ancestorHandles)) {
            $heldAncestor.handle.Dispose()
        }
        throw
    }
}

function Remove-Stage5ExporterOwnedHeldFile {
    param(
        [Parameter(Mandatory = $true)][object]$HeldFile,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $after = Get-Stage5ExporterHandleIdentity $HeldFile.stream.SafeFileHandle $Context
    Assert-Stage5ExporterCondition ($after.fileId -ceq $HeldFile.identity.fileId -and
        $after.volumeSerialNumber -eq $HeldFile.identity.volumeSerialNumber -and
        $after.linkCount -eq 1 -and
        ($after.attributes -band [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context cleanup refused an object whose identity or link count changed."
    Assert-Stage5ExporterCondition (
        [Stage5ReplayExporterNative.FileIdentityNative]::MarkFileForDeletion(
            $HeldFile.stream.SafeFileHandle)) `
        "$Context cleanup could not mark the exact opened object for deletion."
}

function Invoke-Stage5ExporterCommitObserver {
    param(
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$TemporaryPath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )
    if ($null -ne $script:Stage5ExporterCommitObserver) {
        & $script:Stage5ExporterCommitObserver $Stage $Kind $TemporaryPath `
            $DestinationPath
    }
}

function Open-Stage5ExporterHeldFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][Int64]$MaximumLength,
        [Int64]$MinimumLength = 0,
        [IO.FileShare]$Share = [IO.FileShare]::Read
    )
    $full = Get-Stage5ExporterFullPath $Path $Context
    $ancestorHandles = @(Open-Stage5ExporterAncestorHandles $full $Context)
    $stream = $null
    try {
        $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, $Share)
        $identity = Get-Stage5ExporterHandleIdentity $stream.SafeFileHandle $Context
        $length = [Int64]$stream.Length
        Assert-Stage5ExporterCondition (
            [String]::Equals($identity.canonicalPath, $full,
                [StringComparison]::OrdinalIgnoreCase)) `
            "$Context opened handle resolves to a different canonical path."
        Assert-Stage5ExporterCondition (
            ($identity.attributes -band [UInt32][IO.FileAttributes]::Directory) -eq 0 -and
            ($identity.attributes -band [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context opened file is a directory or reparse point."
        Assert-Stage5ExporterCondition ($identity.linkCount -eq 1) `
            "$Context opened file has $($identity.linkCount) hard links; exactly one is required."
        Assert-Stage5ExporterCondition ([UInt64]$length -eq $identity.length) `
            "$Context opened-handle extent differs from its file identity."
        Assert-Stage5ExporterCondition ($length -ge $MinimumLength -and
            $length -le $MaximumLength) `
            "$Context length $length is outside the allowed $MinimumLength..$MaximumLength byte range."
        return [pscustomobject]@{
            path = $full
            stream = $stream
            ancestorHandles = $ancestorHandles
            identity = $identity
            length = $length
        }
    }
    catch {
        if ($null -ne $stream) { $stream.Dispose() }
        foreach ($heldAncestor in @($ancestorHandles)) {
            $heldAncestor.handle.Dispose()
        }
        throw
    }
}

function Assert-Stage5ExporterHeldFileUnchanged {
    param(
        [Parameter(Mandatory = $true)][object]$HeldFile,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $after = Get-Stage5ExporterHandleIdentity $HeldFile.stream.SafeFileHandle $Context
    Assert-Stage5ExporterCondition ($HeldFile.stream.Length -eq $HeldFile.length -and
        $after.length -eq [UInt64]$HeldFile.length -and
        $after.fileId -ceq $HeldFile.identity.fileId -and
        $after.volumeSerialNumber -eq $HeldFile.identity.volumeSerialNumber -and
        $after.linkCount -eq 1 -and
        [String]::Equals($after.canonicalPath, $HeldFile.path,
            [StringComparison]::OrdinalIgnoreCase) -and
        ($after.attributes -band [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context changed identity, extent, link count, or canonical path while held."
    Assert-Stage5ExporterAncestorHandlesUnchanged $HeldFile.ancestorHandles $Context
}

function Get-Stage5ExporterFixedStreamSha256 {
    param(
        [Parameter(Mandatory = $true)][IO.Stream]$Stream,
        [Parameter(Mandatory = $true)][Int64]$Length,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition $Stream.CanSeek `
        "$Context hashing requires a seekable held stream."
    $originalPosition = $Stream.Position
    try {
        $Stream.Position = 0
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $buffer = New-Object byte[] 1048576
            [Int64]$remaining = $Length
            while ($remaining -gt 0) {
                $requested = [int][Math]::Min([Int64]$buffer.Length, $remaining)
                $read = $Stream.Read($buffer, 0, $requested)
                Assert-Stage5ExporterCondition ($read -gt 0) `
                    "$Context ended before its admitted extent."
                [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
                $remaining -= $read
            }
            $extra = $Stream.ReadByte()
            Assert-Stage5ExporterCondition ($extra -eq -1 -and
                $Stream.Length -eq $Length) `
                "$Context grew or changed after its admitted extent."
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            return (($sha.Hash | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $Stream.Position = $originalPosition }
}

function Read-Stage5ExporterFixedBytes {
    param(
        [Parameter(Mandatory = $true)][IO.Stream]$Stream,
        [Parameter(Mandatory = $true)][Int64]$Length,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition ($Length -le [Int32]::MaxValue) `
        "$Context is too large for an in-memory bounded snapshot."
    $bytes = New-Object byte[] ([int]$Length)
    $Stream.Position = 0
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $read = $Stream.Read($bytes, $offset, $bytes.Length - $offset)
        Assert-Stage5ExporterCondition ($read -gt 0) `
            "$Context ended before its admitted extent."
        $offset += $read
    }
    Assert-Stage5ExporterCondition ($Stream.ReadByte() -eq -1 -and
        $Stream.Length -eq $Length) `
        "$Context grew or changed after its admitted extent."
    return ,$bytes
}

function Assert-Stage5ExporterRegularDirectory {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $full -PathType Container) `
        "$Context directory was not found: $full"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context directory is a reparse point: $full"
    return $full.TrimEnd('\')
}

function Assert-Stage5ExporterExplicitTaskRoot {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    $root = [IO.Path]::GetPathRoot($full)
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($root)) `
        "$Context has no resolvable filesystem root: $full"
    Assert-Stage5ExporterCondition (-not [String]::Equals($full, $root,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must name a directory below a filesystem root: $full"
    return Assert-Stage5ExporterRegularDirectory $full $Context
}

function Assert-Stage5ExporterContainedPathNoReparse {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath,
        [Parameter(Mandatory = $true)][string]$Context,
        [switch]$AllowBase
    )
    $base = Get-Stage5ExporterFullPath $BaseDirectory "$Context base"
    $candidate = Get-Stage5ExporterFullPath $CandidatePath $Context
    $base = $base.TrimEnd('\')
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Stage5ExporterCondition ($baseRoot -is [string] -and
        $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."

    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $minimumParts = if ($AllowBase) { $baseParts.Count } else { $baseParts.Count + 1 }
    Assert-Stage5ExporterCondition ($candidateParts.Count -ge $minimumParts) `
        "$Context path escapes or is not below its containing directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Stage5ExporterCondition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its containing directory."
    }

    $rootCurrent = $baseRoot
    foreach ($segment in @($candidate.Substring($candidateRoot.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $rootCurrent = Join-Path $rootCurrent $segment
        if (-not (Test-Path -LiteralPath $rootCurrent)) { break }
        $rootItem = Get-Item -LiteralPath $rootCurrent -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context ancestor path component '$segment' is a reparse point."
    }
    $baseItem = Get-Item -LiteralPath $base -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context containing directory is a reparse point."
    $current = $base
    for ($index = $baseParts.Count; $index -lt $candidateParts.Count; ++$index) {
        $current = Join-Path $current $candidateParts[$index]
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$($candidateParts[$index])' is a reparse point."
    }
    if (-not $AllowBase) {
        Assert-Stage5ExporterCondition (-not [String]::Equals($candidate, $base,
            [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path must not be the containing directory itself."
    }
    return $candidate
}

function Assert-Stage5ExporterTextContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath,
        [Parameter(Mandatory = $true)][string]$Context
    )
    # Source provenance may outlive the ephemeral source tree.  Keep validating
    # its textual containment without treating the missing path as an export
    # success; the durable destination and its hash remain authoritative.
    $base = (Get-Stage5ExporterFullPath $BaseDirectory "$Context base").TrimEnd('\')
    $candidate = Get-Stage5ExporterFullPath $CandidatePath $Context
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Stage5ExporterCondition ($baseRoot -is [string] -and
        $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."

    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    Assert-Stage5ExporterCondition ($candidateParts.Count -ge ($baseParts.Count + 1)) `
        "$Context path escapes or is not below its containing directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Stage5ExporterCondition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its containing directory."
    }
    return $candidate
}

function Ensure-Stage5ExporterDirectory {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    if (-not (Test-Path -LiteralPath $full)) {
        New-Item -ItemType Directory -Path $full -Force | Out-Null
    }
    return Assert-Stage5ExporterRegularDirectory $full $Context
}

function Assert-Stage5ExporterPathAbsent {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    $item = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
    Assert-Stage5ExporterCondition ($null -eq $item) `
        "$Context already exists; refusing overwrite: $full"
}

function Get-Stage5ExporterSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $held = Open-Stage5ExporterHeldFile $Path 'Exporter JSON/hash input' `
        $script:Stage5ExporterMaximumJsonBytes
    try {
        $digest = Get-Stage5ExporterFixedStreamSha256 $held.stream $held.length `
            'Exporter JSON/hash input'
        Assert-Stage5ExporterHeldFileUnchanged $held 'Exporter JSON/hash input'
        return $digest
    }
    finally { Close-Stage5ExporterHeldFile $held }
}

function Get-Stage5ExporterStreamSha256 {
    param([Parameter(Mandatory = $true)][IO.Stream]$Stream)
    Assert-Stage5ExporterCondition $Stream.CanSeek `
        'Exporter hashing requires a seekable held file stream.'
    return Get-Stage5ExporterFixedStreamSha256 $Stream ([Int64]$Stream.Length) `
        'Exporter held-stream input'
}

function Get-Stage5ExporterUtf16String {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $builder = New-Object Text.StringBuilder
    while ($Reader.BaseStream.Position + 2 -le $Reader.BaseStream.Length) {
        $character = $Reader.ReadUInt16()
        if ($character -eq 0) {
            return $builder.ToString()
        }
        Assert-Stage5ExporterCondition ($builder.Length -lt 65536) `
            "$Context wide string is unreasonably long."
        [void]$builder.Append([char]$character)
    }
    throw "$Context wide string is unterminated."
}

function Get-Stage5ReplayQualification {
    param(
        [Parameter(Mandatory = $true)][string]$VersionTime,
        [Parameter(Mandatory = $true)][object]$ReplayContract,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $markerMatches = @([regex]::Matches($VersionTime,
        '\[(?:GeneralsPathfinding|GeneralsAI|SkirmishAI)[^\]]*(?:\]|$)'))
    $markerValues = @($markerMatches | ForEach-Object { $_.Value })

    if ($ReplayContract.title -ceq 'Generals') {
        $aiMarkers = @($markerValues | Where-Object {
            $_ -match '^\[GeneralsAI'
        })
        $pathMarkers = @($markerValues | Where-Object {
            $_ -match '^\[GeneralsPathfinding'
        })
        $zeroHourMarkers = @($markerValues | Where-Object {
            $_ -match '^\[SkirmishAI'
        })
        Assert-Stage5ExporterCondition ($aiMarkers.Count -eq 1 -and
            $aiMarkers[0] -ceq $ReplayContract.marker) `
            "$Context does not carry exactly one current Generals AI marker."
        Assert-Stage5ExporterCondition ($zeroHourMarkers.Count -eq 0) `
            "$Context mixes Generals and Zero Hour replay markers."

        if ($pathMarkers.Count -eq 0) {
            Assert-Stage5ExporterCondition ($markerValues.Count -eq 1 -and
                $VersionTime.EndsWith($ReplayContract.markerSuffix,
                    [StringComparison]::Ordinal)) `
                "$Context has an invalid legacy Generals AI-only marker suffix."
            return [pscustomobject]@{
                pathfindingReplayEpoch = $null
                pathfindingReplayMarker = $null
                replayQualificationVersion = 1
                replayQualification = 'legacy-ai-only'
            }
        }

        $pairSuffix = $ReplayContract.pathfindingMarkerSuffix +
            $ReplayContract.markerSuffix
        Assert-Stage5ExporterCondition ($pathMarkers.Count -eq 1 -and
            $pathMarkers[0] -ceq $ReplayContract.pathfindingMarker -and
            $markerValues.Count -eq 2 -and
            $markerValues[0] -ceq $ReplayContract.pathfindingMarker -and
            $markerValues[1] -ceq $ReplayContract.marker -and
            $VersionTime.EndsWith($pairSuffix, [StringComparison]::Ordinal)) `
            "$Context does not carry the exact current Generals pathfinding/AI marker pair."
        return [pscustomobject]@{
            pathfindingReplayEpoch = $ReplayContract.pathfindingEpoch
            pathfindingReplayMarker = $ReplayContract.pathfindingMarker
            replayQualificationVersion = $ReplayContract.qualificationVersion
            replayQualification = $ReplayContract.qualification
        }
    }

    Assert-Stage5ExporterCondition ($markerValues.Count -eq 1 -and
        $markerValues[0] -ceq $ReplayContract.marker -and
        $VersionTime.EndsWith($ReplayContract.markerSuffix,
            [StringComparison]::Ordinal)) `
        "$Context does not carry exactly one current $($ReplayContract.title) epoch-$($ReplayContract.epoch) marker."
    return [pscustomobject]@{
        pathfindingReplayEpoch = $null
        pathfindingReplayMarker = $null
        replayQualificationVersion = $ReplayContract.qualificationVersion
        replayQualification = $ReplayContract.qualification
    }
}

function Assert-Stage5ReplayFreshQualification {
    param(
        [Parameter(Mandatory = $true)][object]$Header,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $replayContract = Get-Stage5ReplayTitleContract $ExpectedTitle $Context
    if ($ExpectedTitle -ceq 'Generals') {
        Assert-Stage5ExporterCondition (
            [int]$Header.replayQualificationVersion -eq
                [int]$replayContract.qualificationVersion -and
            [string]$Header.replayQualification -ceq
                [string]$replayContract.qualification -and
            [int]$Header.pathfindingReplayEpoch -eq
                [int]$replayContract.pathfindingEpoch) `
            "$Context requires the current Generals pathfinding/AI marker pair."
    }
    return $Header
}

function Read-Stage5ReplayContainer {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [IO.Stream]$Stream
    )
    $replayContract = Get-Stage5ReplayTitleContract $ExpectedTitle
    $full = Get-Stage5ExporterFullPath $Path 'Replay file'
    $ownsStream = $null -eq $Stream
    $held = $null
    if ($ownsStream) {
        $held = Open-Stage5ExporterHeldFile $full 'Replay file' `
            $script:Stage5ExporterMaximumReplayBytes 46
        $Stream = $held.stream
        [void](Get-Stage5ExporterFixedStreamSha256 $Stream $held.length `
            'Replay file')
    }
    try {
        $Stream.Position = 0
        Assert-Stage5ExporterCondition ($Stream.Length -ge 46) `
            "Replay file is shorter than the RPL3/GENREP prefix: $full"
        Assert-Stage5ExporterCondition ($Stream.Length -le
            $script:Stage5ExporterMaximumReplayBytes) `
            "Replay file exceeds the 256 MiB exporter limit: $full"
        $reader = [IO.BinaryReader]::new($Stream, [Text.Encoding]::UTF8, $true)
        try {
            $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            Assert-Stage5ExporterCondition ($magic -ceq 'RPL3') `
                "Replay file does not begin with RPL3: $full"
            $schemaVersion = $reader.ReadUInt32()
            $engineEpoch = $reader.ReadUInt32()
            [void]$reader.ReadUInt64()
            [void]$reader.ReadUInt64()
            $payloadByteCount = $reader.ReadUInt64()
            [void]$reader.ReadUInt32()
            Assert-Stage5ExporterCondition ($schemaVersion -eq 2) `
                "Replay file has unsupported RPL3 schema ${schemaVersion}: $full"
            Assert-Stage5ExporterCondition ($engineEpoch -eq 1) `
                "Replay file has unsupported runtime engine epoch ${engineEpoch}: $full"
            Assert-Stage5ExporterCondition ($payloadByteCount -ne [UInt64]::MaxValue -and
                $payloadByteCount -eq [UInt64]($Stream.Length - 40)) `
                "Replay file has an incomplete or inconsistent RPL3 payload length: $full"

            $Stream.Position = 40
            $genrep = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(6))
            Assert-Stage5ExporterCondition ($genrep -ceq 'GENREP') `
                "Replay file does not contain GENREP at the native payload start: $full"

            # Skip the native fixed fields written before replay/version strings:
            # start time, end time, frame count, two flags, and MAX_SLOTS flags.
            [void]$reader.ReadUInt32()
            [void]$reader.ReadUInt32()
            [void]$reader.ReadUInt32()
            [void]$reader.ReadByte()
            [void]$reader.ReadByte()
            for ($index = 0; $index -lt 8; ++$index) {
                [void]$reader.ReadByte()
            }
            [void](Get-Stage5ExporterUtf16String $reader 'Replay name')
            for ($index = 0; $index -lt 8; ++$index) {
                [void]$reader.ReadUInt16()
            }
            [void](Get-Stage5ExporterUtf16String $reader 'Replay version')
            $versionTime = Get-Stage5ExporterUtf16String $reader 'Replay version-time'
            $qualification = Get-Stage5ReplayQualification $versionTime `
                $replayContract "Replay file '$full'"
            $result = [pscustomobject]@{
                magic = $magic
                schemaVersion = [int]$schemaVersion
                engineEpoch = [int]$engineEpoch
                payloadByteCount = [UInt64]$payloadByteCount
                genrep = $genrep
                skirmishAiReplayEpoch = $replayContract.epoch
                versionTime = $versionTime
                pathfindingReplayEpoch = $qualification.pathfindingReplayEpoch
                pathfindingReplayMarker = $qualification.pathfindingReplayMarker
                replayQualificationVersion = $qualification.replayQualificationVersion
                replayQualification = $qualification.replayQualification
            }
            if ($ownsStream) {
                Assert-Stage5ExporterHeldFileUnchanged $held 'Replay file'
            }
            return $result
        }
        finally { $reader.Dispose() }
    }
    finally {
        if ($ownsStream) { Close-Stage5ExporterHeldFile $held }
    }
}

function Get-Stage5ExporterReplaySnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $held = Open-Stage5ExporterHeldFile $Path $Context `
        $script:Stage5ExporterMaximumReplayBytes 46
    try {
        return Get-Stage5ExporterHeldReplaySnapshot $held $ExpectedTitle $Context
    }
    finally { Close-Stage5ExporterHeldFile $held }
}

function Get-Stage5ExporterHeldReplaySnapshot {
    param(
        [Parameter(Mandatory = $true)][object]$HeldFile,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $sha256 = Get-Stage5ExporterFixedStreamSha256 $HeldFile.stream `
        $HeldFile.length $Context
    $header = Read-Stage5ReplayContainer -Path $HeldFile.path `
        -ExpectedTitle $ExpectedTitle -Stream $HeldFile.stream
    Assert-Stage5ExporterHeldFileUnchanged $HeldFile $Context
    $identity = Get-Stage5ExporterHandleIdentity $HeldFile.stream.SafeFileHandle $Context
    return [pscustomobject]@{
        path = $HeldFile.path
        canonicalPath = $identity.canonicalPath
        fileId = $identity.fileId
        linkCount = $identity.linkCount
        length = $HeldFile.length
        sha256 = $sha256
        header = $header
    }
}

function Get-Stage5ReplayCompletionFields {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][int]$ExpectedSeed,
        [Parameter(Mandatory = $true)][string]$ExpectedScenario,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [string]$Context = 'Fresh replay completion'
    )
    $replayContract = Get-Stage5ReplayTitleContract $ExpectedTitle
    $lines = @($Output -split "`r?`n" | Where-Object {
        $_.StartsWith('SKIRMISH_AI_TEST_COMPLETE ', [StringComparison]::Ordinal)
    })
    Assert-Stage5ExporterCondition ($lines.Count -eq 1) `
        "$Context requires exactly one SKIRMISH_AI_TEST_COMPLETE line."
    $line = $lines[0]
    $fields = @{}
    $matches = [regex]::Matches($line.Substring('SKIRMISH_AI_TEST_COMPLETE '.Length),
        '(?<name>[A-Za-z_][A-Za-z0-9_]*)=(?:"(?<quoted>[^"]*)"|(?<plain>[^\s]+))')
    foreach ($match in $matches) {
        $name = $match.Groups['name'].Value
        Assert-Stage5ExporterCondition (-not $fields.ContainsKey($name)) `
            "$Context repeats field '$name'."
        $fields[$name] = if ($match.Groups['quoted'].Success) {
            $match.Groups['quoted'].Value
        }
        else {
            $match.Groups['plain'].Value
        }
    }
    foreach ($required in @('seed', 'scenario', 'run_nonce', 'replay_epoch',
        'replay_sha256', 'replay_retained')) {
        Assert-Stage5ExporterCondition ($fields.ContainsKey($required)) `
            "$Context is missing field '$required'."
    }
    [int]$seed = 0
    Assert-Stage5ExporterCondition ([int]::TryParse([string]$fields['seed'], [ref]$seed) -and
        $seed -eq $ExpectedSeed) "$Context seed does not match the plan."
    Assert-Stage5ExporterCondition ([string]$fields['scenario'] -ceq $ExpectedScenario) `
        "$Context scenario does not match the plan."
    [int]$replayEpoch = 0
    Assert-Stage5ExporterCondition ([int]::TryParse([string]$fields['replay_epoch'], [ref]$replayEpoch) -and
        $replayEpoch -eq $replayContract.epoch) `
        "$Context replay epoch is not $($replayContract.epoch) for title '$($replayContract.title)'."
    $replaySha256 = [string]$fields['replay_sha256']
    Assert-Stage5ExporterCondition ($replaySha256 -match '^[0-9A-Fa-f]{64}$') `
        "$Context replay SHA-256 is invalid."
    $runNonce = [string]$fields['run_nonce']
    Assert-Stage5ExporterCondition ($runNonce -match '^[0-9A-Fa-f-]{1,64}$') `
        "$Context run nonce is invalid."
    $replayRetained = [string]$fields['replay_retained']
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($replayRetained) -and
        $replayRetained -cne 'unavailable') "$Context has no retained replay path."
    return [pscustomobject]@{
        seed = $seed
        scenario = [string]$fields['scenario']
        runNonce = $runNonce
        replayEpoch = $replayEpoch
        replaySha256 = $replaySha256.ToUpperInvariant()
        replayRetained = $replayRetained
    }
}

function Get-Stage5ExporterMetadataValue {
    param([Collections.IDictionary]$Metadata, [string]$Name)
    Assert-Stage5ExporterCondition ($Metadata.Contains($Name)) `
        "Fresh replay metadata is missing '$Name'."
    return [string]$Metadata[$Name]
}

function Assert-Stage5ExporterMetadata {
    param([Collections.IDictionary]$Metadata)
    Assert-Stage5ExporterCondition ($null -ne $Metadata) `
        'Fresh replay metadata is required.'
    foreach ($name in @('title', 'category', 'scenario', 'seed', 'runNonce',
        'executableSha256', 'origin')) {
        [void](Get-Stage5ExporterMetadataValue $Metadata $name)
    }
    $title = Get-Stage5ExporterMetadataValue $Metadata 'title'
    Assert-Stage5ExporterCondition ($title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Fresh replay metadata title contains unsupported path characters.'
    Get-Stage5ReplayTitleContract $title 'Fresh replay metadata title' | Out-Null
    $category = Get-Stage5ExporterMetadataValue $Metadata 'category'
    Assert-Stage5ExporterCondition ($category -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') `
        'Fresh replay metadata category contains unsupported path characters.'
    $scenario = Get-Stage5ExporterMetadataValue $Metadata 'scenario'
    Assert-Stage5ExporterCondition ($scenario -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') `
        'Fresh replay metadata scenario contains unsupported path characters.'
    $seed = Get-Stage5ExporterMetadataInteger $Metadata 'seed' 1 `
        ([Int32]::MaxValue)
    $runNonce = Get-Stage5ExporterMetadataValue $Metadata 'runNonce'
    Assert-Stage5ExporterCondition ($runNonce -match '^[0-9A-Fa-f-]{1,64}$') `
        'Fresh replay metadata runNonce is invalid.'
    $executableSha256 = Get-Stage5ExporterMetadataValue $Metadata 'executableSha256'
    Assert-Stage5ExporterCondition ($executableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Fresh replay metadata executableSha256 is invalid.'
    $origin = Get-Stage5ExporterMetadataValue $Metadata 'origin'
    Assert-Stage5ExporterCondition ($origin -ceq 'native-fresh-runtime') `
        "Fresh replay metadata origin must be 'native-fresh-runtime'."
    if ($category -ceq 'local-capacity-ai') {
        $contract = Get-Stage5ExporterAiContract $scenario 'Fresh replay metadata scenario'
        $actualAi = Get-Stage5ExporterMetadataInteger $Metadata 'actualAi' 0 $contract.actualAi
        Assert-Stage5ExporterCondition ($actualAi -eq $contract.actualAi) `
            'Fresh replay metadata actualAi does not match its scenario.'
        $actualTeams = Get-Stage5ExporterMetadataValue $Metadata 'actualTeams'
        Assert-Stage5ExporterCondition ($actualTeams -ceq $contract.actualTeams) `
            'Fresh replay metadata actualTeams does not match its scenario.'
    }
}

function ConvertTo-Stage5ExporterPathComponent {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value.Replace(' ', '_')
}

function Copy-Stage5ReplayWithStableSource {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$TemporaryPath
    )
    $sourceHeld = Open-Stage5ExporterHeldFile $SourcePath 'Replay source' `
        $script:Stage5ExporterMaximumReplayBytes 46 ([IO.FileShare]::None)
    $temporaryHeld = $null
    $ownershipTransferred = $false
    try {
        $source = $sourceHeld.stream
        $sourceLength = $sourceHeld.length
        $temporaryHeld = New-Stage5ExporterHeldCommitFile $TemporaryPath `
            'Replay temporary destination'
        $temporary = $temporaryHeld.stream
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $buffer = New-Object byte[] 1048576
            [Int64]$remaining = $sourceLength
            while ($remaining -gt 0) {
                $requested = [int][Math]::Min([Int64]$buffer.Length, $remaining)
                $read = $source.Read($buffer, 0, $requested)
                Assert-Stage5ExporterCondition ($read -gt 0) `
                    'Replay source ended before its admitted extent.'
                $temporary.Write($buffer, 0, $read)
                [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
                $remaining -= $read
            }
            Assert-Stage5ExporterCondition ($source.ReadByte() -eq -1 -and
                $source.Length -eq $sourceLength) `
                'Replay source grew or changed after its admitted extent.'
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            $sourceSha256 = (($sha.Hash | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
        $temporary.Flush($true)
        $temporaryHeld.length = $sourceLength
        Assert-Stage5ExporterHeldFileUnchanged $temporaryHeld `
            'Replay temporary destination'
        Assert-Stage5ExporterHeldFileUnchanged $sourceHeld 'Replay source'
        $ownershipTransferred = $true
        return [pscustomobject]@{
            sha256 = $sourceSha256
            length = $sourceLength
            sourceFileId = $sourceHeld.identity.fileId
            temporaryHeld = $temporaryHeld
        }
    }
    finally {
        if ($null -ne $temporaryHeld -and -not $ownershipTransferred) {
            try {
                Remove-Stage5ExporterOwnedHeldFile $temporaryHeld `
                    'Replay temporary destination'
            }
            finally { Close-Stage5ExporterHeldFile $temporaryHeld }
        }
        Close-Stage5ExporterHeldFile $sourceHeld
    }
}

function Export-Stage5ReviewedAiReplayMap {
    param([object]$ReviewedMap,[string]$CorpusRoot)
    $map=$ReviewedMap.binding
    $destination=Join-Path $CorpusRoot $map.mapKey
    if (-not(Test-Path -LiteralPath $destination)) {
        Copy-Stage5ReviewedAiMapSnapshot -ReviewedMap $ReviewedMap -DestinationRoot $CorpusRoot | Out-Null
    }
    # Reopen retained bytes independently; never reopen the mutable source map.
    Read-Stage5ReviewedAiMap -ManifestDirectory $CorpusRoot -Map ([ordered]@{
        source=$map.mapKey;mapKey=$map.mapKey;sha256=$map.sha256;byteCount=$map.byteCount;crc=$map.crc
    }) | Out-Null
    return [pscustomobject][ordered]@{
        source=$map.mapKey;profileRelativePath=$map.mapKey;sha256=$map.sha256;byteCount=$map.byteCount;crc=$map.crc
    }
}

function Export-Stage5FreshReplayArtifact {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$TaskRunRoot,
        [Parameter(Mandatory = $true)][string]$ProfileRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Metadata,
        [object]$ReviewedMap = $null
    )
    Assert-Stage5ExporterMetadata $Metadata
    $title = Get-Stage5ExporterMetadataValue $Metadata 'title'
    Assert-Stage5ExporterCondition ($ExpectedSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Expected replay SHA-256 is invalid.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $taskRunRootFull = Assert-Stage5ExporterRegularDirectory $TaskRunRoot 'Task run root'
    $profileRootFull = Assert-Stage5ExporterRegularDirectory $ProfileRoot 'Profile root'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $taskRunRootFull `
        'Task run root' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $taskRunRootFull $profileRootFull `
        'Profile root' | Out-Null
    $sourceFull = Assert-Stage5ExporterContainedPathNoReparse $profileRootFull $SourcePath `
        'Retained replay source'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $sourceFull -PathType Leaf) `
        "Retained replay source was not found: $sourceFull"
    $sourceItem = Get-Item -LiteralPath $sourceFull -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Retained replay source is a reparse point: $sourceFull"

    $corpusRootFull = Get-Stage5ExporterFullPath $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    Assert-Stage5ExporterCondition (-not [String]::Equals($corpusRootFull, $taskRootFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'CorpusExportRoot must be below TaskRoot.'
    Ensure-Stage5ExporterDirectory $corpusRootFull 'CorpusExportRoot' | Out-Null
    $exportedMap = $null
    if ($null -ne $ReviewedMap) {
        Assert-Stage5ExporterCondition ((Get-Stage5ExporterMetadataValue $Metadata 'scenario') -ceq '4v2') `
            'Reviewed AI replay map binding is restricted to4v2.'
        $exportedMap = Export-Stage5ReviewedAiReplayMap $ReviewedMap $corpusRootFull
    }
    $titleComponent = ConvertTo-Stage5ExporterPathComponent `
        (Get-Stage5ExporterMetadataValue $Metadata 'title')
    $categoryComponent = Get-Stage5ExporterMetadataValue $Metadata 'category'
    $scenarioComponent = Get-Stage5ExporterMetadataValue $Metadata 'scenario'
    $seed = Get-Stage5ExporterMetadataInteger $Metadata 'seed' 1 `
        ([Int32]::MaxValue)
    $runNonce = Get-Stage5ExporterMetadataValue $Metadata 'runNonce'
    $actualAi = $null
    $actualTeams = $null
    if ($categoryComponent -ceq 'local-capacity-ai') {
        $contract = Get-Stage5ExporterAiContract $scenarioComponent `
            'Fresh replay metadata scenario'
        $actualAi = Get-Stage5ExporterMetadataInteger $Metadata 'actualAi' 0 `
            $contract.actualAi
        $actualTeams = Get-Stage5ExporterMetadataValue $Metadata 'actualTeams'
    }
    $destinationDirectory = Join-Path (Join-Path $corpusRootFull $titleComponent) $categoryComponent
    Ensure-Stage5ExporterDirectory $destinationDirectory 'Replay export directory' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $destinationDirectory `
        'Replay export directory' | Out-Null
    $destinationName = '{0}-scenario-{1}-seed-{2}-{3}.rep' -f `
        $categoryComponent, $scenarioComponent, $seed, $runNonce
    $destinationFull = [IO.Path]::GetFullPath((Join-Path $destinationDirectory $destinationName))
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $destinationFull `
        'Replay export destination' | Out-Null
    Assert-Stage5ExporterPathAbsent $destinationFull 'Replay export destination'

    $temporaryFull = '{0}.tmp-{1}' -f $destinationFull, [Guid]::NewGuid().ToString('N')
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $temporaryFull `
        'Replay export temporary file' | Out-Null
    $temporaryHeld = $null
    $commitAccepted = $false
    try {
        $copy = Copy-Stage5ReplayWithStableSource $sourceFull $temporaryFull
        $temporaryHeld = $copy.temporaryHeld
        Assert-Stage5ExporterCondition ($copy.sha256 -ceq $ExpectedSha256.ToUpperInvariant()) `
            "Retained replay source SHA-256 mismatch. Expected $ExpectedSha256, got $($copy.sha256)."
        $temporarySnapshot = Get-Stage5ExporterHeldReplaySnapshot $temporaryHeld $title `
            'Fresh replay temporary copy'
        $header = $temporarySnapshot.header
        Assert-Stage5ReplayFreshQualification $header $title `
            'Fresh replay export' | Out-Null
        Assert-Stage5ExporterCondition ($temporarySnapshot.sha256 -ceq $copy.sha256 -and
            $temporarySnapshot.length -eq $copy.length) `
            'Temporary replay copy SHA-256 differs from the stable source snapshot.'
        Invoke-Stage5ExporterCommitObserver 'before-commit' 'replay' `
            $temporaryFull $destinationFull
        Assert-Stage5ExporterHeldFileUnchanged $temporaryHeld `
            'Fresh replay temporary copy before commit'
        [IO.File]::Move($temporaryFull, $destinationFull)
        Invoke-Stage5ExporterCommitObserver 'after-commit' 'replay' `
            $temporaryFull $destinationFull
        $committedIdentity = Get-Stage5ExporterHandleIdentity `
            $temporaryHeld.stream.SafeFileHandle 'Fresh replay export destination'
        Assert-Stage5ExporterCondition ($committedIdentity.fileId -ceq
            $temporaryHeld.identity.fileId -and
            $committedIdentity.volumeSerialNumber -eq
                $temporaryHeld.identity.volumeSerialNumber -and
            $committedIdentity.linkCount -eq 1 -and
            $committedIdentity.length -eq [UInt64]$copy.length -and
            [String]::Equals($committedIdentity.canonicalPath, $destinationFull,
                [StringComparison]::OrdinalIgnoreCase) -and
            ($committedIdentity.attributes -band
                [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
            'Replay export commit changed file identity, canonical destination, extent, or link count.'
        $temporaryHeld.path = $destinationFull
        $temporaryHeld.identity = $committedIdentity
        $destinationSnapshot = Get-Stage5ExporterHeldReplaySnapshot `
            $temporaryHeld $title `
            'Fresh replay export destination'
        $destinationSha256 = $destinationSnapshot.sha256
        Assert-Stage5ExporterCondition ($destinationSha256 -ceq $copy.sha256 -and
            $destinationSnapshot.length -eq $copy.length) `
            'Replay export destination SHA-256 differs from the source snapshot.'
        $destinationHeader = $destinationSnapshot.header
        Assert-Stage5ReplayFreshQualification $destinationHeader $title `
            'Fresh replay export destination' | Out-Null
        Assert-Stage5ExporterCondition ($destinationHeader.magic -ceq $header.magic -and
            $destinationHeader.schemaVersion -eq $header.schemaVersion -and
            $destinationHeader.engineEpoch -eq $header.engineEpoch -and
            $destinationHeader.skirmishAiReplayEpoch -eq $header.skirmishAiReplayEpoch -and
            $destinationHeader.pathfindingReplayEpoch -eq $header.pathfindingReplayEpoch -and
            $destinationHeader.replayQualificationVersion -eq
                $header.replayQualificationVersion -and
            $destinationHeader.replayQualification -ceq
                $header.replayQualification) `
            'Replay export destination header differs from the validated source snapshot.'
        $result = [pscustomobject]@{
            origin = Get-Stage5ExporterMetadataValue $Metadata 'origin'
            title = Get-Stage5ExporterMetadataValue $Metadata 'title'
            category = $categoryComponent
            scenario = $scenarioComponent
            seed = $seed
            actualAi = $actualAi
            actualTeams = $actualTeams
            runNonce = $runNonce
            executableSha256 = (Get-Stage5ExporterMetadataValue $Metadata 'executableSha256').ToUpperInvariant()
            sourceProfileRoot = $profileRootFull
            sourcePath = $sourceFull
            destinationPath = $destinationFull
            sourceSha256 = $copy.sha256
            destinationSha256 = $destinationSha256
            length = $copy.length
            containerMagic = $destinationHeader.magic
            containerSchemaVersion = $destinationHeader.schemaVersion
            containerEngineEpoch = $destinationHeader.engineEpoch
            payloadMagic = $destinationHeader.genrep
            skirmishAiReplayEpoch = $destinationHeader.skirmishAiReplayEpoch
            pathfindingReplayEpoch = $destinationHeader.pathfindingReplayEpoch
            pathfindingReplayMarker = $destinationHeader.pathfindingReplayMarker
            replayQualificationVersion = $destinationHeader.replayQualificationVersion
            replayQualification = $destinationHeader.replayQualification
            exportedUtc = ([DateTime]::UtcNow).ToString('o')
        }
        if ($null -ne $exportedMap) { $result | Add-Member -NotePropertyName maps -NotePropertyValue @($exportedMap) }
        $commitAccepted = $true
        return $result
    }
    finally {
        if ($null -ne $temporaryHeld) {
            try {
                if (-not $commitAccepted) {
                    Remove-Stage5ExporterOwnedHeldFile $temporaryHeld `
                        'Replay export failed commit'
                }
            }
            finally { Close-Stage5ExporterHeldFile $temporaryHeld }
        }
    }
}

function Assert-Stage5ExporterRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [switch]$AllowMissingSource
    )
    $requiredNames = @('origin', 'title', 'category', 'scenario', 'seed',
        'runNonce', 'executableSha256', 'sourceProfileRoot', 'sourcePath',
        'destinationPath', 'sourceSha256', 'destinationSha256',
        'length', 'containerMagic', 'containerSchemaVersion', 'containerEngineEpoch',
        'payloadMagic', 'skirmishAiReplayEpoch')
    if ($Title -ceq 'Generals') {
        $requiredNames += @('pathfindingReplayEpoch',
            'replayQualificationVersion', 'replayQualification')
    }
    if ([string]$Record.category -ceq 'local-capacity-ai') {
        $requiredNames += @('actualAi', 'actualTeams', 'sequence',
            'configuration', 'repeat', 'replayEpoch', 'replaySha256')
    }
    foreach ($name in $requiredNames) {
        Assert-Stage5ExporterCondition ($null -ne $Record.PSObject.Properties[$name]) `
            "Corpus manifest record is missing '$name'."
    }
    $replayContract = Get-Stage5ReplayTitleContract $Title 'Corpus manifest title'
    Assert-Stage5ExporterCondition ([string]$Record.origin -ceq 'native-fresh-runtime') `
        'Corpus manifest record origin is not native-fresh-runtime.'
    Assert-Stage5ExporterCondition ([string]$Record.title -ceq $Title) `
        'Corpus manifest record title does not match the manifest title.'
    Assert-Stage5ExporterCondition ([string]$Record.executableSha256 -ceq $ExecutableSha256.ToUpperInvariant()) `
        'Corpus manifest record executable SHA-256 does not match the manifest executable.'
    $recordSeed = Get-Stage5ExporterRecordInteger $Record 'seed' 1 ([Int32]::MaxValue)
    $recordSequence = Get-Stage5ExporterRecordInteger $Record 'sequence' 1 `
        ([Int32]::MaxValue) -Optional
    $recordRepeat = Get-Stage5ExporterRecordInteger $Record 'repeat' 1 10 -Optional
    if ([string]$Record.category -ceq 'local-capacity-ai') {
        $contract = Get-Stage5ExporterAiContract ([string]$Record.scenario) `
            'Corpus manifest record scenario'
        $actualAi = Get-Stage5ExporterRecordInteger $Record 'actualAi' 0 $contract.actualAi
        Assert-Stage5ExporterCondition ($actualAi -eq $contract.actualAi) `
            'Corpus manifest record actualAi does not match its scenario.'
        $actualTeamsProperty = $Record.PSObject.Properties['actualTeams']
        Assert-Stage5ExporterCondition ($null -ne $actualTeamsProperty -and
            $actualTeamsProperty.Value -is [string] -and
            $actualTeamsProperty.Value -ceq $contract.actualTeams) `
            'Corpus manifest record actualTeams does not match its scenario.'
    }
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusRoot 'CorpusExportRoot'
    $profileRootCandidate = [IO.Path]::GetFullPath([string]$Record.sourceProfileRoot)
    $profileRootExists = Test-Path -LiteralPath $profileRootCandidate -PathType Container
    if ($AllowMissingSource -and -not $profileRootExists) {
        $profileRootFull = Assert-Stage5ExporterTextContainedPath $taskRootFull `
            $profileRootCandidate 'Corpus record source profile root'
    }
    else {
        $profileRootFull = Assert-Stage5ExporterRegularDirectory $profileRootCandidate `
            'Corpus record source profile root'
        Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $profileRootFull `
            'Corpus record source profile root' | Out-Null
    }
    $sourceCandidate = [IO.Path]::GetFullPath([string]$Record.sourcePath)
    $sourceExists = Test-Path -LiteralPath $sourceCandidate -PathType Leaf
    if ($AllowMissingSource -and -not $sourceExists) {
        $sourceFull = Assert-Stage5ExporterTextContainedPath $profileRootFull `
            $sourceCandidate 'Corpus record source path'
    }
    else {
        $sourceFull = Assert-Stage5ExporterContainedPathNoReparse $profileRootFull `
            $sourceCandidate 'Corpus record source path'
    }
    $destinationFull = Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull `
        ([string]$Record.destinationPath) 'Corpus record destination path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $destinationFull -PathType Leaf) `
        "Corpus record destination file was not found: $destinationFull"
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'Corpus manifest record references a reparse-point artifact.'
    $recordLength = Get-Stage5ExporterRecordInteger $Record 'length' 46 `
        $script:Stage5ExporterMaximumReplayBytes
    if ($null -ne $Record.PSObject.Properties['maps']) {
        Assert-Stage5ExporterCondition ($Record.maps -is [Array] -and $Record.maps.Count -le 1 -and
            ($Record.maps.Count -eq 0 -or [string]$Record.scenario -ceq '4v2')) 'Reviewed replay maps must retain one4v2 map binding or the legacy empty list.'
        foreach ($map in $Record.maps) {
            Read-Stage5ReviewedAiMap -ManifestDirectory $corpusRootFull -Map ([ordered]@{
                source=$map.source;mapKey=$map.profileRelativePath;sha256=$map.sha256;byteCount=$map.byteCount;crc=$map.crc
            }) | Out-Null
        }
    }
    $destinationSnapshot = Get-Stage5ExporterReplaySnapshot $destinationFull `
        $Title 'Corpus record destination'
    Assert-Stage5ExporterCondition ($recordLength -eq $destinationSnapshot.length) `
        'Corpus manifest record length does not match its destination file.'
    Assert-Stage5ExporterCondition ([string]$Record.sourceSha256 -match '^[0-9A-Fa-f]{64}$' -and
        [string]$Record.destinationSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest record contains an invalid artifact SHA-256.'
    Assert-Stage5ExporterCondition ($destinationSnapshot.sha256 -ceq
        ([string]$Record.destinationSha256).ToUpperInvariant()) `
        'Corpus manifest record artifact SHA-256 does not match its files.'
    Assert-Stage5ExporterCondition ([string]$Record.sourceSha256 -ceq
        [string]$Record.destinationSha256) `
        'Corpus manifest record source/destination SHA-256 values differ.'
    if ($sourceExists) {
        $sourceSnapshot = Get-Stage5ExporterReplaySnapshot $sourceFull $Title `
            'Corpus record source'
        Assert-Stage5ExporterCondition ($recordLength -eq $sourceSnapshot.length) `
            'Corpus manifest record length does not match its source file.'
        Assert-Stage5ExporterCondition ($sourceSnapshot.sha256 -ceq
            ([string]$Record.sourceSha256).ToUpperInvariant()) `
            'Corpus manifest record source SHA-256 does not match its file.'
    }
    else {
        Assert-Stage5ExporterCondition ($AllowMissingSource -and
            [string]$Record.sourceSha256 -ceq ([string]$Record.destinationSha256)) `
            'Corpus manifest record source is unavailable and cannot be provenance-matched.'
    }
    $containerSchemaVersion = Get-Stage5ExporterRecordInteger $Record `
        'containerSchemaVersion' 2 2
    $containerEngineEpoch = Get-Stage5ExporterRecordInteger $Record `
        'containerEngineEpoch' 1 1
    $skirmishAiReplayEpoch = Get-Stage5ExporterRecordInteger $Record `
        'skirmishAiReplayEpoch' $replayContract.epoch $replayContract.epoch
    Assert-Stage5ExporterCondition ([string]$Record.containerMagic -ceq 'RPL3' -and
        [string]$Record.payloadMagic -ceq 'GENREP' -and
        $containerSchemaVersion -eq 2 -and $containerEngineEpoch -eq 1 -and
        $skirmishAiReplayEpoch -eq $replayContract.epoch) `
        'Corpus manifest record has an invalid native replay container contract.'
    if ($Title -ceq 'Generals') {
        $pathfindingReplayEpoch = Get-Stage5ExporterRecordInteger $Record `
            'pathfindingReplayEpoch' $replayContract.pathfindingEpoch `
            $replayContract.pathfindingEpoch
        $replayQualificationVersion = Get-Stage5ExporterRecordInteger $Record `
            'replayQualificationVersion' $replayContract.qualificationVersion `
            $replayContract.qualificationVersion
        Assert-Stage5ExporterCondition ($pathfindingReplayEpoch -eq
            $replayContract.pathfindingEpoch -and
            $replayQualificationVersion -eq $replayContract.qualificationVersion -and
            [string]$Record.replayQualification -ceq
                [string]$replayContract.qualification) `
            'Corpus manifest record has invalid current Generals path qualification metadata.'
    }
    elseif ($null -ne $Record.PSObject.Properties['replayQualificationVersion']) {
        [void](Get-Stage5ExporterRecordInteger $Record 'replayQualificationVersion' `
            $replayContract.qualificationVersion $replayContract.qualificationVersion)
    }
    $replayEpochProperty = $Record.PSObject.Properties['replayEpoch']
    if ($null -ne $replayEpochProperty) {
        $replayEpoch = Get-Stage5ExporterRecordInteger $Record 'replayEpoch' `
            $replayContract.epoch $replayContract.epoch
        Assert-Stage5ExporterCondition ($replayEpoch -eq $replayContract.epoch) `
            'Corpus manifest record completion epoch does not match its title.'
    }
    $replayShaProperty = $Record.PSObject.Properties['replaySha256']
    if ($null -ne $replayShaProperty) {
        Assert-Stage5ExporterCondition ([string]$replayShaProperty.Value -match
            '^[0-9A-Fa-f]{64}$' -and [string]$replayShaProperty.Value -ceq
            [string]$Record.destinationSha256) `
            'Corpus manifest record completion replay SHA-256 differs from its artifact.'
    }
    $header = $destinationSnapshot.header
    Assert-Stage5ReplayFreshQualification $header $Title `
        'Corpus manifest record destination' | Out-Null
    $headerMatchesRecord = ($header.magic -ceq 'RPL3' -and
        $header.schemaVersion -eq 2 -and $header.engineEpoch -eq 1 -and
        $header.genrep -ceq 'GENREP' -and
        $header.skirmishAiReplayEpoch -eq $replayContract.epoch)
    if ($Title -ceq 'Generals') {
        $headerMatchesRecord = ($headerMatchesRecord -and
            $header.pathfindingReplayEpoch -eq $Record.pathfindingReplayEpoch -and
            $header.replayQualificationVersion -eq
                $Record.replayQualificationVersion -and
            $header.replayQualification -ceq $Record.replayQualification)
    }
    Assert-Stage5ExporterCondition $headerMatchesRecord `
        'Corpus manifest record destination failed native replay revalidation.'
}

function Get-Stage5ExporterJsonProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition ($null -ne $Object) `
        "$Context must be a JSON object."
    $property = $Object.PSObject.Properties[$Name]
    Assert-Stage5ExporterCondition ($null -ne $property -and $null -ne $property.Value) `
        "$Context is missing required property '$Name'."
    return $property.Value
}

function Get-Stage5ExporterJsonInteger {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][Int64]$Minimum,
        [Parameter(Mandatory = $true)][Int64]$Maximum,
        [switch]$Optional
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        if ($Optional) { return $null }
        throw "$Context is missing required property '$Name'."
    }
    Assert-Stage5ExporterCondition (Test-Stage5ExporterInteger $property.Value) `
        "$Context property '$Name' must be an integer."
    Assert-Stage5ExporterCondition ($property.Value -ge $Minimum -and
        $property.Value -le $Maximum) `
        "$Context property '$Name' must be between $Minimum and $Maximum."
    return [Int64]$property.Value
}

function Get-Stage5ExporterJsonSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context,
        [string]$ExpectedSha256
    )
    $held = Open-Stage5ExporterHeldFile $Path $Context `
        $script:Stage5ExporterMaximumJsonBytes 1
    try {
        return Get-Stage5ExporterHeldJsonSnapshot $held $Context $ExpectedSha256
    }
    finally { Close-Stage5ExporterHeldFile $held }
}

function Get-Stage5ExporterHeldJsonSnapshot {
    param(
        [Parameter(Mandatory = $true)][object]$HeldFile,
        [Parameter(Mandatory = $true)][string]$Context,
        [string]$ExpectedSha256
    )
    $bytes = Read-Stage5ExporterFixedBytes $HeldFile.stream $HeldFile.length $Context
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $actualSha256 = (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
        Assert-Stage5ExporterCondition ($ExpectedSha256 -match
            '^[0-9A-Fa-f]{64}$') "$Context SHA-256 is invalid."
        Assert-Stage5ExporterCondition ($actualSha256 -ceq
            $ExpectedSha256.ToUpperInvariant()) `
            "$Context SHA-256 mismatch. Expected $ExpectedSha256, got $actualSha256."
    }
    $offset = if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { 3 } else { 0 }
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    try { $json = $encoding.GetString($bytes, $offset, $bytes.Length - $offset) }
    catch { throw "$Context is not valid UTF-8: $($_.Exception.Message)" }
    try { $document = $json | ConvertFrom-Json }
    catch { throw "$Context is not valid JSON: $($_.Exception.Message)" }
    Assert-Stage5ExporterHeldFileUnchanged $HeldFile $Context
    $identity = Get-Stage5ExporterHandleIdentity $HeldFile.stream.SafeFileHandle $Context
    return [pscustomobject]@{
        path = $HeldFile.path
        canonicalPath = $identity.canonicalPath
        fileId = $identity.fileId
        linkCount = $identity.linkCount
        sha256 = $actualSha256
        length = $HeldFile.length
        document = $document
    }
}

function Get-Stage5ExporterBoundJsonFile {
    param(
        [Parameter(Mandatory = $true)][object]$Binding,
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $path = [string](Get-Stage5ExporterJsonProperty $Binding 'path' $Context)
    $expectedSha256 = [string](Get-Stage5ExporterJsonProperty $Binding 'sha256' $Context)
    $full = Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $path $Context
    return Get-Stage5ExporterJsonSnapshot $full $Context $expectedSha256
}

function Get-Stage5ExporterRecordIdentity {
    param([Parameter(Mandatory = $true)][object]$Record)
    $names = @('origin', 'title', 'category', 'scenario', 'seed', 'actualAi',
        'actualTeams', 'runNonce',
        'executableSha256', 'sourceProfileRoot', 'sourcePath', 'sourceSha256',
        'destinationPath', 'destinationSha256', 'length', 'containerMagic',
        'containerSchemaVersion', 'containerEngineEpoch', 'payloadMagic',
        'skirmishAiReplayEpoch', 'pathfindingReplayEpoch',
        'replayQualificationVersion', 'replayQualification', 'sequence',
        'configuration', 'repeat',
        'replayEpoch', 'replaySha256', 'exportedUtc')
    $values = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $names) {
        $property = $Record.PSObject.Properties[$name]
        $value = ''
        if ($null -ne $property) { $value = [string]$property.Value }
        $values.Add($value) | Out-Null
    }
    return [string]::Join('|', $values.ToArray())
}

function Assert-Stage5ExporterRecordSetsEqual {
    param(
        [Parameter(Mandatory = $true)][object[]]$Left,
        [Parameter(Mandatory = $true)][object[]]$Right,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $leftKeys = @($Left | ForEach-Object { Get-Stage5ExporterRecordIdentity $_ } |
        Sort-Object)
    $rightKeys = @($Right | ForEach-Object { Get-Stage5ExporterRecordIdentity $_ } |
        Sort-Object)
    Assert-Stage5ExporterCondition ($leftKeys.Count -eq $rightKeys.Count) `
        "$Context record counts differ."
    for ($index = 0; $index -lt $leftKeys.Count; ++$index) {
        Assert-Stage5ExporterCondition ($leftKeys[$index] -ceq $rightKeys[$index]) `
            "$Context records differ."
    }
}

function Assert-Stage5ExporterValidationResults {
    param(
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][object]$ResultsDocument
    )
    $results = @($ResultsDocument)
    Assert-Stage5ExporterCondition ($results.Count -gt 0) `
        'Validation-results document must contain a non-empty result array.'
    foreach ($record in @($Records | Where-Object {
        [string]$_.category -ceq 'local-capacity-ai'
    })) {
        $sequence = Get-Stage5ExporterRecordInteger $record 'sequence' 1 `
            ([Int32]::MaxValue)
        $matches = @($results | Where-Object {
            $sequenceProperty = $_.PSObject.Properties['sequence']
            $null -ne $sequenceProperty -and
                (Test-Stage5ExporterInteger $sequenceProperty.Value) -and
                [Int64]$sequenceProperty.Value -eq $sequence
        })
        Assert-Stage5ExporterCondition ($matches.Count -eq 1) `
            "Corpus record sequence $sequence must bind uniquely to validation-results."
        $result = $matches[0]
        [void](Get-Stage5ExporterJsonInteger $result 'sequence' `
            "Validation result sequence $sequence" $sequence $sequence)
        $resultSeed = Get-Stage5ExporterJsonInteger $result 'seed' `
            "Validation result sequence $sequence" 1 ([Int32]::MaxValue)
        $resultRepeat = Get-Stage5ExporterJsonInteger $result 'repeat' `
            "Validation result sequence $sequence" 1 10
        Assert-Stage5ExporterCondition ($resultSeed -eq
            (Get-Stage5ExporterRecordInteger $record 'seed' 1 ([Int32]::MaxValue)) -and
            $resultRepeat -eq (Get-Stage5ExporterRecordInteger $record 'repeat' 1 10) -and
            [string](Get-Stage5ExporterJsonProperty $result 'kind' `
                "Validation result sequence $sequence") -ceq 'ai' -and
            [string](Get-Stage5ExporterJsonProperty $result 'title' `
                "Validation result sequence $sequence") -ceq [string]$record.title -and
            [string](Get-Stage5ExporterJsonProperty $result 'scenario' `
                "Validation result sequence $sequence") -ceq [string]$record.scenario -and
            [string](Get-Stage5ExporterJsonProperty $result 'configuration' `
                "Validation result sequence $sequence") -ceq
                [string]$record.configuration) `
            "Corpus record sequence $sequence disagrees with its validation-results provenance."
        [void](Get-Stage5ExporterJsonInteger $result 'exitCode' `
            "Validation result sequence $sequence" 0 0)
        $timedOut = Get-Stage5ExporterJsonProperty $result 'timedOut' `
            "Validation result sequence $sequence"
        Assert-Stage5ExporterCondition ($timedOut -is [bool] -and -not $timedOut) `
            "Validation result sequence $sequence did not complete successfully."
        $aiEvidence = Get-Stage5ExporterJsonProperty $result 'aiEvidence' `
            "Validation result sequence $sequence"
        $fields = Get-Stage5ExporterJsonProperty $aiEvidence 'fields' `
            "Validation result sequence $sequence AI evidence"
        $actualAiRaw = Get-Stage5ExporterJsonProperty $fields 'actual_ai' `
            "Validation result sequence $sequence AI evidence fields"
        [Int64]$actualAi = 0
        $actualAiValid = if ($actualAiRaw -is [string]) {
            $actualAiRaw -match '^(?:0|[1-9][0-9]*)$' -and
                [Int64]::TryParse($actualAiRaw, [ref]$actualAi)
        }
        else {
            (Test-Stage5ExporterInteger $actualAiRaw) -and
                (($actualAi = [Int64]$actualAiRaw) -ge 0)
        }
        $evidenceRunNonce = [string](Get-Stage5ExporterJsonProperty $fields `
            'run_nonce' "Validation result sequence $sequence AI evidence fields")
        $evidenceReplayEpochRaw = Get-Stage5ExporterJsonProperty $fields `
            'replay_epoch' "Validation result sequence $sequence AI evidence fields"
        [Int64]$evidenceReplayEpoch = 0
        $evidenceReplayEpochValid = if ($evidenceReplayEpochRaw -is [string]) {
            $evidenceReplayEpochRaw -match '^(?:0|[1-9][0-9]*)$' -and
                [Int64]::TryParse($evidenceReplayEpochRaw,
                    [ref]$evidenceReplayEpoch)
        }
        else {
            (Test-Stage5ExporterInteger $evidenceReplayEpochRaw) -and
                (($evidenceReplayEpoch = [Int64]$evidenceReplayEpochRaw) -ge 0)
        }
        $evidenceReplaySha256 = [string](Get-Stage5ExporterJsonProperty $fields `
            'replay_sha256' "Validation result sequence $sequence AI evidence fields")
        $evidenceReplayRetained = [string](Get-Stage5ExporterJsonProperty $fields `
            'replay_retained' "Validation result sequence $sequence AI evidence fields")
        $evidenceReplayRetainedFull = Get-Stage5ExporterFullPath `
            $evidenceReplayRetained `
            "Validation result sequence $sequence replay_retained"
        Assert-Stage5ExporterCondition ($actualAiValid -and
            $actualAi -eq (Get-Stage5ExporterRecordInteger $record 'actualAi' 0 8) -and
            [string](Get-Stage5ExporterJsonProperty $fields 'actual_teams' `
                "Validation result sequence $sequence AI evidence fields") -ceq
                [string]$record.actualTeams -and
            $evidenceRunNonce -match '^[0-9A-Fa-f-]{1,64}$' -and
            $evidenceRunNonce -ceq [string]$record.runNonce -and
            $evidenceReplayEpochValid -and $evidenceReplayEpoch -eq
                (Get-Stage5ExporterRecordInteger $record 'replayEpoch' 1 3) -and
            $evidenceReplaySha256 -match '^[0-9A-Fa-f]{64}$' -and
            $evidenceReplaySha256.ToUpperInvariant() -ceq
                ([string]$record.replaySha256).ToUpperInvariant() -and
            [string]$record.replaySha256 -ceq
                [string]$record.destinationSha256 -and
            [String]::Equals($evidenceReplayRetainedFull,
                [IO.Path]::GetFullPath([string]$record.sourcePath),
                [StringComparison]::OrdinalIgnoreCase)) `
            "Corpus record sequence $sequence lacks matching live-AI and replay-hash provenance."
        $hasMapEvidence = if ($fields -is [Collections.IDictionary]) {
            @($fields.Keys | Where-Object { [string]$_ -ceq 'map_sha256' }).Count -gt 0
        } else { $fields.PSObject.Properties.Name -ccontains 'map_sha256' }
        $hasRetainedMap = $null -ne $record.PSObject.Properties['maps'] -and $record.maps.Count -gt 0
        Assert-Stage5ExporterCondition ($hasMapEvidence -eq $hasRetainedMap) `
            'Reviewed completion map identity and retained map dependency must both be present.'
        if ($hasRetainedMap) {
            $map = $record.maps[0]
            $mapFields = @{}
            foreach ($name in @('map','map_sha256','map_crc','map_size')) {
                $field = Get-Stage5ExporterJsonProperty $fields $name 'Reviewed replay map completion'
                Assert-Stage5ExporterCondition ($field -is [string]) 'Reviewed replay map completion fields must retain native string types.'
                $mapFields[$name] = $field
            }
            Assert-Stage5ReviewedAiMapCompletion -Fields $mapFields -Entry ([pscustomobject]@{
                scenario='4v2';reviewedMap=[pscustomobject]@{
                    source=$map.source;mapKey=$map.profileRelativePath;sha256=$map.sha256;byteCount=$map.byteCount;crc=$map.crc
                }
            })
        }
    }
}

function Write-Stage5ExporterJsonAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Document,
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $full = Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $Path $Context
    Assert-Stage5ExporterPathAbsent $full $Context
    $temporaryFull = '{0}.tmp-{1}' -f $full, [Guid]::NewGuid().ToString('N')
    Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $temporaryFull `
        "$Context temporary path" | Out-Null
    $temporaryHeld = $null
    $commitAccepted = $false
    try {
        $json = $Document | ConvertTo-Json -Depth 24
        $bytes = [Text.Encoding]::UTF8.GetBytes($json)
        Assert-Stage5ExporterCondition ($bytes.LongLength -gt 0 -and
            $bytes.LongLength -le $script:Stage5ExporterMaximumJsonBytes) `
            "$Context JSON exceeds the 64 MiB exporter limit."
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $expectedSha256 = (($sha.ComputeHash($bytes) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
        $temporaryHeld = New-Stage5ExporterHeldCommitFile $temporaryFull `
            "$Context temporary path"
        $temporaryHeld.stream.Write($bytes, 0, $bytes.Length)
        $temporaryHeld.stream.Flush($true)
        $temporaryHeld.length = [Int64]$bytes.LongLength
        $temporarySnapshot = Get-Stage5ExporterHeldJsonSnapshot $temporaryHeld `
            "$Context temporary path" $expectedSha256
        Assert-Stage5ExporterCondition ($temporarySnapshot.length -eq
            $bytes.LongLength) `
            "$Context temporary JSON extent differs from the admitted output."
        Invoke-Stage5ExporterCommitObserver 'before-commit' 'json' `
            $temporaryFull $full
        Assert-Stage5ExporterHeldFileUnchanged $temporaryHeld `
            "$Context temporary path before commit"
        [IO.File]::Move($temporaryFull, $full)
        Invoke-Stage5ExporterCommitObserver 'after-commit' 'json' `
            $temporaryFull $full
        $committedIdentity = Get-Stage5ExporterHandleIdentity `
            $temporaryHeld.stream.SafeFileHandle $Context
        Assert-Stage5ExporterCondition ($committedIdentity.fileId -ceq
            $temporaryHeld.identity.fileId -and
            $committedIdentity.volumeSerialNumber -eq
                $temporaryHeld.identity.volumeSerialNumber -and
            $committedIdentity.linkCount -eq 1 -and
            $committedIdentity.length -eq [UInt64]$bytes.LongLength -and
            [String]::Equals($committedIdentity.canonicalPath, $full,
                [StringComparison]::OrdinalIgnoreCase) -and
            ($committedIdentity.attributes -band
                [UInt32][IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context commit changed file identity, canonical destination, extent, or link count."
        $temporaryHeld.path = $full
        $temporaryHeld.identity = $committedIdentity
        $snapshot = Get-Stage5ExporterHeldJsonSnapshot $temporaryHeld $Context `
            $expectedSha256
        Assert-Stage5ExporterCondition ($snapshot.sha256 -ceq $expectedSha256 -and
            $snapshot.length -eq $bytes.LongLength) `
            "$Context final JSON bytes differ from the admitted output."
        $result = [pscustomobject]@{
            path = $full
            sha256 = $snapshot.sha256
        }
        $commitAccepted = $true
        return $result
    }
    finally {
        if ($null -ne $temporaryHeld) {
            try {
                if (-not $commitAccepted) {
                    Remove-Stage5ExporterOwnedHeldFile $temporaryHeld `
                        "$Context failed commit"
                }
            }
            finally { Close-Stage5ExporterHeldFile $temporaryHeld }
        }
    }
}

function Read-Stage5FreshReplayCorpusBundle {
    param([Parameter(Mandatory = $true)][string]$CorpusManifestPath)
    $manifestFull = Get-Stage5ExporterFullPath $CorpusManifestPath 'Corpus manifest'
    $manifestSnapshot = Get-Stage5ExporterJsonSnapshot $manifestFull 'Corpus manifest'
    $manifest = $manifestSnapshot.document
    [void](Get-Stage5ExporterJsonInteger $manifest 'schemaVersion' `
        'Corpus manifest' 1 1)
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $manifest `
        'kind' 'Corpus manifest') -ceq 'stage5-native-replay-corpus') `
        'Corpus manifest kind is not stage5-native-replay-corpus.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $manifest `
        'origin' 'Corpus manifest') -ceq 'native-fresh-runtime') `
        'Corpus manifest origin is not native-fresh-runtime.'
    $title = [string](Get-Stage5ExporterJsonProperty $manifest 'title' 'Corpus manifest')
    Get-Stage5ReplayTitleContract $title 'Corpus manifest title' | Out-Null
    $executableSha256 = [string](Get-Stage5ExporterJsonProperty $manifest `
        'executableSha256' 'Corpus manifest')
    Assert-Stage5ExporterCondition ($executableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest executable SHA-256 is invalid.'
    $captureModeProperty = $manifest.PSObject.Properties['captureMode']
    $captureMode = if ($null -eq $captureModeProperty) {
        'local-capacity-ai'
    } else { [string]$captureModeProperty.Value }
    $captureMode = $captureMode.ToLowerInvariant()
    Assert-Stage5ExporterCondition ($captureMode -in @(
            'local-capacity-ai', 'serial-baseline-ai')) `
        "Corpus manifest captureMode '$captureMode' is unsupported."
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot `
        ([string](Get-Stage5ExporterJsonProperty $manifest 'taskRoot' 'Corpus manifest')) `
        'Corpus manifest taskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory `
        ([string](Get-Stage5ExporterJsonProperty $manifest 'corpusExportRoot' 'Corpus manifest')) `
        'Corpus manifest corpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'Corpus manifest corpusExportRoot' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $manifestFull `
        'Corpus manifest path' | Out-Null
    $resultsBinding = Get-Stage5ExporterBoundJsonFile `
        (Get-Stage5ExporterJsonProperty $manifest 'validationResults' 'Corpus manifest') `
        $taskRootFull 'Corpus manifest validation results'
    $receiptBinding = Get-Stage5ExporterBoundJsonFile `
        (Get-Stage5ExporterJsonProperty $manifest 'validationReceipt' 'Corpus manifest') `
        $taskRootFull 'Corpus manifest validation receipt'
    $receipt = $receiptBinding.document
    [void](Get-Stage5ExporterJsonInteger $receipt 'schemaVersion' `
        'Validation receipt' 1 1)
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'receiptKind' 'Validation receipt') -ceq 'stage5-local-capacity-receipt') `
        'Corpus manifest validation receipt has an unexpected kind.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'status' 'Validation receipt') -ceq 'passed-non-acceptance') `
        'Corpus manifest validation receipt is not a passed non-acceptance receipt.'
    Assert-Stage5ExporterCondition ((Get-Stage5ExporterJsonProperty $receipt `
        'notAnAcceptanceEnvelope' 'Validation receipt') -is [bool] -and
        [bool](Get-Stage5ExporterJsonProperty $receipt 'notAnAcceptanceEnvelope' `
            'Validation receipt')) `
        'Corpus manifest validation receipt is not marked non-acceptance.'
    Assert-Stage5ExporterCondition ((Get-Stage5ExporterJsonProperty $receipt `
        'finalAcceptanceEligible' 'Validation receipt') -is [bool] -and
        -not [bool](Get-Stage5ExporterJsonProperty $receipt 'finalAcceptanceEligible' `
            'Validation receipt')) `
        'Corpus manifest validation receipt unexpectedly claims final acceptance.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'validationMode' 'Validation receipt') -ceq 'LocalCapacity' -and
        [string](Get-Stage5ExporterJsonProperty $receipt 'capacityMode' `
            'Validation receipt') -ceq 'LocalCapacity') `
        'Validation receipt is not a LocalCapacity result.'
    Assert-Stage5ExporterCondition ((Get-Stage5ExporterJsonProperty $receipt `
        'corpusExportRequested' 'Validation receipt') -is [bool] -and
        [bool](Get-Stage5ExporterJsonProperty $receipt 'corpusExportRequested' `
            'Validation receipt')) `
        'Validation receipt does not confirm that corpus export was requested.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'resultsSha256' 'Validation receipt') -ceq $resultsBinding.sha256) `
        'Validation receipt results SHA-256 differs from the corpus manifest binding.'
    $receiptExport = Get-Stage5ExporterJsonProperty $receipt 'corpusExport' `
        'Validation receipt'
    $receiptCaptureModeProperty = $receiptExport.PSObject.Properties['captureMode']
    $receiptCaptureMode = if ($null -eq $receiptCaptureModeProperty) {
        'local-capacity-ai'
    } else { [string]$receiptCaptureModeProperty.Value }
    Assert-Stage5ExporterCondition ($receiptCaptureMode -ceq $captureMode) `
        'Validation receipt corpusExport captureMode differs from the corpus manifest.'
    $artifactIndexBinding = Get-Stage5ExporterBoundJsonFile `
        ([pscustomobject]@{
            path = Get-Stage5ExporterJsonProperty $receiptExport 'artifactIndexPath' `
                'Validation receipt corpusExport'
            sha256 = Get-Stage5ExporterJsonProperty $receiptExport 'artifactIndexSha256' `
                'Validation receipt corpusExport'
        }) $taskRootFull 'Validation receipt artifact index'
    $artifactIndex = $artifactIndexBinding.document
    [void](Get-Stage5ExporterJsonInteger $artifactIndex 'schemaVersion' `
        'Artifact index' 1 1)
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'kind' 'Artifact index') -ceq 'stage5-native-replay-artifact-index') `
        'Artifact index kind is invalid.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'origin' 'Artifact index') -ceq 'native-fresh-runtime') `
        'Artifact index origin is invalid.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'title' 'Artifact index') -ceq $title) `
        'Artifact index title does not match the corpus manifest.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'executableSha256' 'Artifact index') -ceq $executableSha256.ToUpperInvariant()) `
        'Artifact index executable SHA-256 does not match the corpus manifest.'
    $indexCaptureModeProperty = $artifactIndex.PSObject.Properties['captureMode']
    $indexCaptureMode = if ($null -eq $indexCaptureModeProperty) {
        'local-capacity-ai'
    } else { [string]$indexCaptureModeProperty.Value }
    Assert-Stage5ExporterCondition ($indexCaptureMode -ceq $captureMode) `
        'Artifact index captureMode differs from the corpus manifest.'
    $indexResults = Get-Stage5ExporterJsonProperty $artifactIndex `
        'validationResults' 'Artifact index'
    $indexResultsPath = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull `
        ([string](Get-Stage5ExporterJsonProperty $indexResults 'path' `
            'Artifact index validation results')) 'Artifact index validation results'
    $indexResultsSha256 = [string](Get-Stage5ExporterJsonProperty $indexResults `
        'sha256' 'Artifact index validation results')
    Assert-Stage5ExporterCondition ($indexResultsSha256 -ceq $resultsBinding.sha256 -and
        [String]::Equals($indexResultsPath, $resultsBinding.path,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index validation-results binding differs from the corpus manifest.'
    $expectedManifestFull = [IO.Path]::GetFullPath((Join-Path $corpusRootFull `
        'native-replay-corpus-manifest.json'))
    Assert-Stage5ExporterCondition ([String]::Equals($manifestFull, $expectedManifestFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Corpus manifest must use the fixed native-replay-corpus-manifest.json filename.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receiptExport `
        'status' 'Validation receipt corpusExport') -ceq 'passed') `
        'Validation receipt corpusExport status is not passed.'
    $receiptCorpusRoot = Get-Stage5ExporterFullPath `
        ([string](Get-Stage5ExporterJsonProperty $receipt 'corpusExportRoot' `
            'Validation receipt')) 'Validation receipt corpusExportRoot'
    Assert-Stage5ExporterCondition ([String]::Equals($receiptCorpusRoot, $corpusRootFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Validation receipt corpusExportRoot differs from the corpus manifest.'
    $nestedReceiptRootFull = Get-Stage5ExporterFullPath `
        ([string](Get-Stage5ExporterJsonProperty $receiptExport `
            'corpusExportRoot' 'Validation receipt corpusExport')) `
        'Validation receipt nested corpusExportRoot'
    Assert-Stage5ExporterCondition ([String]::Equals($nestedReceiptRootFull,
        $receiptCorpusRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals($nestedReceiptRootFull, $corpusRootFull,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Validation receipt top-level, nested, and manifest corpusExportRoot values differ.'
    Assert-Stage5ExporterCondition ([String]::Equals(
        [string](Get-Stage5ExporterJsonProperty $artifactIndex 'taskRoot' 'Artifact index'),
        $taskRootFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index taskRoot differs from the corpus manifest.'
    Assert-Stage5ExporterCondition ([String]::Equals(
        [string](Get-Stage5ExporterJsonProperty $artifactIndex 'corpusExportRoot' 'Artifact index'),
        $corpusRootFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index corpusExportRoot differs from the corpus manifest.'
    $expectedArtifactIndexFull = [IO.Path]::GetFullPath((Join-Path $corpusRootFull `
        'native-replay-artifact-index.json'))
    Assert-Stage5ExporterCondition ([String]::Equals($artifactIndexBinding.path,
        $expectedArtifactIndexFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index must use the fixed native-replay-artifact-index.json filename.'
    $records = @((Get-Stage5ExporterJsonProperty $manifest 'records' 'Corpus manifest'))
    $indexRecords = @((Get-Stage5ExporterJsonProperty $artifactIndex 'records' 'Artifact index'))
    $receiptRecords = @((Get-Stage5ExporterJsonProperty $receiptExport 'records' `
        'Validation receipt corpusExport'))
    if ($captureMode -ceq 'serial-baseline-ai') {
        foreach ($record in $records) {
            Assert-Stage5ExporterCondition (([string](Get-Stage5ExporterRecordPropertyValue `
                    $record 'configuration' 'Serial-baseline corpus manifest record')) -ceq
                'serial-1' -and
                ([string](Get-Stage5ExporterRecordPropertyValue $record 'category' `
                    'Serial-baseline corpus manifest record')) -ceq 'local-capacity-ai' -and
                ([string](Get-Stage5ExporterRecordPropertyValue $record 'origin' `
                    'Serial-baseline corpus manifest record')) -ceq 'native-fresh-runtime') `
                'Serial-baseline corpus manifest contains a non-serial or non-native capture record.'
        }
    }
    $receiptRecordCount = Get-Stage5ExporterJsonInteger $receiptExport 'recordCount' `
        'Validation receipt corpusExport' 1 ([Int32]::MaxValue)
    $indexRecordCount = Get-Stage5ExporterJsonInteger $artifactIndex 'recordCount' `
        'Artifact index' 1 ([Int32]::MaxValue)
    Assert-Stage5ExporterCondition ($records.Count -gt 0 -and
        $records.Count -eq $indexRecords.Count -and
        $records.Count -eq $receiptRecords.Count -and
        $records.Count -eq $indexRecordCount -and
        $records.Count -eq $receiptRecordCount) `
        'Corpus manifest, validation receipt, and artifact index record counts are invalid.'
    foreach ($record in $records) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    foreach ($record in $indexRecords) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    foreach ($record in $receiptRecords) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    Assert-Stage5ExporterRecordSetsEqual $records $indexRecords `
        'Corpus manifest and artifact index'
    Assert-Stage5ExporterRecordSetsEqual $records $receiptRecords `
        'Corpus manifest and validation receipt'
    Assert-Stage5ExporterValidationResults $records $resultsBinding.document
    return [pscustomobject]@{
        manifestPath = $manifestFull
        manifestSha256 = $manifestSnapshot.sha256
        manifest = $manifest
        taskRoot = $taskRootFull
        corpusRoot = $corpusRootFull
        title = $title
        captureMode = $captureMode
        executableSha256 = $executableSha256.ToUpperInvariant()
        results = $resultsBinding
        receipt = $receiptBinding
        artifactIndex = $artifactIndexBinding
        records = $records
    }
}

function Write-Stage5FreshReplayArtifactIndex {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$ValidationResultsPath,
        [ValidateSet('local-capacity-ai', 'serial-baseline-ai')]
        [string]$CaptureMode = 'local-capacity-ai'
    )
    $CaptureMode = $CaptureMode.ToLowerInvariant()
    Assert-Stage5ExporterCondition ($Title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Artifact index title is invalid.'
    Get-Stage5ReplayTitleContract $Title 'Artifact index title' | Out-Null
    Assert-Stage5ExporterCondition ($ExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Artifact index executable SHA-256 is invalid.'
    Assert-Stage5ExporterCondition ($null -ne $Records -and $Records.Count -gt 0) `
        'Artifact index requires at least one exported replay record.'
    if ($CaptureMode -ceq 'serial-baseline-ai') {
        Assert-Stage5ExporterCondition (@($Records | Where-Object {
            $null -eq $_.PSObject.Properties['configuration'] -or
            [string]$_.configuration -cne 'serial-1'
        }).Count -eq 0) `
            'Serial-baseline artifact index accepts only serial-1 capture records.'
    }
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    $resultsFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull `
        $ValidationResultsPath 'Validation results path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $resultsFull -PathType Leaf) `
        "Validation results file was not found: $resultsFull"
    foreach ($record in @($Records)) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $Title $ExecutableSha256
    }
    $indexFull = Join-Path $corpusRootFull 'native-replay-artifact-index.json'
    $document = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-artifact-index'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        taskRoot = $taskRootFull
        corpusExportRoot = $corpusRootFull
        title = $Title
        captureMode = $CaptureMode
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        validationResults = [ordered]@{
            path = $resultsFull
            sha256 = Get-Stage5ExporterSha256 $resultsFull
        }
        recordCount = $Records.Count
        records = @($Records)
    }
    $written = Write-Stage5ExporterJsonAtomically $indexFull $document `
        $corpusRootFull 'Artifact index'
    return [pscustomobject]@{
        path = $written.path
        sha256 = $written.sha256
        captureMode = $CaptureMode
        recordCount = $Records.Count
    }
}

function Write-Stage5FreshReplayCorpusManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$ValidationResultsPath,
        [string]$ValidationReceiptPath = '',
        [ValidateSet('local-capacity-ai', 'serial-baseline-ai')]
        [string]$CaptureMode = 'local-capacity-ai'
    )
    $CaptureMode = $CaptureMode.ToLowerInvariant()
    Assert-Stage5ExporterCondition ($Title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Corpus manifest title is invalid.'
    Get-Stage5ReplayTitleContract $Title 'Corpus manifest title' | Out-Null
    Assert-Stage5ExporterCondition ($ExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest executable SHA-256 is invalid.'
    Assert-Stage5ExporterCondition ($null -ne $Records -and $Records.Count -gt 0) `
        'Corpus manifest requires at least one exported replay record.'
    if ($CaptureMode -ceq 'serial-baseline-ai') {
        Assert-Stage5ExporterCondition (@($Records | Where-Object {
            $null -eq $_.PSObject.Properties['configuration'] -or
            [string]$_.configuration -cne 'serial-1'
        }).Count -eq 0) `
            'Serial-baseline corpus manifest accepts only serial-1 capture records.'
    }
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    $resultsFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $ValidationResultsPath `
        'Validation results path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $resultsFull -PathType Leaf) `
        "Validation results file was not found: $resultsFull"
    $manifestFull = Join-Path $corpusRootFull 'native-replay-corpus-manifest.json'
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $manifestFull `
        'Corpus manifest path' | Out-Null
    Assert-Stage5ExporterPathAbsent $manifestFull 'Corpus manifest'
    foreach ($record in @($Records)) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $Title $ExecutableSha256
    }
    $receiptBinding = $null
    if (-not [string]::IsNullOrWhiteSpace($ValidationReceiptPath)) {
        $receiptFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $ValidationReceiptPath `
            'Validation receipt path'
        Assert-Stage5ExporterCondition (Test-Path -LiteralPath $receiptFull -PathType Leaf) `
            "Validation receipt file was not found: $receiptFull"
        $receiptBinding = [ordered]@{
            path = $receiptFull
            sha256 = Get-Stage5ExporterSha256 $receiptFull
        }
    }
    $document = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-corpus'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        taskRoot = $taskRootFull
        corpusExportRoot = $corpusRootFull
        title = $Title
        captureMode = $CaptureMode
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        validationResults = [ordered]@{
            path = $resultsFull
            sha256 = Get-Stage5ExporterSha256 $resultsFull
        }
        validationReceipt = $receiptBinding
        records = @($Records)
    }
    $written = Write-Stage5ExporterJsonAtomically $manifestFull $document `
        $corpusRootFull 'Corpus manifest'
    return [pscustomobject]@{
        path = $written.path
        sha256 = $written.sha256
        captureMode = $CaptureMode
        recordCount = $Records.Count
    }
}

function Get-Stage5ExporterSelectionKey {
    param([Parameter(Mandatory = $true)][object]$Record)
    $sequence = 0
    $sequenceProperty = $Record.PSObject.Properties['sequence']
    if ($null -ne $sequenceProperty) {
        $sequence = Get-Stage5ExporterRecordInteger $Record 'sequence' 1 `
            ([Int32]::MaxValue)
    }
    $repeat = 0
    $repeatProperty = $Record.PSObject.Properties['repeat']
    if ($null -ne $repeatProperty) {
        $repeat = Get-Stage5ExporterRecordInteger $Record 'repeat' 1 10
    }
    $configuration = ''
    $configurationProperty = $Record.PSObject.Properties['configuration']
    if ($null -ne $configurationProperty) { $configuration = [string]$configurationProperty.Value }
    $seed = Get-Stage5ExporterRecordInteger $Record 'seed' 1 ([Int32]::MaxValue)
    return ('{0:D10}|{1}|{2:D10}|{3:D10}|{4}|{5}|{6}|{7}' -f `
        $sequence, [string]$Record.scenario, $seed, $repeat,
        $configuration, [string]$Record.runNonce,
        [string]$Record.sourceSha256, [string]$Record.destinationSha256)
}

function Assert-Stage5ExporterFullAiSeedMatrix {
    param([Parameter(Mandatory = $true)][object[]]$Records)
    $requiredScenarios = @('4v3', '4v2', 'hard-ai-2v6')
    $seedSets = @{}
    foreach ($scenario in $requiredScenarios) {
        $scenarioSeeds = @($Records | Where-Object {
            [string]$_.scenario -ceq $scenario
        } | ForEach-Object {
            Get-Stage5ExporterRecordInteger $_ 'seed' 1 ([Int32]::MaxValue)
        } | Sort-Object -Unique)
        Assert-Stage5ExporterCondition ($scenarioSeeds.Count -ge 3) `
            "Native fixture conversion requires at least three distinct seeds for scenario '$scenario'."
        $seedSets[$scenario] = $scenarioSeeds
    }
    $referenceSeeds = @($seedSets['4v3'])
    foreach ($scenario in @('4v2', 'hard-ai-2v6')) {
        $scenarioSeeds = @($seedSets[$scenario])
        $sameSeeds = $scenarioSeeds.Count -eq $referenceSeeds.Count
        for ($index = 0; $sameSeeds -and $index -lt $referenceSeeds.Count; ++$index) {
            $sameSeeds = [Int64]$scenarioSeeds[$index] -eq [Int64]$referenceSeeds[$index]
        }
        Assert-Stage5ExporterCondition $sameSeeds `
            'Native fixture conversion requires the full scenario-by-seed cross-product with the same seed set for 4v3, 4v2, and hard-ai-2v6.'
    }
    return [pscustomobject]@{
        seeds = $referenceSeeds
        scenarios = $requiredScenarios
    }
}

function Convert-Stage5FreshReplayCorpusManifestToFixtures {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CorpusManifestPath,
        [Parameter(Mandatory = $true)][string]$FixtureManifestPath,
        [Parameter(Mandatory = $true)][string]$ProvenancePath,
        [Parameter(Mandatory = $true)][string]$Executable
    )
    Assert-Stage5ExporterCondition ($Executable -match '^[A-Za-z0-9._-]+\.exe$') `
        'Native fixture executable must be a leaf .exe name.'
    $bundle = Read-Stage5FreshReplayCorpusBundle $CorpusManifestPath
    Assert-Stage5ExporterCondition ($bundle.title -ceq 'Generals' -or
        $bundle.title -ceq 'ZeroHour') `
        'Native fixture conversion requires a Generals or ZeroHour corpus title.'
    $expectedExecutablePrefix = if ($bundle.title -ceq 'Generals') {
        'generalsv'
    }
    else {
        'generalszh'
    }
    Assert-Stage5ExporterCondition ($Executable -match ('^' +
        [regex]::Escape($expectedExecutablePrefix) +
        '(?:-[A-Za-z0-9._-]+)?\.exe$')) `
        "Native fixture executable '$Executable' does not belong to corpus title '$($bundle.title)'."
    $fixtureFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
        $FixtureManifestPath 'Native fixture manifest path'
    $provenanceFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
        $ProvenancePath 'Native fixture provenance path'
    $fixtureParent = [IO.Path]::GetDirectoryName($fixtureFull).TrimEnd('\')
    $provenanceParent = [IO.Path]::GetDirectoryName($provenanceFull).TrimEnd('\')
    Assert-Stage5ExporterCondition ([String]::Equals($fixtureParent, $bundle.corpusRoot,
        [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals($provenanceParent, $bundle.corpusRoot,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Native fixture manifest and provenance must be direct children of CorpusExportRoot.'
    Assert-Stage5ExporterCondition (-not [String]::Equals($fixtureFull, $provenanceFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Native fixture manifest and provenance paths must differ.'
    Assert-Stage5ExporterPathAbsent $fixtureFull 'Native fixture manifest'
    Assert-Stage5ExporterPathAbsent $provenanceFull 'Native fixture provenance'

    $records = @($bundle.records)
    $aiRecords = @($records | Where-Object {
        [string]$_.category -ceq 'local-capacity-ai'
    })
    Assert-Stage5ExporterCondition ($aiRecords.Count -eq $records.Count) `
        'Native fixture conversion accepts only local-capacity-ai corpus records.'
    $aiMatrix = Assert-Stage5ExporterFullAiSeedMatrix $aiRecords
    $aiSeeds = @($aiMatrix.seeds)
    $aiScenarios = @($aiMatrix.scenarios)
    $groups = @($records | Group-Object -Property {
        ([string]$_.destinationSha256).ToUpperInvariant()
    } | Sort-Object Name)
    Assert-Stage5ExporterCondition ($groups.Count -ge 10) `
        'Native fixture conversion requires at least 10 unique replay SHA-256 values.'
    $stressGroups = @($groups | Where-Object {
        @($_.Group | Where-Object {
            [string]$_.scenario -ceq 'hard-ai-2v6'
        }).Count -gt 0
    })
    Assert-Stage5ExporterCondition ($stressGroups.Count -gt 0) `
        'Native fixture conversion requires a hard-ai-2v6 replay for the single stress fixture.'
    $stressGroup = @($stressGroups | Sort-Object Name | Select-Object -First 1)[0]
    $stressRecord = @($stressGroup.Group | Where-Object {
        [string]$_.scenario -ceq 'hard-ai-2v6'
    } | Sort-Object @{ Expression = { Get-Stage5ExporterSelectionKey $_ } } |
        Select-Object -First 1)[0]
    $stressGroupNames = @($stressGroups | ForEach-Object { $_.Name })
    $normalRecords = @($groups | Where-Object { $stressGroupNames -notcontains $_.Name } |
        ForEach-Object {
            @($_.Group | Sort-Object @{
                Expression = { Get-Stage5ExporterSelectionKey $_ }
            } | Select-Object -First 1)
        } | Sort-Object Name)
    Assert-Stage5ExporterCondition ($normalRecords.Count -ge 9) `
        'Native fixture conversion requires nine non-stress unique replay SHA-256 values.'

    $selected = New-Object 'Collections.Generic.List[object]'
    foreach ($record in @($normalRecords | Select-Object -First 9)) {
        $selected.Add([pscustomobject]@{ record = $record; stress = $false }) | Out-Null
    }
    $selected.Add([pscustomobject]@{ record = $stressRecord; stress = $true }) | Out-Null
    Assert-Stage5ExporterCondition ($selected.Count -eq 10) `
        'Native fixture conversion did not select exactly 10 records.'
    Assert-Stage5ExporterCondition (@($selected | Where-Object { $_.stress }).Count -eq 1) `
        'Native fixture conversion did not select exactly one stress record.'

    $fixtureEntries = New-Object 'Collections.Generic.List[object]'
    $richFixtures = New-Object 'Collections.Generic.List[object]'
    $normalIndex = 1
    foreach ($selectedRecord in $selected.ToArray()) {
        $record = $selectedRecord.record
        Assert-Stage5ExporterCondition ([string]$record.category -ceq 'local-capacity-ai') `
            'Native fixture conversion accepts only local-capacity-ai records.'
        Assert-Stage5ExporterCondition ([string]$record.origin -ceq 'native-fresh-runtime') `
            'Native fixture conversion accepts only native-fresh-runtime records.'
        Assert-Stage5ExporterCondition ([string]$record.scenario -ceq '4v2' -or
            [string]$record.scenario -ceq '4v3' -or
            [string]$record.scenario -ceq 'hard-ai-2v6') `
            'Native fixture conversion found an unsupported AI scenario.'
        Assert-Stage5ExporterCondition ($record.seed -gt 0) `
            'Native fixture conversion found a non-positive AI seed.'
        Assert-Stage5ExporterCondition ([string]$record.sourceSha256 -ceq
            [string]$record.destinationSha256) `
            'Native fixture conversion found a source/destination SHA-256 mismatch.'
        $destinationFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
            ([string]$record.destinationPath) 'Native fixture destination path'
        $relative = $destinationFull.Substring($bundle.corpusRoot.Length)
        while ($relative.StartsWith('\') -or $relative.StartsWith('/')) {
            $relative = $relative.Substring(1)
        }
        Assert-Stage5ExporterCondition (-not [IO.Path]::IsPathRooted($relative) -and
            $relative -notmatch '(^|[\\/])\.\.([\\/]|$)') `
            'Native fixture source path must remain relative to CorpusExportRoot.'
        $relative = $relative.Replace('/', '\')
        $id = if ($selectedRecord.stress) {
            'native-stress-hard-ai-2v6'
        }
        else {
            'native-{0:D2}' -f $normalIndex++
        }
        $fixtureMaps = @()
        if ($null -ne $record.PSObject.Properties['maps']) {
            $fixtureMaps = @($record.maps | ForEach-Object {
                [ordered]@{source=$_.source;profileRelativePath=$_.profileRelativePath;sha256=$_.sha256}
            })
        }
        $fixtureEntries.Add([ordered]@{
            id = $id
            source = $relative
            sha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            stress = [bool]$selectedRecord.stress
            maps = $fixtureMaps
        }) | Out-Null
        $rich = [ordered]@{
            id = $id
            source = $relative
            sha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            stress = [bool]$selectedRecord.stress
            category = [string]$record.category
            scenario = [string]$record.scenario
            seed = $record.seed
            actualAi = $record.actualAi
            actualTeams = $record.actualTeams
            runNonce = [string]$record.runNonce
            origin = [string]$record.origin
            title = [string]$record.title
            executableSha256 = ([string]$record.executableSha256).ToUpperInvariant()
            sourceProfileRoot = [string]$record.sourceProfileRoot
            sourcePath = [string]$record.sourcePath
            sourceSha256 = ([string]$record.sourceSha256).ToUpperInvariant()
            destinationPath = $destinationFull
            destinationSha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            length = [Int64]$record.length
            containerMagic = [string]$record.containerMagic
            containerSchemaVersion = [int]$record.containerSchemaVersion
            containerEngineEpoch = [int]$record.containerEngineEpoch
            payloadMagic = [string]$record.payloadMagic
            skirmishAiReplayEpoch = [int]$record.skirmishAiReplayEpoch
            pathfindingReplayEpoch = if ($null -ne $record.PSObject.Properties['pathfindingReplayEpoch']) {
                [int]$record.pathfindingReplayEpoch
            } else { $null }
            replayQualificationVersion = if ($null -ne $record.PSObject.Properties['replayQualificationVersion']) {
                [int]$record.replayQualificationVersion
            } else { $null }
            replayQualification = if ($null -ne $record.PSObject.Properties['replayQualification']) {
                [string]$record.replayQualification
            } else { $null }
            sequence = if ($null -ne $record.PSObject.Properties['sequence']) {
                $record.sequence
            } else { $null }
            configuration = if ($null -ne $record.PSObject.Properties['configuration']) {
                [string]$record.configuration
            } else { $null }
            repeat = if ($null -ne $record.PSObject.Properties['repeat']) {
                $record.repeat
            } else { $null }
            replayEpoch = if ($null -ne $record.PSObject.Properties['replayEpoch']) {
                [int]$record.replayEpoch
            } else { $null }
            replaySha256 = if ($null -ne $record.PSObject.Properties['replaySha256']) {
                ([string]$record.replaySha256).ToUpperInvariant()
            } else { $null }
            exportedUtc = if ($null -ne $record.PSObject.Properties['exportedUtc']) {
                [string]$record.exportedUtc
            } else { $null }
        }
        $richFixtures.Add($rich) | Out-Null
    }
    $fixtureDocument = [ordered]@{
        schemaVersion = 1
        title = [string]$bundle.title
        executable = $Executable
        executableSha256 = [string]$bundle.executableSha256
        fixtures = $fixtureEntries.ToArray()
        ai = [ordered]@{
            seeds = $aiSeeds
            scenarios = $aiScenarios
            repeats = 1
        }
    }
    $fixtureWritten = Write-Stage5ExporterJsonAtomically $fixtureFull $fixtureDocument `
        $bundle.corpusRoot 'Native fixture manifest'
    $provenanceDocument = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-fixture-provenance'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        title = [string]$bundle.title
        captureMode = [string]$bundle.captureMode
        executable = $Executable
        executableSha256 = [string]$bundle.executableSha256
        corpusManifest = [ordered]@{
            path = $bundle.manifestPath
            sha256 = $bundle.manifestSha256
        }
        artifactIndex = [ordered]@{
            path = $bundle.artifactIndex.path
            sha256 = $bundle.artifactIndex.sha256
        }
        validationReceipt = [ordered]@{
            path = $bundle.receipt.path
            sha256 = $bundle.receipt.sha256
        }
        validationResults = [ordered]@{
            path = $bundle.results.path
            sha256 = $bundle.results.sha256
        }
        fixtureManifest = [ordered]@{
            path = $fixtureWritten.path
            sha256 = $fixtureWritten.sha256
        }
        fixtureCount = $fixtureEntries.Count
        stressFixtureCount = @($selected | Where-Object { $_.stress }).Count
        fixtures = $richFixtures.ToArray()
    }
    $provenanceWritten = Write-Stage5ExporterJsonAtomically $provenanceFull `
        $provenanceDocument $bundle.corpusRoot 'Native fixture provenance'
    return [pscustomobject]@{
        status = 'passed'
        fixtureManifest = $fixtureWritten
        provenance = $provenanceWritten
        fixtureCount = $fixtureEntries.Count
        stressFixtureCount = @($selected | Where-Object { $_.stress }).Count
        selectedSha256 = @($fixtureEntries.ToArray() | ForEach-Object { $_.sha256 })
    }
}

Export-ModuleMember -Function Get-Stage5ReplayCompletionFields, `
    Export-Stage5FreshReplayArtifact, Write-Stage5FreshReplayArtifactIndex, `
    Write-Stage5FreshReplayCorpusManifest, Convert-Stage5FreshReplayCorpusManifestToFixtures
