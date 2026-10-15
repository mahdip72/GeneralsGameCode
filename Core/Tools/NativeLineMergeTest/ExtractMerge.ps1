param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot)
$ErrorActionPreference='Stop'
$utf8=New-Object System.Text.UTF8Encoding($false)
$receipts=New-Object 'System.Collections.Generic.List[object]'
function Hash-Text([string]$Text) {
    $hash=[System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($utf8.GetBytes($Text)))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Unique-Anchor([string]$Text,[string]$Anchor,[string]$Path) {
    $matches=[regex]::Matches($Text,[regex]::Escape($Anchor))
    if($matches.Count -ne 1){throw "Expected unique anchor '$Anchor' in $Path, got $($matches.Count)"}
    return $matches[0].Index
}
function Block([string]$Text,[string]$First,[string]$Last,[string]$Path,[string]$Name) {
    $start=Unique-Anchor $Text $First $Path
    $end=Unique-Anchor $Text $Last $Path
    if($end -le $start){throw "Invalid block order for $Name in $Path"}
    $body=$Text.Substring($start,$end-$start)
    $receipts.Add([pscustomobject]@{path=$Path;block=$Name;
        first_line=1+($Text.Substring(0,$start).Split("`n").Length-1);
        sha256=(Hash-Text $body)})
    return $body
}
# All extraction and validation precede writes, so a missing/ambiguous anchor
# fails the build rather than silently generating a reduced test.
$outputs=@{}
foreach($producer in @('segline','streak')) {
    $file=if($producer -eq 'segline'){'seglinerenderer.cpp'}else{'streakRender.cpp'}
    $path="Core/Libraries/Source/WWVegas/WW3D2/$file"
    $absolute=Join-Path $SourceRoot $path
    $text=[System.IO.File]::ReadAllText($absolute).Replace("`r`n","`n")
    $receipts.Add([pscustomobject]@{path=$path;block='source-file';sha256=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash})
    $parameterMatches=[regex]::Matches($text,'(?m)^[\t ]*const[\t ]+float[\t ]+parallel_factor[\t ]*=[^;\r\n]+;')
    if($parameterMatches.Count -ne 1){throw "Expected unique parallel_factor initializer in $path, got $($parameterMatches.Count)"}
    $parameter=$parameterMatches[0]
    $parallelFactor=$parameter.Value+"`n"
    $receipts.Add([pscustomobject]@{path=$path;block='parallel-factor';
        first_line=1+($text.Substring(0,$parameter.Index).Split("`n").Length-1);
        sha256=(Hash-Text $parameter.Value)})
    $mean=Block $text 'unsigned int new_count = curr_int->PointCount + next_int->PointCount;' '// Calculate new intersection direction by intersecting prev_seg with next_seg' $path 'weighted-mean'
    $direction=Block $text 'vdp = Vector3::Dot_Product(prev_seg->EdgePlane[edge], next_seg->EdgePlane[edge]);' '// Now check to see if the merge caused any funky results - if so abort it.' $path 'direction'
    # The branch uses the production variable names. Only declarations and result
    # packaging belong to the fixture; new_direction receives a test-owned seed.
    $rgba=if($producer -eq 'segline'){'new_diffuse'}else{'Vector4(0,0,0,0)'}
    $outputs["$producer.inc"]="static MergeResult Merge_$producer(const Intersection& current, const Intersection& next, const Segment& previous, const Segment& skipped, const Segment& following, const Vector3& seed) {`n"+
        "const auto* curr_int=&current; const auto* next_int=&next; const auto* prev_seg=&previous; const auto* curr_seg=&skipped; const auto* next_seg=&following; const int edge=0; float vdp; bool new_parallel; Vector3 new_direction=seed;`n"+
        $parallelFactor+$mean+$direction+"return {new_count,new_point,new_tex_v,$rgba,new_direction,new_parallel};`n}`n"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
foreach($name in @('segline.inc','streak.inc')) {
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot $name),$outputs[$name],$utf8)
    $receipts.Add([pscustomobject]@{path=$name;block='generated';sha256=(Hash-Text $outputs[$name])})
}
[System.IO.File]::WriteAllText((Join-Path $OutputRoot 'extraction-receipt.json'),
    (ConvertTo-Json -InputObject @($receipts.ToArray()) -Depth 5),$utf8)
