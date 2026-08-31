/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ParallelSkinning.h"
#include "WWMath/matrix3d.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#if defined(_WIN32) && defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
#include <float.h>
#endif

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <thread>
#if defined(_WIN32)
#include <float.h>
#include <xmmintrin.h>
#endif
#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_skinning_set_test_fault(unsigned fault, unsigned occurrence);
extern "C" void rts_job_system_set_test_fault(unsigned fault, unsigned occurrence);
extern "C" void rts_job_system_set_test_pause_mask(unsigned mask);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned point, unsigned timeout);
extern "C" void rts_job_system_release_test_pause(unsigned point);
#endif
#endif

namespace
{
#if defined(_WIN32) && defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
class GameFloatingPointModeGuard
{
public:
	GameFloatingPointModeGuard() : m_previous(_controlfp(0, 0))
	{
		// Match GameLogic::setFPMode for the VC6 WWMath differential oracle.
		_fpreset();
		_controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
	}
	~GameFloatingPointModeGuard()
	{
		_controlfp(m_previous, _MCW_PC | _MCW_RC);
	}

private:
	unsigned m_previous;
	GameFloatingPointModeGuard(const GameFloatingPointModeGuard &);
	GameFloatingPointModeGuard &operator=(const GameFloatingPointModeGuard &);
};
#endif

int check(bool value, const char *message)
{
	if (value) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

rts::SkinningMatrix flatten(const Matrix3D &matrix)
{
	rts::SkinningMatrix result;
	for (unsigned row = 0; row != 3; ++row)
		for (unsigned column = 0; column != 4; ++column)
			result.row[row][column] = matrix[row][column];
	return result;
}

Matrix3D expand(const rts::SkinningMatrix &matrix)
{
	Matrix3D result;
	for (unsigned row = 0; row != 3; ++row)
		for (unsigned column = 0; column != 4; ++column)
			result[row][column] = matrix.row[row][column];
	return result;
}

bool equalVector(const rts::SkinningVector &a, const rts::SkinningVector &b)
{
	return memcmp(&a.x, &b.x, sizeof(float)) == 0 &&
		memcmp(&a.y, &b.y, sizeof(float)) == 0 && memcmp(&a.z, &b.z, sizeof(float)) == 0;
}

bool equalVertices(const std::vector<rts::SkinnedVertex> &a,
	const std::vector<rts::SkinnedVertex> &b, bool normals)
{
	if (a.size() != b.size()) return false;
	for (unsigned index = 0; index != a.size(); ++index)
		if (!equalVector(a[index].position, b[index].position) ||
			(normals && !equalVector(a[index].normal, b[index].normal))) return false;
	return true;
}

bool equalPose(const std::vector<rts::PoseTransform> &a,
	const std::vector<rts::PoseTransform> &b)
{
	if (a.size() != b.size()) return false;
	for (unsigned index = 0; index != a.size(); ++index)
		if (memcmp(&a[index].transform, &b[index].transform, sizeof(rts::SkinningMatrix)) != 0 ||
			a[index].visible != b[index].visible) return false;
	return true;
}

void makeVertices(unsigned count, std::vector<rts::SkinningVertex> &vertices,
	std::vector<rts::SkinningMatrix> &bones)
{
	bones.resize(257);
	for (unsigned boneIndex = 0; boneIndex != bones.size(); ++boneIndex)
	{
		Matrix3D matrix(true);
		matrix[0][0] = 0.96f; matrix[0][1] = -0.28f;
		matrix[1][0] = 0.28f; matrix[1][1] = 0.96f;
		matrix[2][2] = boneIndex % 3 == 0 ? -0.75f : 1.125f;
		matrix.Set_Translation(Vector3(static_cast<float>(boneIndex) * 0.031f,
			static_cast<float>(boneIndex % 11) * -2.125f, 7.123456f));
		bones[boneIndex] = flatten(matrix);
	}
	vertices.resize(count);
	for (unsigned index = 0; index != count; ++index)
	{
		rts::SkinningVertex &vertex = vertices[index];
		vertex.position.x = static_cast<float>(static_cast<int>(index % 193) - 96) * 0.019f;
		vertex.position.y = static_cast<float>(index % 71) * 0.413f;
		vertex.position.z = static_cast<float>(index % 97) * -0.153f;
		vertex.normal.x = 0.3f;
		vertex.normal.y = index % 2 ? -0.8f : 0.8f;
		vertex.normal.z = 0.1234567f;
		// Include both long runs and interleaved bone links.
		vertex.bone = (index < count / 2 ? index / 37 : index * 17) % static_cast<unsigned>(bones.size());
	}
}

void legacySkin(const std::vector<rts::SkinningVertex> &vertices,
	const std::vector<rts::SkinningMatrix> &bones, std::vector<rts::SkinnedVertex> &output)
{
	output.resize(vertices.size());
	for (unsigned index = 0; index != vertices.size(); ++index)
	{
		Matrix3D matrix = expand(bones[vertices[index].bone]);
		const rts::SkinningVertex &vertex = vertices[index];
		Vector3 position(vertex.position.x, vertex.position.y, vertex.position.z), transformed;
		Matrix3D::Transform_Vector(matrix, position, &transformed);
		output[index].position.x = transformed.X;
		output[index].position.y = transformed.Y;
		output[index].position.z = transformed.Z;
		matrix.Set_Translation(Vector3(0.0f, 0.0f, 0.0f));
		Vector3 normal(vertex.normal.x, vertex.normal.y, vertex.normal.z);
		matrix.mulVector3Array(&normal, &transformed, 1);
		output[index].normal.x = transformed.X;
		output[index].normal.y = transformed.Y;
		output[index].normal.z = transformed.Z;
	}
}

void makePose(unsigned count, bool chain, std::vector<rts::PoseBone> &bones)
{
	bones.resize(count);
	for (unsigned index = 0; index != count; ++index)
	{
		rts::PoseBone &bone = bones[index];
		bone.parent = index == 0 ? 0 : (chain ? index - 1 : (index - 1) / 8);
		Matrix3D base(true), rotation(true);
		base[0][1] = 0.01234567f;
		base.Set_Translation(Vector3(0.003f * static_cast<float>(index % 11), -0.119f, 0.2111f));
		rotation[0][0] = 0.96f; rotation[0][1] = -0.28f;
		rotation[1][0] = 0.28f; rotation[1][1] = 0.96f;
		bone.base = flatten(base);
		bone.rotation = flatten(rotation);
		bone.translation.x = 0.1777f;
		bone.translation.y = -0.12777f;
		bone.translation.z = 0.00443f;
		bone.translate = index % 5 != 0;
		bone.rotate = index % 7 != 0;
		bone.visible = index % 3 != 0;
	}
}

void legacyPose(const std::vector<rts::PoseBone> &bones,
	const rts::SkinningMatrix &root, std::vector<rts::PoseTransform> &output)
{
	output.resize(bones.size());
	if (bones.empty()) return;
	output[0].transform = root;
	output[0].visible = true;
	for (unsigned index = 1; index != bones.size(); ++index)
	{
		const rts::PoseBone &bone = bones[index];
		Matrix3D matrix, parent = expand(output[bone.parent].transform), base = expand(bone.base);
		Matrix3D::Multiply(parent, base, &matrix);
		if (bone.translate) matrix.Translate(Vector3(bone.translation.x, bone.translation.y, bone.translation.z));
		if (bone.rotate) matrix.postMul(expand(bone.rotation));
		output[index].transform = flatten(matrix);
		output[index].visible = bone.visible;
	}
}

int scratchReuse()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	result |= check(jobs.start(config), "start reusable scratch fixture");
	unsigned warmedCapacity = 0, warmedAllocations = 0;
	{
		rts::SkinningScratchLease lease;
		result |= check(lease.prepareSkin(1024, 65), "initial owner scratch allocation");
		if (!lease.vertices()) { jobs.shutdown(); return 1; }
		rts::SkinningVertex *oldVertices = lease.vertices();
		oldVertices[0].position.x = 9876.0f;
#if defined(RTS_BUILD_CORE_EXTRAS)
		rts_skinning_set_test_fault(4, 1);
		result |= check(!lease.prepareSkin(262144, 16384) && lease.vertices() == oldVertices &&
			lease.vertices()[0].position.x == 9876.0f, "growth failure preserves previous lease storage transactionally");
		rts_skinning_set_test_fault(0, 0);
#endif
		result |= check(lease.prepareSkin(262144, 16384), "warm bounded cross-model high-water scratch");
		warmedCapacity = lease.capacityBytes();
		warmedAllocations = lease.allocationCount();
		rts::SkinningVertex *ownedVertices = lease.vertices();
		result |= check(warmedCapacity <= 32u * 1024u * 1024u && !lease.prepareSkin(262145, 65) &&
			!lease.preparePose(16385), "owner scratch enforces byte and element budgets");
		rts::SkinningScratchLease nested;
		result |= check(!nested.prepareSkin(1024, 65), "nested owner lease cannot alias in-flight scratch");
		bool threadIsolation = false;
		std::thread otherOwner([&]() {
			const bool rejectedForeign = !lease.prepareSkin(1024, 65) && lease.vertices() == 0;
			rts::SkinningScratchLease independent;
			threadIsolation = rejectedForeign && independent.prepareSkin(1024, 65) &&
				independent.vertices() != ownedVertices;
		});
		otherOwner.join();
		result |= check(threadIsolation, "foreign owners reject another lease and get independent scratch");
	}
	std::vector<rts::SkinningVertex> vertices;
	std::vector<rts::SkinningMatrix> matrices;
	makeVertices(4096, vertices, matrices);
	std::vector<rts::SkinnedVertex> expected, actual(vertices.size());
	legacySkin(vertices, matrices, expected);
	std::vector<rts::PoseBone> pose;
	makePose(129, false, pose);
	const rts::SkinningMatrix root = flatten(Matrix3D(true));
	std::vector<rts::PoseTransform> expectedPose, actualPose(pose.size());
	legacyPose(pose, root, expectedPose);
	for (unsigned frame = 0; frame != 16; ++frame)
	{
		rts::SkinningScratchLease lease;
		result |= check(lease.prepareSkin(static_cast<unsigned>(vertices.size()), static_cast<unsigned>(matrices.size())), "reuse skin snapshot lease");
		memcpy(lease.vertices(), &vertices[0], vertices.size() * sizeof(rts::SkinningVertex));
		memcpy(lease.matrices(), &matrices[0], matrices.size() * sizeof(rts::SkinningMatrix));
		rts::SkinningOptions options;
		options.scratch = &lease;
		options.minimumGrain = 16;
		result |= check(rts::SkinningCompleted(rts::SkinVertices(lease.vertices(), static_cast<unsigned>(vertices.size()),
			lease.matrices(), static_cast<unsigned>(matrices.size()), true, lease.skinOutput(), options)), "leased skin kernel completes");
		memcpy(&actual[0], lease.skinOutput(), actual.size() * sizeof(rts::SkinnedVertex));
		result |= check(equalVertices(expected, actual, true), "reused cross-model skin buffers retain oracle parity");
		result |= check(lease.preparePose(static_cast<unsigned>(pose.size())), "reuse arena for another hierarchy");
		memcpy(lease.poseBones(), &pose[0], pose.size() * sizeof(rts::PoseBone));
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(lease.poseBones(), static_cast<unsigned>(pose.size()),
			root, lease.poseOutput(), options)), "leased pose kernel completes");
		memcpy(&actualPose[0], lease.poseOutput(), actualPose.size() * sizeof(rts::PoseTransform));
		result |= check(equalPose(expectedPose, actualPose), "reused hierarchy schedule retains oracle parity");
		result |= check(lease.capacityBytes() == warmedCapacity && lease.allocationCount() == warmedAllocations,
			"stable-size frames allocate no new snapshot/staging/schedule arena");
	}
	jobs.shutdown();
	return result;
#else
	return 0;
#endif
}

int parity(unsigned workerCount)
{
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	config.workerCount = workerCount;
	config.queueCapacity = 8192;
	result |= check(jobs.start(config), "start parity worker configuration");
	const unsigned counts[] = { 0, 1, 31, 1025, 262144 };
	for (unsigned skinTest = 0; skinTest != sizeof(counts) / sizeof(counts[0]); ++skinTest)
	{
		std::vector<rts::SkinningVertex> vertices;
		std::vector<rts::SkinningMatrix> bones;
		std::vector<rts::SkinnedVertex> expected, serial(counts[skinTest]), parallel(counts[skinTest]);
		makeVertices(counts[skinTest], vertices, bones);
		legacySkin(vertices, bones, expected);
		rts::SkinningOptions options;
		options.parallel = false;
		result |= check(rts::SkinningCompleted(rts::SkinVertices(vertices.empty() ? 0 : &vertices[0], counts[skinTest],
			&bones[0], static_cast<unsigned>(bones.size()), true, serial.empty() ? 0 : &serial[0], options)), "serial skin completes");
		result |= check(equalVertices(expected, serial, true), "serial skin bit parity with WWMath position/normal transform");
		options.parallel = true;
		rts::SkinningMetrics metrics;
		result |= check(rts::SkinningCompleted(rts::SkinVertices(vertices.empty() ? 0 : &vertices[0], counts[skinTest],
			&bones[0], static_cast<unsigned>(bones.size()), true, parallel.empty() ? 0 : &parallel[0], options, &metrics)), "parallel skin completes");
		result |= check(equalVertices(expected, parallel, true), "parallel skin bit parity including unsorted bone links");
#if !defined(_MSC_VER) || _MSC_VER >= 1300
		if (counts[skinTest] >= 1025 && jobs.workerCount() > 1)
			result |= check(metrics.submittedJobs > 1, "large skin executes multiple jobs");
#else
		result |= check(metrics.submittedJobs == 0, "VC6 retains the serial reference lane");
#endif
		if (!vertices.empty())
		{
			parallel[0].normal.x = 9876.0f;
			result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], counts[skinTest], &bones[0],
				static_cast<unsigned>(bones.size()), false, &parallel[0], options)), "position-only skin completes");
			result |= check(equalVertices(expected, parallel, false) && parallel[0].normal.x == 9876.0f,
				"position-only path preserves normal destination");
		}
	}
	const unsigned poseCounts[] = { 0, 1, 19, 129, 4097 };
	Matrix3D rootMatrix(true);
	rootMatrix.Set_Translation(Vector3(123.987f, -35.03f, 0.01f));
	const rts::SkinningMatrix root = flatten(rootMatrix);
	for (unsigned poseTest = 0; poseTest != sizeof(poseCounts) / sizeof(poseCounts[0]); ++poseTest)
	{
		std::vector<rts::PoseBone> bones;
		makePose(poseCounts[poseTest], false, bones);
		std::vector<rts::PoseTransform> expected, serial(bones.size()), parallel(bones.size());
		legacyPose(bones, root, expected);
		rts::SkinningOptions options;
		options.minimumGrain = 16;
		options.parallel = false;
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(bones.empty() ? 0 : &bones[0], poseCounts[poseTest], root,
			serial.empty() ? 0 : &serial[0], options)), "serial pose completes");
		result |= check(equalPose(expected, serial), "serial pose matches legacy multiply/translate/postMul rounding");
		options.parallel = true;
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(bones.empty() ? 0 : &bones[0], poseCounts[poseTest], root,
			parallel.empty() ? 0 : &parallel[0], options)), "parallel pose completes");
		result |= check(equalPose(expected, parallel), "parallel hierarchy preserves all parent dependencies and visibility");
	}
	std::vector<rts::PoseBone> chain;
	makePose(1024, true, chain);
	std::vector<rts::PoseTransform> chainExpected, chainActual(chain.size());
	legacyPose(chain, root, chainExpected);
	rts::SkinningOptions chainOptions;
	chainOptions.minimumGrain = 16;
	result |= check(rts::EvaluatePose(&chain[0], static_cast<unsigned>(chain.size()), root, &chainActual[0], chainOptions) == rts::SKINNING_SERIAL,
		"narrow hierarchy avoids a serialized chain of tiny jobs");
	result |= check(equalPose(chainExpected, chainActual), "deep-chain matrix order matches legacy");
	printf("Skinning parity: requested workers=%u actual=%u\n", workerCount, jobs.workerCount());
	jobs.shutdown();
	return result;
}

int failurePaths()
{
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	result |= check(jobs.start(config), "start failure tests");
	std::vector<rts::SkinningVertex> vertices;
	std::vector<rts::SkinningMatrix> bones;
	makeVertices(65536, vertices, bones);
	std::vector<rts::SkinnedVertex> expected, output(vertices.size());
	legacySkin(vertices, bones, expected);
	rts::SkinningOptions options;
	options.maximumScratchBytes = 1;
	result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()),
		&bones[0], static_cast<unsigned>(bones.size()), true, &output[0], options)), "scratch-budget fallback completes");
	result |= check(equalVertices(expected, output, true), "scratch-budget fallback is exact");
	options.maximumScratchBytes = 16 * 1024 * 1024;
	output[0].position.x = 9876.0f;
	const unsigned savedLink = vertices.back().bone;
	vertices.back().bone = static_cast<unsigned>(bones.size());
	result |= check(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
		static_cast<unsigned>(bones.size()), true, &output[0], options) == rts::SKINNING_INVALID_INPUT && output[0].position.x == 9876.0f,
		"invalid late bone link cannot partially publish");
	vertices.back().bone = savedLink;
	std::vector<rts::PoseBone> pose;
	makePose(4097, false, pose);
	const rts::SkinningMatrix root = flatten(Matrix3D(true));
	std::vector<rts::PoseTransform> poseExpected, poseOutput(pose.size());
	legacyPose(pose, root, poseExpected);
	poseOutput[0].transform.row[0][0] = 9876.0f;
	const unsigned savedParent = pose.back().parent;
	pose.back().parent = static_cast<unsigned>(pose.size()) - 1;
	result |= check(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options) == rts::SKINNING_INVALID_INPUT &&
		poseOutput[0].transform.row[0][0] == 9876.0f, "cyclic/forward parent rejected before publication");
	pose.back().parent = savedParent;
	options.minimumGrain = 16;
	options.maximumScratchBytes = 1;
	result |= check(rts::SkinningCompleted(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options)) &&
		equalPose(poseExpected, poseOutput), "pose schedule allocation budget uses exact serial fallback");
	options.maximumScratchBytes = 16 * 1024 * 1024;
	rts::JobGroup cancellation = jobs.createGroup();
	jobs.cancel(cancellation);
	options.cancellationGroup = &cancellation;
	output[0].position.x = 9876.0f;
	result |= check(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
		static_cast<unsigned>(bones.size()), true, &output[0], options) == rts::SKINNING_CANCELLED && output[0].position.x == 9876.0f,
		"pre-cancelled skin leaves result untouched");
	poseOutput[0].transform.row[0][0] = 9876.0f;
	result |= check(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options) == rts::SKINNING_CANCELLED &&
		poseOutput[0].transform.row[0][0] == 9876.0f, "pre-cancelled pose leaves result untouched");
	options.cancellationGroup = 0;
#if (!defined(_MSC_VER) || _MSC_VER >= 1300) && defined(RTS_BUILD_CORE_EXTRAS)
	if (jobs.workerCount() > 1)
	{
		for (unsigned allocationFault = 1; allocationFault <= 3; ++allocationFault)
		{
			rts_skinning_set_test_fault(allocationFault, allocationFault == 2 ? 3 : 1);
			result |= check(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
				static_cast<unsigned>(bones.size()), true, &output[0], options) == rts::SKINNING_SERIAL_FALLBACK,
				"allocation/partial-submission/execution fault reports fallback");
			result |= check(equalVertices(expected, output, true), "failed skin drains before serial rewrite");
			rts_skinning_set_test_fault(allocationFault, allocationFault == 2 ? 3 : 1);
			result |= check(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options) == rts::SKINNING_SERIAL_FALLBACK,
				"pose allocation/partial-submission/execution fault reports fallback");
			result |= check(equalPose(poseExpected, poseOutput), "failed pose drains all dependent jobs before fallback");
		}
		rts_skinning_set_test_fault(0, 0);
		const unsigned runtimeFaults[] = { 4, 5, 6 };
		for (unsigned runtimeFault = 0; runtimeFault != 3; ++runtimeFault)
		{
			rts_job_system_set_test_fault(runtimeFaults[runtimeFault], runtimeFaults[runtimeFault] == 4 ? 1 : 3);
			result |= check(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
				static_cast<unsigned>(bones.size()), true, &output[0], options) == rts::SKINNING_SERIAL_FALLBACK,
				"runtime group/job/queue fault takes fallback");
			result |= check(equalVertices(expected, output, true), "runtime rejection preserves exact skin result");
		}
		rts_job_system_set_test_fault(0, 0);
		// Hold actual accepted work, cancel externally, then release it. Neither
		// staging nor the cancellation token may be reclaimed before the join.
		rts::JobGroup activeCancellation = jobs.createGroup();
		options.cancellationGroup = &activeCancellation;
		for (unsigned outputIndex = 0; outputIndex != output.size(); ++outputIndex) output[outputIndex].position.x = 9876.0f;
		rts::SkinningResult cancelledResult = rts::SKINNING_INVALID_INPUT;
		rts_job_system_set_test_pause_mask(32);
		std::thread producer([&]() {
			cancelledResult = rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
				static_cast<unsigned>(bones.size()), true, &output[0], options);
		});
		const bool paused = rts_job_system_wait_for_test_pause(32, 5000);
		result |= check(paused, "skin work reaches the active-job cancellation gate");
		jobs.cancel(activeCancellation);
		rts_job_system_release_test_pause(32);
		producer.join();
		rts_job_system_set_test_pause_mask(0);
		result |= check(cancelledResult == rts::SKINNING_CANCELLED, "active cancellation is not reported as success");
		for (unsigned cancelledOutputIndex = 0; cancelledOutputIndex != output.size(); ++cancelledOutputIndex)
			if (output[cancelledOutputIndex].position.x != 9876.0f) { result |= check(false, "cancelled batch cannot publish any range"); break; }
		result |= check(jobs.outstandingJobCount() == 0, "cancelled batch drains before snapshot lifetime ends");
		options.cancellationGroup = 0;
		result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
			static_cast<unsigned>(bones.size()), true, &output[0], options)) && equalVertices(expected, output, true),
			"new frame succeeds after cancellation without stale publication");
		rts::JobGroup activePoseCancellation = jobs.createGroup();
		options.cancellationGroup = &activePoseCancellation;
		for (unsigned poseIndex = 0; poseIndex != poseOutput.size(); ++poseIndex)
			poseOutput[poseIndex].transform.row[0][0] = 9876.0f;
		rts_job_system_set_test_pause_mask(32);
		std::thread poseProducer([&]() {
			cancelledResult = rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options);
		});
		result |= check(rts_job_system_wait_for_test_pause(32, 5000), "pose parent work reaches active cancellation gate");
		jobs.cancel(activePoseCancellation);
		rts_job_system_release_test_pause(32);
		poseProducer.join();
		rts_job_system_set_test_pause_mask(0);
		result |= check(cancelledResult == rts::SKINNING_CANCELLED, "dependent pose cancellation is not success");
		for (unsigned cancelledPoseIndex = 0; cancelledPoseIndex != poseOutput.size(); ++cancelledPoseIndex)
			if (poseOutput[cancelledPoseIndex].transform.row[0][0] != 9876.0f)
				{ result |= check(false, "cancelled pose cannot publish a partial hierarchy"); break; }
		result |= check(jobs.outstandingJobCount() == 0, "pose dependency jobs drained before snapshot lifetime ends");
		options.cancellationGroup = 0;
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options)) &&
			equalPose(poseExpected, poseOutput), "pose update recovers cleanly on the next frame");
	}
#endif
	jobs.shutdown();
	return result;
}

int scalingEvidence()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	config.workerCount = 8;
	config.queueCapacity = 8192;
	result |= check(jobs.start(config), "start worker scaling fixture");
	std::vector<rts::SkinningVertex> vertices;
	std::vector<rts::SkinningMatrix> bones;
	makeVertices(1048576, vertices, bones);
	std::vector<rts::SkinnedVertex> output(vertices.size());
	rts::SkinningOptions options;
	options.maximumScratchBytes = 32 * 1024 * 1024;
	options.minimumGrain = 32768;
	jobs.resetMetrics();
	for (unsigned repeat = 0; repeat != 4; ++repeat)
		result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()),
			&bones[0], static_cast<unsigned>(bones.size()), true, &output[0], options)), "large scalable skin completes");
	const rts::JobSystemMetrics metrics = jobs.metrics();
	printf("Skinning workers: configured=%u peak-active=%u executed=%llu waits=%llu\n", jobs.workerCount(), metrics.maximumActiveWorkers,
		static_cast<unsigned long long>(metrics.executedJobCount), static_cast<unsigned long long>(metrics.waitCount));
	if (jobs.workerCount() > 2)
		result |= check(metrics.maximumActiveWorkers > 2, "real skin jobs execute beyond two workers");
	jobs.shutdown();
	return result;
#else
	return 0;
#endif
}

int floatingPointParity()
{
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	result |= check(jobs.start(config), "start floating-point parity fixture");
	std::vector<rts::SkinningVertex> vertices;
	std::vector<rts::SkinningMatrix> bones;
	makeVertices(65536, vertices, bones);
	vertices[0].position.x = 1.0e-38f;
	vertices[0].position.y = vertices[0].position.z = 0.0f;
	std::vector<rts::SkinnedVertex> expected, output(vertices.size());
	std::vector<rts::PoseBone> pose;
	makePose(4097, false, pose);
	const rts::SkinningMatrix root = flatten(Matrix3D(true));
	std::vector<rts::PoseTransform> poseExpected, poseOutput(pose.size());
	const unsigned savedMxcsr = _mm_getcsr();
#if !defined(_WIN64)
	const unsigned savedControl = _controlfp(0, 0);
	_controlfp(_PC_53 | _RC_DOWN, _MCW_PC | _MCW_RC);
#endif
	// Deliberately differ from the workers' initial mode. Include FTZ so a job
	// cannot merely assume near/default SIMD state from thread creation.
	const unsigned altered = (savedMxcsr & ~0x6000u) | 0x2000u | 0x8000u;
	_mm_setcsr(altered);
	legacySkin(vertices, bones, expected);
	legacyPose(pose, root, poseExpected);
	rts::SkinningOptions options;
	options.minimumGrain = 16;
	result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()), &bones[0],
		static_cast<unsigned>(bones.size()), true, &output[0], options)) && equalVertices(expected, output, true),
		"parallel skin reproduces caller FP control state");
	result |= check(rts::SkinningCompleted(rts::EvaluatePose(&pose[0], static_cast<unsigned>(pose.size()), root, &poseOutput[0], options)) &&
		equalPose(poseExpected, poseOutput), "parallel pose reproduces caller FP control state");
	result |= check((_mm_getcsr() & 0xffc0u) == (altered & 0xffc0u), "owner-help jobs preserve caller SIMD control state");
#if !defined(_WIN64)
	result |= check((_controlfp(0, 0) & (_MCW_PC | _MCW_RC)) == (_PC_53 | _RC_DOWN), "owner-help jobs preserve caller x87 state");
	_controlfp(savedControl, _MCW_PC | _MCW_RC);
#endif
	_mm_setcsr(savedMxcsr);
	jobs.shutdown();
	return result;
#else
	return 0;
#endif
}
}

int main()
{
#if defined(_WIN32) && defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
	GameFloatingPointModeGuard gameFloatingPointMode;
#endif
	int result = 0;
	result |= scratchReuse();
	const unsigned workers[] = { 1, 2, 4, 8, 16, 0 };
	for (unsigned index = 0; index != sizeof(workers) / sizeof(workers[0]); ++index)
		result |= parity(workers[index]);
	result |= failurePaths();
	result |= scalingEvidence();
	result |= floatingPointParity();
	return result;
}
