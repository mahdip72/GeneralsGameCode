param(
    [string]$SourceRoot = (Get-Location).Path,
    [string]$Baseline = '754cdae0170472a242095acded0cf253ced89512',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# This is intentionally a small lexical audit rather than a C++ parser.  It
# protects the migrated product surface from adding new calls through a raw
# IDirect3DDevice8 pointer.  The legacy backend and its compatibility facade
# are owned by dx8wrapper/d3d11legacybridge and are not scanned here.
$rendererMethods = @(
    'SetRenderState',
    'SetTextureStageState',
    'SetTexture',
    'SetTransform',
    'SetVertexShader',
    'SetPixelShader',
    'SetVertexShaderConstant',
    'SetPixelShaderConstant',
    'SetStreamSource',
    'SetIndices',
    'DrawPrimitive',
    'DrawIndexedPrimitive',
    'DrawPrimitiveUP',
    'DrawIndexedPrimitiveUP',
    'Clear'
)

$productRoots = @(
    'Core/GameEngineDevice/Source/W3DDevice',
    'Generals/Code/GameEngineDevice/Source/W3DDevice',
    'GeneralsMD/Code/GameEngineDevice/Source/W3DDevice',
    'Generals/Code/Libraries/Source/WWVegas/WW3D2',
    'GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2'
)

function Assert-Audit {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "Renderer bypass audit self-test failed: $Message"
    }
}

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Text)

    # Keep line endings so that diagnostics retain their source line number.
    $withoutBlocks = [regex]::Replace($Text, '/\*[\s\S]*?\*/', {
        param($Match)
        return ($Match.Value -replace '[^\r\n]', ' ')
    })
    return [regex]::Replace($withoutBlocks, '//[^\r\n]*', '')
}

function Get-LiveRendererBypassMatches {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $methodPattern = ($rendererMethods -join '|')
    $directPattern = "(?i)(?:\b(?:m_pDev|D3DDevice|device)\s*->\s*|_Get_D3D_Device8\(\)\s*->\s*)($methodPattern)\s*\("
    $macroPattern = "(?i)\bDX8CALL(?:_HRES)?\s*\(\s*($methodPattern)\s*\("
    # -split consumes a regular expression. Keep the escapes literal so the
    # scanner processes each source line independently; the old interpolated
    # string became CR + '?' + LF and collapsed normal LF fixtures into one
    # line, defeating the preprocessor-state checks.
    $lines = (Remove-CppComments $Text) -split '\r?\n'

    # Track only preprocessor conditions whose state is known to this build.
    # Unknown feature switches stay active so the audit is conservative.  The
    # old pre-transformed terrain path is explicitly disabled in
    # BaseHeightMap.h and is not a live product path today.
    $frames = New-Object System.Collections.ArrayList
    $active = $true
    $results = @()
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        $directive = [regex]::Match($line, '^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
        if ($directive.Success) {
            $kind = $directive.Groups[1].Value
            $expression = $directive.Groups[2].Value.Trim()
            if ($kind -eq 'if' -or $kind -eq 'ifdef' -or $kind -eq 'ifndef') {
                $knownDisabled = $false
                if ($kind -eq 'if' -and $expression -match '^0(?:\s|$)') {
                    $knownDisabled = $true
                } elseif ($kind -eq 'ifdef' -and $expression -match '^PRE_TRANSFORM_VERTEX\b') {
                    $knownDisabled = $true
                } elseif ($kind -eq 'if' -and $expression -match '^defined\s*\(\s*PRE_TRANSFORM_VERTEX\s*\)') {
                    $knownDisabled = $true
                }
                [void]$frames.Add([pscustomobject]@{
                    ParentActive = $active
                    KnownDisabled = $knownDisabled
                    BranchTaken = (-not $knownDisabled)
                })
                $active = $active -and (-not $knownDisabled)
                continue
            }
            if ($frames.Count -gt 0 -and $kind -eq 'else') {
                $frame = $frames[$frames.Count - 1]
                $active = $frame.ParentActive -and $frame.KnownDisabled
                $frame.BranchTaken = $true
                continue
            }
            if ($frames.Count -gt 0 -and $kind -eq 'elif') {
                $frame = $frames[$frames.Count - 1]
                # A known-disabled branch can have an unknown #elif.  Keep it
                # active so a newly enabled bypass cannot hide from the audit.
                $active = $frame.ParentActive -and $frame.KnownDisabled
                $frame.KnownDisabled = $false
                $frame.BranchTaken = $true
                continue
            }
            if ($frames.Count -gt 0 -and $kind -eq 'endif') {
                $frame = $frames[$frames.Count - 1]
                $active = $frame.ParentActive
                $frames.RemoveAt($frames.Count - 1)
                continue
            }
        }

        if (-not $active) {
            continue
        }

        $match = [regex]::Match($line, $directPattern)
        if (-not $match.Success) {
            $match = [regex]::Match($line, $macroPattern)
        }
        if ($match.Success) {
            $results += [pscustomobject]@{
                Path = $Path
                Line = $index + 1
                Method = $match.Groups[1].Value
                Text = $line.Trim()
            }
        }
    }
    return @($results)
}

function Invoke-SelfTest {
    $fixture = @'
// m_pDev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
/* DX8CALL(SetTexture(2, nullptr)); */
#if 0
    m_pDev->SetRenderState(D3DRS_ZENABLE, 0);
#endif
#ifdef PRE_TRANSFORM_VERTEX
    m_pDev->SetStreamSource(0, vb, 16);
#else
    DX8CALL(SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE));
#endif
surface->Clear();
'@
    $matches = @(Get-LiveRendererBypassMatches $fixture 'fixture.cpp')
    Assert-Audit ($matches.Count -eq 1) 'comments, #if 0, and the disabled pre-transform branch must be ignored'
    Assert-Audit ($matches[0].Method -eq 'SetTextureStageState') (
        'active DX8CALL method was not recognized (actual: {0})' -f
        $matches[0].Method)

    $elseFixture = @'
#ifdef PRE_TRANSFORM_VERTEX
    m_pDev->SetStreamSource(0, vb, 16);
#else
    m_pDev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
#endif
'@
    $elseMatches = @(Get-LiveRendererBypassMatches $elseFixture 'else.cpp')
    Assert-Audit ($elseMatches.Count -eq 1 -and $elseMatches[0].Method -eq 'DrawPrimitive') 'the active #else branch must be audited'
    Write-Output 'Renderer bypass audit self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$sourceRootPath = (Resolve-Path -LiteralPath $SourceRoot).Path
& git -C $sourceRootPath cat-file -e "$Baseline`^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "Baseline '$Baseline' is not available."
}
& git -C $sourceRootPath merge-base --is-ancestor $Baseline HEAD 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "Baseline '$Baseline' must be an ancestor of HEAD."
}

$extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inl')
$relativeFiles = @()
foreach ($root in $productRoots) {
    $absoluteRoot = Join-Path $sourceRootPath ($root -replace '/', '\')
    if (-not (Test-Path -LiteralPath $absoluteRoot -PathType Container)) {
        continue
    }
    $relativeFiles += @(Get-ChildItem -LiteralPath $absoluteRoot -Recurse -File | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant()
    } | ForEach-Object {
        $_.FullName.Substring($sourceRootPath.Length + 1).Replace('\', '/')
    })
}
$relativeFiles = @($relativeFiles | Sort-Object -Unique)

$violations = @()
$totalMatches = 0
foreach ($relativePath in $relativeFiles) {
    $currentPath = Join-Path $sourceRootPath ($relativePath -replace '/', '\')
    $currentText = [IO.File]::ReadAllText($currentPath)
    $currentMatches = @(Get-LiveRendererBypassMatches $currentText $relativePath)
    $totalMatches += $currentMatches.Count

    $baselineText = ''
    $baselinePathExists = @(& git -C $sourceRootPath ls-tree -r --name-only $Baseline -- $relativePath)
    if ($baselinePathExists -contains $relativePath) {
        $baselineText = (& git -C $sourceRootPath show "$Baseline`:$relativePath") -join "`n"
    }
    if ([string]::IsNullOrEmpty($baselineText)) {
        $baselineMatches = @()
    } else {
        $baselineMatches = @(Get-LiveRendererBypassMatches $baselineText $relativePath)
    }
    if ($currentMatches.Count -gt $baselineMatches.Count) {
        $violations += "${relativePath}: active raw render calls baseline=$($baselineMatches.Count) current=$($currentMatches.Count)"
        foreach ($newMatch in $currentMatches | Select-Object -Skip $baselineMatches.Count) {
            $violations += "  line $($newMatch.Line): $($newMatch.Text)"
        }
    }
}

Write-Output "renderer-bypass-audit: active raw render calls $totalMatches (ratcheted against $Baseline)"
if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Output $_ }
    exit 1
}
Write-Output 'Renderer bypass audit found no newly active raw render calls.'
