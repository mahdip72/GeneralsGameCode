/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/JobSystem.h"

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
	if (actualWorkerCount <= 1)
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

void TestWorkerCountsAndOwnerFloatingPointState()
{
	const unsigned counts[] = { 1, 2, 4, 8, 16 };
	for (unsigned index = 0; index != sizeof(counts) / sizeof(counts[0]); ++index)
		RunWorkerCount(counts[index]);

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	const unsigned savedMxcsr = _mm_getcsr();
	_mm_setcsr((savedMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_DOWN);
	RunWorkerCount(4);
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

void TestShadowWorkAtTwoAndSixteenWorkers()
{
	RunShadowWorkerCount(2);
	RunShadowWorkerCount(16);
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
	const rts::PhysicsIntegrationBatchResult runtimeExpectedResult =
		rts::JobSystem::instance().workerCount() <= 1 ?
		rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE : expectedResult;
	assert(rts::PreparePhysicsIntegrationPrefixes(&snapshots[0], count,
		&outputs[0], count, &scratch[0], count, options, &metrics) ==
		runtimeExpectedResult);
	assert(SameBytes(&outputs[0], &sentinel[0],
		count * sizeof(rts::PhysicsIntegrationOutput)));
}

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
	if (actualWorkerCount <= 1)
		assert(rts::PreflightPhysicsIntegrationPrefixes() ==
			rts::PHYSICS_INTEGRATION_POLICY_INELIGIBLE);
	else
		assert(rts::PreflightPhysicsIntegrationPrefixes() ==
			rts::PHYSICS_INTEGRATION_PARALLEL);

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
}

int main()
{
#if defined(_MSC_VER)
	_set_error_mode(_OUT_TO_STDERR);
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	TestScalarByteAndFieldParity();
	TestLoadedTransportMassUsesLegacyOracle();
	TestBoundedOwnerIndexWithAdversarialSparseIDs();
	TestDampingFlagAndSleepyHeapParity();
	TestOwnerGenerationAndHeapValidationFields();
	TestOwnerCommitGateMutationDestructionAndReset();
	TestWorkerCountsAndOwnerFloatingPointState();
	TestShadowWorkAtTwoAndSixteenWorkers();
	TestTransactionalFailurePaths();
	TestBelowGrainSlicesAreNotSchedulerFallbacks();
	TestPolicyIneligibleIsDistinctFromSafetyFallback();
	TestInvalidAndNonfiniteInputDoesNotPublish();
	TestRuntimeMetricsCountOnlyExplicitEvents();
	printf("Physics integration kernel tests passed.\n");
	return 0;
}
