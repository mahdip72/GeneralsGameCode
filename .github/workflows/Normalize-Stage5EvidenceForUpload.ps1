[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$inputFull = [IO.Path]::GetFullPath($InputRoot).TrimEnd('\')
$outputFull = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
if (-not (Test-Path -LiteralPath $inputFull -PathType Container)) {
    throw "Stage 5 evidence input directory was not found: $InputRoot"
}
if ($outputFull -ceq $inputFull -or
    $outputFull.StartsWith($inputFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage 5 normalized evidence output must be a distinct directory outside the input.'
}
if (Test-Path -LiteralPath $outputFull) {
    throw "Stage 5 normalized evidence output already exists: $OutputRoot"
}

New-Item -ItemType Directory -Path $outputFull -Force | Out-Null
foreach ($entry in @(Get-ChildItem -LiteralPath $inputFull -Force)) {
    Copy-Item -LiteralPath $entry.FullName `
        -Destination (Join-Path $outputFull $entry.Name) -Recurse -Force
}

# Evidence is immutable and internally hash-bound.  Upload preparation must be
# byte preserving; path redaction or JSON reserialization would invalidate the
# very receipts the artifact is intended to retain.  Any publishable path
# minimization therefore belongs in the producer before hashes are sealed.
Write-Output "Copied immutable Stage 5 evidence for upload: $outputFull"
