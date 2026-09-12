[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ScratchRoot)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
function Check([bool]$value, [string]$message) { if (-not $value) { throw $message } }
function Reject([scriptblock]$action, [string]$message) {
    $caught = $false
    try { & $action | Out-Null } catch { $caught = $true; $script:LastBaseBindingRejection = $_.Exception.Message }
    Check $caught $message
}
$root = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('base-binding-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$base = Join-Path $root 'Generals'
$zh = Join-Path $root 'ZeroHour'
New-Item -ItemType Directory -Path $base, $zh | Out-Null
foreach ($name in @('English.big', 'INI.big', 'Maps.big', 'W3D.big')) {
    [IO.File]::WriteAllText((Join-Path $base $name), "synthetic-$name")
}
Import-Module (Join-Path $PSScriptRoot 'Stage5BaseGeneralsBinding.psm1') -Force
$baseModule = Get-Module Stage5BaseGeneralsBinding
Reject { & $baseModule { Resolve-Stage5BaseRegularPath 'H:\' Container } } 'A volume root became a drive-relative installation path.'
Check ($script:LastBaseBindingRejection -match 'root') 'Volume-root rejection did not reach the root boundary.'
Reject { Get-Stage5BaseGeneralsBinding -RuntimeRoot $zh -GeneralsInstallRoot '' } 'Missing base root was accepted.'
Reject { Get-Stage5BaseGeneralsBinding -RuntimeRoot $zh -GeneralsInstallRoot $zh } 'ZH fallback root was accepted.'
$observed = Get-Stage5BaseGeneralsBinding -RuntimeRoot $zh -GeneralsInstallRoot $base
Check ($observed.runtimeRoot -ceq $base -and $observed.files.Count -eq 4 -and
    $observed.identityMode -ceq 'diagnostic') 'Diagnostic binding ignored the supplied complete base root.'
Check (-not $observed.Contains('artifactSetSha256')) 'Diagnostic observation fabricated acceptance bindings.'
Check ($observed.files[0].sha256 -ceq (Get-FileHash (Join-Path $base 'English.big')).Hash) 'Observed base hash is incorrect.'
Reject { Get-Stage5BaseGeneralsBinding -RuntimeRoot $zh -GeneralsInstallRoot $base -AcceptanceSourceCommit ('a' * 40) } 'Partial acceptance binding was accepted.'
Check (Test-Stage5RegistryScopeInactive -ExecutablePaths @('H:\Candidate\generalszh.exe') -ProcessProvider { param($names) @() }) 'Empty process fixture was not inactive.'
Check (-not (Test-Stage5RegistryScopeInactive -ExecutablePaths @('H:\Candidate\generalszh.exe') -ProcessProvider {
    param($names) @([pscustomobject]@{ ProcessName='generals'; Path='H:\Foreign\generals.exe' })
})) 'Foreign Generals process was ignored by a paired scope.'
Check (-not (Test-Stage5RegistryScopeInactive -ExecutablePaths @('H:\Candidate\generalszh.exe') -ProcessProvider { param($names) throw 'unreadable process identity' })) 'Unreadable activity failed open.'
$layoutRoot = Join-Path $root 'layout'
New-Item -ItemType Directory -Path $layoutRoot | Out-Null
$artifactPath = Join-Path $layoutRoot 'ArtifactSet.json'
$manifestPath = Join-Path $layoutRoot 'Acceptance.json'
$artifact = @{ sourceCommit = ('a' * 40); artifacts = @(
    @{ role='generals-executable'; path='Generals/generals.exe' },
    @{ role='zerohour-executable'; path='ZeroHour/generalszh.exe' }
) }
[IO.File]::WriteAllText($artifactPath, ($artifact | ConvertTo-Json -Depth 8))
[IO.File]::WriteAllText($manifestPath, (@{ sourceCommit=('a' * 40); artifactSet=@{
    path='ArtifactSet.json'; sha256=(Get-FileHash $artifactPath).Hash
} } | ConvertTo-Json -Depth 8))
$layout = Get-Stage5ReplayRuntimeLayout -SourceRoot $layoutRoot -AcceptanceManifestPath $manifestPath -SourceCommit ('a' * 40) -Title ZeroHour
Check ($layout.runtimeRoot -ceq (Join-Path $layoutRoot 'ZeroHour') -and
    $layout.generalsRuntimeRoot -ceq (Join-Path $layoutRoot 'Generals')) 'Workflow did not route both downloads to the artifact-role directories.'
$originalArtifactText = [IO.File]::ReadAllText($artifactPath)
$replacementArtifactText = $originalArtifactText.Replace('Generals/generals.exe', 'OtherGenerals/generals.exe')
# Replace the file after its bytes/digest have been acquired, before parsing.
# Both wrappers call the real readers; assertions concern the resulting paths.
& $baseModule {
    param($path, $replacement)
    $script:swapPath = $path; $script:swapReplacement = $replacement
    $script:snapshotOwner = Get-Module DeterministicSimulationEvidence
    function script:Get-FileHash {
        param([string]$LiteralPath, [string]$Algorithm = 'SHA256')
        $result = Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
        if ($LiteralPath -ceq $script:swapPath) { [IO.File]::WriteAllText($LiteralPath, $script:swapReplacement) }
        return $result
    }
    function script:Get-Stage5FinalAcceptanceFileSnapshot {
        param([string]$Path, [string]$Context, [switch]$HashOnly, [string]$EvidenceKind = 'JsonReceipt')
        $result = & $script:snapshotOwner {
            param($path, $context, $hashOnly, $evidenceKind)
            Get-Stage5FinalAcceptanceFileSnapshot -Path $path -Context $context -HashOnly:$hashOnly -EvidenceKind $evidenceKind
        } $Path $Context ([bool]$HashOnly) $EvidenceKind
        if ($Path -ceq $script:swapPath) { [IO.File]::WriteAllText($Path, $script:swapReplacement) }
        return $result
    }
} $artifactPath $replacementArtifactText
$racedLayout = Get-Stage5ReplayRuntimeLayout -SourceRoot $layoutRoot -AcceptanceManifestPath $manifestPath -SourceCommit ('a' * 40) -Title ZeroHour
Check ([IO.File]::ReadAllText($artifactPath) -match 'OtherGenerals') 'Replacement fixture did not reach the acquisition/parse boundary.'
Check ($racedLayout.generalsRuntimeRoot -ceq (Join-Path $layoutRoot 'Generals')) 'Layout parsed replacement bytes that were not covered by the artifact-set hash.'
[IO.File]::WriteAllText($artifactPath, $originalArtifactText)
Import-Module (Join-Path $PSScriptRoot 'Stage5BaseGeneralsBinding.psm1') -Force
function TextHash([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','') }
    finally { $sha.Dispose() }
}
function JsonFile([string]$path, [object]$value) { [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 20)) }
$dependencies = @(); $roles = @()
foreach ($title in @('Generals','ZeroHour')) {
    $folder = Join-Path $root $title
    $prefix = if ($title -ceq 'Generals') { 'generals' } else { 'zerohour' }
    foreach ($entry in @(@('executable', $(if ($title -ceq 'Generals') {'generalsv.exe'} else {'generalszh.exe'})),
        @('launcher','launcher.exe'), @('launcher-config','launcher.lcf'), @('dll','support.dll'), @('asset','native.hlsl'))) {
        $path = Join-Path $folder $entry[1]
        [IO.File]::WriteAllText($path, "synthetic-$title-$($entry[0])")
        $relative = "$title/$($entry[1])"; $hash = (Get-FileHash $path).Hash
        $dependencies += @{ title=$title; kind=$entry[0]; path=$relative; sha256=$hash }
        if (@('executable','launcher','launcher-config') -contains $entry[0]) {
            $roles += @{role="$prefix-$($entry[0])"; path=$relative; sha256=$hash}
        }
    }
}
$dependencyPath = Join-Path $root 'RuntimeDependencies.json'
JsonFile $dependencyPath @{schemaVersion=1;sourceCommit=('a'*40);architecture='x64';productSet=@('Generals','ZeroHour');files=$dependencies}
$lines = [string[]]@($dependencies | ForEach-Object { "$($_.title)|$($_.kind)|$($_.path)|$($_.sha256)" })
[Array]::Sort($lines,[StringComparer]::Ordinal)
$closure = TextHash (($lines -join "`n") + "`n")
$artifactPath = Join-Path $root 'ArtifactSet.json'
JsonFile $artifactPath @{schemaVersion=1;sourceCommit=('a'*40);architecture='x64';productSet=@('Generals','ZeroHour');artifacts=$roles;
    runtimeClosure=@{dependencyManifest=@{path='RuntimeDependencies.json';sha256=(Get-FileHash $dependencyPath).Hash};closureSha256=$closure}}
New-Item -ItemType Directory -Path (Join-Path $base 'Data/Scripts') -Force | Out-Null
foreach ($name in @('MultiplayerScripts.scb','SkirmishScripts.scb')) { [IO.File]::WriteAllText((Join-Path $base "Data/Scripts/$name"),$name) }
$dataFiles = @(@('Data/Scripts/MultiplayerScripts.scb','Data/Scripts/SkirmishScripts.scb','English.big','INI.big','Maps.big','W3D.big') | ForEach-Object {
    @{path=$_;sha256=(Get-FileHash (Join-Path $base $_)).Hash}
})
$dataClosure = TextHash ((@($dataFiles | ForEach-Object { "$($_.path)|$($_.sha256)" }) -join "`n") + "`n")
$qualificationPath = Join-Path $root 'QualificationData.json'
JsonFile $qualificationPath @{schemaVersion=1;evidenceKind='stage5-simulation-qualification-data';producer='genci-r2-trimmed-data';sourceCommit=('a'*40);
    title='Generals';archiveSource=@{object='s3://github-ci/generals108_gamedata_trimmed.7z';sha256='37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'};
    files=$dataFiles;closureSha256=$dataClosure}
$acceptanceArguments = @{RuntimeRoot=$zh;GeneralsInstallRoot=$base;AcceptanceSourceCommit=('a'*40);AcceptanceArtifactSetPath=$artifactPath;
    AcceptanceArtifactSetSha256=(Get-FileHash $artifactPath).Hash;AcceptanceRuntimeDependencyManifestSha256=(Get-FileHash $dependencyPath).Hash;
    AcceptanceRuntimeClosureSha256=$closure;GeneralsQualificationDataManifestPath=$qualificationPath;GeneralsQualificationDataManifestSha256=(Get-FileHash $qualificationPath).Hash}
$accepted = Get-Stage5BaseGeneralsBinding @acceptanceArguments
Check ($accepted.identityMode -ceq 'acceptance-bound' -and $accepted.files.Count -eq 6) 'Complete paired closure was not validated.'
Assert-Stage5BaseGeneralsBindingCurrent $accepted
$foreign = Join-Path $root 'Foreign'
Copy-Item -LiteralPath $base -Destination $foreign -Recurse
$acceptanceArguments.GeneralsInstallRoot = $foreign
Reject { Get-Stage5BaseGeneralsBinding @acceptanceArguments } 'Byte-identical foreign directory satisfied artifact-role binding.'
Check ($script:LastBaseBindingRejection -match 'artifact-role directory') 'Foreign-root test did not reach the path identity boundary.'
$acceptanceArguments.GeneralsInstallRoot = $base
[IO.File]::WriteAllText((Join-Path $base 'Foreign.big'),'undeclared')
Reject { Assert-Stage5BaseGeneralsBindingCurrent $accepted } 'Acceptance permitted an undeclared base archive.'
Write-Output 'Stage 5 base Generals binding tests passed.'
