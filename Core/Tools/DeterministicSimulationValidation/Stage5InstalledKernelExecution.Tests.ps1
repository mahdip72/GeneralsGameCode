param([string]$ScratchRoot = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Copy-TestDocument {
    param([object]$Document)
    $json = $Document | ConvertTo-Json -Depth 30
    $command = Get-Command ConvertFrom-Json
    if ($command.Parameters.ContainsKey('DateKind')) {
        return $json | ConvertFrom-Json -DateKind String
    }
    return $json | ConvertFrom-Json
}

function Test-DocumentAgainstSchema {
    param([object]$Document, [string]$SchemaPath)
    return ($Document | ConvertTo-Json -Depth 30) | Test-Json `
        -SchemaFile $SchemaPath -ErrorAction SilentlyContinue
}

function New-TestKernel {
    param([string]$Name)
    return [ordered]@{
        name = $Name
        worker = [ordered]@{
            submittedJobs = 2
            completedJobs = 2
            physicalWorkerJobs = 2
            ownerHelpedJobs = 0
            physicalWorkerMask = '0000000000000003'
            distinctPhysicalWorkers = 2
            physicalWorkerMaskComplete = $true
        }
        streams = @([ordered]@{
            subtype = 0
            attemptedBatches = 1
            admittedBatches = 1
            committedBatches = 1
            abortedBatches = 0
            validatedBatchCount = 1
            committedBatchCount = 1
            abortedBatchCount = 0
            validatedOperationCount = 2
            committedOperationCount = 2
            inputSha256 = '11' * 32
            outputSha256 = '22' * 32
            commitSha256 = '33' * 32
        })
    }
}

function New-TestRun {
    param([string]$Title, [int]$Ordinal)
    $titleCode = if ($Title -ceq 'Generals') { '1' } else { '2' }
    return [ordered]@{
        ordinal = $Ordinal
        runId = "$($Title.ToLowerInvariant())-dense-$Ordinal"
        runNonce = '40000000-0000-4000-8{0}00-{1:D12}' -f $titleCode, $Ordinal
        processId = 1000 + $Ordinal
        processCreationTimeUtc100ns = 100000 + $Ordinal
        receipt = [ordered]@{
            path = "Titles/$Title/dense-$Ordinal/receipt.json"
            sha256 = '44' * 32
        }
        rawLog = [ordered]@{
            path = "Titles/$Title/dense-$Ordinal/raw.log"
            sha256 = '45' * 32
        }
        timing = [ordered]@{
            path = "Titles/$Title/dense-$Ordinal/timing.csv"
            sha256 = '46' * 32
        }
        attemptStart = [ordered]@{
            path = "Titles/$Title/dense-$Ordinal/attempt.start.json"
            sha256 = '47' * 32
        }
        attemptResult = [ordered]@{
            path = "Titles/$Title/dense-$Ordinal/attempt.result.json"
            sha256 = '48' * 32
        }
        finalCrc = 305419896
        worker = [ordered]@{
            requestedCount = 4
            effectiveCount = 4
            policy = 'auto'
            pinned = $true
            selectedWorkerPhysicalCoreCount = 4
            selectedWorkerPhysicalCoreMask = '000000000000000F'
            selectedWorkerPhysicalCoreMaskComplete = $true
        }
        scheduler = [ordered]@{
            submittedJobCount = 12
            executedJobCount = 12
            failedJobCount = 0
            cancelledJobCount = 0
            serialFallbackCount = 0
            workerWaitRejectionCount = 0
            maximumActiveWorkers = 4
        }
        kernels = @(
            (New-TestKernel 'physics')
            (New-TestKernel 'status')
            (New-TestKernel 'collision')
            (New-TestKernel 'ai-planning')
            (New-TestKernel 'spatial')
            (New-TestKernel 'path')
        )
    }
}

function New-TestTitleProof {
    param([string]$Title)
    $epoch = if ($Title -ceq 'Generals') { 1 } else { 3 }
    return [ordered]@{
        title = $Title
        executableSha256 = $(if ($Title -ceq 'Generals') { 'AA' * 32 } else { 'BB' * 32 })
        hostAggregate = [ordered]@{
            path = "Titles/$Title/Stage5InstalledKernelExecutionHost.json"
            sha256 = '55' * 32
        }
        validationManifest = [ordered]@{
            path = "Titles/$Title/Stage5InstalledKernelExecutionValidationManifest.json"
            sha256 = '56' * 32
        }
        runPlan = [ordered]@{
            path = "Titles/$Title/phase-plan.json"
            sha256 = '57' * 32
        }
        attemptManifest = [ordered]@{
            path = "Titles/$Title/phase-attempts.json"
            sha256 = '58' * 32
        }
        fixtureProduction = [ordered]@{
            hostReceipt = [ordered]@{
                path = "Titles/$Title/FixtureProduction/Stage5NativePerformanceFixture.json"
                sha256 = '76' * 32
            }
            rawLog = [ordered]@{
                path = "Titles/$Title/FixtureProduction/stdout.log"
                sha256 = '77' * 32
            }
            reviewedFixtureManifest = [ordered]@{
                path = "Titles/$Title/FixtureProduction/ReviewedFixture.json"
                sha256 = '75' * 32
            }
            prelaunchPlan = [ordered]@{
                path = "Titles/$Title/FixtureProduction/prelaunch/fixture-plan.json"
                sha256 = '78' * 32
            }
            attemptStart = [ordered]@{
                path = "Titles/$Title/FixtureProduction/prelaunch/fixture-start.json"
                sha256 = '79' * 32
            }
            recordedUtc = '2026-09-04T11:30:00.0000000Z'
            cohortNonce = '10000000-0000-4000-8000-000000000001'
            cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
            qualificationMode = 'InstalledKernelExecution'
            acceptanceScope = 'fixture-production-only'
            finalAcceptanceClaim = $false
            performanceScalingClaim = $false
            sourceCommit = 'a' * 40
            artifactSetSha256 = 'CC' * 32
            runtimeClosure = [ordered]@{
                dependencyManifestSha256 = 'DD' * 32
                closureSha256 = 'EE' * 32
            }
            process = [ordered]@{
                id = 900
                creationTimeUtc100ns = 90000
                exitCode = 0
            }
            hostRunNonce = '30000000-0000-4000-8{0}00-000000000001' -f $epoch
            nativeRunNonce = '00000384-12345678-000006C1'
            executableSha256 = $(if ($Title -ceq 'Generals') { 'AA' * 32 } else { 'BB' * 32 })
            commandLine = "generals -headless -runStage5PerformanceFixture Maps\\Stage5Dense\\Stage5Dense.map 1729 3600"
            argumentString = "-headless -runStage5PerformanceFixture Maps\\Stage5Dense\\Stage5Dense.map 1729 3600"
            map = [ordered]@{
                path = "Titles/$Title/FixtureProduction/Maps/Stage5Dense/Stage5Dense.map"
                key = 'Maps\\Stage5Dense\\Stage5Dense.map'
                sha256 = '88' * 32
                crc32 = '1234ABCD'
                byteCount = 16384
            }
            retainedReplay = [ordered]@{
                path = "Titles/$Title/FixtureProduction/Stage5Performance.rep"
                sha256 = '66' * 32
                frameCount = 1201
            }
            frameBudget = 3600
            endFrame = 1200
            winnerTeam = 0
            finalCrc = 305419896
            replayEpoch = $epoch
            observedFirstFrame = 1
            observedLastFrame = 1200
            observedFrameSamples = 1200
            observedPlayerCount = 8
            initialUnitCount = 8000
            peakUnitCount = 12000
            playerUnits = @(0..7 | ForEach-Object {
                [ordered]@{ slot = $_; initialUnitCount = 1000; peakUnitCount = 1500 }
            })
            lifecycle = [ordered]@{
                mutexAcquired = $true
                noInstalledTitleProcessesAtPreflight = $true
                childExitProven = $true
                registryRestored = $true
                profileRemoved = $true
                recoveryJournalAbsent = $true
            }
        }
        fixture = [ordered]@{
            id = 'dense-eight-player'
            kind = 'replay'
            sha256 = '66' * 32
            seed = 1729
            playerCount = 8
            requestedMinimumUnitCount = 8000
            minimumInitialUnitCount = 8000
            minimumObservedUnitCount = 8000
            maximumObservedUnitCount = 12000
            rosterStable = $true
            contiguous = $true
        }
        schedule = [ordered]@{
            lane = 'physical-4'
            requestedWorkers = 4
            warmupRuns = 1
            measuredRuns = 3
        }
        runs = @(
            (New-TestRun $Title 1)
            (New-TestRun $Title 2)
            (New-TestRun $Title 3)
        )
    }
}

$schemaPath = Join-Path $PSScriptRoot 'Stage5InstalledKernelExecution.schema.json'
Assert-True (Test-Path -LiteralPath $schemaPath -PathType Leaf) `
    'The installed-kernel-execution schema must exist.'

$valid = [ordered]@{
    schemaVersion = 1
    evidenceKind = 'stage5-installed-kernel-execution'
    producer = 'installed-runtime-kernel-execution-projector-v1'
    status = 'passed'
    recordedUtc = '2026-09-04T12:00:00.0000000Z'
    cohortNonce = '10000000-0000-4000-8000-000000000001'
    cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
    qualificationMode = 'InstalledKernelExecution'
    acceptanceScope = 'kernel-execution-only'
    finalAcceptanceClaim = $false
    performanceScalingClaim = $false
    sourceCommit = 'a' * 40
    artifactSetSha256 = 'CC' * 32
    runtimeClosure = [ordered]@{
        dependencyManifestSha256 = 'DD' * 32
        closureSha256 = 'EE' * 32
    }
    titleProofs = @(
        (New-TestTitleProof 'Generals')
        (New-TestTitleProof 'ZeroHour')
    )
}

Assert-True (Test-DocumentAgainstSchema $valid $schemaPath) `
    'The exact two-title non-scaling installed-kernel contract must validate.'

$mutations = @(
    [ordered]@{
        name = 'a scaling claim'
        edit = { param($d) $d.performanceScalingClaim = $true }
    },
    [ordered]@{
        name = 'a final-acceptance claim'
        edit = { param($d) $d.finalAcceptanceClaim = $true }
    },
    [ordered]@{
        name = 'a local smoke relabelled as kernel authority'
        edit = { param($d) $d.qualificationMode = 'LocalCapacitySmoke' }
    },
    [ordered]@{
        name = 'single-title coverage'
        edit = { param($d) $d.titleProofs = @($d.titleProofs[0]) }
    },
    [ordered]@{
        name = 'reversed title coverage'
        edit = { param($d) $d.titleProofs = @($d.titleProofs[1], $d.titleProofs[0]) }
    },
    [ordered]@{
        name = 'a non-dense fixture'
        edit = { param($d) $d.titleProofs[0].fixture.id = 'one-thousand-units' }
    },
    [ordered]@{
        name = 'a replay without native fixture-production evidence'
        edit = { param($d) $d.titleProofs[0].PSObject.Properties.Remove('fixtureProduction') }
    },
    [ordered]@{
        name = 'fixture production without an independently reviewed map manifest'
        edit = { param($d) $d.titleProofs[0].fixtureProduction.PSObject.Properties.Remove('reviewedFixtureManifest') }
    },
    [ordered]@{
        name = 'a synthetic or failed fixture process'
        edit = { param($d) $d.titleProofs[0].fixtureProduction.process.exitCode = 1 }
    },
    [ordered]@{
        name = 'fixture production outside the shared execution cohort'
        edit = { param($d) $d.titleProofs[0].fixtureProduction.PSObject.Properties.Remove('cohortNonce') }
    },
    [ordered]@{
        name = 'a UUID substituted for the native recorder nonce'
        edit = { param($d) $d.titleProofs[0].fixtureProduction.nativeRunNonce =
            '30000000-0000-4000-8000-000000000001' }
    },
    [ordered]@{
        name = 'an unclosed fixture profile lifecycle'
        edit = { param($d) $d.titleProofs[0].fixtureProduction.lifecycle.profileRemoved = $false }
    },
    [ordered]@{
        name = 'a lane above or outside the local contract'
        edit = { param($d) $d.titleProofs[0].schedule.lane = 'physical-8' }
    },
    [ordered]@{
        name = 'fewer than three measured executions'
        edit = { param($d) $d.titleProofs[0].runs = @($d.titleProofs[0].runs[0], $d.titleProofs[0].runs[1]) }
    },
    [ordered]@{
        name = 'owner-only kernel work'
        edit = { param($d) $d.titleProofs[0].runs[0].kernels[0].worker.physicalWorkerJobs = 0 }
    },
    [ordered]@{
        name = 'an uncommitted kernel stream'
        edit = { param($d) $d.titleProofs[0].runs[0].kernels[0].streams[0].committedBatches = 0 }
    },
    [ordered]@{
        name = 'a serial fallback'
        edit = { param($d) $d.titleProofs[0].runs[0].scheduler.serialFallbackCount = 1 }
    },
    [ordered]@{
        name = 'a Windows alternate-data-stream path'
        edit = { param($d) $d.titleProofs[0].runs[0].rawLog.path =
            'Titles/Generals/dense-1/raw.log:forged' }
    }
)

foreach ($mutation in $mutations) {
    $changed = Copy-TestDocument $valid
    & $mutation.edit $changed
    Assert-True (-not (Test-DocumentAgainstSchema $changed $schemaPath)) `
        "The schema must reject $($mutation.name)."
}

Write-Output 'Stage 5 installed kernel execution schema tests passed.'
