param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [string]$Baseline = '754cdae0170472a242095acded0cf253ced89512'
)

$ErrorActionPreference = 'Stop'
$sourceRootPath = (Resolve-Path -LiteralPath $SourceRoot).Path

$rules = @(
    [pscustomobject]@{
        Name = 'pointer-to-32-bit-cast'
        Pattern = 'reinterpret_cast\s*<\s*(Int|UnsignedInt|int|unsigned|DWORD|LONG)\s*>'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'x86-inline-assembly-or-context'
        Pattern = '(^|[^A-Za-z0-9_])(__asm|_asm|Eip|Esp|Ebp)([^A-Za-z0-9_]|$)'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'pointer-sized-serialization'
        Pattern = '(sizeof\s*\([^\)]*\*\)|xfer[^\r\n]*(void\s*\*|uintptr_t|intptr_t))'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'pointer-bearing-window-message'
        Pattern = '(WindowMsgData|WM_[A-Z0-9_]+)[^\r\n]*(void\s*\*|reinterpret_cast|\(Int\)|\(UnsignedInt\))'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'raw-d3d8-surface-area'
        Pattern = '(IDirect3D[A-Za-z0-9_]*8|D3D[A-Z0-9_]*8|DX8Wrapper)'
        RejectAddedLine = $false
    }
)

$extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inl')
$tracked = & git -C $sourceRootPath ls-files
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed'
}
$files = @($tracked | Where-Object {
    $extensions -contains [IO.Path]::GetExtension($_).ToLowerInvariant()
})

$violations = @()
foreach ($rule in $rules) {
    $count = 0
    foreach ($relativePath in $files) {
        $path = Join-Path $sourceRootPath $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            continue
        }
        $count += ([regex]::Matches(
            [IO.File]::ReadAllText($path),
            $rule.Pattern,
            [Text.RegularExpressions.RegexOptions]::Multiline)).Count
    }
    [pscustomobject]@{ Rule = $rule.Name; WholeTreeMatches = $count }
}

$diff = & git -C $sourceRootPath diff --unified=0 $Baseline -- @(
    '*.c', '*.cc', '*.cpp', '*.cxx', '*.h', '*.hpp', '*.inl'
)
if ($LASTEXITCODE -ne 0) {
    throw "git diff against $Baseline failed"
}

$currentFile = ''
$lineNumber = 0
foreach ($line in $diff) {
    if ($line -match '^\+\+\+ b/(.+)$') {
        $currentFile = $Matches[1]
        continue
    }
    if ($line -match '^@@ -[^ ]+ \+(\d+)') {
        $lineNumber = [int]$Matches[1]
        continue
    }
    if ($line.StartsWith('+') -and -not $line.StartsWith('+++')) {
        $content = $line.Substring(1)
        foreach ($rule in $rules) {
            if ($rule.RejectAddedLine -and $content -match $rule.Pattern) {
                $violations += "${currentFile}:${lineNumber}: $($rule.Name)"
            }
        }
        ++$lineNumber
    } elseif (-not $line.StartsWith('-')) {
        ++$lineNumber
    }
}

if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output "Portability audit found no new high-confidence violations relative to $Baseline."
