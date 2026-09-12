/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"
#include <chrono>
#include <thread>
#endif
#include "../TestSupport/LocalCapacityTestLane.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#if defined(NDEBUG)
#error Physics integration kernel tests require active assertions.
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
#include <xmmintrin.h>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#endif

namespace
{
rts::PhysicsIntegrationSnapshot MakeSnapshot(unsigned ordinal)
{
	rts::PhysicsIntegrationSnapshot snapshot;
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.frame = 900;
	snapshot.worldEpoch = 7;
	snapshot.objectID = ordinal + 1;
	snapshot.motionGeneration = 10 + ordinal;
	snapshot.physicsGeneration = 20 + ordinal;
	snapshot.wakePriority = (snapshot.frame << 2) | 1;
	snapshot.heapOrdinal = ordinal * 3 + 1;
	snapshot.flags = rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	snapshot.matrix[0] = 0.93629336f;
	snapshot.matrix[1] = -0.27509585f;
	snapshot.matrix[2] = 0.21835066f;
	snapshot.matrix[3] = 100.0f + ordinal;
	snapshot.matrix[4] = 0.28962949f;
	snapshot.matrix[5] = 0.95642507f;
	snapshot.matrix[6] = -0.03695701f;
	snapshot.matrix[7] = -50.0f + ordinal * 0.25f;
	snapshot.matrix[8] = -0.19866933f;
	snapshot.matrix[9] = 0.09784339f;
	snapshot.matrix[10] = 0.97517033f;
	snapshot.matrix[11] = 12.0f;
	snapshot.position[0] = snapshot.matrix[3];
	snapshot.position[1] = snapshot.matrix[7];
	snapshot.position[2] = snapshot.matrix[11];
	snapshot.acceleration[0] = 0.21f + ordinal * 0.0001f;
	snapshot.acceleration[1] = -0.17f;
	snapshot.acceleration[2] = 0.03f;
	snapshot.velocity[0] = 1.75f;
	snapshot.velocity[1] = -0.625f - ordinal * 0.0002f;
	snapshot.velocity[2] = 0.045f;
	snapshot.yawRate = 0.013f;
	snapshot.rollRate = -0.009f;
	snapshot.pitchRate = 0.017f;
	snapshot.gravity = -0.03f;
	snapshot.mass = 3.5f;
	snapshot.forwardFriction = 0.06f;
	snapshot.lateralFriction = 0.15f;
	snapshot.aerodynamicFriction = 0.01f;
	snapshot.pitchRollYawFactor = 0.8f;
	snapshot.centerOfMassOffset = 0.35f;
	snapshot.directionX = 0.93937272f;
	snapshot.directionY = 0.34289780f;
	if ((ordinal & 1) != 0)
		snapshot.flags |= rts::PHYSICS_INTEGRATION_MOTIVE;
	if ((ordinal & 2) != 0)
		snapshot.flags |= rts::PHYSICS_INTEGRATION_BRAKING;
	if ((ordinal & 4) != 0)
		snapshot.flags |= rts::PHYSICS_INTEGRATION_PROJECTILE;
	if ((ordinal & 8) != 0)
		snapshot.flags |=
			rts::PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE;
	if ((ordinal & 16) != 0)
		snapshot.flags |=
			rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN;
	return snapshot;
}

bool SameBytes(const void *left, const void *right, unsigned byteCount)
{
	return memcmp(left, right, byteCount) == 0;
}

#if defined(_WIN32) && !defined(_WIN64) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300) && \
	((defined(_MSC_VER) && defined(_M_IX86)) || \
		(defined(__GNUC__) && defined(__i386__)))
unsigned short ReadDirectX87ControlWord()
{
	unsigned short controlWord = 0;
#if defined(_MSC_VER)
	__asm { fnstcw [controlWord] } // portability-audit: x87-control-word
#else
	__asm__ __volatile__("fnstcw %0" : "=m"(controlWord)); // portability-audit: x87-control-word
#endif
	return controlWord;
}

void WriteDirectX87ControlWord(unsigned short controlWord)
{
#if defined(_MSC_VER)
	__asm { fldcw [controlWord] } // portability-audit: x87-control-word
#else
	__asm__ __volatile__("fldcw %0" : : "m"(controlWord)); // portability-audit: x87-control-word
#endif
}

unsigned short PerturbDirectX87ControlWord(unsigned short controlWord)
{
	const unsigned short modeMask = 0x1f3f;
	const unsigned short precision =
		(controlWord & 0x0300) == 0 ? 0x0200 : 0;
	const unsigned short rounding =
		(controlWord & 0x0c00) == 0x0400 ? 0x0800 : 0x0400;
	// Toggle only invalid-operation masking so precision remains masked while
	// the control-word API implementation executes any inexact arithmetic.
	const unsigned short exceptions =
		static_cast<unsigned short>((controlWord & 0x003f) ^ 0x0001);
	const unsigned short infinity =
		(controlWord & 0x1000) == 0 ? 0x1000 : 0;
	return static_cast<unsigned short>((controlWord & ~modeMask) |
		precision | rounding | exceptions | infinity);
}
#endif

void OracleRotateX(float *matrix, float theta)
{
	const float sine = sinf(theta);
	const float cosine = cosf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		const float first = matrix[base + 1];
		const float second = matrix[base + 2];
		matrix[base + 1] = (float)(cosine * first + sine * second);
		matrix[base + 2] = (float)(-sine * first + cosine * second);
	}
}

void OracleRotateY(float *matrix, float theta)
{
	const float sine = sinf(theta);
	const float cosine = cosf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		const float first = matrix[base];
		const float second = matrix[base + 2];
		matrix[base] = (float)(cosine * first - sine * second);
		matrix[base + 2] = (float)(sine * first + cosine * second);
	}
}

void OracleRotateZ(float *matrix, float theta)
{
	const float cosine = cosf(theta);
	const float sine = sinf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		const float first = matrix[base];
		const float second = matrix[base + 1];
		matrix[base] = (float)(cosine * first + sine * second);
		matrix[base + 1] = (float)(-sine * first + cosine * second);
	}
}

bool ComputeIndependentLegacyOracle(
	const rts::PhysicsIntegrationSnapshot &snapshot,
	rts::PhysicsIntegrationOutput &output)
{
	if (!rts::ValidatePhysicsIntegrationSnapshot(snapshot))
		return false;
	memset(&output, 0, sizeof(output));
	output.frame = snapshot.frame;
	output.worldEpoch = snapshot.worldEpoch;
	output.objectID = snapshot.objectID;
	output.motionGeneration = snapshot.motionGeneration;
	output.physicsGeneration = snapshot.physicsGeneration;
	output.wakePriority = snapshot.wakePriority;
	output.heapOrdinal = snapshot.heapOrdinal;
	output.flags = snapshot.flags;
	memcpy(output.matrix, snapshot.matrix, sizeof(output.matrix));
	memcpy(output.acceleration, snapshot.acceleration, sizeof(output.acceleration));
	memcpy(output.velocity, snapshot.velocity, sizeof(output.velocity));
	output.yawRate = snapshot.yawRate;
	output.rollRate = snapshot.rollRate;
	output.pitchRate = snapshot.pitchRate;

	output.acceleration[2] += snapshot.gravity;
	const bool usesGroundFriction =
		(snapshot.flags & rts::PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE) != 0 ||
		(snapshot.flags & rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN) == 0;
	if (usesGroundFriction)
	{
		const float damping = 1.0f - 0.15f;
		output.pitchRate *= damping;
		output.rollRate *= damping;
		output.yawRate *= damping;
		if (snapshot.velocity[0] || snapshot.velocity[1])
		{
			const float sideX = -snapshot.directionY;
			const float sideDot = snapshot.velocity[0] * sideX +
				snapshot.velocity[1] * snapshot.directionX;
			const float sideVelocityX = sideDot * sideX;
			const float sideVelocityY = sideDot * snapshot.directionX;
			const float lateralForce = snapshot.mass * snapshot.lateralFriction;
			float forceX = -(lateralForce * sideVelocityX);
			float forceY = -(lateralForce * sideVelocityY);
			const bool motive =
				(snapshot.flags & rts::PHYSICS_INTEGRATION_MOTIVE) != 0;
			if (!motive)
			{
				const float forwardDot =
					snapshot.velocity[0] * snapshot.directionX +
					snapshot.velocity[1] * snapshot.directionY;
				const float forwardVelocityX = forwardDot * snapshot.directionX;
				const float forwardVelocityY = forwardDot * snapshot.directionY;
				const float forwardForce = snapshot.mass * snapshot.forwardFriction;
				forceX += -(forwardForce * forwardVelocityX);
				forceY += -(forwardForce * forwardVelocityY);
			}
			float acceptedForceX = forceX;
			float acceptedForceY = forceY;
			if (motive)
			{
				const float projected = forceX * sideX +
					forceY * snapshot.directionX;
				acceptedForceX = projected * sideX;
				acceptedForceY = projected * snapshot.directionX;
			}
			const float inverseMass = 1.0f / snapshot.mass;
			output.acceleration[0] += acceptedForceX * inverseMass;
			output.acceleration[1] += acceptedForceY * inverseMass;
			output.acceleration[2] += 0.0f * inverseMass;
		}
	}
	else
	{
		const float aerodynamics = -snapshot.aerodynamicFriction;
		output.acceleration[0] += snapshot.velocity[0] * aerodynamics;
		output.acceleration[1] += snapshot.velocity[1] * aerodynamics;
		output.acceleration[2] += snapshot.velocity[2] * aerodynamics;
		const float damping = 1.0f + aerodynamics;
		output.pitchRate *= damping;
		output.rollRate *= damping;
		output.yawRate *= damping;
	}
	if (output.pitchRate != 0.0f || output.rollRate != 0.0f ||
		output.yawRate != 0.0f)
		output.flags |= rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	else
		output.flags &= ~rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;

	output.velocity[0] += output.acceleration[0];
	output.velocity[1] += output.acceleration[1];
	output.velocity[2] += output.acceleration[2];
	if (fabsf(output.velocity[0]) < 0.001f) output.velocity[0] = 0.0f;
	if (fabsf(output.velocity[1]) < 0.001f) output.velocity[1] = 0.0f;
	if (fabsf(output.velocity[2]) < 0.001f) output.velocity[2] = 0.0f;

	if ((snapshot.flags & rts::PHYSICS_INTEGRATION_BRAKING) != 0)
	{
		if ((snapshot.flags & rts::PHYSICS_INTEGRATION_PROJECTILE) == 0)
			output.matrix[11] += output.velocity[2];
	}
	else
	{
		output.matrix[3] += output.velocity[0];
		output.matrix[7] += output.velocity[1];
		output.matrix[11] += output.velocity[2];
	}

	if ((output.flags & rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) != 0)
	{
		const float yaw = output.yawRate * snapshot.pitchRollYawFactor;
		float pitch = output.pitchRate * snapshot.pitchRollYawFactor;
		const float roll = output.rollRate * snapshot.pitchRollYawFactor;
		if (snapshot.centerOfMassOffset != 0.0f)
		{
			const float xy = sqrtf(output.matrix[0] * output.matrix[0] +
				output.matrix[4] * output.matrix[4]);
			const float pitchAngle = (float)atan2(output.matrix[8], xy);
			const float halfPi = 3.14159265359f / 2;
			const float remaining = snapshot.centerOfMassOffset > 0.0f ?
				(halfPi - pitchAngle) : (-halfPi + pitchAngle);
			pitch *= sinf(remaining);
		}
		OracleRotateX(output.matrix, roll);
		OracleRotateY(output.matrix, pitch);
		OracleRotateZ(output.matrix, yaw);
	}
	return rts::ValidatePhysicsIntegrationOutput(snapshot, output);
}


void FillSentinel(std::vector<rts::PhysicsIntegrationOutput> &outputs)
{
	memset(&outputs[0], 0xa5,
		outputs.size() * sizeof(rts::PhysicsIntegrationOutput));
}

void TestScalarByteAndFieldParity()
{
	for (unsigned ordinal = 0; ordinal != 64; ++ordinal)
	{
		rts::PhysicsIntegrationSnapshot snapshot = MakeSnapshot(ordinal);
		rts::PhysicsIntegrationOutput first;
		rts::PhysicsIntegrationOutput second;
		assert(rts::ComputePhysicsIntegrationPrefix(snapshot, first));
		assert(ComputeIndependentLegacyOracle(snapshot, second));
		assert(SameBytes(&first, &second, sizeof(first)));
		unsigned firstField = ~0u;
		assert(rts::PhysicsIntegrationOutputsEqual(first, second, &firstField));
		assert(rts::ValidatePhysicsIntegrationOutput(snapshot, first));
		assert(first.frame == snapshot.frame);
		assert(first.worldEpoch == snapshot.worldEpoch);
		assert(first.objectID == snapshot.objectID);
		assert(first.heapOrdinal == snapshot.heapOrdinal);

		rts::PhysicsIntegrationOutput changed = first;
		changed.velocity[1] = -changed.velocity[1];
		assert(!rts::PhysicsIntegrationOutputsEqual(first, changed, &firstField));
	}
}

#if defined(_WIN32) && !defined(_WIN64) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300) && \
	((defined(_MSC_VER) && defined(_M_IX86)) || \
		(defined(__GNUC__) && defined(__i386__)))
void TestNestedFloatingPointScopeRestoration()
{
	const unsigned short baselineX87 = ReadDirectX87ControlWord();
	const unsigned baselineMxcsr = _mm_getcsr();
	const unsigned short firstX87 =
		PerturbDirectX87ControlWord(baselineX87);
	const unsigned short secondX87 =
		PerturbDirectX87ControlWord(firstX87);
	const unsigned firstMxcsr =
		(baselineMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_DOWN;
	const unsigned secondMxcsr =
		(baselineMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_UP;
	assert(firstX87 != baselineX87);
	assert(secondX87 != firstX87);
	assert((firstX87 & 0x0300) != (baselineX87 & 0x0300));
	assert((firstX87 & 0x0c00) != (baselineX87 & 0x0c00));
	assert((firstX87 & 0x003f) != (baselineX87 & 0x003f));
	assert((firstX87 & 0x1000) != (baselineX87 & 0x1000));
	WriteDirectX87ControlWord(firstX87);
	_mm_setcsr(firstMxcsr);
	const rts::JobFloatingPointState firstState;
	WriteDirectX87ControlWord(secondX87);
	_mm_setcsr(secondMxcsr);
	const rts::JobFloatingPointState secondState;
	WriteDirectX87ControlWord(baselineX87);
	_mm_setcsr(baselineMxcsr);
	{
		rts::JobFloatingPointScope firstScope(firstState);
		assert(ReadDirectX87ControlWord() == firstX87);
		assert(_mm_getcsr() == firstMxcsr);
		{
			rts::JobFloatingPointScope secondScope(secondState);
			assert(ReadDirectX87ControlWord() == secondX87);
			assert(_mm_getcsr() == secondMxcsr);
		}
		assert(ReadDirectX87ControlWord() == firstX87);
		assert(_mm_getcsr() == firstMxcsr);
	}
	assert(ReadDirectX87ControlWord() == baselineX87);
	assert(_mm_getcsr() == baselineMxcsr);
}
#endif

void TestLoadedTransportMassUsesLegacyOracle()
{
	rts::PhysicsIntegrationSnapshot loaded = MakeSnapshot(5);
	const float baseVehicleMass = 3.125f;
	const float containedItemsMass = 7.375f;
	loaded.mass = baseVehicleMass + containedItemsMass;
	rts::PhysicsIntegrationOutput prepared;
	rts::PhysicsIntegrationOutput legacy;
	assert(rts::ComputePhysicsIntegrationPrefix(loaded, prepared));
	assert(ComputeIndependentLegacyOracle(loaded, legacy));
	assert(SameBytes(&prepared, &legacy, sizeof(prepared)));

	rts::PhysicsIntegrationSnapshot unloaded = loaded;
	unloaded.mass = baseVehicleMass;
	rts::PhysicsIntegrationOutput rawMassResult;
	assert(rts::ComputePhysicsIntegrationPrefix(unloaded, rawMassResult));
	assert(!SameBytes(&prepared, &rawMassResult, sizeof(prepared)));
}

void TestBoundedOwnerIndexWithAdversarialSparseIDs()
{
	const unsigned count = 4096;
	const unsigned legacyHashCapacity = 8192;
	const unsigned lowBits = 17;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOwnerIndexEntry> entries(count);
	for (unsigned inputIndex = 0; inputIndex != count; ++inputIndex)
	{
		snapshots[inputIndex] = MakeSnapshot(inputIndex);
		const unsigned sparseOrdinal = (inputIndex * 4051u) & (count - 1);
		snapshots[inputIndex].objectID = lowBits +
			sparseOrdinal * legacyHashCapacity;
		assert((snapshots[inputIndex].objectID &
			(legacyHashCapacity - 1)) == lowBits);
	}
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	assert(rts::BuildPhysicsIntegrationOwnerIndex(&snapshots[0], count,
		&entries[0], count));
	for (unsigned sortedIndex = 1; sortedIndex != count; ++sortedIndex)
		assert(entries[sortedIndex - 1].objectID < entries[sortedIndex].objectID);
	for (unsigned lookupIndex = count; lookupIndex != 0; --lookupIndex)
	{
		const unsigned originalIndex = lookupIndex - 1;
		unsigned batchIndex = ~0u;
		assert(rts::FindPhysicsIntegrationOwnerIndex(&entries[0], count,
			snapshots[originalIndex].objectID, &batchIndex));
		assert(batchIndex == originalIndex);
		assert(snapshots[batchIndex].heapOrdinal ==
			MakeSnapshot(originalIndex).heapOrdinal);
	}
	unsigned missingIndex = ~0u;
	assert(!rts::FindPhysicsIntegrationOwnerIndex(&entries[0], count,
		lowBits + count * legacyHashCapacity, &missingIndex));
	const rts::PhysicsIntegrationRuntimeMetrics metrics =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	assert(metrics.acceptedBatches == 0 && metrics.acceptedPrefixes == 0 &&
		metrics.unexpectedFallbacks == 0);

	snapshots[1].objectID = snapshots[0].objectID;
	assert(!rts::BuildPhysicsIntegrationOwnerIndex(&snapshots[0], count,
		&entries[0], count));
}

void TestDampingFlagAndSleepyHeapParity()
{
	rts::PhysicsIntegrationSnapshot zeroDamping = MakeSnapshot(41);
	zeroDamping.flags |= rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW |
		rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN;
	zeroDamping.flags &=
		~rts::PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE;
	zeroDamping.aerodynamicFriction = 1.0f;
	rts::PhysicsIntegrationOutput prepared;
	rts::PhysicsIntegrationOutput legacy;
	assert(rts::ComputePhysicsIntegrationPrefix(zeroDamping, prepared));
	assert(ComputeIndependentLegacyOracle(zeroDamping, legacy));
	assert(SameBytes(&prepared, &legacy, sizeof(prepared)));
	assert((prepared.flags &
		rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) == 0);
	assert(prepared.pitchRate == 0.0f && prepared.rollRate == 0.0f &&
		prepared.yawRate == 0.0f);
	assert(prepared.heapOrdinal == zeroDamping.heapOrdinal);
	assert(rts::ValidatePhysicsIntegrationCommit(zeroDamping, zeroDamping,
		prepared, true, true, true));
	rts::PhysicsIntegrationOutput staleFlag = prepared;
	staleFlag.flags |= rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	assert(!rts::ValidatePhysicsIntegrationOutput(zeroDamping, staleFlag));
	assert(!rts::PhysicsIntegrationOutputsEqual(prepared, staleFlag));

	rts::PhysicsIntegrationSnapshot sleepyCandidate = zeroDamping;
	sleepyCandidate.gravity = 0.0f;
	memset(sleepyCandidate.acceleration, 0,
		sizeof(sleepyCandidate.acceleration));
	memset(sleepyCandidate.velocity, 0, sizeof(sleepyCandidate.velocity));
	rts::PhysicsIntegrationOutput sleepyPrepared;
	rts::PhysicsIntegrationOutput sleepyLegacy;
	assert(rts::ComputePhysicsIntegrationPrefix(sleepyCandidate,
		sleepyPrepared));
	assert(ComputeIndependentLegacyOracle(sleepyCandidate, sleepyLegacy));
	assert(SameBytes(&sleepyPrepared, &sleepyLegacy,
		sizeof(sleepyPrepared)));
	const bool preparedSleepGate = sleepyPrepared.velocity[0] == 0.0f &&
		sleepyPrepared.velocity[1] == 0.0f && sleepyPrepared.velocity[2] == 0.0f &&
		sleepyPrepared.acceleration[0] == 0.0f &&
		sleepyPrepared.acceleration[1] == 0.0f &&
		sleepyPrepared.acceleration[2] == 0.0f &&
		(sleepyPrepared.flags &
		 rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) == 0;
	const bool legacySleepGate = sleepyLegacy.velocity[0] == 0.0f &&
		sleepyLegacy.velocity[1] == 0.0f && sleepyLegacy.velocity[2] == 0.0f &&
		sleepyLegacy.acceleration[0] == 0.0f &&
		sleepyLegacy.acceleration[1] == 0.0f &&
		sleepyLegacy.acceleration[2] == 0.0f &&
		(sleepyLegacy.flags &
		 rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) == 0;
	assert(preparedSleepGate && legacySleepGate);
	assert(sleepyPrepared.heapOrdinal == sleepyCandidate.heapOrdinal);

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	const unsigned savedMxcsr = _mm_getcsr();
	_mm_setcsr(savedMxcsr | _MM_FLUSH_ZERO_ON);
	unsigned minimumNormalBits = 0x00800000u;
	float minimumNormal;
	memcpy(&minimumNormal, &minimumNormalBits, sizeof(minimumNormal));
	const unsigned count = 65;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> expected(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned snapshotIndex = 0; snapshotIndex != count; ++snapshotIndex)
	{
		snapshots[snapshotIndex] = MakeSnapshot(snapshotIndex);
		snapshots[snapshotIndex].flags |=
			rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
		snapshots[snapshotIndex].flags &=
			~rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN;
		snapshots[snapshotIndex].pitchRate = minimumNormal;
		snapshots[snapshotIndex].rollRate = -minimumNormal;
		snapshots[snapshotIndex].yawRate = minimumNormal;
		assert(ComputeIndependentLegacyOracle(snapshots[snapshotIndex],
			expected[snapshotIndex]));
		assert((expected[snapshotIndex].flags &
			rts::PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) == 0);
	}
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 32;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	assert(jobs.workerCount() == config.workerCount);
#endif
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options) ==
		rts::PHYSICS_INTEGRATION_PARALLEL);
	for (unsigned outputIndex = 0; outputIndex != count; ++outputIndex)
	{
		assert(SameBytes(&outputs[outputIndex], &expected[outputIndex],
			sizeof(outputs[outputIndex])));
		assert(outputs[outputIndex].heapOrdinal ==
			snapshots[outputIndex].heapOrdinal);
	}
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	_mm_setcsr(savedMxcsr);
#endif
}

void TestOwnerGenerationAndHeapValidationFields()
{
	rts::PhysicsIntegrationSnapshot captured = MakeSnapshot(9);
	rts::PhysicsIntegrationSnapshot current = captured;
	unsigned firstField = ~0u;
	assert(rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));

	current.motionGeneration++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.physicsGeneration++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.worldEpoch++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.objectID++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.wakePriority++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.heapOrdinal++;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.matrix[7] += 1.0f;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
	current = captured;
	current.acceleration[0] += 1.0f;
	assert(!rts::PhysicsIntegrationSnapshotsEqual(captured, current, &firstField));
}

void TestOwnerCommitGateMutationDestructionAndReset()
{
	rts::PhysicsIntegrationSnapshot captured = MakeSnapshot(12);
	rts::PhysicsIntegrationSnapshot current = captured;
	rts::PhysicsIntegrationOutput output;
	assert(rts::ComputePhysicsIntegrationPrefix(captured, output));
	assert(rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, true, true));

	current.motionGeneration++;
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, true, true));
	current = captured;
	current.physicsGeneration++;
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, true, true));
	current = captured;
	current.worldEpoch++;
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, true, true));
	current = captured;
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		false, true, true));
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, false, true));
	assert(!rts::ValidatePhysicsIntegrationCommit(captured, current, output,
		true, true, false));
}

void RunWorkerCount(unsigned workerCount)
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = workerCount;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	const unsigned actualWorkerCount = jobs.workerCount();

	const unsigned count = 257;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> expected(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
	{
		snapshots[index] = MakeSnapshot(index);
		assert(rts::ComputePhysicsIntegrationPrefix(snapshots[index], expected[index]));
	}
	FillSentinel(outputs);
	std::vector<rts::PhysicsIntegrationOutput> sentinel = outputs;
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	rts::PhysicsIntegrationMetrics metrics;
	const rts::PhysicsIntegrationBatchResult result =
		rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
			&outputs[0], count, &scratch[0], count, options, &metrics);
#if defined(_MSC_VER) && _MSC_VER < 1300
	assert(actualWorkerCount == 1);
	if (actualWorkerCount <= 1)
#else
	assert(actualWorkerCount == workerCount);
	if (workerCount == 1)
#endif
	{
		assert(result == rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
		assert(SameBytes(&outputs[0], &sentinel[0],
			count * sizeof(rts::PhysicsIntegrationOutput)));
		assert(metrics.submittedJobs == 0);
		assert(metrics.serialFallbacks == 0);
		assert(jobs.metrics().serialFallbackCount == 0);
	}
	else
	{
		assert(result == rts::PHYSICS_INTEGRATION_PARALLEL);
		assert(metrics.rangeCount == rts::JobSystem::chooseRangeCount(
			count, options.minimumGrain, actualWorkerCount));
		const unsigned arrayBytes = metrics.rangeCount * static_cast<unsigned>(
			sizeof(rts::JobSubmission) + sizeof(rts::JobHandle) +
			sizeof(void *));
		assert(metrics.allocatedBytes > arrayBytes);
		assert(metrics.submittedJobs == metrics.rangeCount);
		assert(metrics.completedJobs == metrics.submittedJobs);
		assert(metrics.physicalWorkerJobs == metrics.completedJobs);
		assert(metrics.ownerHelpedJobs == 0);
		assert(metrics.physicalWorkerMask != 0);
		assert(metrics.distinctPhysicalWorkers != 0 &&
			metrics.distinctPhysicalWorkers <= actualWorkerCount);
		assert(metrics.peakConcurrentPhysicalWorkers != 0 &&
			metrics.peakConcurrentPhysicalWorkers <= actualWorkerCount);
		for (unsigned index = 0; index != count; ++index)
		{
			assert(SameBytes(&outputs[index], &expected[index],
				sizeof(rts::PhysicsIntegrationOutput)));
			assert(outputs[index].heapOrdinal == snapshots[index].heapOrdinal);
		}
	}

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestWorkerCountsAndOwnerFloatingPointState(bool localCapacity)
{
	const unsigned counts[] = { 1, 2, 4, 8, 16 };
	for (unsigned index = 0; index != sizeof(counts) / sizeof(counts[0]); ++index)
	{
		const unsigned requestedWorkerCount = counts[index];
		const unsigned workerCount = rts_test::ResolveActualWorkerCount(
			requestedWorkerCount, localCapacity);
		rts_test::PrintWorkerCountSubstitution(
			"Physics integration", requestedWorkerCount, workerCount,
			localCapacity);
		RunWorkerCount(workerCount);
	}

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	const unsigned savedMxcsr = _mm_getcsr();
	_mm_setcsr((savedMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_DOWN);
	RunWorkerCount(rts_test::ResolveActualWorkerCount(4, localCapacity));
	_mm_setcsr(savedMxcsr);
#endif
}

void RunShadowWorkerCount(unsigned workerCount)
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = workerCount;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	const unsigned actualWorkerCount = jobs.workerCount();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	assert(actualWorkerCount == workerCount);
#endif

	const unsigned count = 257;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> expected(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
	{
		snapshots[index] = MakeSnapshot(index);
		assert(ComputeIndependentLegacyOracle(snapshots[index], expected[index]));
	}
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	rts::PhysicsIntegrationMetrics sliceMetrics;
	const rts::PhysicsIntegrationBatchResult result =
		rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &sliceMetrics);
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (actualWorkerCount <= 1)
	{
		assert(result == rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
		assert(sliceMetrics.rangeCount == 0);
		assert(sliceMetrics.submittedJobs == 0);
		assert(sliceMetrics.completedJobs == 0);
		jobs.shutdown();
		assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
		return;
	}
#endif
	assert(result == rts::PHYSICS_INTEGRATION_PARALLEL);
	bool matched = true;
	for (unsigned outputIndex = 0; outputIndex != count; ++outputIndex)
	{
		if (!SameBytes(&outputs[outputIndex], &expected[outputIndex],
			sizeof(outputs[outputIndex])))
		{
			matched = false;
			break;
		}
	}
	assert(matched);
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	rts::RecordPhysicsIntegrationShadow(matched, count, sliceMetrics);
	const rts::PhysicsIntegrationRuntimeMetrics runtime =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	assert(runtime.acceptedBatches == 0);
	assert(runtime.acceptedPrefixes == 0);
	assert(runtime.shadowBatches == 1);
	assert(runtime.shadowPrefixes == count);
	assert(runtime.shadowRanges == sliceMetrics.rangeCount);
	assert(runtime.shadowSubmittedJobs == sliceMetrics.submittedJobs);
	assert(runtime.shadowCompletedJobs == sliceMetrics.completedJobs);
	assert(runtime.shadowRanges > 0 && runtime.shadowSubmittedJobs > 0);
	assert(runtime.shadowSubmittedJobs == runtime.shadowCompletedJobs);
	assert(runtime.shadowMatches == 1 && runtime.shadowMismatches == 0);

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestShadowWorkAtTwoAndSixteenWorkers(bool localCapacity)
{
	const unsigned requestedWorkerCounts[] = { 2, 16 };
	for (unsigned index = 0;
		index != sizeof(requestedWorkerCounts) / sizeof(requestedWorkerCounts[0]); ++index)
	{
		const unsigned requestedWorkerCount = requestedWorkerCounts[index];
		const unsigned workerCount = rts_test::ResolveActualWorkerCount(
			requestedWorkerCount, localCapacity);
		rts_test::PrintWorkerCountSubstitution(
			"Physics integration shadow", requestedWorkerCount, workerCount,
			localCapacity);
		RunShadowWorkerCount(workerCount);
	}
}

void ExpectTransactionalFailure(rts::PhysicsIntegrationTestFault fault,
	unsigned ordinal, rts::PhysicsIntegrationBatchResult expectedResult)
{
	const unsigned count = 129;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
		snapshots[index] = MakeSnapshot(index);
	FillSentinel(outputs);
	std::vector<rts::PhysicsIntegrationOutput> sentinel = outputs;
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	options.testFault = fault;
	options.testOrdinal = ordinal;
	rts::PhysicsIntegrationMetrics metrics;
#if defined(_MSC_VER) && _MSC_VER < 1300
	const rts::PhysicsIntegrationBatchResult runtimeExpectedResult =
		rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE;
#else
	const rts::PhysicsIntegrationBatchResult runtimeExpectedResult = expectedResult;
#endif
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		runtimeExpectedResult);
	assert(SameBytes(&outputs[0], &sentinel[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
}

#if defined(RTS_BUILD_CORE_EXTRAS)
void ExpectGroupAssignmentAllocationFailure()
{
	const unsigned count = 129;
	const unsigned groupAssignmentAllocationFault = 11;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
		snapshots[index] = MakeSnapshot(index);
	FillSentinel(outputs);
	const std::vector<rts::PhysicsIntegrationOutput> sentinel = outputs;
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	rts::PhysicsIntegrationMetrics metrics;
	rts::PhysicsIntegrationBatchResult result =
		rts::PHYSICS_INTEGRATION_INVALID_INPUT;
	bool exceptionEscaped = false;
	rts_job_system_set_test_fault(groupAssignmentAllocationFault, 1);
	try
	{
		result = rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
			&outputs[0], count, &scratch[0], count, options, &metrics);
	}
	catch (...)
	{
		exceptionEscaped = true;
	}
	rts_job_system_set_test_fault(0, 0);
	assert(!exceptionEscaped);
	assert(result == rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(metrics.serialFallbacks == 1);
	assert(SameBytes(&outputs[0], &sentinel[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
}
#endif

void TestTransactionalFailurePaths()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	assert(jobs.workerCount() == config.workerCount);
#endif

	ExpectTransactionalFailure(rts::PHYSICS_INTEGRATION_TEST_ALLOCATION_FAILURE,
		0, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	ExpectTransactionalFailure(rts::PHYSICS_INTEGRATION_TEST_GROUP_FAILURE,
		0, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	for (unsigned ordinal = 0; ordinal != 4; ++ordinal)
	{
		ExpectTransactionalFailure(
			rts::PHYSICS_INTEGRATION_TEST_JOB_ALLOCATION_FAILURE,
			ordinal, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
		ExpectTransactionalFailure(
			rts::PHYSICS_INTEGRATION_TEST_ADMISSION_FAILURE,
			ordinal, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
		ExpectTransactionalFailure(
			rts::PHYSICS_INTEGRATION_TEST_WORKER_FAILURE,
			ordinal, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
		ExpectTransactionalFailure(
			rts::PHYSICS_INTEGRATION_TEST_NONFINITE_OUTPUT,
			ordinal, rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	}
	ExpectTransactionalFailure(
		rts::PHYSICS_INTEGRATION_TEST_CANCEL_AFTER_ADMISSION,
		0, rts::PHYSICS_INTEGRATION_CANCELLED);
	ExpectTransactionalFailure(
		rts::PHYSICS_INTEGRATION_TEST_PHYSICAL_WAIT_TIMEOUT,
		0, rts::PHYSICS_INTEGRATION_CANCELLED);
#if defined(RTS_BUILD_CORE_EXTRAS)
	ExpectGroupAssignmentAllocationFailure();
#endif

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestBelowGrainSlicesAreNotSchedulerFallbacks()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	assert(jobs.workerCount() == config.workerCount);
#endif
	jobs.resetMetrics();
	for (unsigned count = 1; count != 64; ++count)
	{
		std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
		std::vector<rts::PhysicsIntegrationOutput> outputs(count);
		std::vector<rts::PhysicsIntegrationOutput> scratch(count);
		for (unsigned snapshotIndex = 0; snapshotIndex != count; ++snapshotIndex)
			snapshots[snapshotIndex] = MakeSnapshot(snapshotIndex);
		FillSentinel(outputs);
		std::vector<rts::PhysicsIntegrationOutput> sentinel = outputs;
		rts::PhysicsIntegrationOptions options;
		rts::PhysicsIntegrationMetrics metrics;
		assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
			&outputs[0], count, &scratch[0], count, options, &metrics) ==
			rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
		assert(metrics.serialFallbacks == 0);
		assert(metrics.submittedJobs == 0);
		assert(SameBytes(&outputs[0], &sentinel[0],
			count * sizeof(rts::PhysicsIntegrationOutput)));
	}
	assert(jobs.metrics().serialFallbackCount == 0);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

class WorkerClassificationProbe : public rts::Job
{
public:
	WorkerClassificationProbe(const rts::PhysicsIntegrationSnapshot *snapshots,
		unsigned count, rts::PhysicsIntegrationOutput *outputs,
		rts::PhysicsIntegrationOutput *scratch,
		rts::PhysicsIntegrationBatchResult *result, unsigned *fallbacks)
		: m_snapshots(snapshots), m_count(count), m_outputs(outputs),
		  m_scratch(scratch), m_result(result), m_fallbacks(fallbacks)
	{
	}

	virtual void execute(rts::JobContext &)
	{
		rts::PhysicsIntegrationOptions options;
		options.minimumGrain = 1;
		rts::PhysicsIntegrationMetrics metrics;
		*m_result = rts::PreparePhysicsIntegrationPrefixes(m_snapshots, m_count,
			m_outputs, m_count, m_scratch, m_count, options, &metrics);
		*m_fallbacks = metrics.serialFallbacks;
	}

private:
	const rts::PhysicsIntegrationSnapshot *m_snapshots;
	unsigned m_count;
	rts::PhysicsIntegrationOutput *m_outputs;
	rts::PhysicsIntegrationOutput *m_scratch;
	rts::PhysicsIntegrationBatchResult *m_result;
	unsigned *m_fallbacks;
};

void TestPolicyIneligibleIsDistinctFromSafetyFallback()
{
	const unsigned count = 65;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
		snapshots[index] = MakeSnapshot(index);
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	rts::PhysicsIntegrationMetrics metrics;
	rts::JobSystem &jobs = rts::JobSystem::instance();

	assert(!jobs.isRunning());
	jobs.resetMetrics();
	assert(rts::PreflightPhysicsIntegrationPrefixes() ==
		rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(jobs.metrics().serialFallbackCount == 1);
	jobs.resetMetrics();
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(metrics.serialFallbacks == 1);
	assert(jobs.metrics().serialFallbackCount == 1);

	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_RENDER));
	const unsigned actualWorkerCount = jobs.workerCount();
#if defined(_MSC_VER) && _MSC_VER < 1300
	assert(actualWorkerCount == 1);
#else
	assert(actualWorkerCount == config.workerCount);
#endif
	jobs.resetMetrics();
	assert(rts::PreflightPhysicsIntegrationPrefixes() ==
		rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(jobs.metrics().serialFallbackCount == 1);
	jobs.resetMetrics();
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(metrics.serialFallbacks == 1);
	assert(jobs.metrics().serialFallbackCount == 1);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_RENDER));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (actualWorkerCount <= 1)
		assert(rts::PreflightPhysicsIntegrationPrefixes() ==
			rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
	else
		assert(rts::PreflightPhysicsIntegrationPrefixes() ==
			rts::PHYSICS_INTEGRATION_PARALLEL);
#else
	assert(rts::PreflightPhysicsIntegrationPrefixes() ==
		rts::PHYSICS_INTEGRATION_PARALLEL);
#endif

	rts::PhysicsIntegrationBatchResult workerResult =
		rts::PHYSICS_INTEGRATION_INVALID_INPUT;
	unsigned workerFallbacks = 0;
	WorkerClassificationProbe *probe = new WorkerClassificationProbe(
		&snapshots[0], count, &outputs[0], &scratch[0], &workerResult,
		&workerFallbacks);
	rts::JobGroup group = jobs.createGroup();
	jobs.resetMetrics();
	rts::JobHandle handle = jobs.trySubmit(probe,
		rts::JOB_PRIORITY_FRAME_CRITICAL, group);
	if (!handle.isValid())
		delete probe;
	assert(handle.isValid());
	assert(jobs.wait(group));
	assert(handle.succeeded());
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (actualWorkerCount <= 1)
	{
		assert(workerResult == rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
		assert(workerFallbacks == 0);
		assert(jobs.metrics().serialFallbackCount == 0);
	}
	else
	{
		assert(workerResult == rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
		assert(workerFallbacks == 1);
		assert(jobs.metrics().serialFallbackCount == 1);
	}
#else
	assert(workerResult == rts::PHYSICS_INTEGRATION_SERIAL_FALLBACK);
	assert(workerFallbacks == 1);
	assert(jobs.metrics().serialFallbackCount == 1);
#endif

	jobs.resetMetrics();
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], 1,
		&outputs[0], 1, &scratch[0], 1, rts::PhysicsIntegrationOptions(),
		&metrics) == rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
	assert(metrics.serialFallbacks == 0);
	assert(jobs.metrics().serialFallbackCount == 0);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));

	config.workerCount = 1;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	assert(jobs.workerCount() == config.workerCount);
#endif
	jobs.resetMetrics();
	assert(rts::PreflightPhysicsIntegrationPrefixes() ==
		rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
	assert(jobs.metrics().submittedJobCount == 0 &&
		jobs.metrics().executedJobCount == 0 &&
		jobs.metrics().serialFallbackCount == 0);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestInvalidAndNonfiniteInputDoesNotPublish()
{
	rts::PhysicsIntegrationSnapshot snapshot = MakeSnapshot(0);
	rts::PhysicsIntegrationOutput output;
	rts::PhysicsIntegrationOutput scratch;
	memset(&output, 0xa5, sizeof(output));
	rts::PhysicsIntegrationOutput sentinel = output;
	rts::PhysicsIntegrationOptions options;
	snapshot.velocity[0] = static_cast<float>(HUGE_VAL);
	assert(!rts::ComputePhysicsIntegrationPrefix(snapshot, scratch));
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshot, 1, &output, 1,
		&scratch, 1, options) == rts::PHYSICS_INTEGRATION_INVALID_INPUT);
	assert(SameBytes(&output, &sentinel, sizeof(output)));

	snapshot = MakeSnapshot(0);
	snapshot.flags |= 1u << 31;
	assert(!rts::ValidatePhysicsIntegrationSnapshot(snapshot));
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshot, 1, &output, 1,
		&scratch, 1, options) == rts::PHYSICS_INTEGRATION_INVALID_INPUT);
	assert(SameBytes(&output, &sentinel, sizeof(output)));
}

void TestRuntimeMetricsCountOnlyExplicitEvents()
{
	const rts::PhysicsIntegrationRuntimeMetrics beforeReset =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	rts::PhysicsIntegrationMetrics sliceMetrics;
	sliceMetrics.rangeCount = 4;
	sliceMetrics.submittedJobs = 4;
	sliceMetrics.completedJobs = 4;
	sliceMetrics.physicalWorkerJobs = 4;
	sliceMetrics.ownerHelpedJobs = 0;
	sliceMetrics.physicalWorkerMask = 0xf;
	sliceMetrics.distinctPhysicalWorkers = 4;
	sliceMetrics.peakConcurrentPhysicalWorkers = 3;
	sliceMetrics.allocatedBytes = 4096;
	sliceMetrics.captureNanoseconds = 101;
	sliceMetrics.prepareNanoseconds = 202;
	sliceMetrics.waitNanoseconds = 303;
	sliceMetrics.commitNanoseconds = 404;
	sliceMetrics.storageBytes = 8192;
	sliceMetrics.storageCapacityBytes = 16384;
	sliceMetrics.storageAllocations = 1;
	rts::RecordPhysicsIntegrationAuthoritativeSlice(17, sliceMetrics);
	rts::RecordPhysicsIntegrationShadow(true, 17, sliceMetrics);
	rts::RecordPhysicsIntegrationShadow(false, 17, sliceMetrics);
	rts::RecordPhysicsIntegrationOwnerFallback(false);
	rts::RecordPhysicsIntegrationOwnerFallback(true);
	rts::RecordPhysicsIntegrationIneligibleSlice();
	rts::RecordPhysicsIntegrationUnexpectedFallback();
	rts::RecordPhysicsIntegrationCircuitBreakerTrip();
	const rts::PhysicsIntegrationRuntimeMetrics metrics =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	assert(metrics.resetEpoch == beforeReset.resetEpoch + 1);
	assert(metrics.acceptedBatches == 1);
	assert(metrics.acceptedPrefixes == 17);
	assert(metrics.acceptedRanges == 4);
	assert(metrics.acceptedSubmittedJobs == 4);
	assert(metrics.acceptedCompletedJobs == 4);
	assert(metrics.acceptedPhysicalWorkerJobs == 4);
	assert(metrics.acceptedOwnerHelpedJobs == 0);
	assert(metrics.acceptedPhysicalWorkerMask == 0xf);
	assert(metrics.maximumAcceptedDistinctPhysicalWorkers == 4);
	assert(metrics.maximumAcceptedPeakConcurrentPhysicalWorkers == 3);
	assert(metrics.acceptedAllocatedBytes == 4096);
	assert(metrics.acceptedCaptureNanoseconds == 101);
	assert(metrics.acceptedPrepareNanoseconds == 202);
	assert(metrics.acceptedWaitNanoseconds == 303);
	assert(metrics.acceptedCommitNanoseconds == 404);
	assert(metrics.acceptedStorageBytes == 8192);
	assert(metrics.acceptedStorageCapacityBytes == 16384);
	assert(metrics.acceptedStorageAllocations == 1);
	assert(metrics.shadowBatches == 2);
	assert(metrics.shadowPrefixes == 34);
	assert(metrics.shadowRanges == 8);
	assert(metrics.shadowSubmittedJobs == 8);
	assert(metrics.shadowCompletedJobs == 8);
	assert(metrics.shadowMatches == 1);
	assert(metrics.shadowMismatches == 1);
	assert(metrics.ownerFallbacks == 2);
	assert(metrics.ineligibleSlices == 1);
	assert(metrics.unexpectedFallbacks == 1);
	assert(metrics.staleRejections == 1);
	assert(metrics.circuitBreakerTrips == 1);
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	rts::PhysicsIntegrationMetrics highCoreMetrics;
	highCoreMetrics.rangeCount = 65;
	highCoreMetrics.submittedJobs = 65;
	highCoreMetrics.completedJobs = 65;
	highCoreMetrics.physicalWorkerJobs = 65;
	highCoreMetrics.physicalWorkerMask = ~static_cast<
		rts::PhysicsIntegrationMetricCounter>(0);
	highCoreMetrics.distinctPhysicalWorkers = 65;
	highCoreMetrics.physicalWorkerMaskComplete = false;
	highCoreMetrics.peakConcurrentPhysicalWorkers = 65;
	rts::RecordPhysicsIntegrationAuthoritativeSlice(65, highCoreMetrics);
	const rts::PhysicsIntegrationRuntimeMetrics highCoreRuntime =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	assert(highCoreRuntime.acceptedBatches == 1);
	assert(highCoreRuntime.maximumAcceptedDistinctPhysicalWorkers == 65);
	assert(!highCoreRuntime.acceptedPhysicalWorkerMaskComplete);
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	const rts::PhysicsIntegrationRuntimeMetrics afterSecondReset =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	assert(afterSecondReset.resetEpoch == metrics.resetEpoch + 2);
	assert(afterSecondReset.acceptedBatches == 0);
	assert(afterSecondReset.acceptedPrefixes == 0);
	assert(afterSecondReset.acceptedPhysicalWorkerJobs == 0);
	assert(afterSecondReset.acceptedPhysicalWorkerMask == 0);
	assert(afterSecondReset.acceptedPhysicalWorkerMaskComplete);
	assert(afterSecondReset.shadowBatches == 0);
	assert(afterSecondReset.shadowPrefixes == 0);
	assert(afterSecondReset.shadowRanges == 0);
	assert(afterSecondReset.shadowSubmittedJobs == 0);
	assert(afterSecondReset.shadowCompletedJobs == 0);
	assert(afterSecondReset.unexpectedFallbacks == 0);
}

#if defined(_WIN64)
struct KernelPerformanceClock
{
	KernelPerformanceClock() : now(1000) {}
	rts::JobMetricCounter now;
	static rts::JobMetricCounter read(void *context)
	{
		KernelPerformanceClock &clock = *static_cast<KernelPerformanceClock *>(context);
		clock.now += 10;
		return clock.now;
	}
};

void TestKernelPerformanceTokenReachesPhysicsStages()
{
	const unsigned count = 64;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
		snapshots[index] = MakeSnapshot(index);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	KernelPerformanceClock clock;
	rts::performance::KernelPerformanceLedger &ledger =
		rts::performance::KernelPerformanceLedger::instance();
	assert(ledger.beginRun(true, KernelPerformanceClock::read, &clock));
	const rts::performance::KernelPerformanceBatch token = ledger.beginBatch(
		rts::performance::KERNEL_PERFORMANCE_PHYSICS, 0, 900, 1);
	assert(token.valid());
	{
		rts::performance::KernelPerformanceScope capture(&ledger, token,
			rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		clock.now += 10;
	}

	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	options.performanceBatch = token;
	rts::PhysicsIntegrationMetrics metrics;
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		rts::PHYSICS_INTEGRATION_PARALLEL);
	{
		rts::performance::KernelPerformanceScope commit(&ledger, token,
			rts::performance::KERNEL_PERFORMANCE_COMMIT);
		clock.now += 10;
	}
	assert(ledger.endBatch(token,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED));
	const rts::performance::KernelPerformanceSnapshot snapshot = ledger.freeze();
	assert(snapshot.complete && snapshot.streamCount == 1);
	const rts::performance::KernelPerformanceStream &stream = snapshot.streams[0];
	assert(stream.kernel == rts::performance::KERNEL_PERFORMANCE_PHYSICS &&
		stream.subtype == 0 && stream.attemptedBatches == 1 &&
		stream.admittedBatches == 1 && stream.committedBatches == 1);
	for (unsigned stage = 0;
		stage != rts::performance::KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		assert(stream.stageSamples[stage] >= 1 && stream.stageNanoseconds[stage] > 0);

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestKernelPerformanceReferenceTransportReachesPhysicsParallelPath()
{
	const unsigned count = 64;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	for (unsigned index = 0; index != count; ++index)
		snapshots[index] = MakeSnapshot(index);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	KernelPerformanceClock timingClock;
	rts::performance::KernelPerformanceLedger &timingLedger =
		rts::performance::KernelPerformanceLedger::instance();
	assert(timingLedger.beginRun(true, KernelPerformanceClock::read,
		&timingClock));
	const rts::performance::KernelPerformanceBatch timingBatch =
		timingLedger.beginBatch(rts::performance::KERNEL_PERFORMANCE_PHYSICS,
		0, 900, 1);
	assert(timingBatch.valid());
	{
		rts::performance::KernelPerformanceScope capture(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		timingClock.now += 10;
	}

	KernelPerformanceClock referenceClock;
	rts::performance::KernelPerformanceReferenceLedger referenceLedger;
	assert(referenceLedger.beginRun(
		rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING,
		KernelPerformanceClock::read, &referenceClock));
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	options.performanceBatch = timingBatch;
	options.performanceReferenceLedger = &referenceLedger;
	options.performanceReferenceBatch = &referenceBatch;
	rts::PhysicsIntegrationMetrics metrics;
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		rts::PHYSICS_INTEGRATION_PARALLEL);
	{
		rts::performance::KernelPerformanceScope commit(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_COMMIT);
		timingClock.now += 10;
	}
	assert(timingLedger.endBatch(timingBatch,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED));
	assert(referenceBatch.valid());
	assert(referenceLedger.finishBatch(referenceBatch, true));
	const rts::performance::KernelPerformanceReferenceSnapshot reference =
		referenceLedger.freeze();
	assert(reference.complete && reference.streamCount == 1);
	if (reference.streamCount == 1)
	{
		const rts::performance::KernelPerformanceReferenceStream &stream =
			reference.streams[0];
		assert(stream.kernel == rts::performance::KERNEL_PERFORMANCE_PHYSICS &&
			stream.subtype == 0 && stream.validatedBatchCount == 1 &&
			stream.committedBatchCount == 1 &&
			stream.validatedOperationCount == count &&
			stream.committedOperationCount == count &&
			stream.serialSampleCount == 0 &&
			stream.serialNanoseconds == 0);
	}
	assert(referenceClock.now == 1000);

	assert(timingLedger.freeze().complete);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestKernelPerformanceReferenceSerialPhysicsUsesDedicatedOutput()
{
	const unsigned count = 64;
	std::vector<rts::PhysicsIntegrationSnapshot> snapshots(count);
	std::vector<rts::PhysicsIntegrationOutput> outputs(count);
	std::vector<rts::PhysicsIntegrationOutput> scratch(count);
	std::vector<rts::PhysicsIntegrationOutput> referenceOutputs(count);
	std::vector<rts::PhysicsIntegrationOutput> expected(count);
	for (unsigned index = 0; index != count; ++index)
	{
		snapshots[index] = MakeSnapshot(index);
		assert(rts::ComputePhysicsIntegrationPrefix(snapshots[index],
			expected[index]));
	}
	FillSentinel(referenceOutputs);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	KernelPerformanceClock timingClock;
	rts::performance::KernelPerformanceLedger &timingLedger =
		rts::performance::KernelPerformanceLedger::instance();
	assert(timingLedger.beginRun(true, KernelPerformanceClock::read,
		&timingClock));
	const rts::performance::KernelPerformanceBatch timingBatch =
		timingLedger.beginBatch(rts::performance::KERNEL_PERFORMANCE_PHYSICS,
		0, 900, 1);
	assert(timingBatch.valid());
	{
		rts::performance::KernelPerformanceScope capture(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		timingClock.now += 10;
	}

	KernelPerformanceClock referenceClock;
	rts::performance::KernelPerformanceReferenceLedger referenceLedger;
	assert(referenceLedger.beginRun(
		rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE,
		KernelPerformanceClock::read, &referenceClock));
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 1;
	options.performanceBatch = timingBatch;
	options.performanceReferenceLedger = &referenceLedger;
	options.performanceReferenceBatch = &referenceBatch;
	options.performanceReferenceOutput = &referenceOutputs[0];
	options.performanceReferenceOutputCapacity = count;
	rts::PhysicsIntegrationMetrics metrics;
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		rts::PHYSICS_INTEGRATION_PARALLEL);
	assert(SameBytes(&outputs[0], &expected[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
	assert(SameBytes(&referenceOutputs[0], &expected[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
	assert(SameBytes(&scratch[0], &expected[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
	{
		rts::performance::KernelPerformanceScope commit(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_COMMIT);
		timingClock.now += 10;
	}
	assert(timingLedger.endBatch(timingBatch,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED));
	assert(referenceBatch.valid());
	assert(referenceLedger.finishBatch(referenceBatch, true));
	const rts::performance::KernelPerformanceReferenceSnapshot reference =
		referenceLedger.freeze();
	assert(reference.complete && reference.streamCount == 1);
	if (reference.streamCount == 1)
	{
		const rts::performance::KernelPerformanceReferenceStream &stream =
			reference.streams[0];
		assert(stream.kernel == rts::performance::KERNEL_PERFORMANCE_PHYSICS &&
			stream.subtype == 0 && stream.validatedBatchCount == 1 &&
			stream.committedBatchCount == 1 &&
			stream.validatedOperationCount == count &&
			stream.committedOperationCount == count &&
			stream.serialSampleCount == 1 &&
			stream.serialNanoseconds != 0);
	}
	assert(referenceClock.now == 1020);

	assert(timingLedger.freeze().complete);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

// These tests enter the real public native kernel. They do not implement a
// dispatcher, range body, checkpoint probe, canonical serializer or oracle.
int g_actualNativePhysicsFailures = 0;
void NativePhysicsExpect(bool condition, const char *message)
{
	if (!condition) { ++g_actualNativePhysicsFailures; printf("FAIL: %s\n", message); }
}

struct ActualNativePhysicsObservations
{
	std::atomic<unsigned> entries[2]{}, finishes[2]{}, items[384]{}, polls[2][4]{};
	std::atomic<unsigned> units[2]{}, completed[2]{}, releases[2]{};
	std::atomic<unsigned> validations{0}, reductions{0}, publications{0};
	std::atomic<bool> wrongIdentity{false};
	std::atomic<unsigned> held{0}, truePredicates[2]{};
	std::atomic<bool> releaseHeld{false}, waitExpired{false};
	unsigned waitNotifications = 0, cancelNotifications = 0, releaseNotifications = 0;
	unsigned releasedCompleted = 0, releasedSubmitted = 0, releasedReason = 0;
	bool releasedCancelled = false;

	rts_test::NativeKernelClock *clock = 0;
	bool baseline = false;
	unsigned variant = 0;
	~ActualNativePhysicsObservations() { releaseHeld.store(true, std::memory_order_release); }

	static void beforeWait(void *opaque)
	{
		auto &self = *static_cast<ActualNativePhysicsObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongIdentity = true;
		++self.waitNotifications;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
		while (self.held.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (self.held.load(std::memory_order_acquire) != 2)
		{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
	}
	static void afterCancel(void *opaque)
	{
		auto &self = *static_cast<ActualNativePhysicsObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongIdentity = true;
		++self.cancelNotifications;
		self.releaseHeld.store(true, std::memory_order_release);
	}
	static void releasedGroup(void *opaque, bool cancelled, unsigned completedBodies, unsigned submitted, unsigned reason)
	{
		auto &self = *static_cast<ActualNativePhysicsObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) ||
			rts::JobSystem::instance().outstandingJobCount() != 0 ||
			rts::JobSystem::instance().pendingOwnerCompletionCount() != 0) self.wrongIdentity = true;
		++self.releaseNotifications; self.releasedCancelled = cancelled;
		self.releasedCompleted = completedBodies; self.releasedSubmitted = submitted; self.releasedReason = reason;
	}

	static void observe(void *opaque, rts::PhysicsIntegrationTestEvent event,
		unsigned rangeIndex, unsigned begin, unsigned end, unsigned workUnits,
		bool complete, rts::PhysicsIntegrationOutput *mutableStorage)
	{
		auto &self = *static_cast<ActualNativePhysicsObservations *>(opaque);
		const bool body = event <= rts::PHYSICS_INTEGRATION_TEST_RANGE_FINISHED;
		if (rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) !=
			(body ? self.baseline : true)) self.wrongIdentity = true;
		if (body || event == rts::PHYSICS_INTEGRATION_TEST_RANGE_RELEASED)
		{
			if (rangeIndex >= 2 || begin != rangeIndex * 192 || end != begin + 192)
			{ self.wrongIdentity = true; return; }
		}
		if (event == rts::PHYSICS_INTEGRATION_TEST_RANGE_ENTERED)
		{
			++self.entries[rangeIndex];
			if (!self.baseline)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
				while ((self.entries[0] == 0 || self.entries[1] == 0) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::yield();
				if (self.entries[0] != 1 || self.entries[1] != 1) self.wrongIdentity = true;
			}
		}
		else if (event == rts::PHYSICS_INTEGRATION_TEST_ITEM_EVALUATED)
		{
			if (workUnits >= 192) self.wrongIdentity = true;
			else ++self.items[begin + workUnits];
			++self.clock->now;
		}
		else if (event == rts::PHYSICS_INTEGRATION_TEST_RANGE_FINISHED)
		{
			++self.finishes[rangeIndex]; self.units[rangeIndex] = workUnits;
			self.completed[rangeIndex] = complete ? 1 : 0;
			if (self.variant == 5 && !self.baseline)
			{
				if (!complete || workUnits != 192) self.wrongIdentity = true;
				self.held.fetch_add(1, std::memory_order_release);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
				while (!self.releaseHeld.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::yield();
				if (!self.releaseHeld.load(std::memory_order_acquire)) self.waitExpired = true;
			}
		}
		else if (event == rts::PHYSICS_INTEGRATION_TEST_RANGE_RELEASED)
		{
			++self.releases[rangeIndex];
			const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
			if (scheduler.pendingJobs != 0 || scheduler.outstandingJobs != 0) self.wrongIdentity = true;
			self.clock->now.fetch_add(13);
		}
		else
		{
			if (event == rts::PHYSICS_INTEGRATION_TEST_OWNER_VALIDATION)
			{
				++self.validations;
				if (self.variant == 4) { if (mutableStorage == 0) self.wrongIdentity = true; else mutableStorage[0].objectID = 0; }
			}
			else if (event == rts::PHYSICS_INTEGRATION_TEST_PUBLICATION) ++self.publications;
			else self.wrongIdentity = true;
			self.clock->now.fetch_add(17);
		}
	}

	static bool checkpoint(void *opaque, unsigned rangeIndex,
		rts::PhysicsIntegrationTestCheckpoint site, unsigned workUnits, bool actual)
	{
		auto &self = *static_cast<ActualNativePhysicsObservations *>(opaque);
		if (rangeIndex >= 2 || static_cast<unsigned>(site) >= 4 ||
			rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) != self.baseline)
		{ self.wrongIdentity = true; return true; }
		++self.polls[rangeIndex][static_cast<unsigned>(site)];
		if ((site == rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_ENTRY && workUnits != 0) ||
			(site == rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_BLOCK && (workUnits % 64 != 0 || workUnits >= 192)) ||
			(site >= rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_POST_BODY && workUnits != 192))
			self.wrongIdentity = true;
		const bool cut = rangeIndex == 0 && (
			(self.variant == 1 && site == rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_ENTRY) ||
			(self.variant == 2 && site == rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_BLOCK && workUnits == 128) ||
			(self.variant == 3 && site == rts::PHYSICS_INTEGRATION_TEST_CHECKPOINT_POST_BODY));
		// A current deadline is deliberately the opposite of the source cut.
		// Only the authenticated native replay probe may select baseline work.
		if (!self.baseline && (actual || cut)) ++self.truePredicates[rangeIndex];
		return self.baseline ? !cut : actual || cut;
	}
};

bool RunActualNativePhysicsRole(rts_test::NativeKernelTrace &trace, bool baseline, unsigned variant)
{
	using namespace rts::performance;
	printf("B_NATIVE Physics BEGIN role=%s variant=%u\n",
		baseline ? "consumer" : "source", variant);
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = baseline ? 1 : 2; config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{ NativePhysicsExpect(false, "Physics source fixture starts its declared native worker policy"); return false; }
	rts_test::NativeKernelOwnerRun run;
	const bool started = run.begin(trace, baseline, 900, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND);
	NativePhysicsExpect(started, "Physics owner validates real source artifact binding before native entry");
	if (!started) { jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PHYSICS, 0);
	NativePhysicsExpect(attempt.valid(), "Physics owner opens the authentic attempt before native capture");
	auto timingBatch = run.timing.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 900, 1);
	const auto capture = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_CAPTURE);
	std::vector<rts::PhysicsIntegrationSnapshot> input(384);
	std::vector<rts::PhysicsIntegrationOutput> output(384), scratch(384), untouched(384), detached(384);
	memset(output.data(), 0xcd, output.size() * sizeof(output[0])); untouched = output; detached = output;
	for (unsigned index = 0; index != 384; ++index)
	{
		auto &value = input[index];
		value.frame = 900; value.worldEpoch = 7; value.objectID = index + 1;
		value.motionGeneration = 10; value.physicsGeneration = 20; value.wakePriority = 1; value.heapOrdinal = index;
		value.flags = rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN;
		value.matrix[0] = value.matrix[5] = value.matrix[10] = 1;
		value.matrix[3] = value.position[0] = static_cast<float>(index);
		value.matrix[7] = value.position[1] = 2; value.matrix[11] = value.position[2] = 4;
		value.acceleration[0] = 1; value.acceleration[1] = .5f; value.acceleration[2] = .25f;
		value.velocity[0] = 2; value.velocity[1] = -1; value.velocity[2] = 1;
		value.gravity = -.125f; value.mass = 1;
	}
	run.clock.now.fetch_add(5);
	NativePhysicsExpect(run.timing.endInterval(capture), "Physics immutable owner capture closes before native work");
	KernelPerformanceReferenceBatch validated;
	ActualNativePhysicsObservations observed;
	observed.clock = &run.clock; observed.baseline = baseline; observed.variant = variant;
	rts::PhysicsIntegrationTestHooks hooks;
	hooks.context = &observed; hooks.observe = ActualNativePhysicsObservations::observe;
	hooks.checkpoint = ActualNativePhysicsObservations::checkpoint;
	hooks.physicalWaitMilliseconds = 1000;
	if (variant == 5)
	{
		hooks.beforeWait = ActualNativePhysicsObservations::beforeWait;
		hooks.afterCancel = ActualNativePhysicsObservations::afterCancel;
		hooks.releasedGroup = ActualNativePhysicsObservations::releasedGroup;
		hooks.physicalWaitMilliseconds = 1;
	}
	rts::PhysicsIntegrationOptions options;
	options.minimumGrain = 192; options.testHooks = &hooks;
	options.performanceBatch = timingBatch; options.performanceReferenceLedger = &run.reference;
	options.performanceReferenceAttempt = attempt; options.performanceReferenceBatch = &validated;
	options.performanceReferenceOutput = detached.data(); options.performanceReferenceOutputCapacity = 384;
	rts::PhysicsIntegrationMetrics metrics;
	const auto result = rts::PreparePhysicsIntegrationPrefixes(input.data(), 384,
		output.data(), 384, scratch.data(), 384, options, &metrics);
	const bool accepted = result == rts::PHYSICS_INTEGRATION_PARALLEL;
	NativePhysicsExpect(accepted == (variant == 0), "Physics native outcome matches the predeclared source case without retries");
	if (variant == 5)
		NativePhysicsExpect(result == rts::PHYSICS_INTEGRATION_CANCELLED,
			"Physics late real group cancellation retains the exact native cancelled result");
	if (accepted)
	{
		for (unsigned index = 0; index != 384; ++index)
		{
			rts::PhysicsIntegrationOutput expected = {};
			expected.frame = 900; expected.worldEpoch = 7; expected.objectID = index + 1;
			expected.motionGeneration = 10; expected.physicsGeneration = 20; expected.wakePriority = 1; expected.heapOrdinal = index;
			expected.flags = rts::PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN;
			expected.matrix[0] = expected.matrix[5] = expected.matrix[10] = 1;
			expected.matrix[3] = static_cast<float>(index) + 3; expected.matrix[7] = 1.5f; expected.matrix[11] = 5.125f;
			expected.acceleration[0] = 1; expected.acceleration[1] = .5f; expected.acceleration[2] = .125f;
			expected.velocity[0] = 3; expected.velocity[1] = -.5f; expected.velocity[2] = 1.125f;
			NativePhysicsExpect(rts::PhysicsIntegrationOutputsEqual(output[index], expected),
				"actual native physics matches hand-derived gravity/velocity/translation bytes");
		}
	}
	else NativePhysicsExpect(memcmp(output.data(), untouched.data(), output.size() * sizeof(output[0])) == 0,
		"actual native physics abort preserves every output byte");
	NativePhysicsExpect(memcmp(detached.data(), untouched.data(), detached.size() * sizeof(detached[0])) == 0,
		"Physics source and baseline do not execute detached serial-reference storage");
	const unsigned targetUnits = variant == 1 ? 0 : variant == 2 ? 128 : 192;
	for (unsigned range = 0; range != 2; ++range)
	{
		const unsigned units = range == 0 ? targetUnits : 192;
		NativePhysicsExpect(observed.entries[range] == 1 && observed.finishes[range] == 1 && observed.releases[range] == 1 &&
			observed.units[range] == units && observed.completed[range] == ((range == 0 && variant >= 1 && variant <= 3) ? 0U : 1U),
			"Physics real admitted range enters once, retains its exact terminal prefix and releases after drain");
		for (unsigned index = 0; index != 192; ++index)
			NativePhysicsExpect(observed.items[range * 192 + index] == (index < units ? 1U : 0U),
				"Physics actual compiled item helper executes precisely the recorded prefix once");
		NativePhysicsExpect(observed.polls[range][0] == 1 &&
			observed.polls[range][1] == ((range == 0 && variant == 1) ? 0U : 2U) &&
			observed.polls[range][2] == ((range == 0 && (variant == 1 || variant == 2)) ? 0U : 1U),
			"Physics exact native entry, 64-item and post-body checkpoint sites are reached");
	}
	NativePhysicsExpect(!observed.wrongIdentity && observed.publications == (variant == 0 ? 1U : 0U) &&
		observed.validations == ((variant == 0 || variant == 4) ? 1U : 0U),
		"Physics owner-only validation/reduction rejects finished-discarded bodies before publication");
	if (variant == 5 && !baseline)
	{
		NativePhysicsExpect(!observed.waitExpired && observed.held == 2 && observed.waitNotifications == 1 &&
			observed.cancelNotifications == 1 && observed.releaseNotifications == 1 && observed.releasedCancelled &&
			observed.releasedCompleted == 2 && observed.releasedSubmitted == 2 &&
			observed.truePredicates[0] == 0 && observed.truePredicates[1] == 0 && metrics.completedJobs == 0,
			"Physics real owner timeout cancels and drains two completed bodies without inventing a true body poll or successful handle");
		NativePhysicsExpect(observed.releasedReason == 3,
			"Physics source reason records actual late group cancellation independently of completed checkpoints");
	}
	if (variant == 5 && baseline)
		NativePhysicsExpect(!observed.waitExpired && observed.held == 0 && observed.waitNotifications == 0 &&
			observed.cancelNotifications == 0 && observed.releaseNotifications == 0,
			"Physics baseline replays late disposal without a physical wait, cancellation or source release callback");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	NativePhysicsExpect(scheduler.pendingJobs == 0 && scheduler.outstandingJobs == 0 && scheduler.ownerHelpJobs == 0,
		"Physics native return follows actual release/acquire and scheduler drain");
	NativePhysicsExpect(metrics.referenceAdmissionAccepted,
		"Physics metrics retain authenticated admission for physical and baseline-inline execution");
	if (baseline)
		NativePhysicsExpect(scheduler.submittedJobs == 0 && scheduler.executedJobs == 0 && metrics.submittedJobs == 0 &&
			metrics.physicalWorkerJobs == 0 && metrics.ownerHelpedJobs == 0 && metrics.physicalWorkerMask == 0 &&
			metrics.distinctPhysicalWorkers == 0 && metrics.peakConcurrentPhysicalWorkers == 0,
			"Physics source-shaped baseline bodies manufacture no worker or owner-help authority");
	else
		NativePhysicsExpect(scheduler.submittedJobs == 2 && scheduler.executedJobs == 2 && metrics.submittedJobs == 2,
			"Physics source dispatch is the two real admitted native jobs");
	NativePhysicsExpect(validated.valid() == accepted, "Physics kernel links only actually published output to its attempt");
	if (validated.valid()) NativePhysicsExpect(run.reference.finishBatch(validated, accepted),
		"Physics owner closes the actual validated batch before attempt finish");
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = accepted ? KERNEL_PERFORMANCE_COMMITTED : KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1; finish.reason = accepted ? 1 : 2; finish.validatedBatch = validated;
	NativePhysicsExpect(run.reference.finishAttempt(attempt, finish), "Physics authentic native attempt closes its actual outcome");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs; reap.outstandingJobs = scheduler.outstandingJobs;
	NativePhysicsExpect(run.reference.reapAttempt(attempt, reap), "Physics native owner reaps only after real storage release and drain");
	if (accepted)
	{
		const auto commit = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_COMMIT);
		run.clock.now.fetch_add(11); run.timing.endInterval(commit);
	}
	NativePhysicsExpect(run.timing.endBatch(timingBatch, finish.disposition), "Physics timing records actual native outcome");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(scheduler);
	NativePhysicsExpect(timingClosed, "Physics actual scheduler and owner phase timing reconcile");
	if (baseline && timingClosed)
	{
		const auto &phase = run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_LEGACY_MUTABLE_ISLAND];
		NativePhysicsExpect(phase.pureNanoseconds == (variant == 0 ? 384U : 0U) && phase.serialNanoseconds >= 31,
			"Physics only committed actual native bodies are pure; reduction and all discarded work stay serial");
	}
	const bool canonicalState = variant == 0 ? snapshot.complete && snapshot.streamCount == 1 :
		!snapshot.complete && snapshot.streamCount == 0;
	const bool sourceComplete = sealed && canonicalState && snapshot.errors == 0 && snapshot.trace.complete &&
		snapshot.trace.attemptCount == 1 && snapshot.trace.admittedAttemptCount == 1 &&
		snapshot.trace.capturedAttemptCount == 1 && snapshot.trace.capturedOperationCount == 384 &&
		snapshot.trace.dispatchCount == 1 && snapshot.trace.rangeCount == 2 &&
		snapshot.trace.releasedRangeCount == 2 && snapshot.trace.reapCount == 1;
	NativePhysicsExpect(sourceComplete, "Physics actual native entry supplies capture, dispatch, exact released bodies and attempt closure");
	if (!baseline) trace.source = snapshot;
	else if (sourceComplete && accepted)
		NativePhysicsExpect(snapshot.streams[0].inputDigest.equals(trace.source.streams[0].inputDigest) &&
			snapshot.streams[0].outputDigest.equals(trace.source.streams[0].outputDigest) &&
			snapshot.streams[0].commitDigest.equals(trace.source.streams[0].commitDigest),
			"Physics once-only native consumer binds every canonical input/output/commit byte to source");
	printf("B_NATIVE Physics END role=%s variant=%u source_closure=%u\n",
		baseline ? "consumer" : "source", variant, static_cast<unsigned>(sourceComplete));
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	return sourceComplete && (variant != 5 || baseline || observed.releasedReason == 3);
}

void TestActualNativePhysicsSourceConsumer()
{
	// Breaks caught: missing native integration, copied/detached executor,
	// recomputed baseline range shape, changed polling, partial publication,
	// premature release, or treating completed-discarded bodies as pure.
	// Variant five catches a real owner cancellation after every body poll and
	// body completion, while the native jobs are still awaiting retirement.
	for (unsigned variant = 0; variant != 6; ++variant)
	{
		rts_test::NativeKernelTrace trace(60 + variant);
		// A real source failure is not repaired by synthesizing trace records.
		// All source buffers leave scope before the authenticated consumer call.
		if (RunActualNativePhysicsRole(trace, false, variant))
			RunActualNativePhysicsRole(trace, true, variant);
	}
}
#endif
}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity))
	{
		fprintf(stderr,
			"Usage: core_physics_integration_kernel_tests "
			"[--local-capacity|--external-qualification]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
#if defined(_MSC_VER)
	_set_error_mode(_OUT_TO_STDERR);
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	TestScalarByteAndFieldParity();
#if defined(_WIN32) && !defined(_WIN64) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300) && \
	((defined(_MSC_VER) && defined(_M_IX86)) || \
		(defined(__GNUC__) && defined(__i386__)))
	TestNestedFloatingPointScopeRestoration();
#endif
	TestLoadedTransportMassUsesLegacyOracle();
	TestBoundedOwnerIndexWithAdversarialSparseIDs();
	TestDampingFlagAndSleepyHeapParity();
	TestOwnerGenerationAndHeapValidationFields();
	TestOwnerCommitGateMutationDestructionAndReset();
	TestWorkerCountsAndOwnerFloatingPointState(localCapacity);
	TestShadowWorkAtTwoAndSixteenWorkers(localCapacity);
	TestTransactionalFailurePaths();
	TestBelowGrainSlicesAreNotSchedulerFallbacks();
	TestPolicyIneligibleIsDistinctFromSafetyFallback();
	TestInvalidAndNonfiniteInputDoesNotPublish();
	TestRuntimeMetricsCountOnlyExplicitEvents();
#if defined(_WIN64)
	TestKernelPerformanceTokenReachesPhysicsStages();
	TestKernelPerformanceReferenceTransportReachesPhysicsParallelPath();
	TestKernelPerformanceReferenceSerialPhysicsUsesDedicatedOutput();
	TestActualNativePhysicsSourceConsumer();
#endif
#if defined(_WIN64)
	if (g_actualNativePhysicsFailures != 0) return 1;
#endif
	printf("Physics integration kernel tests passed.\n");
	return 0;
}
