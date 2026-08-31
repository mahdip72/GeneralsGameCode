/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ParallelSkinning.h"
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
rts::SkinningMatrix identity()
{
	rts::SkinningMatrix matrix;
	memset(&matrix, 0, sizeof(matrix));
	matrix.row[0][0] = matrix.row[1][1] = matrix.row[2][2] = 1.0f;
	return matrix;
}

int check(bool value, const char *message)
{
	if (value) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}
}

int main()
{
	int result = 0;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	std::vector<rts::SkinningMatrix> matrices(65, identity());
	for (unsigned matrixIndex = 0; matrixIndex != matrices.size(); ++matrixIndex)
	{
		matrices[matrixIndex].row[0][3] = static_cast<float>(matrixIndex) * 0.037f;
		matrices[matrixIndex].row[1][0] = 0.17777f;
	}
	std::vector<rts::SkinningVertex> vertices(65537);
	for (unsigned vertexIndex = 0; vertexIndex != vertices.size(); ++vertexIndex)
	{
		vertices[vertexIndex].position.x = static_cast<float>(vertexIndex % 71) * 0.1234567f;
		vertices[vertexIndex].position.y = -0.3125f;
		vertices[vertexIndex].position.z = 0.333333f;
		vertices[vertexIndex].normal.x = 0.5f;
		vertices[vertexIndex].normal.y = -0.8f;
		vertices[vertexIndex].normal.z = 0.99999f;
		vertices[vertexIndex].bone = (vertexIndex * 17) % static_cast<unsigned>(matrices.size());
	}
	std::vector<rts::PoseBone> bones(4097);
	for (unsigned boneIndex = 0; boneIndex != bones.size(); ++boneIndex)
	{
		bones[boneIndex].base = matrices[boneIndex % matrices.size()];
		bones[boneIndex].rotation = identity();
		bones[boneIndex].parent = boneIndex == 0 ? 0 : (boneIndex - 1) / 8;
		bones[boneIndex].translation.x = 0.111111f;
		bones[boneIndex].translation.y = -0.3f;
		bones[boneIndex].translation.z = 1.25f;
		bones[boneIndex].translate = true;
		bones[boneIndex].rotate = boneIndex % 2 != 0;
		bones[boneIndex].visible = boneIndex % 3 != 0;
	}
	const unsigned configurations[] = { 1, 2, 4, 8, 16, 0 };
	for (unsigned configuration = 0; configuration != 6; ++configuration)
	{
		rts::JobSystemConfig config = rts::JobSystem::startupConfig();
		config.workerCount = configurations[configuration];
		config.queueCapacity = 8192;
		result |= check(jobs.start(config), "start portable worker configuration");
		rts::SkinningOptions options;
		options.minimumGrain = 16;
		options.parallel = false;
		std::vector<rts::SkinnedVertex> serial(vertices.size()), parallel(vertices.size());
		std::vector<rts::PoseTransform> serialPose(bones.size()), parallelPose(bones.size());
		result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()),
			&matrices[0], static_cast<unsigned>(matrices.size()), true, &serial[0], options)), "portable serial skin");
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(&bones[0], static_cast<unsigned>(bones.size()),
			identity(), &serialPose[0], options)), "portable serial pose");
		options.parallel = true;
		result |= check(rts::SkinningCompleted(rts::SkinVertices(&vertices[0], static_cast<unsigned>(vertices.size()),
			&matrices[0], static_cast<unsigned>(matrices.size()), true, &parallel[0], options)), "portable parallel skin");
		result |= check(memcmp(&serial[0], &parallel[0], serial.size()*sizeof(rts::SkinnedVertex)) == 0, "portable skin parity");
		result |= check(rts::SkinningCompleted(rts::EvaluatePose(&bones[0], static_cast<unsigned>(bones.size()),
			identity(), &parallelPose[0], options)), "portable parallel pose");
		for (unsigned poseIndex = 0; poseIndex != bones.size(); ++poseIndex)
		{
			if (memcmp(&serialPose[poseIndex].transform, &parallelPose[poseIndex].transform, sizeof(rts::SkinningMatrix)) != 0 ||
				serialPose[poseIndex].visible != parallelPose[poseIndex].visible)
			{
				result |= check(false, "portable pose parity");
				break;
			}
		}
		result |= check(rts::SkinningCompleted(rts::SkinVertices(0, 0, 0, 0, false, 0, options)) &&
			rts::SkinningCompleted(rts::EvaluatePose(0, 0, identity(), 0, options)), "portable empty inputs");
		jobs.shutdown();
	}
	return result;
}
