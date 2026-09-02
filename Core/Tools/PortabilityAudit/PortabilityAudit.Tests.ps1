param(
    [Parameter(Mandatory = $true)]
    [string]$AuditScript
)

$ErrorActionPreference = 'Stop'

$exceptPath = Join-Path $PSScriptRoot '..\..\Libraries\Source\WWVegas\WWLib\Except.cpp'
$exceptText = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $exceptPath))
if ($exceptText -notmatch '(?m)^\s*typedef\s+DWORD64\s+\(WINAPI \*SymLoadModuleType\)\s*\(') {
    throw 'DbgHelp SymLoadModule64 contract must use a DWORD64 return type on x64.'
}
$symLoadResults = [regex]::Matches($exceptText,
    '(?m)^\s*ExceptAddress\s+symload\s*=\s*0;\s*$').Count
if ($symLoadResults -ne 2) {
    throw 'DbgHelp SymLoadModule64 results must remain pointer-width at both call sites.'
}
if ($exceptText -notmatch '(?m)^\s*while\s*\(pointer_index\s*<\s*num_addresses\)\s*\{\s*$' -or
    $exceptText -match 'num_addresses\s*\+\s*1') {
    throw 'Stack_Walk must bound writes by the caller-provided return-address capacity.'
}

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
        [switch]$StrictD3D8Boundary,
        [switch]$StrictFinal
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
    if ($StrictFinal) {
        $startInfo.Arguments += ' -StrictFinal'
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
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/Include/GameClient/GameWindow.h' @'
int ExistingGameWindow();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Include/Lib/JobFloatingPointState.h' @'
int ExistingFloatingPointState();
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest.cpp' @'
int ExistingPhysicsIntegrationKernelTest();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/TaskRuntime/JobFloatingPointState.cpp' @'
int ExistingWrongFloatingPointState();
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
    Set-FixtureFile $fixtureRoot 'untracked-pointer-buffer-sizing.cpp' @'
void reserve(void **buffer, unsigned count)
{
  const unsigned bytes = count * sizeof(void *);
  (void)buffer;
  (void)bytes;
}
'@
    Set-FixtureFile $fixtureRoot 'untracked-xfer-buffer.cpp' @'
void appendXferEvent(const void *bytes, unsigned byteCount)
{
  xferUser((void *)bytes, byteCount);
}
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
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/Include/GameClient/GameWindow.h' @'
inline WindowMsgData WindowMsgDataFromPointer(const void *value)
{
  return reinterpret_cast<WindowMsgData>(value);
}
'@
    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Include/Common/StackDump.h' @'
void GetFunctionDetails(void *pointer, char *name, char *filename, unsigned int *lineNumber, unsigned int *address);
__inline void GetFunctionDetails(void *pointer, char *name, char *filename, unsigned int *lineNumber, unsigned int *address) {}
'@

    $failure = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($failure.ExitCode -ne 0) 'growth fixture must fail closed'
    Assert-Fixture ($failure.Output -match 'existing-grow\.cpp.*baseline=1.*current=2') 'per-file growth must be reported with baseline and current counts'
    Assert-Fixture ($failure.Output -notmatch 'dx8wrapper\.(cpp|h).*raw-d3d8-surface-area') 'the explicit wrapper boundary may grow during migration'
    Assert-Fixture ($failure.Output -notmatch 'd3d11legacybridge\.(cpp|h).*raw-d3d8-surface-area') 'the explicit bridge boundary may grow during migration'
    Assert-Fixture ($failure.Output -match 'untracked\.cpp.*baseline=0.*current=1') 'untracked raw-D3D8 files must be included in the fail-closed ratchet'
    Assert-Fixture ($failure.Output -notmatch 'existing\.cpp.*raw-d3d8-surface-area') 'same-count edits must not be reported as raw-D3D8 growth'
    Assert-Fixture ($failure.Output -match 'untracked-pointer\.cpp: pointer-to-32-bit-cast') 'untracked pointer-cast sources must be rejected'
    Assert-Fixture ($failure.Output -notmatch 'untracked-pointer-buffer-sizing\.cpp: pointer-sized-serialization') 'pointer-buffer sizing must not be classified as pointer serialization'
    Assert-Fixture ($failure.Output -notmatch 'untracked-xfer-buffer\.cpp: pointer-sized-serialization') 'xfer buffer declarations/calls must not be classified as pointer serialization'
    Assert-Fixture ($failure.Output -match 'untracked-serialization\.cpp: pointer-sized-serialization') 'untracked serialization sources must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-asm\.cpp: x86-inline-assembly-or-context') 'untracked inline-assembly sources must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-window-message\.cpp: window-message-implicit-narrowing') 'untracked WindowMsgData scalar narrowing must be rejected'
    Assert-Fixture ($failure.Output -match 'untracked-window-message\.cpp: window-message-raw-pointer-cast') 'untracked raw WindowMsgData pointer casts must be rejected'
    Assert-Fixture ($failure.Output -notmatch 'GameWindow\.h:.*pointer-bearing-window-message') 'the explicit pointer-width message boundary must remain allowed'
    Assert-Fixture ($failure.Output -match 'StackDump\.h: stackdump-address-width expected=2 current=0') '32-bit stack-dump output declarations must be rejected'

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_except.cpp' @'
int Existing();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_debug.cpp' @'
int ExistingLegacyCallerAddress();
'@
    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
int ExistingStackDump();
'@
    Invoke-FixtureGit $fixtureRoot @('add', 'Core/Libraries/Source/debug/debug_except.cpp', 'Core/Libraries/Source/debug/debug_debug.cpp', 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp') | Out-Null
    Invoke-FixtureGit $fixtureRoot @('commit', '--quiet', '-m', 'add debug exception fixture') | Out-Null
    $annotatedBaseline = (@(Invoke-FixtureGit $fixtureRoot @('rev-parse', 'HEAD'))[0]).Trim()
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Include/Lib/JobFloatingPointState.h' @'
void CaptureOrRestore(unsigned short controlWord)
{
  __asm { fnstcw [controlWord] } // portability-audit: x87-control-word
  __asm { fldcw [controlWord] } // portability-audit: x87-control-word
  __asm__ __volatile__("fnstcw %0" : "=m"(controlWord)); // portability-audit: x87-control-word
  __asm__ __volatile__("fldcw %0" : : "m"(controlWord)); // portability-audit: x87-control-word
}
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest.cpp' @'
void CaptureOrRestore(unsigned short controlWord)
{
  __asm { fnstcw [controlWord] } // portability-audit: x87-control-word
  __asm { fldcw [controlWord] } // portability-audit: x87-control-word
  __asm__ __volatile__("fnstcw %0" : "=m"(controlWord)); // portability-audit: x87-control-word
    __asm__ __volatile__("fldcw %0" : : "m"(controlWord)); // portability-audit: x87-control-word
}
'@
    $annotatedX87 = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($annotatedX87.Output -notmatch 'Core/Libraries/Include/Lib/JobFloatingPointState\.h:.*x86-inline-assembly-or-context') 'the exact annotated x87 control-word lines must be allowed in the implementation header'
    Assert-Fixture ($annotatedX87.Output -notmatch 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest\.cpp:.*x86-inline-assembly-or-context') 'the exact annotated x87 control-word lines must be allowed in the focused test'
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Include/Lib/JobFloatingPointState.h' @'
void CaptureOrRestore(unsigned short controlWord)
{
  __asm { fnstcw [controlWord] }
}
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/TaskRuntime/JobFloatingPointState.cpp' @'
void CaptureOrRestore(unsigned short controlWord)
{
  __asm { fldcw [controlWord] } // portability-audit: x87-control-word
}
'@
    $rejectedX87 = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($rejectedX87.Output -match 'Core/Libraries/Include/Lib/JobFloatingPointState\.h:.*x86-inline-assembly-or-context') 'an unannotated x87 instruction must remain rejected even in the approved implementation file'
    Assert-Fixture ($rejectedX87.Output -match 'Core/Libraries/Source/TaskRuntime/JobFloatingPointState\.cpp:.*x86-inline-assembly-or-context') 'an annotated x87 instruction in a wrong file must remain rejected'
    Set-FixtureFile $fixtureRoot 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest.cpp' @'
void CaptureOrRestore(unsigned short controlWord)
{
  __asm { fldcw [controlWord] }
}
'@
    $rejectedFocusedX87 = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($rejectedFocusedX87.Output -match 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest\.cpp:.*x86-inline-assembly-or-context') 'an unannotated x87 instruction must remain rejected even in the approved focused test file'
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Include/Lib/JobFloatingPointState.h' @'
void ExistingFloatingPointState();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/TaskRuntime/JobFloatingPointState.cpp' @'
int ExistingWrongFloatingPointState();
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/PhysicsIntegrationKernelTest/PhysicsIntegrationKernelTest.cpp' @'
int ExistingPhysicsIntegrationKernelTest();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_except.cpp' @'
uintptr_t Current(const CONTEXT &ctx)
{
  return static_cast<uintptr_t>(ctx.Eip); // portability-audit: x86-context
}
'@
    $annotated = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($annotated.ExitCode -ne 0) 'unrelated fixture violations must still fail before annotation isolation'
    Assert-Fixture ($annotated.Output -notmatch 'debug_except\.cpp:.*x86-inline-assembly-or-context') 'approved x86 context adapter must not be reported'

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_debug.cpp' @'
void CurrentLegacyCallerAddress()
{
  _asm
}
'@
    $legacyCallerAddress = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($legacyCallerAddress.Output -match 'debug_debug\.cpp:.*x86-inline-assembly-or-context') 'unannotated VC6 inline assembly must remain rejected'
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/debug/debug_debug.cpp' @'
void CurrentLegacyCallerAddress()
{
  _asm // portability-audit: vc6-caller-address
  mov eax,[ebp+4] // portability-audit: vc6-caller-address
}
'@
    $annotatedLegacyCallerAddress = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($annotatedLegacyCallerAddress.Output -notmatch 'debug_debug\.cpp:.*x86-inline-assembly-or-context') 'approved VC6 caller-address annotation must not be reported'

    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
void Current(CONTEXT *context, void (*callback)(const char *))
{
  StackDumpFromContext(context->Eip, context->Esp, context->Ebp, nullptr);
  MakeStackTrace(eip,esp,ebp, 0, callback); // portability-audit: x86-context
  const uintptr_t instructionPointer = static_cast<uintptr_t>(context->Eip); // portability-audit: x86-context
}
'@
    $stackAnnotated = Invoke-Audit $fixtureRoot $annotatedBaseline
    Assert-Fixture ($stackAnnotated.Output -match 'StackDump\.cpp:4: x86-inline-assembly-or-context') 'unannotated x86 context use in StackDump must remain rejected'
    Assert-Fixture ($stackAnnotated.Output -notmatch 'StackDump\.cpp:(5|6): x86-inline-assembly-or-context') 'approved StackDump x86 compatibility lines must not be reported'
    Set-FixtureFile $fixtureRoot 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp' @'
void GetFunctionDetails(void *pointer, char *name, char *filename, unsigned int *lineNumber, std::uintptr_t *address);
void GetFunctionDetails(void *pointer, char *name, char *filename, unsigned int *lineNumber, std::uintptr_t *address) {}
'@

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/W3DWater.cpp' @'
// A commented D3DFVF_XYZ token must not be a way to hide a D3D8 dependency.
D3DFVF_XYZ;
'@
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/CMakeLists.txt' @'
target_link_libraries(game d3d8lib rts_d3d8_headers rts_native_d3d8_compat_boundary d3d8to9)
'@
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/NativeCutoverLeak.cpp' @'
HMODULE LoadLegacyRuntime()
{
  HMODULE module = LoadLibraryA("d3d8.dll");
  HMODULE moduleExWide = LoadLibraryExW(L"d3d8.dll", nullptr, 0);
  HMODULE moduleExUtf8 = LoadLibraryExA(u8"d3d8.dll", nullptr, 0);
  HMODULE moduleLowerL = LoadLibrary(l"d3d8.dll");
  HMODULE moduleLowerU = LoadLibrary(u"d3d8.dll");
  HMODULE moduleUpperU = LoadLibraryEx(U"d3d8.dll", nullptr, 0);
  HMODULE moduleText = LoadLibrary(TEXT("d3d8.dll"));
  HMODULE unrelated = LoadLibraryA("d3d9.dll");
  HMODULE suffixed = LoadLibraryA("d3d8.dll.bak");
  HMODULE extended = LoadLibraryA(u8"d3d8.dllx");
  GetProcAddress(module, "Direct3DCreate8");
  return module;
}
'@
    $strictFailure = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    $strictFailureRepeat = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictFailure.ExitCode -ne 0) 'strict boundary violations must fail closed'
    Assert-Fixture ($strictFailureRepeat.ExitCode -eq $strictFailure.ExitCode -and
        $strictFailureRepeat.Output -eq $strictFailure.Output) 'strict boundary output must be deterministic'
    Assert-Fixture ($strictFailure.Output -match 'W3DWater\.cpp:2: d3d8-fvf') 'strict audit must report active raw D3D8 tokens'
    Assert-Fixture ($strictFailure.Output -match 'W3DWater\.cpp:1: d3d8-fvf') 'strict audit must report raw D3D8 tokens in comments'
    Assert-Fixture ($strictFailure.Output -match 'CMakeLists\.txt:1: d3d8-build-dependency') 'strict audit must report product build dependencies'
    Assert-Fixture ($strictFailure.Output -match 'CMakeLists\.txt:1: native-d3d8-compat-build-dependency') 'strict audit must report native compatibility dependency leakage'
    foreach ($line in @(3, 4, 5, 6, 7, 8, 9)) {
        Assert-Fixture ($strictFailure.Output -match ('NativeCutoverLeak\.cpp:{0}: d3d8-dynamic-library-load' -f $line)) `
            ('strict audit must report dynamic D3D8 library loading on fixture line {0}' -f $line)
    }
    foreach ($line in @(10, 11, 12)) {
        Assert-Fixture ($strictFailure.Output -notmatch ('NativeCutoverLeak\.cpp:{0}: d3d8-dynamic-library-load' -f $line)) `
            ('strict audit must not classify a near-miss dynamic library literal on fixture line {0}' -f $line)
    }
    Assert-Fixture ($strictFailure.Output -match 'NativeCutoverLeak\.cpp:13: direct3dcreate8-dynamic-lookup') 'strict audit must report Direct3DCreate8 dynamic lookup'
    Assert-Fixture ($strictFailure.Output -notmatch 'authoring\.cpp') 'authoring paths must remain outside product-runtime scope'

    $strictFinalFailure = Invoke-Audit $fixtureRoot $baseline -StrictFinal
    Assert-Fixture ($strictFinalFailure.ExitCode -ne 0) 'strict final must reject dynamic D3D8 library loading'
    foreach ($line in @(3, 4, 5, 6, 7, 8, 9)) {
        Assert-Fixture ($strictFinalFailure.Output -match ('NativeCutoverLeak\.cpp:{0}: d3d8-dynamic-library-load' -f $line)) `
            ('strict final must report dynamic D3D8 library loading on fixture line {0}' -f $line)
    }
    foreach ($line in @(10, 11, 12)) {
        Assert-Fixture ($strictFinalFailure.Output -notmatch ('NativeCutoverLeak\.cpp:{0}: d3d8-dynamic-library-load' -f $line)) `
            ('strict final must not classify a near-miss dynamic library literal on fixture line {0}' -f $line)
    }

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
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Generals/Code/GameEngine/Include/Common/StackDump.h') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/W3DWater.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/GameEngine/CMakeLists.txt') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/GameEngine/NativeCutoverLeak.cpp') -Force

    $clean = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($clean.ExitCode -eq 0) 'same-count edits and temporary backend additions must pass'
    $strictClean = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictClean.ExitCode -eq 0) 'strict boundary must pass when only explicit migration files retain D3D8'
    $finalBlocked = Invoke-Audit $fixtureRoot $baseline -StrictFinal
    Assert-Fixture ($finalBlocked.ExitCode -ne 0 -and
        $finalBlocked.Output -match 'strict-final native-d3d8-free occurrences=') 'final cutover must remain blocked while an explicit migration file retains D3D8'

    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp' @'
int NativeBridgeImplementation();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.h' @'
int NativeBridgeContract();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp' @'
int NativeRendererImplementation();
'@
    Set-FixtureFile $fixtureRoot 'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h' @'
int NativeRendererContract();
'@
    $finalClean = Invoke-Audit $fixtureRoot $baseline -StrictFinal
    Assert-Fixture ($finalClean.ExitCode -eq 0) (
        'final cutover must pass after the product migration boundary is D3D8-free: ' +
        $finalClean.Output.Trim())

    # The device-free renderer contract target deliberately extracts and
    # declares raw-D3D8-shaped doubles.  Permit only its exact intermediate
    # fixture paths; neighboring test files and product-runtime files must
    # continue to fail the ordinary ratchet and the strict product gate.
    Set-FixtureFile $fixtureRoot 'Core/Tools/RendererContractTest/CMakeLists.txt' @'
add_executable(renderer_contract_fixture)
set(LEGACY_SURFACE "IDirect3DSurface8")
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/RendererContractTest/LegacyAsyncBridgeCompletionTest.cpp' @'
struct IDirect3DSurface8 {};
IDirect3DTexture8 *texture;
'@
    Set-FixtureFile $fixtureRoot 'Core/Tools/RendererContractTest/NeighborFixture.cpp' @'
IDirect3DSurface8 *neighbor;
'@
    Set-FixtureFile $fixtureRoot 'Core/GameEngine/RendererContractTestFixture.cpp' @'
IDirect3DTexture8 *product;
'@
    $boundaryFailure = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($boundaryFailure.ExitCode -ne 0) 'unapproved neighboring and product raw-D3D8 fixtures must fail the intermediate ratchet'
    Assert-Fixture ($boundaryFailure.Output -match 'NeighborFixture\.cpp.*raw-d3d8-surface-area') 'a neighboring test path must remain rejected'
    Assert-Fixture ($boundaryFailure.Output -match 'Core/GameEngine/RendererContractTestFixture\.cpp.*raw-d3d8-surface-area') 'a product-runtime path must remain rejected'
    Assert-Fixture ($boundaryFailure.Output -notmatch 'RendererContractTest/CMakeLists\.txt.*raw-d3d8-surface-area') 'the exact renderer CMake fixture exemption must apply'
    Assert-Fixture ($boundaryFailure.Output -notmatch 'LegacyAsyncBridgeCompletionTest\.cpp.*raw-d3d8-surface-area') 'the exact async bridge fixture exemption must apply'

    $strictBoundaryFailure = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictBoundaryFailure.ExitCode -ne 0) 'strict product gate must still reject the product fixture'
    Assert-Fixture ($strictBoundaryFailure.Output -match 'RendererContractTestFixture\.cpp:1: d3d8-interface') 'strict product gate must report the product fixture'
    Assert-Fixture ($strictBoundaryFailure.Output -notmatch 'LegacyAsyncBridgeCompletionTest\.cpp.*d3d8-interface') 'strict product gate must not scan the device-free test fixture'

    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/Tools/RendererContractTest/NeighborFixture.cpp') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/GameEngine/RendererContractTestFixture.cpp') -Force
    $boundaryClean = Invoke-Audit $fixtureRoot $baseline
    Assert-Fixture ($boundaryClean.ExitCode -eq 0) 'the exact temporary renderer fixture paths must pass after neighboring violations are removed'
    $strictBoundaryClean = Invoke-Audit $fixtureRoot $baseline -StrictD3D8Boundary
    Assert-Fixture ($strictBoundaryClean.ExitCode -eq 0) 'strict product gate must remain clean with only the exact test fixtures'

    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/Tools/RendererContractTest/CMakeLists.txt') -Force
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'Core/Tools/RendererContractTest/LegacyAsyncBridgeCompletionTest.cpp') -Force

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
