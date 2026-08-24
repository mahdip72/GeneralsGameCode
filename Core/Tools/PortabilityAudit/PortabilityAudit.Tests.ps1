param(
    [Parameter(Mandatory = $true)]
    [string]$AuditScript
)

$ErrorActionPreference = 'Stop'

function Invoke-FixtureGit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = @(& git -C $Root @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed: $($output -join [Environment]::NewLine)"
    }
    return $output
}

function Set-FixtureFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$RelativePath,
        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $path = Join-Path $Root ($RelativePath -replace '/', '\')
    $directory = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [IO.File]::WriteAllText($path, $Content, (New-Object Text.UTF8Encoding($false)))
}

function Invoke-Audit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Baseline,
        [switch]$StrictD3D8Boundary
    )

    $powershellPath = (Get-Command powershell.exe -CommandType Application).Source
    $escapedAuditScript = $AuditScript.Replace('"', '\"')
    $escapedRoot = $Root.Replace('"', '\"')
    $escapedBaseline = $Baseline.Replace('"', '\"')
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $powershellPath
    $startInfo.Arguments = '-NoProfile -ExecutionPolicy Bypass' +
        ' -File "' + $escapedAuditScript + '"' +
        ' -SourceRoot "' + $escapedRoot + '"' +
        ' -Baseline "' + $escapedBaseline + '"'
    if ($StrictD3D8Boundary) {
        $startInfo.Arguments += ' -StrictD3D8Boundary'
    }
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw 'failed to start audit process'
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $output = $stdoutTask.Result + [Environment]::NewLine + $stderrTask.Result
    }
    finally {
        $process.Dispose()
    }
    [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Assert-Fixture {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "Portability audit self-test failed: $Message"
    }
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'PortabilityAudit-' + [Guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    Invoke-FixtureGit $fixtureRoot @('init', '--quiet') | Out-Null
    Invoke-FixtureGit $fixtureRoot @('config', 'user.email', 'portability-audit@example.invalid') |
        Out-Null
    Invoke-FixtureGit $fixtureRoot @('config', 'user.name', 'Portability Audit') |
        Out-Null
    Invoke-FixtureGit $fixtureRoot @('config', 'core.autocrlf', 'false') |
        Out-Null

    Set-FixtureFile $fixtureRoot 'existing.cpp' @'
IDirect3DDevice8 *device;
DX8Wrapper::Set_Texture(0, texture);
'@
    Set-FixtureFile $fixtureRoot 'existing-reduced.cpp' @'
IDirect3DDevice8 *device;
IDirect3DTexture8 *texture;
'@
    Set-FixtureFile $fixtureRoot 'existing-grow.cpp' @'
IDirect3DDevice8 *device;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp' @'
DX8Wrapper *wrapper;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h' @'
DX8Wrapper *wrapper;
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/W3DView/authoring.cpp' @'
// Authoring tools are intentionally outside the product-runtime audit scope.
D3DFVF_XYZ;
'@
    Invoke-FixtureGit $fixtureRoot @('add', '--', '.') | Out-Null
    Invoke-FixtureGit $fixtureRoot @('commit', '--quiet', '-m', 'baseline') | Out-Null
    $baseline = (@(Invoke-FixtureGit $fixtureRoot @('rev-parse', 'HEAD'))[0]).Trim()

    Set-FixtureFile $fixtureRoot 'existing.cpp' @'
// Editing an existing migration file must not require a new legacy token.
IDirect3DDevice8 *device;
DX8Wrapper::Set_Texture(0, texture);
DX8Wrapper::Set_Shader(shader);
'@
    Set-FixtureFile $fixtureRoot 'existing-reduced.cpp' @'
// The ratchet permits reducing the legacy surface area.
IDirect3DDevice8 *device;
'@
    Set-FixtureFile $fixtureRoot 'existing-grow.cpp' @'
IDirect3DDevice8 *device;
IDirect3DTexture8 *texture;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp' @'
DX8Wrapper *wrapper;
IDirect3DDevice8 *device;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h' @'
DX8Wrapper *wrapper;
IDirect3DDevice8 *device;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp' @'
IDirect3DDevice8 *legacyDevice;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.h' @'
IDirect3DTexture8 *legacyTexture;
'@
    Set-FixtureFile $fixtureRoot 'untracked.cpp' @'
IDirect3DDevice8 *untrackedDevice;
'@
    Set-FixtureFile $fixtureRoot 'untracked-pointer.cpp' @'
int converted = reinterpret_cast<int>(address);
'@
    Set-FixtureFile $fixtureRoot 'untracked-serialization.cpp' @'
void save(void *value) { xfer(value, sizeof(void*)); }
'@
    Set-FixtureFile $fixtureRoot 'untracked-asm.cpp' @'
void copy() { __asm mov eax, ebx; }
'@
    Set-FixtureFile $fixtureRoot 'untracked-window-message.cpp' @'
void callback(WindowMsgData mData1, WindowMsgData mData2)
{
  UnsignedByte key = mData1;
  GameWindow *control = (GameWindow *)mData1;
}
'@

    $failure = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($failure.ExitCode -ne 0) 'growth fixture must fail closed'
    Assert-Fixture ($failure.Output -match 'existing-grow\.cpp.*baseline=1.*current=2') 'per-file growth must be reported with baseline and current counts'
    Assert-Fixture ($failure.Output -notmatch 'dx8wrapper\.(cpp|h).*raw-d3d8-surface-area') 'the explicit wrapper boundary may grow during migration'
    Assert-Fixture ($failure.Output -notmatch 'd3d11legacybridge\.(cpp|h).*raw-d3d8-surface-area') 'the explicit bridge boundary may grow during migration'
    Assert-Fixture ($failure.Output -match 'untracked\.cpp.*baseline=0.*current=1') 'untracked raw-D3D8 files must be included in the fail-closed ratchet'
    Assert-Fixture ($failure.Output -notmatch 'existing\.cpp.*raw-d3d8-surface-area') 'same-count edits must not be reported as raw-D3D8 growth'
    Assert-Fixture ($failure.Output -match 'untracked-pointer\.cpp: pointer-to-32-bit-cast') 'untracked pointer-cast sources must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-serialization\.cpp: pointer-sized-serialization') 'untracked serialization sources must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-asm\.cpp: x86-inline-assembly-or-context') 'untracked inline-assembly sources must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-window-message\.cpp: window-message-implicit-narrowing') 'untracked WindowMsgData scalar narrowing must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-window-message\.cpp: window-message-raw-pointer-cast') 'untracked raw WindowMsgData pointer casts must be rejected'

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_except.cpp' @'
int Existing();
'@
    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
int ExistingStackDump();
'@
    Invoke-FixtureGit $fixtureRoot @('add', 'Core/Libraries/Source/debug/debug_except.cpp', 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp') | Out-Null
    Invoke-FixtureGit $fixtureRoot @('commit', '--quiet', '-m', 'add debug exception fixture') | Out-Null
    $annotatedBaseline = (@(Invoke-FixtureGit $fixtureRoot @('rev-parse', 'HEAD'))[0]).Trim()
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_except.cpp' @'
uintptr_t Current(const CONTEXT &ctx)
{
  return static_cast<uintptr_t>(ctx.Eip); // portability-audit: x86-context
}
'@
    $annotated = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($annotated.ExitCode -ne 0) 'unrelated fixture violations must still fail before annotation isolation'
    Assert-Fixture ($annotated.Output -notmatch 'debug_except\.cpp:.*x86-inline-assembly-or-context') 'approved x86 context adapter must not be reported'

    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
void Current(CONTEXT *context, void (*callback)(const char *))
{
  StackDumpFromContext(context->Eip, context->Esp, context->Ebp, nullptr);
  MakeStackTrace(eip,esp,ebp, 0, callback); // portability-audit: x86-context
  const std::uintptr_t instructionPointer = static_cast<std::uintptr_t>(context->Eip); // portability-audit: x86-context
}
'@
    $stackAnnotated = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($stackAnnotated.Output -match 'StackDump\.cpp:4: x86-inline-assembly-or-context') 'unannotated x86 context use in StackDump must remain rejected'
    Assert-Fixture ($stackAnnotated.Output -notmatch 'StackDump\.cpp:(5|6): x86-inline-assembly-or-context') 'approved StackDump x86 compatibility lines must not be reported'
    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
int ExistingStackDump();
'@

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/W3DWater.cpp' @'
// A commented D3DFVF_XYZ token must not be a way to hide a D3D8 dependency.
D3DFVF_XYZ;
'@
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/CMakeLists.txt' @'
target_link_libraries(game d3d8lib)
'@
    $strictFailure = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    $strictFailureRepeat = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictFailure.ExitCode -ne 0) 'strict boundary violations must fail closed'
    Assert-Fixture ($strictFailureRepeat.ExitCode -eq $strictFailure.ExitCode -and
        $strictFailureRepeat.Output -eq $strictFailure.Output) 'strict boundary output must be deterministic'
    Assert-Fixture ($strictFailure.Output -match 'W3DWater\.cpp:2: d3d8-fvf') 'strict audit must report active raw D3D8 tokens'
    Assert-Fixture ($strictFailure.Output -match 'W3DWater\.cpp:1: d3d8-fvf') 'strict audit must report raw D3D8 tokens in comments'
    Assert-Fixture ($strictFailure.Output -match 'CMakeLists\.txt:1: d3d8-build-dependency') 'strict audit must report product build dependencies'
    Assert-Fixture ($strictFailure.Output -notmatch 'authoring\.cpp') 'authoring paths must remain outside product-runtime scope'

    Set-FixtureFile $fixtureRoot 'existing-grow.cpp' @'
IDirect3DDevice8 *device;
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp' @'
DX8Wrapper *wrapper;
'@
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'untracked.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'untracked-pointer.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'untracked-serialization.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'untracked-asm.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'untracked-window-message.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/W3DWater.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/GameEngine/CMakeLists.txt') -Force

    $clean = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($clean.ExitCode -eq 0) 'same-count edits and temporary backend additions must pass'
    $strictClean = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictClean.ExitCode -eq 0) 'strict boundary must pass when only explicit migration files retain D3D8'

    $tree = (@(Invoke-FixtureGit $fixtureRoot @('rev-parse', 'HEAD^{tree}'))[0]).Trim()
    $nonAncestor = (@(Invoke-FixtureGit $fixtureRoot @('commit-tree', $tree, '-m', 'unrelated baseline'))[0]).Trim()
    $ancestry = Invoke-Audit $fixtureRoot $nonAncestor
    Assert-Fixture ($ancestry.ExitCode -ne 0 -and $ancestry.Output -match 'must be an ancestor') 'a baseline outside the checked-out ancestry must fail explicitly'

    Write-Output 'Portability audit self-tests passed.'
}
finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}
