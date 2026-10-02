param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][ValidateSet('Generals','GeneralsMD')][string]$Title)
$ErrorActionPreference='Stop'
$utf8=New-Object System.Text.UTF8Encoding($false)
$receipts=New-Object 'System.Collections.Generic.List[object]'
function Hash-Text([string]$Text) {
    $hash=[System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($utf8.GetBytes($Text)))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Read-Source([string]$Path) {
    return ([System.IO.File]::ReadAllText((Join-Path $SourceRoot $Path))).Replace("`r`n","`n")
}
function Add-Receipt([string]$Text,[int]$Start,[string]$Body,[string]$Pattern,[string]$Path) {
    $receipts.Add([pscustomobject]@{title=$Title;path=$Path;pattern=$Pattern;
        first_line=1+($Text.Substring(0,$Start).Split("`n").Length-1);sha256=(Hash-Text $Body)})
}
function Extract-Method([string]$Text,[string]$Pattern,[string]$Path) {
    $matches=[regex]::Matches($Text,$Pattern,[System.Text.RegularExpressions.RegexOptions]::Multiline)
    if($matches.Count -ne 1){throw "Expected unique method $Pattern in $Path, got $($matches.Count)"}
    $start=$matches[0].Index
    $depth=0;$mode='code';$end=-1;$seenOpen=$false
    # Find the complete balanced method, ignoring braces inside comments/literals.
    for($i=$start;$i -lt $Text.Length;$i++) {
        $c=$Text[$i];$next=if($i+1 -lt $Text.Length){$Text[$i+1]}else{[char]0}
        if($mode -eq 'line'){if($c -eq "`n"){$mode='code'};continue}
        if($mode -eq 'block'){if($c -eq '*' -and $next -eq '/'){$mode='code';$i++};continue}
        if($mode -eq 'string' -or $mode -eq 'char') {
            if($c -eq '\'){$i++;continue}
            if(($mode -eq 'string' -and $c -eq '"') -or ($mode -eq 'char' -and $c -eq "'")){$mode='code'}
            continue
        }
        if($c -eq '/' -and $next -eq '/'){$mode='line';$i++;continue}
        if($c -eq '/' -and $next -eq '*'){$mode='block';$i++;continue}
        if($c -eq '"'){$mode='string';continue}
        if($c -eq "'"){$mode='char';continue}
        if($c -eq '{'){$depth++;$seenOpen=$true}
        if($c -eq '}') {
            if(!$seenOpen){throw "Unexpected closing brace in $Pattern"}
            $depth--;if($depth -eq 0){$end=$i+1;break}
        }
    }
    if($end -lt 0){throw "Unbalanced or truncated method $Pattern in $Path"}
    $body=$Text.Substring($start,$end-$start)+"`n"
    Add-Receipt $Text $start $body $Pattern $Path
    return $body
}
function Put-Generated([string]$Name,[string]$Text) {
    # Only the fixed generated include/receipt names below are overwritten.
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot $Name),$Text,$utf8)
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$contractPath='Core/Libraries/Include/Renderer/RenderGameClient.h'
$contract=Read-Source $contractPath
$enums=''
foreach($name in @('GameRenderTransformSlot','GameRenderState','GameTextureStageState','GameTextureCoordinateGeneration','GameTextureTransformFlags')) {
    $enums+=(Extract-Method $contract "^enum $name`$" $contractPath)+";`n"
}
$commandPath='Core/Libraries/Include/Renderer/RenderGameClientNative.h'
$command=Read-Source $commandPath
$enums+=(Extract-Method $command '^enum GameRenderCommandType$' $commandPath)+";`n"
Put-Generated 'contracts.inc' ("namespace rts { namespace render {`n"+$enums+"} }`n")
$meshPath='Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp'
$mesh=Read-Source $meshPath
Put-Generated 'category.inc' (Extract-Method $mesh '^void DX8TextureCategoryClass::Render\(\)' $meshPath)
$titlePath="$Title/Code/Libraries/Source/WWVegas/WW3D2/RenderGameClientNativeTitle.cpp"
$facade=Read-Source $titlePath
$helpers=(Extract-Method $facade '^bool IsOperationalOwner\(' $titlePath)+
    (Extract-Method $facade '^RenderResult SubmitCommand\(' $titlePath)
$material=Extract-Method $facade '^void SetGameMaterial\(' $titlePath
Put-Generated 'material-facade.inc' ("namespace rts { namespace render {`n"+$helpers+$material+"} }`n")
$mapperPath="$Title/Code/Libraries/Source/WWVegas/WW3D2/mapper.cpp"
$mapper=Read-Source $mapperPath
$matrixType=if($Title -eq 'Generals'){'Matrix3D'}else{'Matrix4x4'}
$methods=(Extract-Method $mapper '^static rts::render::GameRenderTransformSlot MapperTextureTransformSlot\(' $mapperPath)+
    (Extract-Method $mapper "^static void SetMapperTextureTransform\(unsigned int stage, const $matrixType &" $mapperPath)+
    (Extract-Method $mapper '^static void SetMapperTextureCoordinatePassthrough\(' $mapperPath)+
    (Extract-Method $mapper '^static void SetMapperTextureTransformCount2\(' $mapperPath)+
    (Extract-Method $mapper '^void ScaleTextureMapperClass::Apply\(' $mapperPath)
$linearMethod=if($Title -eq 'Generals'){'Apply'}else{'Calculate_Texture_Matrix'}
$methods+=Extract-Method $mapper "^void LinearOffsetTextureMapperClass::$linearMethod\(" $mapperPath
Put-Generated 'mapper.inc' $methods
$vertexPath="$Title/Code/Libraries/Source/WWVegas/WW3D2/vertmaterial.cpp"
$vertex=Read-Source $vertexPath
$methods=(Extract-Method $vertex '^void VertexMaterialClass::Get_Diffuse\(' $vertexPath)+
    (Extract-Method $vertex '^void VertexMaterialClass::Set_Diffuse\(float' $vertexPath)+
    (Extract-Method $vertex '^float\s+VertexMaterialClass::Get_Opacity\(' $vertexPath)+
    (Extract-Method $vertex '^void\s+VertexMaterialClass::Set_Opacity\(' $vertexPath)
Put-Generated 'material-values.inc' $methods
$ownerPath='Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp'
$owner=Read-Source $ownerPath
$helpers=(Extract-Method $owner '^bool IsFiniteGameFloat\(' $ownerPath)+
    (Extract-Method $owner '^bool IsFiniteGameColor\(' $ownerPath)+
    (Extract-Method $owner '^bool IsValidGameMaterialSource\(' $ownerPath)
$begins=[regex]::Matches($owner,'^\tcase GAME_RENDER_COMMAND_SET_MATERIAL:$',[System.Text.RegularExpressions.RegexOptions]::Multiline)
$ends=[regex]::Matches($owner,'^\tcase GAME_RENDER_COMMAND_SET_LIGHT:$',[System.Text.RegularExpressions.RegexOptions]::Multiline)
if($begins.Count -ne 1 -or $ends.Count -ne 1 -or $ends[0].Index -le $begins[0].Index){throw 'Ambiguous/truncated owner material case'}
$begin=$begins[0].Index;$end=$ends[0].Index
$body=$owner.Substring($begin,$end-$begin)
Add-Receipt $owner $begin $body 'SET_MATERIAL case through next SET_LIGHT label' $ownerPath
Put-Generated 'owner-material.inc' ($helpers+"namespace rts { namespace render {`nRenderResult IGameRenderClientNativeOwner::ExecuteGameRenderCommand(const GameRenderCommand &command)`n{`n if (refuseCommand) { RecordGameFailure(RENDER_RESULT_FAILED); return RENDER_RESULT_FAILED; }`n ++commands; switch(command.type) {`n"+$body+"default: return RENDER_RESULT_INVALID_ARGUMENT; }`n}`n} }`n")
if($receipts.Count -ne 24){throw "Unexpected extraction coverage: $($receipts.Count) fragments"}
Put-Generated 'extraction-receipt.json' ($receipts|ConvertTo-Json -Depth 5)
Write-Output "Extracted $($receipts.Count) actual source fragments for $Title"
