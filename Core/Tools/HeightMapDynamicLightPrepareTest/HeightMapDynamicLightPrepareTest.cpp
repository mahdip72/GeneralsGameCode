#include "Lib/HeightMapDynamicLightKernel.h"
#include "W3DDevice/Common/HeightMapDynamicLightPrepare.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>

#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

static int check(bool value, const char *name, const char *expression)
{
	if (!value)
	{
		fprintf(stderr, "%s: %s\n", name, expression);
		return 1;
	}
	return 0;
}

#define CHECK(name, expression) \
	do { if (check((expression), name, #expression) != 0) return 1; } while (0)

union VertexStorage
{
	HeightMapDynamicLightVertex vertices[16];
	unsigned char bytes[16 * sizeof(HeightMapDynamicLightVertex)];
};

static void initializeVertex(HeightMapDynamicLightVertex *vertex,
	Real x, Real y, Real z, unsigned diffuse)
{
	memset(vertex, 0, sizeof(*vertex));
	vertex->x = x;
	vertex->y = y;
	vertex->z = z;
	vertex->diffuse = diffuse;
	vertex->applyLighting = 1;
	vertex->normalX = 0.0f;
	vertex->normalY = 0.0f;
	vertex->normalZ = 1.0f;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
#if defined(_WIN32)
typedef DWORD HeightMapDynamicLightTestThreadId;
#else
typedef pthread_t HeightMapDynamicLightTestThreadId;
#endif

static HeightMapDynamicLightTestThreadId
heightMapDynamicLightCurrentThreadId()
{
#if defined(_WIN32)
	return GetCurrentThreadId();
#else
	return pthread_self();
#endif
}

class HeightMapDynamicLightWorkerProbe
{
public:
	HeightMapDynamicLightWorkerProbe(unsigned height,
		HeightMapDynamicLightTestThreadId *threadIds, unsigned char *arrivals)
		: m_height(height), m_threadIds(threadIds), m_arrivals(arrivals)
	#if defined(_WIN32)
		, m_firstStarted(CreateEventA(0, TRUE, FALSE, 0)),
		  m_secondStarted(CreateEventA(0, TRUE, FALSE, 0))
	#endif
	{
	#if !defined(_WIN32)
		pthread_mutex_init(&m_gateMutex, 0);
		pthread_cond_init(&m_gateCondition, 0);
		m_startedMask = 0;
	#endif
	}

	~HeightMapDynamicLightWorkerProbe()
	{
	#if defined(_WIN32)
		if (m_firstStarted != 0)
			CloseHandle(m_firstStarted);
		if (m_secondStarted != 0)
			CloseHandle(m_secondStarted);
	#else
		pthread_cond_destroy(&m_gateCondition);
		pthread_mutex_destroy(&m_gateMutex);
	#endif
	}

	bool observe(unsigned rowBegin, unsigned rowEnd)
	{
		const unsigned slot = rowBegin == 0 ? 0 : 1;
		if (rowBegin >= rowEnd || rowEnd > m_height ||
			m_threadIds == 0 || m_arrivals == 0 || slot > 1)
			return false;
		m_threadIds[slot] = heightMapDynamicLightCurrentThreadId();
		m_arrivals[slot] = 1;
	#if defined(_WIN32)
		/* Hold both adapter tasks at a native gate. A single worker cannot
		 * execute both ranges and satisfy the identity assertion. */
		if (slot == 0)
		{
			if (m_firstStarted == 0 || m_secondStarted == 0 ||
				SetEvent(m_firstStarted) == FALSE ||
				WaitForSingleObject(m_secondStarted, 1000) != WAIT_OBJECT_0)
				return false;
		}
		else if (m_secondStarted == 0 || m_firstStarted == 0 ||
			SetEvent(m_secondStarted) == FALSE ||
			WaitForSingleObject(m_firstStarted, 1000) != WAIT_OBJECT_0)
		{
			return false;
		}
	#else
		struct timespec deadline;
		if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
			return false;
		deadline.tv_sec += 1;
		pthread_mutex_lock(&m_gateMutex);
		m_startedMask |= 1u << slot;
		pthread_cond_broadcast(&m_gateCondition);
		while (m_startedMask != 3u)
		{
			if (pthread_cond_timedwait(&m_gateCondition, &m_gateMutex,
				&deadline) != 0)
			{
				pthread_mutex_unlock(&m_gateMutex);
				return false;
			}
		}
		pthread_mutex_unlock(&m_gateMutex);
	#endif
		return true;
	}

private:
	HeightMapDynamicLightWorkerProbe(const HeightMapDynamicLightWorkerProbe &);
	HeightMapDynamicLightWorkerProbe &operator=(
		const HeightMapDynamicLightWorkerProbe &);

	unsigned m_height;
	HeightMapDynamicLightTestThreadId *m_threadIds;
	unsigned char *m_arrivals;
	#if defined(_WIN32)
	HANDLE m_firstStarted;
	HANDLE m_secondStarted;
	#else
	pthread_mutex_t m_gateMutex;
	pthread_cond_t m_gateCondition;
	unsigned m_startedMask;
	#endif
};

static HeightMapDynamicLightWorkerProbe *s_dynamicLightWorkerProbe = 0;

static bool observeDynamicLightWorker(unsigned rowBegin, unsigned rowEnd)
{
	return s_dynamicLightWorkerProbe != 0 &&
		s_dynamicLightWorkerProbe->observe(rowBegin, rowEnd);
}

class HeightMapDynamicLightObserverGuard
{
public:
	HeightMapDynamicLightObserverGuard()
	{
	}

	~HeightMapDynamicLightObserverGuard()
	{
		rts_height_map_dynamic_light_prepare_set_test_observer(0);
		s_dynamicLightWorkerProbe = 0;
	}

private:
	HeightMapDynamicLightObserverGuard(
		const HeightMapDynamicLightObserverGuard &);
	HeightMapDynamicLightObserverGuard &operator=(
		const HeightMapDynamicLightObserverGuard &);
};
#endif

static int testDirectionalAndAlpha()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	memset(&input, 0, sizeof(input));
	memset(&output, 0xA5, sizeof(output));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	initializeVertex(&input.vertices[0], 0.0f, 0.0f, 0.0f, 0x80402010u);
	initializeVertex(&input.vertices[1], 1.0f, 0.0f, 0.0f, 0x80402010u);
	initializeVertex(&input.vertices[2], 1.0f, 1.0f, 0.0f, 0x80402010u);
	initializeVertex(&input.vertices[3], 0.0f, 1.0f, 0.0f, 0x80402010u);
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	light.enabled = 1;
	light.directionX = 0.0f;
	light.directionY = 0.0f;
	light.directionZ = -1.0f;
	light.diffuseRed = 0.25f;
	light.diffuseGreen = 0.5f;
	light.diffuseBlue = 1.0f;
	snapshot.width = 1;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.vertexCount = 4;
	snapshot.vertices = input.vertices;
	snapshot.lights = &light;
	snapshot.lightCount = 1;
	snapshot.outputCapacityBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.batchBytes = 4 * sizeof(HeightMapDynamicLightVertex) * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("directional", PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("directional", ValidatePreparedHeightMapDynamicLightOutput(snapshot,
		output.vertices));
	CHECK("directional", (output.vertices[0].diffuse & 0xFF000000u) ==
		(input.vertices[0].diffuse & 0xFF000000u));
	CHECK("directional", output.vertices[0].diffuse !=
		input.vertices[0].diffuse);
	CHECK("directional", output.vertices[0].diffuse == 0x807F9FFFu);
	CHECK("directional", output.bytes[4 * sizeof(HeightMapDynamicLightVertex)] ==
		0xA5);
	return 0;
}

static int testInvalidInputDoesNotWrite()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSnapshot snapshot;
	memset(&input, 0, sizeof(input));
	memset(&output, 0x5A, sizeof(output));
	memset(&snapshot, 0, sizeof(snapshot));
	initializeVertex(&input.vertices[0], 0.0f, 0.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[1], 1.0f, 0.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[2], 1.0f, 1.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[3], 0.0f, 1.0f, 0.0f, 0xFF000000u);
	snapshot.width = 1;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.vertexCount = 4;
	snapshot.vertices = input.vertices;
	snapshot.outputCapacityBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.batchBytes = sizeof(HeightMapDynamicLightVertex) * 8;
	snapshot.lightCount = 1;
	CHECK("invalid", !PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("invalid", output.bytes[0] == 0x5A);
	return 0;
}

static int testUntouchedCellPreservesBaseline()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	unsigned index;
	memset(&input, 0, sizeof(input));
	memset(&output, 0xA5, sizeof(output));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	for (index = 0; index < 8; ++index)
		initializeVertex(&input.vertices[index], static_cast<Real>(index),
			0.0f, 0.0f, 0x80402010u);
	for (index = 0; index < 4; ++index)
		input.vertices[index].applyLighting = 0;
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	light.enabled = 1;
	light.directionZ = -1.0f;
	light.diffuseRed = 1.0f;
	light.diffuseGreen = 1.0f;
	light.diffuseBlue = 1.0f;
	snapshot.width = 2;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 8 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = 8 * sizeof(HeightMapDynamicLightVertex);
	snapshot.vertexCount = 8;
	snapshot.vertices = input.vertices;
	snapshot.lights = &light;
	snapshot.lightCount = 1;
	snapshot.outputCapacityBytes = 8 * sizeof(HeightMapDynamicLightVertex);
	snapshot.batchBytes = snapshot.outputCapacityBytes * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("cell-mask", PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("cell-mask", ValidatePreparedHeightMapDynamicLightOutput(snapshot,
		output.vertices));
	CHECK("cell-mask", output.vertices[0].diffuse == input.vertices[0].diffuse);
	CHECK("cell-mask", output.vertices[4].diffuse != input.vertices[4].diffuse);
	return 0;
}

static int testArbitraryRowSplitParity()
{
	VertexStorage input;
	VertexStorage serial;
	VertexStorage split;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	unsigned index;
	memset(&input, 0, sizeof(input));
	memset(&serial, 0xA5, sizeof(serial));
	memset(&split, 0xA5, sizeof(split));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	for (index = 0; index < 8; ++index)
		initializeVertex(&input.vertices[index], static_cast<Real>(index),
			static_cast<Real>(index % 2), 0.0f, 0x90603010u);
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	light.enabled = 1;
	light.directionX = 0.0f;
	light.directionY = 0.0f;
	light.directionZ = -1.0f;
	light.diffuseRed = 0.2f;
	light.diffuseGreen = 0.4f;
	light.diffuseBlue = 0.8f;
	snapshot.width = 1;
	snapshot.height = 2;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = snapshot.inputStrideBytes;
	snapshot.vertexCount = 8;
	snapshot.vertices = input.vertices;
	snapshot.lights = &light;
	snapshot.lightCount = 1;
	snapshot.outputCapacityBytes = snapshot.outputStrideBytes * 2;
	snapshot.batchBytes = snapshot.outputCapacityBytes * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("split", PrepareHeightMapDynamicLightRows(snapshot, serial.vertices,
		0, 2));
	CHECK("split", PrepareHeightMapDynamicLightRows(snapshot, split.vertices,
		0, 1));
	CHECK("split", PrepareHeightMapDynamicLightRows(snapshot, split.vertices,
		1, 2));
	CHECK("split", memcmp(serial.bytes, split.bytes,
		snapshot.outputCapacityBytes) == 0);
	CHECK("split", split.bytes[snapshot.outputCapacityBytes] == 0xA5);
	CHECK("split", ValidatePreparedHeightMapDynamicLightOutput(snapshot,
		split.vertices));
	return 0;
}

static int testPointSpotAndDisabledLights()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	unsigned index;
	memset(&input, 0, sizeof(input));
	memset(&output, 0xA5, sizeof(output));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	for (index = 0; index < 4; ++index)
		initializeVertex(&input.vertices[index], 0.0f, 0.0f, 0.0f,
			0x80201010u);
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_POINT;
	light.enabled = 1;
	light.positionZ = 1.0f;
	light.range = 4.0;
	light.midRange = 1.0;
	light.diffuseRed = 1.0f;
	light.diffuseGreen = 0.5f;
	light.diffuseBlue = 0.25f;
	snapshot.width = 1;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.vertexCount = 4;
	snapshot.vertices = input.vertices;
	snapshot.lights = &light;
	snapshot.lightCount = 1;
	snapshot.outputCapacityBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.batchBytes = snapshot.outputCapacityBytes * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("point", PrepareHeightMapDynamicLightRows(snapshot, output.vertices,
		0, 1));
	CHECK("point", output.vertices[0].diffuse != input.vertices[0].diffuse);
	CHECK("point", output.vertices[0].diffuse == 0x80FF8F4Fu);
	light.positionZ = 0.0f;
	memset(output.bytes, 0xA5, sizeof(output));
	CHECK("point-zero-distance", PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("point-zero-distance", output.vertices[0].diffuse ==
		input.vertices[0].diffuse);
	light.positionZ = 1.0f;
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_SPOT;
	memset(output.bytes, 0xA5, sizeof(output));
	CHECK("spot", PrepareHeightMapDynamicLightRows(snapshot, output.vertices,
		0, 1));
	CHECK("spot", output.vertices[0].diffuse != input.vertices[0].diffuse);
	CHECK("spot", output.vertices[0].diffuse == 0x80FF8F4Fu);
	light.enabled = 0;
	memset(output.bytes, 0xA5, sizeof(output));
	CHECK("disabled", PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("disabled", output.vertices[0].diffuse == input.vertices[0].diffuse);
	light.enabled = 1;
	{
		volatile double zero = 0.0;
		light.range = zero / zero;
	}
	memset(output.bytes, 0xA5, sizeof(output));
	CHECK("nan-range", !PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	CHECK("nan-range", output.bytes[0] == 0xA5);
	return 0;
}

static int testNontrivialPointAttenuationExpectedOutput()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	unsigned index;
	memset(&input, 0, sizeof(input));
	memset(&output, 0xA5, sizeof(output));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	for (index = 0; index < 4; ++index)
		initializeVertex(&input.vertices[index], 0.0f, 0.0f, 0.0f,
			0x80000000u);
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_POINT;
	light.enabled = 1;
	light.positionZ = 2.0f;
	light.range = 3.0;
	light.midRange = 1.0;
	light.diffuseRed = 0.5f;
	light.diffuseGreen = 1.0f;
	light.diffuseBlue = 0.0f;
	snapshot.width = 1;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.vertexCount = 4;
	snapshot.vertices = input.vertices;
	snapshot.lights = &light;
	snapshot.lightCount = 1;
	snapshot.outputCapacityBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.batchBytes = snapshot.outputCapacityBytes * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("attenuation", PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	/* distance=2, midRange=1, range=3 gives factor 0.5; this independent
	 * oracle expects the resulting red/green truncation (63/127). */
	CHECK("attenuation", output.vertices[0].diffuse == 0x803F7F00u);
	return 0;
}

static int testAliasingAndAlignmentRejected()
{
	VertexStorage input;
	VertexStorage output;
	HeightMapDynamicLightSceneLight light;
	HeightMapDynamicLightSnapshot snapshot;
	memset(&input, 0, sizeof(input));
	memset(&output, 0x5A, sizeof(output));
	memset(&light, 0, sizeof(light));
	memset(&snapshot, 0, sizeof(snapshot));
	initializeVertex(&input.vertices[0], 0.0f, 0.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[1], 1.0f, 0.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[2], 1.0f, 1.0f, 0.0f, 0xFF000000u);
	initializeVertex(&input.vertices[3], 0.0f, 1.0f, 0.0f, 0xFF000000u);
	snapshot.width = 1;
	snapshot.height = 1;
	snapshot.inputStrideBytes = 4 * sizeof(HeightMapDynamicLightVertex);
	snapshot.outputStrideBytes = snapshot.inputStrideBytes;
	snapshot.vertexCount = 4;
	snapshot.vertices = input.vertices;
	snapshot.outputCapacityBytes = snapshot.outputStrideBytes;
	snapshot.lightCount = 1;
	light.type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	light.enabled = 1;
	light.directionZ = -1.0f;
	light.diffuseRed = 1.0f;
	light.diffuseGreen = 1.0f;
	light.diffuseBlue = 1.0f;
	snapshot.lights = &light;
	snapshot.batchBytes = snapshot.outputStrideBytes * 2 +
		sizeof(HeightMapDynamicLightSceneLight);
	CHECK("alias-input", !PrepareHeightMapDynamicLightRows(snapshot,
		input.vertices, 0, 1));
	CHECK("alias-input", output.bytes[0] == 0x5A);
	CHECK("alias-snapshot", !PrepareHeightMapDynamicLightRows(snapshot,
		reinterpret_cast<HeightMapDynamicLightVertex *>(&snapshot), 0, 1));
	snapshot.vertices = reinterpret_cast<HeightMapDynamicLightVertex *>(
		input.bytes + 1);
	CHECK("misaligned", !PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	snapshot.width = UINT_MAX;
	snapshot.height = 2;
	snapshot.vertexCount = 0;
	CHECK("overflow", !PrepareHeightMapDynamicLightRows(snapshot,
		output.vertices, 0, 1));
	return 0;
}

static int testServiceAdapterAndEmptyStripe()
{
	HeightMapDynamicLightBatch batch;
	RadarTerrainPrepareService service;
	HeightMapDynamicLightSnapshot expectedSnapshot;
	HeightMapDynamicLightVertex expected[8];
	unsigned index;

	CHECK("adapter", batch.initialize(2, 1, 1));
	for (index = 0; index < 8; ++index)
		initializeVertex(&batch.inputVertices()[index],
			static_cast<Real>(index), 0.0f, 0.0f, 0x80402010u);
	memset(batch.lights(), 0, sizeof(*batch.lights()));
	batch.lights()[0].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	batch.lights()[0].enabled = 1;
	batch.lights()[0].directionZ = -1.0f;
	batch.lights()[0].diffuseRed = 0.25f;
	batch.lights()[0].diffuseGreen = 0.5f;
	batch.lights()[0].diffuseBlue = 1.0f;
	memcpy(&expectedSnapshot, &batch.snapshot(), sizeof(expectedSnapshot));
	expectedSnapshot.outputCapacityBytes = sizeof(expected);
	expectedSnapshot.batchBytes = batch.snapshot().batchBytes;
	expectedSnapshot.vertices = batch.inputVertices();
	CHECK("adapter", PrepareHeightMapDynamicLightRows(expectedSnapshot,
		expected, 0, 1));
	/* A one-row edge update uses the deterministic serial cutoff and does not
	 * require the shared runtime or a consumer lease. */
	CHECK("adapter", batch.run(service));
	CHECK("adapter", memcmp(batch.outputVertices(), expected,
		sizeof(expected)) == 0);
	{
		HeightMapDynamicLightVertex *inputStorage = batch.inputVertices();
		HeightMapDynamicLightVertex *outputStorage = batch.outputVertices();
		CHECK("adapter-reuse", batch.initialize(2, 1, 1));
		CHECK("adapter-reuse", batch.inputVertices() == inputStorage);
		CHECK("adapter-reuse", batch.outputVertices() == outputStorage);
		CHECK("adapter-reuse", batch.outputVertices()[0].applyLighting == 255);
	}
	service.shutdown();
	return 0;
}

static int testServiceAdapterWorkerPath()
{
	const unsigned expectedDiffuse[32] = {
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu,
		0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu, 0x807F9FFFu
	};
	HeightMapDynamicLightBatch batch;
	RadarTerrainPrepareService service;
	unsigned index;
	bool ranParallel = false;
	#if defined(RTS_BUILD_CORE_EXTRAS)
	HeightMapDynamicLightTestThreadId threadIds[2];
	unsigned char arrivals[2] = { 0, 0 };
	HeightMapDynamicLightWorkerProbe probe(4, threadIds, arrivals);
	HeightMapDynamicLightObserverGuard observerGuard;
	#endif

	/* The adapter's serial cutoff is eight cells; 2x4 is the smallest
	 * two-row fixture that can reach the two-worker path. */
	CHECK("adapter-worker", batch.initialize(2, 4, 1));
	for (index = 0; index < 32; ++index)
		initializeVertex(&batch.inputVertices()[index],
			static_cast<Real>(index), 0.0f, 0.0f, 0x80402010u);
	memset(batch.lights(), 0, sizeof(*batch.lights()));
	batch.lights()[0].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
	batch.lights()[0].enabled = 1;
	batch.lights()[0].directionZ = -1.0f;
	batch.lights()[0].diffuseRed = 0.25f;
	batch.lights()[0].diffuseGreen = 0.5f;
	batch.lights()[0].diffuseBlue = 1.0f;
	CHECK("adapter-worker", service.initialize(2, 8));
	CHECK("adapter-worker", service.tryAcquire(5));
	#if defined(RTS_BUILD_CORE_EXTRAS)
	s_dynamicLightWorkerProbe = &probe;
	rts_height_map_dynamic_light_prepare_set_test_observer(
		observeDynamicLightWorker);
	#endif
	CHECK("adapter-worker", batch.run(service, &ranParallel));
	CHECK("adapter-worker", ranParallel);
	#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_height_map_dynamic_light_prepare_set_test_observer(0);
	s_dynamicLightWorkerProbe = 0;
	CHECK("adapter-worker", service.pendingTaskCount() == 0);
	CHECK("adapter-worker", arrivals[0] != 0 && arrivals[1] != 0);
	/* Owner waits are allowed to execute eligible ranges. Screenshot and
	 * scheduler stress tests separately prove execution beyond two workers. */
	#endif
	service.release(5);
	for (index = 0; index < 32; ++index)
		CHECK("adapter-worker", batch.outputVertices()[index].diffuse ==
			expectedDiffuse[index]);
	service.shutdown();
	return 0;
}

int main()
{
	if (testDirectionalAndAlpha() != 0)
		return 1;
	if (testInvalidInputDoesNotWrite() != 0)
		return 1;
	if (testUntouchedCellPreservesBaseline() != 0)
		return 1;
	if (testArbitraryRowSplitParity() != 0)
		return 1;
	if (testPointSpotAndDisabledLights() != 0)
		return 1;
	if (testNontrivialPointAttenuationExpectedOutput() != 0)
		return 1;
	if (testAliasingAndAlignmentRejected() != 0)
		return 1;
	if (testServiceAdapterAndEmptyStripe() != 0)
		return 1;
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (testServiceAdapterWorkerPath() != 0)
		return 1;
#endif
	return 0;
}
