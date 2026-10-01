param(
    [string]$SourceRoot,
    [string]$OutputRoot,
    [string]$LineGroupSourceRoot,
    [switch]$SelfCheck)
# Optional alternate line-group root is only for local frozen RED qualification.
# Candidate CI uses the current SourceRoot and never requires a baseline Git ref.
if (!$LineGroupSourceRoot) { $LineGroupSourceRoot=$SourceRoot }
$Title='GeneralsMD'
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
function Test-ExtractorSelfCheck {
    $sample=@'
void Sample()
{
    const char *text = "escaped quote \" and brace } plus backslash \\";
    const char quote = '\'';
    const char brace = '}';
    /* ignored } { */
    // ignored {
    const char *opening = "{";
}
void Next(){}
'@
    $sample=$sample.Replace("`r`n","`n")
    $expected=$sample.Substring(0,$sample.IndexOf('void Next()'))
    $actual=Extract-Method $sample '^void Sample\(' '<self-check>'
    if($actual -cne $expected){throw 'Extractor literal/comment self-check mismatch'}
    $rejected=$false
    try { $null=Extract-Method 'void Broken() { const char *x="escaped \" }";' '^void Broken\(' '<self-check>' }
    catch { $rejected=$true }
    if(!$rejected){throw 'Extractor accepted truncated escaped-literal method'}
    $receipts.Clear()
}
Test-ExtractorSelfCheck
if($SelfCheck){Write-Output 'PASS extractor escaped quotes, literal/comment braces, and truncated method rejection';return}
if(!$SourceRoot -or !$OutputRoot){throw 'SourceRoot and OutputRoot are required unless SelfCheck is specified'}
function Put-Generated([string]$Name,[string]$Text) {
    # Only the fixed generated include/receipt names below are overwritten.
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot $Name),$Text,$utf8)
}

function Extract-Case([string]$Text,[string]$First,[string]$Next,[string]$Path) {
    $begins=[regex]::Matches($Text,"^\tcase $First\:$",[System.Text.RegularExpressions.RegexOptions]::Multiline)
    $ends=[regex]::Matches($Text,"^\tcase $Next\:$",[System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($begins.Count -ne 1 -or $ends.Count -ne 1 -or $ends[0].Index -le $begins[0].Index) {
        throw "Ambiguous/truncated owner case $First"
    }
    $body=$Text.Substring($begins[0].Index,$ends[0].Index-$begins[0].Index)
    Add-Receipt $Text $begins[0].Index $body "$First case through $Next" $Path
    return $body
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$linePath='GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/linegrp.cpp'
$line=([System.IO.File]::ReadAllText((Join-Path $LineGroupSourceRoot $linePath))).Replace("`r`n","`n")
$headerPath='GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/linegrp.h'
$header=Read-Source $headerPath
$body=(Extract-Method $header '^class LineGroupClass$' $headerPath)+";`n"+
    (Extract-Method $line '^struct LineGroupVertex$' $linePath)+";`n"+
    (Extract-Method $line '^static unsigned int PackLineGroupColor\(' $linePath)+
    (Extract-Method $line '^LineGroupClass::LineGroupClass\(' $linePath)+
    (Extract-Method $line '^LineGroupClass::~LineGroupClass\(' $linePath)+
    (Extract-Method $line '^int LineGroupClass::Get_Flag\(' $linePath)+
    (Extract-Method $line '^void\s+LineGroupClass::Render\(' $linePath)
Put-Generated 'linegroup.inc' $body
$commandsPath='Core/Libraries/Source/WWVegas/WW3D2/nativew3dgameclientcommands.cpp'
$commands=Read-Source $commandsPath
$helpers=''
foreach($pattern in @('^bool IsOperationalOwner\(','^void InitializeCommand\(',
    '^RenderResult SubmitCommand\(','^RenderResult DispatchCommand\(',
    '^void CopyMatrix\(const Matrix4x4 &')) {
    $helpers+=Extract-Method $commands $pattern $commandsPath
}
$ownerPath='Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp'
$owner=Read-Source $ownerPath
$helpers+=Extract-Method $owner '^bool CheckedGameSizeMultiply\(' $ownerPath
Put-Generated 'command-helpers.inc' $helpers
$facade=''
foreach($pattern in @('^RenderResult DrawGameSortedIndexedTrianglesUP\(',
    '^RenderResult DrawGamePrimitiveUP\(',
    '^void SetGameTransform\(GameRenderTransformSlot slot, const Matrix4x4 &',
    '^void GetGameTransform\(GameRenderTransformSlot slot, Matrix4x4 \*')) {
    $facade+=Extract-Method $commands $pattern $commandsPath
}
Put-Generated 'command-facade.inc' $facade
$case=Extract-Case $owner 'GAME_RENDER_COMMAND_DRAW_SORTED_INDEXED_TRIANGLES_UP' 'GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP' $ownerPath
# The wrapper is a modeled admission/owner carrier, not the NativeW3D2 ABI.
Put-Generated 'owner-sorted.inc' ("RenderResult FixtureOwner::ExecuteSortedCase(const GameRenderCommand &command)`n{`n switch(command.type) {`n"+
    $case+"default: break;`n}`ninvalid_command:`n RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);`n return RENDER_RESULT_INVALID_ARGUMENT;`n}`n")
if($receipts.Count -ne 18){throw "Unexpected extraction coverage: $($receipts.Count) fragments"}
Put-Generated 'extraction-receipt.json' ([ordered]@{schema='native-linegroup-actual-source-v1';
    source_root=$SourceRoot;linegroup_source_root=$LineGroupSourceRoot;fragments=$receipts.ToArray()}|ConvertTo-Json -Depth 6)
Write-Output "Extracted $($receipts.Count) actual source fragments"
