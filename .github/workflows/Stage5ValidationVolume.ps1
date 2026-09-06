[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Provision', 'Cleanup')]
    [string]$Mode,
    [Parameter(Mandatory)]
    [string]$Token,
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-Stage5SafeValidationToken {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw 'Stage 5 validation volume token is required.'
    }
    $normalized = [regex]::Replace($Value, '[^A-Za-z0-9._-]', '-')
    if ($normalized -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$' -or
        $normalized -in @('.', '..')) {
        throw 'Stage 5 validation volume token is not a bounded safe identifier.'
    }
    return $normalized
}

function Assert-Stage5RegularDirectory {
    param([string]$Path, [string]$Context)

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Context is not a regular directory: $Path"
    }
}

function Assert-Stage5RegularLeaf {
    param([string]$Path, [string]$Context)

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Context is not a regular file: $Path"
    }
}

function Get-Stage5ValidationVolumePaths {
    param(
        [string]$ValidationToken,
        [string]$DriveRoot = 'H:\',
        [string]$RunnerTemp = $env:RUNNER_TEMP
    )

    if ([string]::IsNullOrWhiteSpace($RunnerTemp)) {
        throw 'RUNNER_TEMP is unavailable for Stage 5 validation volume backing.'
    }
    $runnerTemp = [IO.Path]::GetFullPath($RunnerTemp).TrimEnd('\', '/')
    Assert-Stage5RegularDirectory $runnerTemp 'RUNNER_TEMP'
    $baseName = 'Stage5ValidationH-' + $ValidationToken
    $scratchNamespace = [IO.Path]::Combine($DriveRoot, 'Stage5CiScratch')
    $scratchRoot = [IO.Path]::Combine($scratchNamespace, $ValidationToken)
    return [pscustomobject]@{
        RunnerTemp = $runnerTemp
        BackingFile = Join-Path $runnerTemp ($baseName + '.vhdx')
        DiskpartScript = Join-Path $runnerTemp ($baseName + '-provision.txt')
        DetachScript = Join-Path $runnerTemp ($baseName + '-detach.txt')
        ScratchRoot = $scratchRoot
        MarkerPath = [IO.Path]::Combine($scratchRoot, '.stage5-vhd-owner')
    }
}

function Get-Stage5SubstMappings {
    $mappings = @(& subst.exe)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect Windows subst mappings for Stage 5 validation volume ownership.'
    }
    return $mappings
}

function Assert-Stage5NoSubstH {
    param([string]$Context)

    $mappings = @(Get-Stage5SubstMappings)
    if ($mappings -match '(?im)^\s*H:\\?:\s*=>') {
        throw "$Context found an H: subst mapping; refusing to touch an unrelated mapping."
    }
}

function Write-Stage5ValidationEnvironment {
    param(
        [string]$ValidationToken,
        [pscustomobject]$Paths
    )

    if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
        throw 'GITHUB_ENV is unavailable for Stage 5 validation volume ownership bindings.'
    }
    $environmentLines = @(
        'RTS_STAGE5_VALIDATION_VHD_OWNED=true',
        "RTS_STAGE5_VALIDATION_VHD_PATH=$($Paths.BackingFile)",
        "RTS_STAGE5_VALIDATION_VHD_TOKEN=$ValidationToken",
        "RTS_STAGE5_VALIDATION_VHD_MARKER=$($Paths.MarkerPath)",
        "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=$($Paths.ScratchRoot)"
    )
    [IO.File]::AppendAllLines($env:GITHUB_ENV, $environmentLines,
        [Text.UTF8Encoding]::new($false))
}

function Invoke-Stage5ValidationVolumeProvision {
    param(
        [string]$ValidationToken,
        [pscustomobject]$Paths
    )

    if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
        throw 'GITHUB_ENV is unavailable for Stage 5 validation volume ownership bindings.'
    }
    foreach ($exclusivePath in @($Paths.BackingFile, $Paths.DiskpartScript,
            $Paths.DetachScript)) {
        if (Test-Path -LiteralPath $exclusivePath) {
            throw "Stage 5 validation volume path already exists: $exclusivePath"
        }
    }
    Assert-Stage5NoSubstH 'Stage 5 validation volume provisioning'
    $hDrive = Get-PSDrive -Name H -ErrorAction SilentlyContinue
    if ($null -ne $hDrive -or (Test-Path -LiteralPath 'H:\' -PathType Container)) {
        throw 'H: already exists; refusing to format or detach an unrelated volume.'
    }

    # Publish ownership before diskpart mutates the runner. Cleanup can then
    # detach an image even when provisioning fails after creation or attach.
    Write-Stage5ValidationEnvironment $ValidationToken $Paths
    $diskpartCommands = @(
        "create vdisk file=`"$($Paths.BackingFile)`" maximum=32768 type=expandable",
        "select vdisk file=`"$($Paths.BackingFile)`"",
        'attach vdisk',
        'create partition primary',
        'format fs=ntfs quick label=Stage5Validation',
        'assign letter=H'
    )
    [IO.File]::WriteAllLines($Paths.DiskpartScript, $diskpartCommands,
        [Text.UTF8Encoding]::new($false))
    & diskpart.exe /s $Paths.DiskpartScript
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create and attach the unique Stage 5 validation VHDX: $($Paths.BackingFile)"
    }

    $diskImage = Get-DiskImage -ImagePath $Paths.BackingFile -ErrorAction Stop
    if (-not $diskImage.Attached -or $null -eq $diskImage.Number) {
        throw "Stage 5 validation VHDX was not attached: $($Paths.BackingFile)"
    }
    $partition = Get-Partition -DriveLetter H -ErrorAction Stop
    if ($partition.DiskNumber -ne $diskImage.Number) {
        throw 'Stage 5 validation H: partition is not backed by the task-owned VHDX.'
    }
    $volume = Get-Volume -Partition $partition -ErrorAction Stop
    if ($volume.FileSystem -cne 'NTFS') {
        throw "Stage 5 validation H: volume is not NTFS: $($volume.FileSystem)"
    }
    $driveInfo = New-Object IO.DriveInfo 'H:\'
    if ($driveInfo.DriveType -ne [IO.DriveType]::Fixed -or
        $driveInfo.DriveFormat -cne 'NTFS') {
        throw 'Stage 5 validation H: is not a fixed NTFS volume.'
    }
    Assert-Stage5RegularDirectory 'H:\' 'Stage 5 validation H: root'
    Assert-Stage5NoSubstH 'Stage 5 validation volume verification'

    $scratchNamespace = 'H:\Stage5CiScratch'
    if (Test-Path -LiteralPath $scratchNamespace) {
        throw "Stage 5 validation scratch namespace is not fresh: $scratchNamespace"
    }
    New-Item -ItemType Directory -Path $scratchNamespace | Out-Null
    Assert-Stage5RegularDirectory $scratchNamespace 'Stage 5 validation scratch namespace'
    if (Test-Path -LiteralPath $Paths.ScratchRoot) {
        throw "Stage 5 validation scratch path is not fresh: $($Paths.ScratchRoot)"
    }
    New-Item -ItemType Directory -Path $Paths.ScratchRoot | Out-Null
    Assert-Stage5RegularDirectory $Paths.ScratchRoot 'Stage 5 validation scratch path'
    $markerText = "Stage5ValidationVhd=v1`nToken=$ValidationToken`nBackingFile=$($Paths.BackingFile)`n"
    [IO.File]::WriteAllText($Paths.MarkerPath, $markerText,
        [Text.UTF8Encoding]::new($false))
    Assert-Stage5RegularLeaf $Paths.MarkerPath `
        'Stage 5 validation VHD ownership marker'
    Write-Host "Stage 5 validation scratch VHDX: $($Paths.BackingFile) (disk=$($diskImage.Number))"
    Write-Host "Stage 5 validation scratch: $($Paths.ScratchRoot)"
}

function Invoke-Stage5ValidationVolumeCleanup {
    param(
        [string]$ValidationToken,
        [pscustomobject]$Paths
    )

    if ($env:RTS_STAGE5_VALIDATION_VHD_OWNED -cne 'true') {
        Write-Host 'No task-owned Stage 5 validation VHDX was published; nothing to clean.'
        return
    }
    if ($env:RTS_STAGE5_VALIDATION_VHD_TOKEN -cne $ValidationToken) {
        throw 'Stage 5 cleanup ownership token does not match the unique task token.'
    }
    if ([string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_VHD_PATH) -or
        [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_VHD_PATH) -cne
        [IO.Path]::GetFullPath($Paths.BackingFile)) {
        throw 'Stage 5 cleanup ownership path does not match the unique task token.'
    }
    if ([string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_VHD_MARKER) -or
        [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_VHD_MARKER) -cne
        [IO.Path]::GetFullPath($Paths.MarkerPath)) {
        throw 'Stage 5 cleanup ownership marker path does not match the unique task token.'
    }

    $scratchRootExists = Test-Path -LiteralPath $Paths.ScratchRoot
    $diskImage = Get-DiskImage -ImagePath $Paths.BackingFile `
        -ErrorAction SilentlyContinue
    $partition = Get-Partition -DriveLetter H -ErrorAction SilentlyContinue
    # Prove the backing image and H: identity before recursively removing any
    # scratch content. This preserves evidence when ownership is ambiguous.
    if ($scratchRootExists -and
        ($null -eq $diskImage -or -not $diskImage.Attached)) {
        throw 'Stage 5 cleanup found scratch files without the task-owned VHDX attached.'
    }
    if ($scratchRootExists -and
        ($null -eq $partition -or $partition.DiskNumber -ne $diskImage.Number)) {
        throw 'Stage 5 cleanup refused scratch removal because H: is not the task-owned VHDX.'
    }
    if ($scratchRootExists) {
        Assert-Stage5RegularDirectory $Paths.ScratchRoot `
            'Stage 5 cleanup scratch root'
        $redirected = @(Get-ChildItem -LiteralPath $Paths.ScratchRoot -Recurse -Force |
            Where-Object {
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            })
        if ($redirected.Count -ne 0) {
            throw 'Stage 5 cleanup scratch root contains a reparse point.'
        }
        Assert-Stage5RegularLeaf $Paths.MarkerPath `
            'Stage 5 cleanup ownership marker'
        $expectedMarker = "Stage5ValidationVhd=v1`nToken=$ValidationToken`nBackingFile=$($Paths.BackingFile)`n"
        if ([IO.File]::ReadAllText($Paths.MarkerPath) -cne $expectedMarker) {
            throw 'Stage 5 cleanup ownership marker does not match the task-owned VHDX.'
        }
        Remove-Item -LiteralPath $Paths.ScratchRoot -Recurse -Force
    }
    if ($null -ne $diskImage -and $diskImage.Attached) {
        if ($null -ne $partition -and $partition.DiskNumber -ne $diskImage.Number) {
            throw 'Stage 5 cleanup refused to detach H: because it is not the task-owned VHDX.'
        }
        if (Test-Path -LiteralPath $Paths.DetachScript) {
            throw "Stage 5 cleanup detach script path already exists: $($Paths.DetachScript)"
        }
        $detachCommands = @(
            "select vdisk file=`"$($Paths.BackingFile)`"",
            'detach vdisk'
        )
        [IO.File]::WriteAllLines($Paths.DetachScript, $detachCommands,
            [Text.UTF8Encoding]::new($false))
        & diskpart.exe /s $Paths.DetachScript
        if ($LASTEXITCODE -ne 0) {
            throw "Could not detach the task-owned Stage 5 validation VHDX: $($Paths.BackingFile)"
        }
        $remainingImage = Get-DiskImage -ImagePath $Paths.BackingFile `
            -ErrorAction SilentlyContinue
        if ($null -ne $remainingImage -and $remainingImage.Attached) {
            throw "Stage 5 validation VHDX remained attached after cleanup: $($Paths.BackingFile)"
        }
    }
    if (Test-Path -LiteralPath 'H:\' -PathType Container) {
        Assert-Stage5NoSubstH 'Stage 5 cleanup'
        throw 'H: remained present after detaching the task-owned Stage 5 validation VHDX.'
    }
    foreach ($temporaryPath in @($Paths.BackingFile, $Paths.DiskpartScript,
            $Paths.DetachScript)) {
        if (Test-Path -LiteralPath $temporaryPath) {
            Assert-Stage5RegularLeaf $temporaryPath 'Stage 5 cleanup temporary path'
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
    foreach ($temporaryPath in @($Paths.BackingFile, $Paths.DiskpartScript,
            $Paths.DetachScript)) {
        if (Test-Path -LiteralPath $temporaryPath) {
            throw "Stage 5 cleanup left a task-owned temporary path behind: $temporaryPath"
        }
    }
    Write-Host 'Stage 5 validation scratch VHDX detached and removed.'
}

$validationToken = Get-Stage5SafeValidationToken $Token
if ($SelfTest) {
    $selfTestDriveLetter = $null
    foreach ($candidateDriveLetter in @(
        'Z', 'Y', 'X', 'W', 'V', 'U', 'T', 'S', 'R', 'Q', 'P', 'O', 'N',
        'M', 'L', 'K', 'J', 'I', 'G', 'F', 'E', 'D', 'C')) {
        if ($null -ne (Get-PSDrive -Name $candidateDriveLetter `
                -ErrorAction SilentlyContinue)) {
            continue
        }
        if (Test-Path -LiteralPath ($candidateDriveLetter + ':\')) {
            continue
        }
        $selfTestDriveLetter = $candidateDriveLetter
        break
    }
    if ([string]::IsNullOrWhiteSpace($selfTestDriveLetter)) {
        throw 'Stage 5 validation-volume path planning self-test requires an unmapped drive letter.'
    }
    $selfTestDriveRoot = $selfTestDriveLetter + ':\'
    $legacyPlanningFailed = $false
    try {
        [void](Join-Path ([IO.Path]::Combine($selfTestDriveRoot, 'Stage5CiScratch')) `
            'path-plan-selftest')
    }
    catch {
        $legacyPlanningFailed = $true
    }
    if (-not $legacyPlanningFailed) {
        throw 'Stage 5 validation-volume path planning self-test did not reproduce the provider failure.'
    }
    $plannedPaths = Get-Stage5ValidationVolumePaths `
        -ValidationToken 'path-plan-selftest' `
        -DriveRoot $selfTestDriveRoot `
        -RunnerTemp ([IO.Path]::GetTempPath())
    $expectedScratchRoot = [IO.Path]::Combine($selfTestDriveRoot,
        'Stage5CiScratch', 'path-plan-selftest')
    $expectedMarkerPath = [IO.Path]::Combine($expectedScratchRoot,
        '.stage5-vhd-owner')
    if ([string]$plannedPaths.ScratchRoot -cne $expectedScratchRoot -or
        [string]$plannedPaths.MarkerPath -cne $expectedMarkerPath) {
        throw 'Stage 5 validation-volume path planning did not preserve an unmapped drive root.'
    }
    Write-Host "Stage 5 validation-volume path planning self-test passed for unmapped $selfTestDriveRoot."
    exit 0
}
if ($Mode -ceq 'Cleanup' -and
    $env:RTS_STAGE5_VALIDATION_VHD_OWNED -cne 'true') {
    Write-Host 'No task-owned Stage 5 validation VHDX was published; nothing to clean.'
    exit 0
}
$validationPaths = Get-Stage5ValidationVolumePaths $validationToken
if ($Mode -ceq 'Provision') {
    Invoke-Stage5ValidationVolumeProvision $validationToken $validationPaths
}
else {
    Invoke-Stage5ValidationVolumeCleanup $validationToken $validationPaths
}
