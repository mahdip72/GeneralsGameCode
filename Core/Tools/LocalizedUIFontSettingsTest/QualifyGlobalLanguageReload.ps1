<#
Copyright 2026 TheSuperHackers
SPDX-License-Identifier: GPL-3.0-or-later

Optional same-fixture historical RED/candidate GREEN qualification. This is not
part of the normal CMake target and has no Git-history dependency: the baseline
source root is a caller-provided export of the frozen source files.
#>

param(
	[Parameter(Mandatory = $true)][string]$BaselineSourceRoot,
	[Parameter(Mandatory = $true)][string]$CandidateSourceRoot,
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[string]$BuildDirectoryRoot,
	[string]$CompilerPath = 'cl.exe',
	[Int]$ProcessTimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Quote-WindowsArgument([string]$Argument)
{
	if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') { return $Argument }
	$builder = New-Object System.Text.StringBuilder
	[void]$builder.Append('"')
	$slashes = 0
	foreach ($character in $Argument.ToCharArray())
	{
		if ($character -eq [char]92) { ++$slashes; continue }
		if ($character -eq '"')
		{
			[void]$builder.Append([string]::new([char]92, (2 * $slashes) + 1))
			[void]$builder.Append('"')
			$slashes = 0
			continue
		}
		if ($slashes -gt 0) { [void]$builder.Append([string]::new([char]92, $slashes)) }
		$slashes = 0
		[void]$builder.Append($character)
	}
	if ($slashes -gt 0) { [void]$builder.Append([string]::new([char]92, 2 * $slashes)) }
	[void]$builder.Append('"')
	return $builder.ToString()
}

function Add-ReceiptError([System.Collections.Generic.List[string]]$Errors, [string]$Message)
{
	$Errors.Add($Message)
}

function Receive-AsyncTextBounded([object]$Task, [int]$TimeoutMilliseconds)
{
	$result = [ordered]@{ Eof = $false; Text = $null; Error = $null }
	if ($null -eq $Task)
	{
		$result.Error = 'No asynchronous stream reader was created.'
		return $result
	}
	try
	{
		if (!$Task.IsCompleted)
		{
			$completed = $Task.Wait($TimeoutMilliseconds)
			if (!$completed) { return $result }
		}
		if ($Task.IsFaulted)
		{
			$result.Error = $Task.Exception.GetBaseException().Message
			return $result
		}
		if ($Task.IsCanceled)
		{
			$result.Error = 'Asynchronous stream read was canceled.'
			return $result
		}
		$result.Text = $Task.GetAwaiter().GetResult()
		$result.Eof = $true
	}
	catch { $result.Error = $_.Exception.Message }
	return $result
}

function Invoke-BoundedProcess(
	[string]$FileName,
	[string[]]$Arguments,
	[string]$WorkingDirectory,
	[string]$StdoutPath,
	[string]$StderrPath,
	[string]$ProcessReceiptPath)
{
	$timeoutMilliseconds = $ProcessTimeoutSeconds * 1000
	$startInfo = New-Object System.Diagnostics.ProcessStartInfo
	$startInfo.FileName = $FileName
	$startInfo.Arguments = [string]::Join(' ', @($Arguments | ForEach-Object { Quote-WindowsArgument $_ }))
	$startInfo.WorkingDirectory = $WorkingDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true

	$process = New-Object System.Diagnostics.Process
	$process.StartInfo = $startInfo
	$stdoutTask = $null
	$stderrTask = $null
	$stdoutResult = $null
	$stderrResult = $null
	$errors = New-Object 'System.Collections.Generic.List[string]'
	$started = $false
	$timedOut = $false
	$processExited = $false
	$treeKillAttempted = $false
	$treeKillCallSucceeded = $false
	$treeKillError = $null
	$exitCode = $null
	$handleDisposed = $false
	$stdoutLogWritten = $false
	$stderrLogWritten = $false
	$processReceipt = [ordered]@{
		Schema = 'bounded-process-cleanup-v1'
		FileName = $FileName
		Arguments = @($Arguments)
		WorkingDirectory = [System.IO.Path]::GetFullPath($WorkingDirectory)
		TimeoutSeconds = $ProcessTimeoutSeconds
		StartAttempted = $true
		Started = $false
		PID = $null
		StartTimeUtc = $null
		StartTimeQueryError = $null
		RequiredProcessorAffinityMask = '0xFFF'
		ProcessorAffinityReadbackMask = $null
		ProcessorAffinityQueryError = $null
		ProcessorAffinityMatchesRequiredMask = $false
		IdentityEvidenceVerifiedBeforeWait = $false
		TimedOut = $false
		TreeKillAttempted = $false
		TreeKillCallSucceeded = $false
		TreeKillError = $null
		ProcessExited = $false
		ExitCode = $null
		StdoutEof = $false
		StderrEof = $false
		StdoutError = $null
		StderrError = $null
		StdoutPath = [System.IO.Path]::GetFullPath($StdoutPath)
		StderrPath = [System.IO.Path]::GetFullPath($StderrPath)
		StdoutLogWritten = $false
		StderrLogWritten = $false
		StdoutSHA256 = $null
		StderrSHA256 = $null
		HandleDisposed = $false
		CleanupComplete = $false
		Errors = @()
	}

	try
	{
		if (!$process.Start())
		{
			Add-ReceiptError $errors "Process.Start returned false for $FileName."
		}
		else
		{
			$started = $true
			$processReceipt.Started = $true
			$processReceipt.PID = $process.Id
			$stdoutTask = $process.StandardOutput.ReadToEndAsync()
			$stderrTask = $process.StandardError.ReadToEndAsync()
			try
			{
				$childStartTime = $process.StartTime
				$processReceipt.StartTimeUtc = $childStartTime.ToUniversalTime().ToString(
					'o', [System.Globalization.CultureInfo]::InvariantCulture)
			}
			catch
			{
				$processReceipt.StartTimeQueryError = $_.Exception.Message
				Add-ReceiptError $errors "Child UTC start-time query failed: $($_.Exception.Message)"
			}
			try
			{
				$process.Refresh()
				$childAffinity = $process.ProcessorAffinity.ToInt64()
				$processReceipt.ProcessorAffinityReadbackMask = Format-AffinityMask $childAffinity
				$processReceipt.ProcessorAffinityMatchesRequiredMask = ($childAffinity -eq [Int64]0xFFF)
			}
			catch
			{
				$processReceipt.ProcessorAffinityQueryError = $_.Exception.Message
				Add-ReceiptError $errors "Child processor-affinity query failed: $($_.Exception.Message)"
			}
			if ($null -eq $processReceipt.StartTimeUtc -or
				$null -ne $processReceipt.StartTimeQueryError -or
				$null -ne $processReceipt.ProcessorAffinityQueryError -or
				!$processReceipt.ProcessorAffinityMatchesRequiredMask)
			{
				throw 'Child start-time or inherited-affinity evidence is missing or unexpected; refusing to wait for or accept this child.'
			}
			$processReceipt.IdentityEvidenceVerifiedBeforeWait = $true
			try
			{
				if (!$process.WaitForExit($timeoutMilliseconds))
				{
					$timedOut = $true
					$processReceipt.TimedOut = $true
				}
			}
			catch { Add-ReceiptError $errors "Initial bounded process wait failed: $($_.Exception.Message)" }
		}
	}
	catch { Add-ReceiptError $errors "Process start or stream setup failed: $($_.Exception.Message)" }
	finally
	{
		if ($started)
		{
			$hasExited = $false
			try { $hasExited = $process.HasExited }
			catch { Add-ReceiptError $errors "HasExited check failed: $($_.Exception.Message)" }

			if (!$hasExited -or $timedOut)
			{
				$treeKillAttempted = $true
				try
				{
					$process.Kill($true)
					$treeKillCallSucceeded = $true
				}
				catch { $treeKillError = $_.Exception.Message }
			}
			if (!$hasExited)
			{
				try { $processExited = $process.WaitForExit($timeoutMilliseconds) }
				catch { Add-ReceiptError $errors "Bounded post-kill exit wait failed: $($_.Exception.Message)" }
			}
			else { $processExited = $true }

			$stdoutResult = Receive-AsyncTextBounded $stdoutTask $timeoutMilliseconds
			$stderrResult = Receive-AsyncTextBounded $stderrTask $timeoutMilliseconds
			if (!$stdoutResult.Eof -or !$stderrResult.Eof)
			{
				if (!$treeKillAttempted)
				{
					$treeKillAttempted = $true
					try
					{
						$process.Kill($true)
						$treeKillCallSucceeded = $true
					}
					catch { $treeKillError = $_.Exception.Message }
					try
					{
						if (!$process.HasExited)
							{ $processExited = $process.WaitForExit($timeoutMilliseconds) }
						else { $processExited = $true }
					}
					catch { Add-ReceiptError $errors "Bounded exit wait after pipe timeout failed: $($_.Exception.Message)" }
				}
				if (!$stdoutResult.Eof) { $stdoutResult = Receive-AsyncTextBounded $stdoutTask $timeoutMilliseconds }
				if (!$stderrResult.Eof) { $stderrResult = Receive-AsyncTextBounded $stderrTask $timeoutMilliseconds }
			}
			try
			{
				if ($process.HasExited)
				{
					$processExited = $true
					$exitCode = $process.ExitCode
				}
			}
			catch { Add-ReceiptError $errors "Final process exit-code read failed: $($_.Exception.Message)" }
		}

		if ($null -ne $stdoutResult)
		{
			$processReceipt.StdoutEof = [bool]$stdoutResult.Eof
			$processReceipt.StdoutError = $stdoutResult.Error
			if ($stdoutResult.Eof)
			{
				try
				{
					[System.IO.File]::WriteAllText($StdoutPath, [string]$stdoutResult.Text, [System.Text.Encoding]::UTF8)
					$stdoutLogWritten = $true
				}
				catch { Add-ReceiptError $errors "Writing stdout log failed: $($_.Exception.Message)" }
			}
		}
		if ($null -ne $stderrResult)
		{
			$processReceipt.StderrEof = [bool]$stderrResult.Eof
			$processReceipt.StderrError = $stderrResult.Error
			if ($stderrResult.Eof)
			{
				try
				{
					[System.IO.File]::WriteAllText($StderrPath, [string]$stderrResult.Text, [System.Text.Encoding]::UTF8)
					$stderrLogWritten = $true
				}
				catch { Add-ReceiptError $errors "Writing stderr log failed: $($_.Exception.Message)" }
			}
		}
		if ($stdoutLogWritten)
		{
			try { $processReceipt.StdoutSHA256 = (Get-FileHash -LiteralPath $StdoutPath -Algorithm SHA256).Hash }
			catch { Add-ReceiptError $errors "Hashing stdout log failed: $($_.Exception.Message)" }
		}
		if ($stderrLogWritten)
		{
			try { $processReceipt.StderrSHA256 = (Get-FileHash -LiteralPath $StderrPath -Algorithm SHA256).Hash }
			catch { Add-ReceiptError $errors "Hashing stderr log failed: $($_.Exception.Message)" }
		}
		try { $process.Dispose(); $handleDisposed = $true }
		catch { Add-ReceiptError $errors "Process handle disposal failed: $($_.Exception.Message)" }

		$processReceipt.TreeKillAttempted = $treeKillAttempted
		$processReceipt.TreeKillCallSucceeded = $treeKillCallSucceeded
		$processReceipt.TreeKillError = $treeKillError
		$processReceipt.ProcessExited = $processExited
		$processReceipt.ExitCode = $exitCode
		$processReceipt.StdoutLogWritten = $stdoutLogWritten
		$processReceipt.StderrLogWritten = $stderrLogWritten
		$processReceipt.HandleDisposed = $handleDisposed
		$processReceipt.Errors = @($errors.ToArray())
		$processReceipt.CleanupComplete = ($started -and $processExited -and
			$processReceipt.StdoutEof -and $processReceipt.StderrEof -and
			$stdoutLogWritten -and $stderrLogWritten -and $handleDisposed -and
			(!$treeKillAttempted -or $treeKillCallSucceeded))
		[System.IO.File]::WriteAllText($ProcessReceiptPath,
			($processReceipt | ConvertTo-Json -Depth 8), [System.Text.Encoding]::UTF8)
	}

	if (!$processReceipt.CleanupComplete)
		{ throw "$FileName did not complete bounded process/pipe cleanup. Receipt: $ProcessReceiptPath" }
	if ($timedOut)
		{ throw "$FileName exceeded the $ProcessTimeoutSeconds-second process limit; cleanup was recorded in $ProcessReceiptPath" }
	if ($errors.Count -gt 0)
		{ throw "$FileName reported a process error; cleanup was recorded in $ProcessReceiptPath" }
	return [pscustomobject]@{
		ExitCode = $exitCode
		TimedOut = $timedOut
		ReceiptPath = [System.IO.Path]::GetFullPath($ProcessReceiptPath)
		CleanupComplete = [bool]$processReceipt.CleanupComplete
	}
}

function Get-TreeInputHashes([string]$SourceRoot)
{
	$paths = @(
		'Core/GameEngine/Source/GameClient/GlobalLanguage.cpp',
		'Core/GameEngine/Include/GameClient/GlobalLanguage.h',
		'Generals/Code/GameEngine/Include/GameClient/FontDesc.h',
		'GeneralsMD/Code/GameEngine/Include/GameClient/FontDesc.h')
	$hashes = [ordered]@{}
	foreach ($relative in $paths)
	{
		$path = Join-Path $SourceRoot $relative
		if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing source receipt input: $path" }
		$hashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
	}
	return $hashes
}

function Get-TestInputHashes([string]$TestDirectory)
{
	$paths = @(
		'CMakeLists.txt',
		'GlobalLanguageReloadTest.cpp',
		'GlobalLanguageReloadMockTypes.inl',
		'GlobalLanguageReloadAssertions.inl',
		'ExtractGlobalLanguageReloadMethods.ps1',
		'QualifyGlobalLanguageReload.ps1')
	$hashes = [ordered]@{}
	foreach ($relative in $paths)
	{
		$path = Join-Path $TestDirectory $relative
		if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test receipt input: $path" }
		$hashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
	}
	return $hashes
}

function Test-HashMapEqual([System.Collections.IDictionary]$Before, [System.Collections.IDictionary]$After)
{
	if ($Before.Count -ne $After.Count) { return $false }
	foreach ($key in $Before.Keys)
	{
		if ($null -eq $After[$key] -or $Before[$key] -cne $After[$key]) { return $false }
	}
	return $true
}

function Format-AffinityMask([Int64]$Mask)
{
	return ('0x' + $Mask.ToString('X'))
}

function Normalize-DirectoryPath([string]$Path)
{
	$fullPath = [System.IO.Path]::GetFullPath($Path)
	$root = [System.IO.Path]::GetPathRoot($fullPath)
	if ($fullPath.Length -gt $root.Length)
	{
		$trimCharacters = [char[]]@(
			[System.IO.Path]::DirectorySeparatorChar,
			[System.IO.Path]::AltDirectorySeparatorChar)
		return $fullPath.TrimEnd($trimCharacters)
	}
	return $fullPath
}

function Test-DirectoryPathOverlap([string]$FirstPath, [string]$SecondPath)
{
	$first = Normalize-DirectoryPath $FirstPath
	$second = Normalize-DirectoryPath $SecondPath
	if ([string]::Equals($first, $second, [System.StringComparison]::OrdinalIgnoreCase))
		{ return $true }
	$separator = [System.IO.Path]::DirectorySeparatorChar
	$firstUnderSecond = $first.StartsWith($second + $separator, [System.StringComparison]::OrdinalIgnoreCase)
	$secondUnderFirst = $second.StartsWith($first + $separator, [System.StringComparison]::OrdinalIgnoreCase)
	return ($firstUnderSecond -or $secondUnderFirst)
}

if ($ProcessTimeoutSeconds -le 0) { throw 'ProcessTimeoutSeconds must be positive.' }
if ($ProcessTimeoutSeconds -gt 90) { throw 'ProcessTimeoutSeconds must not exceed the 90-second bounded-wait ceiling.' }
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64')
	{ throw 'Run this optional qualification from an x64 Visual Studio Native Tools environment.' }
if (Test-Path -LiteralPath $OutputDirectory)
	{ throw "Refusing to reuse or overwrite qualification output: $OutputDirectory" }
if (!(Test-Path -LiteralPath $BaselineSourceRoot -PathType Container) -or
	!(Test-Path -LiteralPath $CandidateSourceRoot -PathType Container))
	{ throw 'Both source roots must exist as directories.' }

$testDirectory = $PSScriptRoot
$testDirectoryFullPath = [System.IO.Path]::GetFullPath($testDirectory)
$extractor = Join-Path $testDirectory 'ExtractGlobalLanguageReloadMethods.ps1'
$testSource = Join-Path $testDirectory 'GlobalLanguageReloadTest.cpp'
$baselineFullPath = [System.IO.Path]::GetFullPath($BaselineSourceRoot)
$candidateFullPath = [System.IO.Path]::GetFullPath($CandidateSourceRoot)
$outputFullPath = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputFullPath)
	{ throw "Refusing to reuse or overwrite runtime output directory: $outputFullPath" }
$hasSeparateBuildDirectory = ![string]::IsNullOrWhiteSpace($BuildDirectoryRoot)
if ($hasSeparateBuildDirectory)
{
	$buildOutputFullPath = [System.IO.Path]::GetFullPath($BuildDirectoryRoot)
	if (Test-Path -LiteralPath $buildOutputFullPath)
		{ throw "Refusing to reuse or overwrite build output directory: $buildOutputFullPath" }
	foreach ($sourcePath in @($testDirectoryFullPath, $baselineFullPath, $candidateFullPath))
	{
		if (Test-DirectoryPathOverlap $buildOutputFullPath $sourcePath)
			{ throw "BuildDirectoryRoot must not overlap test or source trees: $buildOutputFullPath" }
	}
	if (Test-DirectoryPathOverlap $buildOutputFullPath $outputFullPath)
		{ throw 'A supplied BuildDirectoryRoot must be separate from OutputDirectory.' }
}
else
{
	$buildOutputFullPath = Join-Path $outputFullPath 'build'
	if (Test-Path -LiteralPath $buildOutputFullPath)
		{ throw "Refusing to reuse or overwrite build output directory: $buildOutputFullPath" }
}
$sourceHashesBefore = [ordered]@{
	baseline = Get-TreeInputHashes $baselineFullPath
	candidate = Get-TreeInputHashes $candidateFullPath
}
$testHashesBefore = Get-TestInputHashes $testDirectory

$compilerCommand = Get-Command -Name $CompilerPath -ErrorAction Stop
$compiler = [System.IO.Path]::GetFullPath($compilerCommand.Source)
if (!(Test-Path -LiteralPath $compiler -PathType Leaf))
	{ throw "Compiler did not resolve to an executable file: $compiler" }
$compilerHashBefore = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash
$killTreeMethod = [System.Diagnostics.Process].GetMethod('Kill', [type[]]@([bool]))
if ($null -eq $killTreeMethod)
	{ throw 'This PowerShell/.NET runtime does not support retained Process.Kill(true); use a current x64 PowerShell runtime.' }
$processForPath = [System.Diagnostics.Process]::GetCurrentProcess()
try { $powerShell = $processForPath.MainModule.FileName }
finally { $processForPath.Dispose() }
[System.IO.Directory]::CreateDirectory($outputFullPath) | Out-Null
[System.IO.Directory]::CreateDirectory($buildOutputFullPath) | Out-Null

$lanes = @(
	@{ Name = 'baseline'; Root = $baselineFullPath; AllowMissingReset = $true },
	@{ Name = 'candidate'; Root = $candidateFullPath; AllowMissingReset = $false })
$laneResults = [ordered]@{}
$affinityReceipt = [ordered]@{
	RequiredMask = '0xFFF'
	OriginalMask = $null
	CapSetAttempted = $false
	CapReadbackMask = $null
	CapReadbackMatched = $false
	RestoreAttempted = $false
	RestoreCallSucceeded = $false
	RestoreReadbackMask = $null
	RestoreReadbackMatched = $false
	Error = $null
}
$parentProcess = $null
$originalAffinity = $null
$originalAffinityKnown = $false
$qualificationFailure = $null
$sourceHashesAfter = $null
$testHashesAfter = $null
$compilerHashAfter = $null

try
{
	$parentProcess = [System.Diagnostics.Process]::GetCurrentProcess()
	$originalAffinity = $parentProcess.ProcessorAffinity
	$originalAffinityKnown = $true
	$originalMaskValue = $originalAffinity.ToInt64()
	$affinityReceipt.OriginalMask = Format-AffinityMask $originalMaskValue
	$affinityReceipt.CapSetAttempted = $true
	$parentProcess.ProcessorAffinity = [IntPtr]::new([Int64]0xFFF)
	$parentProcess.Refresh()
	$capReadback = $parentProcess.ProcessorAffinity.ToInt64()
	$affinityReceipt.CapReadbackMask = Format-AffinityMask $capReadback
	$affinityReceipt.CapReadbackMatched = ($capReadback -eq [Int64]0xFFF)
	if (!$affinityReceipt.CapReadbackMatched)
		{ throw 'Parent processor-affinity readback did not equal required mask 0xFFF; no child was launched.' }

	foreach ($lane in $lanes)
	{
		$laneDirectory = Join-Path $outputFullPath $lane.Name
		$buildDirectory = Join-Path $buildOutputFullPath $lane.Name
		$runDirectory = Join-Path $laneDirectory 'run'
		[System.IO.Directory]::CreateDirectory($laneDirectory) | Out-Null
		[System.IO.Directory]::CreateDirectory($buildDirectory) | Out-Null
		[System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
		$laneResult = [ordered]@{
			SourceRoot = $lane.Root
			SourceHashesBefore = $sourceHashesBefore[$lane.Name]
			BuildDirectory = [System.IO.Path]::GetFullPath($buildDirectory)
			Extraction = $null
			Compilation = $null
			Runtime = $null
			ExecutablePath = $null
			ExecutableSHA256 = $null
		}
		$laneResults[$lane.Name] = $laneResult

		$extractArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $extractor, '-SourceRoot', $lane.Root, '-OutputDirectory', $laneDirectory)
		if ($lane.AllowMissingReset) { $extractArgs += '-AllowMissingReset' }
		$extract = Invoke-BoundedProcess -FileName $powerShell -Arguments $extractArgs -WorkingDirectory $testDirectory -StdoutPath (Join-Path $laneDirectory 'extract.stdout.log') -StderrPath (Join-Path $laneDirectory 'extract.stderr.log') -ProcessReceiptPath (Join-Path $laneDirectory 'extract.process.json')
		$laneResult.Extraction = $extract
		if ($extract.ExitCode -ne 0) { throw "$($lane.Name) extraction failed with exit $($extract.ExitCode)." }

		$exe = Join-Path $runDirectory 'global-language-reload.exe'
		$objectDirectory = $buildDirectory + [System.IO.Path]::DirectorySeparatorChar
		$pdbPath = Join-Path $buildDirectory 'global-language-reload.pdb'
		$compileArgs = @('/nologo', '/EHsc', '/std:c++20', '/W4', '/I', $testDirectory, '/I', $laneDirectory, "/Fo$objectDirectory", "/Fd$pdbPath", "/Fe$exe", $testSource)
		$compile = Invoke-BoundedProcess -FileName $compiler -Arguments $compileArgs -WorkingDirectory $buildDirectory -StdoutPath (Join-Path $laneDirectory 'compile.stdout.log') -StderrPath (Join-Path $laneDirectory 'compile.stderr.log') -ProcessReceiptPath (Join-Path $laneDirectory 'compile.process.json')
		$laneResult.Compilation = [ordered]@{
			Process = $compile
			CompilerPath = $compiler
			CompilerSHA256Before = $compilerHashBefore
			Arguments = $compileArgs
			ObjectDirectory = [System.IO.Path]::GetFullPath($buildDirectory)
			PdbPath = [System.IO.Path]::GetFullPath($pdbPath)
		}
		if ($compile.ExitCode -ne 0) { throw "$($lane.Name) fixture compile failed with exit $($compile.ExitCode)." }
		$laneResult.ExecutablePath = [System.IO.Path]::GetFullPath($exe)
		$laneResult.ExecutableSHA256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash

		$run = Invoke-BoundedProcess -FileName $exe -Arguments ([string[]]@()) -WorkingDirectory $runDirectory -StdoutPath (Join-Path $laneDirectory 'run.stdout.log') -StderrPath (Join-Path $laneDirectory 'run.stderr.log') -ProcessReceiptPath (Join-Path $laneDirectory 'run.process.json')
		$laneResult.Runtime = $run
		$runText = [System.IO.File]::ReadAllText((Join-Path $laneDirectory 'run.stdout.log')) + [System.IO.File]::ReadAllText((Join-Path $laneDirectory 'run.stderr.log'))
		if ($lane.Name -eq 'baseline')
		{
			if ($run.ExitCode -eq 0 -or $runText -notmatch "name mismatch: got 'A-Font-\d+', expected 'Arial Unicode MS'")
				{ throw 'Frozen baseline did not produce the expected runtime stale-font RED.' }
		}
		else
		{
			if ($run.ExitCode -ne 0 -or $runText -notmatch 'partial-definition reload contract passed for both title fixtures')
				{ throw 'Candidate did not produce the expected runtime GREEN.' }
		}
	}
}
catch { $qualificationFailure = $_.Exception.Message }
finally
{
	if ($null -ne $parentProcess)
	{
		if ($originalAffinityKnown)
		{
			$affinityReceipt.RestoreAttempted = $true
			try
			{
				$parentProcess.ProcessorAffinity = $originalAffinity
				$affinityReceipt.RestoreCallSucceeded = $true
				$parentProcess.Refresh()
				$restoreReadback = $parentProcess.ProcessorAffinity.ToInt64()
				$affinityReceipt.RestoreReadbackMask = Format-AffinityMask $restoreReadback
				$affinityReceipt.RestoreReadbackMatched = ($restoreReadback -eq $originalAffinity.ToInt64())
			}
			catch { $affinityReceipt.Error = "Parent-affinity restoration/readback failed: $($_.Exception.Message)" }
		}
		try { $parentProcess.Dispose() }
		catch
		{
			if ($null -eq $affinityReceipt.Error)
				{ $affinityReceipt.Error = "Parent process handle disposal failed: $($_.Exception.Message)" }
		}
	}
}

try
{
	$sourceHashesAfter = [ordered]@{
		baseline = Get-TreeInputHashes $baselineFullPath
		candidate = Get-TreeInputHashes $candidateFullPath
	}
}
catch
{
	if ($null -eq $qualificationFailure) { $qualificationFailure = "Post-run source hashing failed: $($_.Exception.Message)" }
}
try { $testHashesAfter = Get-TestInputHashes $testDirectory }
catch
{
	if ($null -eq $qualificationFailure) { $qualificationFailure = "Post-run test hashing failed: $($_.Exception.Message)" }
}
try { $compilerHashAfter = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash }
catch
{
	if ($null -eq $qualificationFailure) { $qualificationFailure = "Post-run compiler hashing failed: $($_.Exception.Message)" }
}

$sourceHashesStable = ($null -ne $sourceHashesAfter) -and (Test-HashMapEqual $sourceHashesBefore.baseline $sourceHashesAfter.baseline) -and (Test-HashMapEqual $sourceHashesBefore.candidate $sourceHashesAfter.candidate)
$testHashesStable = ($null -ne $testHashesAfter) -and (Test-HashMapEqual $testHashesBefore $testHashesAfter)
$compilerHashStable = ($null -ne $compilerHashAfter) -and ($compilerHashBefore -ceq $compilerHashAfter)
if (!$sourceHashesStable -and $null -eq $qualificationFailure) { $qualificationFailure = 'Baseline or candidate source inputs changed during qualification.' }
if (!$testHashesStable -and $null -eq $qualificationFailure) { $qualificationFailure = 'Fixture/test inputs changed during qualification.' }
if (!$compilerHashStable -and $null -eq $qualificationFailure) { $qualificationFailure = 'Compiler executable hash changed during qualification.' }
if (!$affinityReceipt.RestoreReadbackMatched -and $null -eq $qualificationFailure) { $qualificationFailure = 'Parent affinity restoration did not read back the original mask.' }

$receipt = [ordered]@{
	Schema = 'global-language-definition-reload-red-green-v2'
	BaselineIdentity = 'c5df5ea8bef30ea4988b25156bc7cd867a8b8e43'
	CandidateIdentity = 'candidate source root supplied by caller; source hashes are authoritative'
	BaselineSourceRoot = $baselineFullPath
	CandidateSourceRoot = $candidateFullPath
	RuntimeOutputDirectory = $outputFullPath
	BuildDirectoryRoot = $buildOutputFullPath
	Compiler = [ordered]@{ Path = $compiler; SHA256Before = $compilerHashBefore; SHA256After = $compilerHashAfter; HashStable = $compilerHashStable }
	TargetArchitecture = $env:VSCMD_ARG_TGT_ARCH
	ProcessTimeoutSeconds = $ProcessTimeoutSeconds
	Affinity = $affinityReceipt
	SourceHashesBefore = $sourceHashesBefore
	SourceHashesAfter = $sourceHashesAfter
	SourceHashesStable = $sourceHashesStable
	TestInputHashesBefore = $testHashesBefore
	TestInputHashesAfter = $testHashesAfter
	TestInputHashesStable = $testHashesStable
	Expected = 'baseline compile succeeds and runtime REDs on stale partial definitions; candidate compile succeeds and runtime GREENs'
	Lanes = $laneResults
	Failure = $qualificationFailure
}
$receiptPath = Join-Path $outputFullPath 'qualification.json'
[System.IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 12), [System.Text.Encoding]::UTF8)
if ($null -ne $qualificationFailure)
	{ throw "GlobalLanguage reload qualification failed: $qualificationFailure Receipt: $receiptPath" }
if (!$affinityReceipt.CapReadbackMatched -or !$affinityReceipt.RestoreReadbackMatched)
	{ throw "Affinity cap or restore readback failed. Receipt: $receiptPath" }
if (!$sourceHashesStable -or !$testHashesStable -or !$compilerHashStable)
	{ throw "Source, fixture, or compiler hashes changed. Receipt: $receiptPath" }
Write-Output "Prepared runtime RED/GREEN qualification receipt: $receiptPath"
