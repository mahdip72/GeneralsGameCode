param([string]$SourceRoot, [string]$OutputRoot, [switch]$SelfCheck)
$ErrorActionPreference='Stop'
$utf8=New-Object System.Text.UTF8Encoding($false)
$receipts=New-Object 'System.Collections.Generic.List[object]'
function Hash-Text([string]$Text) {
    $hash=[System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($utf8.GetBytes($Text)))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Get-UniqueMatch([string]$Text,[string]$Pattern,[string]$Path) {
    $matches=[regex]::Matches($Text,$Pattern,[System.Text.RegularExpressions.RegexOptions]::Multiline)
    if($matches.Count -ne 1){throw "Expected unique match $Pattern in $Path, got $($matches.Count)"}
    return $matches[0]
}
function Assert-OrderedMatchesInBlock([int]$FirstStart,[int]$FirstEnd,
    [int]$SecondStart,[int]$SecondEnd,[int]$BlockStart,[int]$BlockEnd,
    [string]$Path) {
    if($FirstStart -lt $BlockStart -or $FirstEnd -gt $BlockEnd -or
        $SecondStart -lt $BlockStart -or $SecondEnd -gt $BlockEnd -or
        $FirstStart -ge $SecondStart) {
        throw "Expected refresh before visible Begin in the same display loop in $Path"
    }
}
# Balanced literal/comment scanner retained from the qualified line-group
# extractor. The extracted methods are unmodified production text.
function Extract-Method([string]$Text,[string]$Pattern,[string]$Path) {
    $matches=[regex]::Matches($Text,$Pattern,[System.Text.RegularExpressions.RegexOptions]::Multiline)
    if($matches.Count -ne 1){throw "Expected unique method $Pattern in $Path, got $($matches.Count)"}
    $start=$matches[0].Index
    $depth=0;$mode='code';$end=-1;$seenOpen=$false
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
    $receipts.Add([pscustomobject]@{path=$Path;pattern=$Pattern;
        first_line=1+($Text.Substring(0,$start).Split("`n").Length-1);
        offset=$start;end_offset=$end;sha256=(Hash-Text $body)})
    return $body
}
function Extract-Unique([string]$Text,[string]$Pattern,[string]$Path) {
    $match=Get-UniqueMatch $Text $Pattern $Path
    $body=$match.Value
    $receipts.Add([pscustomobject]@{path=$Path;pattern=$Pattern;
        first_line=1+($Text.Substring(0,$match.Index).Split("`n").Length-1);
        offset=$match.Index;length=$match.Length;sha256=(Hash-Text $body)})
    return $body
}
function Test-Extractor {
    $sample=@'
void Sample()
{
    const char *text = "escaped quote \" and brace } plus backslash \\";
    const char quote = '\'';
    /* } { */ // {
    const char *opening = "{";
}
void Next(){}
'@
    $sample=$sample.Replace("`r`n","`n")
    $actual=Extract-Method $sample '^void Sample\(' '<self-check>'
    if($actual -cne $sample.Substring(0,$sample.IndexOf('void Next()'))){throw 'Literal/comment self-check mismatch'}
    $rejected=$false
    try { $null=Extract-Method 'void Broken() { const char *x="escaped \" }";' '^void Broken\(' '<self-check>' }
    catch { $rejected=$true }
    if(!$rejected){throw 'Extractor accepted a truncated escaped-literal method'}

    $orderedSample=@'
void draw()
{
    do {
        bool renderTargetTexturesReady = true;
        if (refresh()) renderTargetTexturesReady = true;
        if (renderTargetTexturesReady && Begin_Render()) return;
    } while(false);
}
'@
    $orderedSample=$orderedSample.Replace("`r`n","`n")
    $loop=Extract-Method $orderedSample '^\s*do\s*\{\s*bool renderTargetTexturesReady = true;' '<self-check loop>'
    $loopBounds=$receipts[$receipts.Count-1];$receipts.RemoveAt($receipts.Count-1)
    $refresh=Get-UniqueMatch $orderedSample 'refresh\(\)' '<self-check refresh>'
    $begin=Get-UniqueMatch $orderedSample 'renderTargetTexturesReady && Begin_Render\(\)' '<self-check Begin>'
    $loopStart=$loopBounds.offset
    $loopEnd=$loopBounds.end_offset
    Assert-OrderedMatchesInBlock $refresh.Index ($refresh.Index+$refresh.Length) `
        $begin.Index ($begin.Index+$begin.Length) $loopStart $loopEnd '<self-check>'
    $rejectedOrder=$false
    try {
        Assert-OrderedMatchesInBlock $begin.Index ($begin.Index+$begin.Length) `
            $refresh.Index ($refresh.Index+$refresh.Length) $loopStart $loopEnd '<self-check reordered>'
    }
    catch { $rejectedOrder=$true }
    if(!$rejectedOrder){throw 'Extractor order self-check accepted Begin before refresh'}
    $receipts.Clear()
}
Test-Extractor
if($SelfCheck){Write-Output 'PASS extractor literal/comment, truncation, and display-order self-check';return}
if(!$SourceRoot -or !$OutputRoot){throw 'SourceRoot and OutputRoot are required'}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$waterPath='Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp'
$water=([System.IO.File]::ReadAllText((Join-Path $SourceRoot $waterPath))).Replace("`r`n","`n")
$body=(Extract-Method $water '^bool WaterRenderObjClass::updateRenderTargetTextures\(' $waterPath)+
    (Extract-Method $water '^bool WaterRenderObjClass::renderMirror\(' $waterPath)
# The exact drawSea admission prefix stops BEFORE state/shader/geometry work.
# The final marker/body close are fixture-only, not a claimed complete draw.
$sea=Extract-Method $water '^void WaterRenderObjClass::drawSea\(RenderInfoClass & rinfo\)' $waterPath
$seaReceipt=$receipts[$receipts.Count-1]
$boundary=[regex]::Matches($sea,'\n\tLegacyLogicalState previousState;')
if($boundary.Count -ne 1){throw 'Ambiguous/missing drawSea admission boundary'}
$prefix=$sea.Substring(0,$boundary[0].Index)
$receipts.RemoveAt($receipts.Count-1)
$receipts.Add([pscustomobject]@{path=$waterPath;pattern='drawSea admission prefix before LegacyLogicalState previousState';
    first_line=$seaReceipt.first_line;sha256=(Hash-Text $prefix)})
$headerPath='Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DWater.h'
$header=([System.IO.File]::ReadAllText((Join-Path $SourceRoot $headerPath))).Replace("`r`n","`n")
$bumpConstant=Extract-Unique $header '^#define NUM_BUMP_FRAMES\s+[0-9]+[^\n]*' $headerPath
$body=$bumpConstant+"`n"+$body+$prefix+"`n ++control.seaReachedSampling; // Fixture admission marker, not an actual draw.`n}`n"
$titleWrappers=@{}
foreach($title in @('Generals','GeneralsMD')) {
    $path="$title/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    $text=([System.IO.File]::ReadAllText((Join-Path $SourceRoot $path))).Replace("`r`n","`n")
    $loop=Extract-Method $text '^\s*do\s*\{\s*bool renderTargetTexturesReady = true;' "$path display loop"
    $loopBounds=$receipts[$receipts.Count-1];$receipts.RemoveAt($receipts.Count-1)
    $refreshPattern='^\s*if \(TheWaterRenderObj && TheGlobalData->m_waterType == 2\)\n\s*renderTargetTexturesReady = TheWaterRenderObj->updateRenderTargetTextures\(primaryW3DView->get3DCamera\(\)\);'
    $admitPattern='^\s*if \(renderTargetTexturesReady &&[^\n]+WW3D::Begin_Render\([^\n]+\) == WW3D_ERROR_OK\)'
    $refreshMatch=Get-UniqueMatch $text $refreshPattern $path
    $admitMatch=Get-UniqueMatch $text $admitPattern $path
    $loopStart=$loopBounds.offset
    $loopEnd=$loopBounds.end_offset
    Assert-OrderedMatchesInBlock $refreshMatch.Index `
        ($refreshMatch.Index+$refreshMatch.Length) $admitMatch.Index `
        ($admitMatch.Index+$admitMatch.Length) $loopStart $loopEnd $path
    $refresh=Extract-Unique $text $refreshPattern $path
    $admit=Extract-Unique $text $admitPattern $path
    # Execute exact conditions/statements, not the thousands-line display loop.
    # Carrier locals/body are fixture-only; no condition is paraphrased.
    $wrapper="bool DisplayAdmission$title()`n{`n bool renderTargetTexturesReady = true;`n"+
        $refresh+"`n"+$admit+"`n return true;`n return false;`n}`n"
    $titleWrappers[$title]=$wrapper
}
if($receipts.Count -ne 8){throw "Unexpected fragment count $($receipts.Count)"}
[System.IO.File]::WriteAllText((Join-Path $OutputRoot 'water.inc'),$body,$utf8)
foreach($title in @('Generals','GeneralsMD')) {
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot "$title-display.inc"),$titleWrappers[$title],$utf8)
}
[System.IO.File]::WriteAllText((Join-Path $OutputRoot 'extraction-receipt.json'),
    ([ordered]@{schema='native-sorted-texture-pass-v2';source_root=$SourceRoot;
        fragments=$receipts.ToArray()}|ConvertTo-Json -Depth 6),$utf8)
Write-Output "Extracted $($receipts.Count) actual source fragments"
