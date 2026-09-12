param([Parameter(Mandatory=$true)][string]$ScratchRoot)
$ErrorActionPreference='Stop';Set-StrictMode -Version 2.0
[IO.Directory]::CreateDirectory($ScratchRoot)|Out-Null
$env:TEMP=Join-Path $ScratchRoot 'Temp';$env:TMP=$env:TEMP
[IO.Directory]::CreateDirectory($env:TEMP)|Out-Null
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
function Check([bool]$value,[string]$message){if(-not$value){throw $message}}
function Reject([scriptblock]$action,[string]$pattern){$caught=$null;try{&$action|Out-Null}catch{$caught=$_};Check ($null-ne$caught-and$caught.Exception.Message-match$pattern) "Expected rejection /$pattern/: $caught"}
Check ($null-ne(Get-Command Read-Stage5ReviewedAiMap -ErrorAction SilentlyContinue)) 'Reviewed AI map reader is missing.'
$source=Join-Path $ScratchRoot 'fixture.map'
$bytes=New-Object byte[] 16384
[IO.File]::WriteAllBytes($source,$bytes)
$sha=(Get-FileHash $source -Algorithm SHA256).Hash
$map=[ordered]@{source='fixture.map';mapKey='Maps\AiProof\AiProof.map';sha256=$sha;byteCount=16384;crc='00000000'}
$bound=Read-Stage5ReviewedAiMap -Map $map -ManifestDirectory $ScratchRoot
Check ($bound.binding.sha256-ceq$sha-and$bound.binding.byteCount-eq16384-and$bound.binding.crc-ceq'00000000') 'Map byte identity was not preserved.'
# Independently derived vector: CkMp gives0x4CE, then16380zero bytes rotate
# left28bits, yielding E000004C. This exercises the unsigned high-bit wrap.
$highBytes=New-Object byte[] 16384
[Array]::Copy([Text.Encoding]::ASCII.GetBytes('CkMp'),$highBytes,4)
$highPath=Join-Path $ScratchRoot 'high-crc.map';[IO.File]::WriteAllBytes($highPath,$highBytes)
$highMap=[ordered]@{source='high-crc.map';mapKey='Maps\HighCrc\HighCrc.map';sha256=(Get-FileHash $highPath -Algorithm SHA256).Hash;byteCount=16384;crc='E000004C'}
Read-Stage5ReviewedAiMap -Map $highMap -ManifestDirectory $ScratchRoot|Out-Null
$target=Join-Path $ScratchRoot 'junction-target';[IO.Directory]::CreateDirectory($target)|Out-Null
[IO.File]::WriteAllBytes((Join-Path $target 'fixture.map'),$bytes)
$link=Join-Path $ScratchRoot 'junction'
New-Item -ItemType Junction -Path $link -Target $target|Out-Null
$reparsed=@{};foreach($k in $map.Keys){$reparsed[$k]=$map[$k]};$reparsed.source='junction/fixture.map'
Reject {Read-Stage5ReviewedAiMap -Map $reparsed -ManifestDirectory $ScratchRoot} 'reparse'
Reject {Copy-Stage5ReviewedAiMapSnapshot -ReviewedMap $bound -DestinationRoot $link} 'reparse'
foreach($edit in @(@{byteCount='16384'},@{byteCount=1},@{crc='FFFFFFFF'},@{sha256=('FF'*32)},@{source='../fixture.map'},@{source='fixture.map:stream'},@{mapKey='Maps\..\evil.map'},@{mapKey='Maps\AiProof\different.map'},@{mapKey='Maps\Twilight Flame\Twilight Flame.map'})){
    $bad=@{};foreach($k in $map.Keys){$bad[$k]=$map[$k]};foreach($k in $edit.Keys){$bad[$k]=$edit[$k]}
    Reject {Read-Stage5ReviewedAiMap -Map $bad -ManifestDirectory $ScratchRoot} 'map|path|hash|SHA|integer|CRC|size|default'
}
Reject {Assert-Stage5ReviewedAiMapNoCollisions -ReviewedMap $bound -ReplayFixtures @([pscustomobject]@{maps=@([pscustomobject]@{relative='maps\aiproof\AIPROOF.map'})})} 'collision|shadow'
Reject {Assert-Stage5ReviewedAiMapNoCollisions -ReviewedMap $bound -ReplayFixtures @([pscustomobject]@{maps=@([pscustomobject]@{relative='Maps\Twilight Flame\Twilight Flame.map'})})} 'collision|shadow'
$profile=Join-Path $ScratchRoot 'profile';[IO.Directory]::CreateDirectory($profile)|Out-Null
# Mutation after validated read must not alter the bytes used for staging.
[IO.File]::WriteAllText($source,'changed after snapshot')
$staged=Copy-Stage5ReviewedAiMapSnapshot -ReviewedMap $bound -DestinationRoot $profile
Check ((Get-FileHash $staged -Algorithm SHA256).Hash-ceq$sha) 'Staging reopened a changed source instead of snapshot bytes.'
Reject {Copy-Stage5ReviewedAiMapSnapshot -ReviewedMap $bound -DestinationRoot ($profile+':ads')} 'path|sink|directory|ADS'
Reject {Copy-Stage5ReviewedAiMapSnapshot -ReviewedMap $bound -DestinationRoot $profile} 'exist|overwrite'
Write-Output 'PASS: reviewed AI map snapshot, scalar identity and collision isolation'
$tokens=$null;$errors=$null
$runner=Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$errors)
foreach($fn in $ast.FindAll({param($a)$a -is [Management.Automation.Language.FunctionDefinitionAst]},$false)){
    . ([scriptblock]::Create($fn.Extent.Text))
}
[IO.File]::WriteAllBytes($source,$bytes)
$manifest=[ordered]@{schemaVersion=1;title='ZeroHour';executable='generalszh.exe';executableSha256=('AA'*32);fixtures=@();ai=[ordered]@{seeds=@(1729);scenarios=@('4v3','4v2');repeats=2;reviewedMap=$map}}
$manifestPath=Join-Path $ScratchRoot 'manifest.json'
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 8))
$data=Get-ManifestData $manifestPath 'AI' $true '' 'ZeroHour' $true
$plan=@(New-ValidationPlan $data 'AI' 1 1 30 30 'H:\installed\generalszh.exe' $ScratchRoot -CapacityMode LocalCapacity)
$boundEntries=@($plan|Where-Object scenario -CEQ '4v2')
Check ($boundEntries.Count-eq13) 'Reviewed map did not cover12worker/repeat entries plus shadow.'
foreach($entry in $boundEntries){
    Check ($entry.reviewedMap.sha256-ceq$sha-and$entry.reviewedMap.mapKey-ceq$map.mapKey) 'Plan map binding differs from snapshot.'
    $index=[Array]::IndexOf($entry.arguments,'-skirmishAITestReviewedMap')
    Check ($index-ge0-and$entry.arguments[$index+1]-ceq$map.mapKey-and$entry.arguments[$index+2]-ceq$sha-and
        $entry.arguments[$index+3]-ceq'16384'-and$entry.arguments[$index+4]-ceq'00000000') 'Typed native map arguments are missing or wrong.'
}
foreach($entry in @($plan|Where-Object scenario -CEQ '4v3')){
    Check (-not($entry.arguments-contains'-skirmishAITestReviewedMap')-and-not($entry.PSObject.Properties.Name-contains'reviewedMap')) 'Default4v3 map behavior changed.'
}
Write-Output 'PASS: manifest parsing and full4v2 worker/repeat/shadow map propagation'
$entry=$boundEntries[0]
$fields=@{map=$map.mapKey;map_sha256=$sha;map_crc='00000000';map_size='16384'}
Assert-Stage5ReviewedAiMapCompletion -Fields $fields -Entry $entry
foreach($field in @('map','map_sha256','map_crc','map_size')){
    $bad=@{}+$fields;$bad[$field]='wrong'
    Reject {Assert-Stage5ReviewedAiMapCompletion -Fields $bad -Entry $entry} 'map|identity|size|numeric|integer'
}
$changed=$entry|ConvertTo-Json -Depth 8|ConvertFrom-Json
$changed.reviewedMap.sha256='FF'*32
Reject {Assert-Stage5ReviewedAiMapEntryIdentity -Entry $changed -FrozenEntry $entry} 'map|binding|plan'
Write-Output 'PASS: reviewed map completion and frozen plan identity'
$schemaCopy=$manifest|ConvertTo-Json -Depth 8|ConvertFrom-Json
$schemaCopy.fixtures=@([pscustomobject]@{id='schema-only';source='schema-only.rep';sha256=('AA'*32);stress=$false})
$schema=Join-Path $PSScriptRoot 'ReplayFixtureManifest.schema.json'
Check (($schemaCopy|ConvertTo-Json -Depth 8)|Test-Json -SchemaFile $schema) 'Typed reviewed map failed manifest schema.'
$schemaCopy.ai.reviewedMap.byteCount='16384'
Check (-not(($schemaCopy|ConvertTo-Json -Depth 8)|Test-Json -SchemaFile $schema -ErrorAction SilentlyContinue)) 'Schema accepted string byteCount.'
$data.schemaVersion=2
$data.ai|Add-Member -NotePropertyName liveQualification -NotePropertyValue ([pscustomobject]@{
    schemaVersion=1;profileSetId='live-all-slices-v1'
    authorityEntries=@([pscustomobject]@{scenario='4v2';seed=1729;configuration='parallel-4';repeat=1})
    shadowEntry=[pscustomobject]@{scenario='4v2';seed=1729;configuration='shadow-8';repeat=1}
})
$v2=@(New-ValidationPlan $data 'AI' 1 1 30 30 'H:\installed\generalszh.exe' $ScratchRoot -CapacityMode LocalCapacity)
$v2Plan=[pscustomobject]@{schemaVersion=2;liveQualification=$data.ai.liveQualification;entries=$v2}
$authority=@($v2|Where-Object validationRole -CEQ 'live-authority-stress')[0]
Resolve-Stage5LiveValidationRequirements $v2Plan $authority|Out-Null
$tampered=$authority|ConvertTo-Json -Depth 8|ConvertFrom-Json
$tampered.reviewedMap.sha256='FF'*32
Reject {Resolve-Stage5LiveValidationRequirements $v2Plan $tampered} 'map.*binding|plan'
Write-Output 'PASS: JSON schema and V2 authority identity preserve reviewed map binding'
