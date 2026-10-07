param([string]$SourceRoot,[string]$OutputRoot,[string]$MeshSourceRoot)
# Alternate root is a frozen RED source only; normal builds use current source.
if (!$MeshSourceRoot) { $MeshSourceRoot=$SourceRoot }
$Title='shared'
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
if (!$SourceRoot -or !$OutputRoot) { throw 'SourceRoot and OutputRoot required' }
$path='Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp'
$text=([IO.File]::ReadAllText((Join-Path $MeshSourceRoot $path))).Replace("`r`n","`n")
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$task=(Extract-Method $text '^class PolyRenderTaskClass\s*:' $path).TrimEnd()+";`n"
[IO.File]::WriteAllText((Join-Path $OutputRoot 'mesh-task-class.inc'),$task,$utf8)
$body=''
foreach($pattern in @('^void DX8TextureCategoryClass::Add_Render_Task\(', '^void DX8TextureCategoryClass::Render\(', '^void DX8TextureCategoryClass::Clear_Render_List\(', '^void DX8RigidFVFCategoryContainer::Render\(')) {
    $body+=Extract-Method $text $pattern $path
}
[IO.File]::WriteAllText((Join-Path $OutputRoot 'mesh-task-methods.inc'),$body,$utf8)
[IO.File]::WriteAllText((Join-Path $OutputRoot 'extraction-receipt.json'),($receipts | ConvertTo-Json -Depth 4),$utf8)