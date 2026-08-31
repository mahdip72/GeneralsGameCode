#include "Lib/JobSystem.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/HeightMapTerrainKernel.h"
#include "Lib/RadarOverlayKernel.h"
#include "W3DDevice/Common/EffectPrepare.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault, unsigned occurrence);
#endif

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static ParticleRenderBounds bounds()
{
	ParticleRenderBounds result = { 40.0f, -20.0f, 15.0f, 150.0f, 100.0f, 50.0f };
	return result;
}

static void fillParticles(ParticleRenderBatch &batch)
{
	for (unsigned index = 0; index < batch.count(); ++index)
	{
		ParticleRenderInput &particle = batch.input()[index];
		particle.x = static_cast<Real>(static_cast<int>(index%401)-200);
		particle.y = static_cast<Real>(static_cast<int>(index%307)-150);
		particle.z = static_cast<Real>(static_cast<int>(index%113)-40);
		particle.size = static_cast<Real>(index%17)*0.25f;
		particle.red = static_cast<Real>(index%11)*0.1f;
		particle.green = 0.2f;
		particle.blue = 0.7f;
		particle.alpha = static_cast<Real>(index%5)*0.25f;
		particle.angle = (static_cast<Real>(static_cast<int>(index%1021)-510)/255.0f)*PI;
		particle.personality = index*0x9e3779b9u;
	}
	if (batch.count() != 0)
	{
		batch.input()[0].x = 190.0f;
		batch.input()[0].y = -20.0f;
		batch.input()[0].z = 15.0f;
		batch.input()[0].size = 0.0f; // Inclusive culling boundary.
	}
	if (batch.count() > 1)
	{
		batch.input()[1] = batch.input()[0];
		unsigned nextFloat;
		memcpy(&nextFloat, &batch.input()[1].x, sizeof(nextFloat));
		++nextFloat;
		memcpy(&batch.input()[1].x, &nextFloat, sizeof(nextFloat));
		batch.input()[1].personality = 12345;
	}
}

static ParticleRenderOutput referenceParticle(const ParticleRenderInput &particle,
	const ParticleRenderBounds &box)
{
	ParticleRenderOutput result = {0, 0};
	if (fabs(particle.x-box.centerX) > box.extentX+particle.size) return result;
	if (fabs(particle.y-box.centerY) > box.extentY+particle.size) return result;
	if (fabs(particle.z-box.centerZ) > box.extentZ+particle.size) return result;
	result.visible = 1;
	/* This is the original owner's direct byte conversion. */
	result.angle = static_cast<UnsignedByte>(particle.angle*255.0f/(2.0f*PI));
	return result;
}

static int particleParity(RadarTerrainPrepareService &service, unsigned count)
{
	ParticleRenderBatch batch;
	CHECK(batch.initialize(count));
	fillParticles(batch);
	std::vector<ParticleRenderOutput> expected(count);
	std::vector<ParticleRenderInput> captured(count);
	for (unsigned index = 0; index < count; ++index)
	{
		expected[index] = referenceParticle(batch.input()[index], bounds());
		captured[index] = batch.input()[index];
	}
	bool parallel = true;
	CHECK(batch.run(bounds(), service, &parallel));
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	CHECK(!service.hasLease());
	if (count < 256) CHECK(!parallel);
	if (count != 0)
	{
		CHECK(memcmp(batch.output(), &expected[0], count*sizeof(ParticleRenderOutput)) == 0);
		CHECK(memcmp(batch.input(), &captured[0], count*sizeof(ParticleRenderInput)) == 0);
		// Preserve source identity under the owner's stable 512-visible cap.
		unsigned visible = 0;
		unsigned referenceVisible = 0;
		for (unsigned index = 0; index < count; ++index)
		{
			if (batch.output()[index].visible && visible < 512) ++visible;
			if (expected[index].visible && referenceVisible < 512) ++referenceVisible;
			CHECK(visible == referenceVisible);
		}
		ParticleRenderInput *storage = batch.input();
		const ParticleRenderOutput saved = batch.output()[0];
		batch.input()[0].x = 99999.0f;
		CHECK(memcmp(&saved, batch.output(), sizeof(saved)) == 0);
		CHECK(batch.initialize(count));
		CHECK(batch.input() == storage); // No same-size frame allocation.
		CHECK(batch.run(bounds(), service));
		CHECK(batch.output()[0].visible == 0);
	}
	return 0;
}

static void fillWater(WaterMeshBatch &batch)
{
	WaterMeshSnapshot &snapshot = batch.snapshot();
	snapshot.cellSizeX = 10.0f;
	snapshot.uScale = 0.03437f;
	snapshot.diffuse = 0x8fc08734u;
	for (unsigned row = 0; row < snapshot.height; ++row)
	{
		batch.rows()[row].y = static_cast<Real>(row)*10.0f;
		batch.rows()[row].v1 = 0.125f+static_cast<Real>(row)*0.017f;
		batch.rows()[row].v2 = (static_cast<Real>(row)+0.5f)*0.2f+static_cast<Real>(row)*0.06f;
		for (unsigned column = 0; column < snapshot.width; ++column)
			batch.heights()[row*snapshot.width+column] = static_cast<Real>((row*17+column*7)%101)*0.125f;
	}
}

static std::vector<WaterMeshVertex> referenceWater(const WaterMeshSnapshot &snapshot)
{
	std::vector<WaterMeshVertex> result(snapshot.width*snapshot.height);
	for (unsigned row = 0; row < snapshot.height; ++row)
		for (unsigned column = 0; column < snapshot.width; ++column)
		{
			WaterMeshVertex &vertex = result[row*snapshot.width+column];
			const Real x = static_cast<Real>(column)*snapshot.cellSizeX;
			vertex.x = x;
			vertex.y = snapshot.rows[row].y;
			vertex.z = snapshot.heights[row*snapshot.width+column];
			vertex.diffuse = snapshot.diffuse;
			vertex.u1 = static_cast<Real>(column)*snapshot.uScale;
			vertex.v1 = snapshot.rows[row].v1;
			vertex.u2 = static_cast<Real>(column)*snapshot.cellSizeX/50.0f;
			vertex.v2 = snapshot.rows[row].v2;
		}
	return result;
}

static int waterParity(RadarTerrainPrepareService &service, unsigned width, unsigned height)
{
	WaterMeshBatch batch;
	CHECK(batch.initialize(width, height));
	fillWater(batch);
	const std::vector<WaterMeshVertex> expected = referenceWater(batch.snapshot());
	bool parallel = false;
	CHECK(batch.run(service, &parallel));
	CHECK(memcmp(batch.output(), &expected[0], expected.size()*sizeof(WaterMeshVertex)) == 0);
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	CHECK(!service.hasLease());
	Real *heights = batch.heights();
	batch.heights()[0] += 42.0f;
	CHECK(batch.output()[0].z == expected[0].z);
	CHECK(batch.initialize(width, height));
	CHECK(batch.heights() == heights);
	CHECK(batch.run(service));
	CHECK(batch.output()[0].z == expected[0].z+42.0f);
	return 0;
}

static int invalidAndRanges(RadarTerrainPrepareService &service)
{
	ParticleRenderBatch particles;
	CHECK(!particles.run(bounds(), service));
	CHECK(!particles.initialize(PARTICLE_RENDER_MAX_COUNT+1));
	CHECK(particles.initialize(32));
	fillParticles(particles);
	ParticleRenderOutput guarded[34];
	memset(guarded, 0x6a, sizeof(guarded));
	CHECK(PrepareParticleRenderRange(particles.input(), guarded+1, 32, bounds(), 7, 21));
	for (unsigned index = 0; index < 34; ++index)
		if (index < 8 || index >= 22) CHECK(guarded[index].visible == 0x6a6a6a6au);
	CHECK(!PrepareParticleRenderRange(particles.input(), guarded+1, 32, bounds(), 22, 33));
	CHECK(!PrepareParticleRenderRange(particles.input(),
		reinterpret_cast<ParticleRenderOutput *>(particles.input()), 32, bounds(), 0, 32));
	CHECK(PrepareParticleRenderRange(0, 0, 0, bounds(), 0, 0));
	WaterMeshBatch water;
	CHECK(!water.initialize(UINT_MAX, 2));
	CHECK(!water.initialize(0, 2));
	CHECK(water.initialize(7, 9));
	fillWater(water);
	std::vector<WaterMeshVertex> output(7*9+2);
	memset(&output[0], 0x6a, output.size()*sizeof(WaterMeshVertex));
	CHECK(PrepareWaterMeshRows(water.snapshot(), &output[1], 2, 6));
	for (unsigned waterIndex = 0; waterIndex < output.size(); ++waterIndex)
		if (waterIndex < 1+7*2 || waterIndex >= 1+7*6) CHECK(output[waterIndex].diffuse == 0x6a6a6a6au);
	const unsigned width = water.snapshot().width;
	water.snapshot().width = UINT_MAX;
	CHECK(!water.run(service));
	water.snapshot().width = width;
	water.snapshot().uScale = 1048577.0f;
	CHECK(!water.run(service));
	return 0;
}

static int failureRecovery(RadarTerrainPrepareService &service)
{
	ParticleRenderBatch particles;
	CHECK(particles.initialize(8192));
	fillParticles(particles);
	CHECK(service.tryAcquire(99));
	bool parallel = true;
	CHECK(particles.run(bounds(), service, &parallel));
	CHECK(!parallel && service.activeConsumer() == 99);
	service.release(99);
	WaterMeshBatch water;
	CHECK(water.initialize(128, 128));
	fillWater(water);
	const std::vector<WaterMeshVertex> expected = referenceWater(water.snapshot());
	// A failed late stripe must never be reported as a publishable frame.
	const unsigned nanBits = 0x7fc00000u;
	const unsigned last = water.snapshot().width*water.snapshot().height-1;
	const Real savedHeight = water.heights()[last];
	memcpy(&water.heights()[last], &nanBits, sizeof(nanBits));
	CHECK(!water.run(service));
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	CHECK(!service.hasLease());
	water.heights()[last] = savedHeight;
	CHECK(water.run(service));
	CHECK(memcmp(water.output(), &expected[0], expected.size()*sizeof(WaterMeshVertex)) == 0);
#if defined(RTS_BUILD_CORE_EXTRAS)
	for (unsigned fault = 1; fault <= 2; ++fault)
	{
		ParticleRenderBatch failedParticles;
		rts_effect_prepare_set_test_fault(fault);
		CHECK(!failedParticles.initialize(100));
		CHECK(!failedParticles.run(bounds(), service));
		CHECK(failedParticles.initialize(100));
		WaterMeshBatch failedWater;
		rts_effect_prepare_set_test_fault(fault);
		CHECK(!failedWater.initialize(10, 10));
		CHECK(!failedWater.run(service));
		CHECK(failedWater.initialize(10, 10));
	}
	rts_radar_terrain_prepare_set_test_fault(RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
	CHECK(water.run(service));
	CHECK(memcmp(water.output(), &expected[0], expected.size()*sizeof(WaterMeshVertex)) == 0);
	const unsigned faults[] = {4, 5, 6}; // Group, handle, atomic batch admission.
	for (unsigned index = 0; index < sizeof(faults)/sizeof(faults[0]); ++index)
	{
		rts_job_system_set_test_fault(faults[index], 1);
		CHECK(water.run(service));
		CHECK(memcmp(water.output(), &expected[0], expected.size()*sizeof(WaterMeshVertex)) == 0);
		CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
		CHECK(!service.hasLease());
	}
	rts_job_system_set_test_fault(0, 0);
#endif
	CHECK(rts::JobSystem::instance().metrics().serialFallbackCount != 0);
	return 0;
}

static int terrainAdmission()
{
	HeightMapTerrainCellInput cells[8];
	HeightMapTerrainVertex expected[32], actual[32];
	HeightMapTerrainSnapshot snapshot;
	memset(cells, 0, sizeof(cells));
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.width=2; snapshot.height=4; snapshot.cellCount=8;
	snapshot.cells=cells; snapshot.cellRowStrideBytes=2*sizeof(cells[0]);
	snapshot.mapXYFactor=10.0f; snapshot.mapHeightScale=0.625f;
	snapshot.terrainAmbient.red=0.3f; snapshot.terrainAmbient.green=0.5f;
	snapshot.terrainAmbient.blue=0.7f;
	snapshot.outputStrideBytes=8*sizeof(expected[0]);
	snapshot.outputCapacityBytes=sizeof(expected);
	snapshot.batchBytes=sizeof(cells)+sizeof(expected);
	for (unsigned index=0; index<8; ++index)
		for (unsigned corner=0; corner<4; ++corner)
		{
			cells[index].x[corner]=static_cast<Real>(index)*10.0f;
			cells[index].y[corner]=static_cast<Real>(corner)*10.0f;
			cells[index].vertexHeight[corner]=static_cast<UnsignedByte>(index*3+corner);
			cells[index].alpha[corner]=static_cast<UnsignedByte>(index*19+corner);
			cells[index].flip=static_cast<UnsignedByte>(index%2);
		}
	CHECK(PrepareHeightMapTerrainRows(snapshot, expected, 0, 4));
	CHECK(ValidateHeightMapTerrainInput(snapshot, actual));
	CHECK(PrepareValidatedHeightMapTerrainRows(snapshot, actual, 0, 1));
	CHECK(PrepareValidatedHeightMapTerrainRows(snapshot, actual, 1, 3));
	CHECK(PrepareValidatedHeightMapTerrainRows(snapshot, actual, 3, 4));
	CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
	CHECK(ValidatePreparedHeightMapTerrainOutput(snapshot, actual));
	const unsigned nanBits=0x7fc00000u;
	memcpy(&cells[7].x[3], &nanBits, sizeof(nanBits));
	CHECK(!ValidateHeightMapTerrainInput(snapshot, actual));
	memset(actual, 0x6a, sizeof(actual));
	// The public/reference API still rejects invalid cells outside its slice.
	CHECK(!PrepareHeightMapTerrainRows(snapshot, actual, 0, 1));
	for (unsigned vertex=0; vertex<32; ++vertex) CHECK(actual[vertex].diffuse == 0x6a6a6a6au);
	return 0;
}

static int shroudOrder()
{
	const unsigned width = 31, height = 27, stride = width*4+8;
	std::vector<unsigned char> actual(stride*height, 0x6a), expected(actual);
	RadarShroudOverlayCommand commands[] = {
		{-50,-10,40,30,0x80abcdefu}, {3,4,25,21,0x12345678u},
		{0,13,30,13,0x99887766u}, {INT_MIN,INT_MIN,0,0,0xff001122u},
		{28,25,INT_MAX,INT_MAX,0x11223344u}, {5,7,4,7,0xffffffffu}
	};
	RadarShroudOverlaySnapshot snapshot;
	snapshot.width=width; snapshot.height=height; snapshot.bytesPerPixel=4;
	snapshot.formatCode=RADAR_OVERLAY_FORMAT_A8R8G8B8; snapshot.rowBytes=stride;
	snapshot.commandCount=snapshot.commandCapacity=sizeof(commands)/sizeof(commands[0]);
	snapshot.commands=commands; snapshot.output=&actual[0];
	for (unsigned row=0; row<height; ++row)
		for (unsigned command=0; command<snapshot.commandCount; ++command)
			for (unsigned column=0; column<width; ++column)
			{
				const RadarShroudOverlayCommand &item=commands[command];
				if (static_cast<int>(row)<item.minY || static_cast<int>(row)>item.maxY ||
					static_cast<int>(column)<item.minX || static_cast<int>(column)>item.maxX) continue;
				for (unsigned byte=0; byte<4; ++byte)
					expected[row*stride+column*4+byte]=static_cast<unsigned char>(item.packedColor>>(byte*8));
			}
	CHECK(PackRadarShroudRows(snapshot, 0, 9));
	CHECK(PackRadarShroudRows(snapshot, 9, height));
	CHECK(actual == expected);
	return 0;
}

static int floatingPointParity(RadarTerrainPrepareService &service)
{
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	// Workers were started before this intentionally non-default owner mode.
	// The guard also restores state if an assertion returns early.
	const rts::JobFloatingPointState saved;
	rts::JobFloatingPointScope restore(saved);
	const unsigned originalMxcsr = _mm_getcsr();
#if !defined(_WIN64)
	_controlfp(_PC_53 | _RC_UP, _MCW_PC | _MCW_RC);
#endif
	const unsigned altered = (originalMxcsr & ~0x6000u) | 0x4000u | 0x8000u;
	_mm_setcsr(altered);
	CHECK(particleParity(service, 65536) == 0);
	WaterMeshBatch water;
	CHECK(water.initialize(512, 128));
	fillWater(water);
	water.snapshot().cellSizeX = 1.0e-38f; // FTZ-sensitive vertex arithmetic.
	water.snapshot().uScale = 0.1f; // Inexact products expose RC_UP differences.
	const std::vector<WaterMeshVertex> expected = referenceWater(water.snapshot());
	CHECK(water.run(service));
	CHECK(memcmp(water.output(), &expected[0], expected.size()*sizeof(WaterMeshVertex)) == 0);
	CHECK((_mm_getcsr() & 0xffc0u) == (altered & 0xffc0u));
#if !defined(_WIN64)
	CHECK((_controlfp(0, 0) & (_MCW_PC | _MCW_RC)) == (_PC_53 | _RC_UP));
#endif
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
#else
	(void)service; // VC6 runs the reference kernel inline on the same owner.
#endif
	return 0;
}

class SerialRowProbe : public RadarPrepareRowWork
{
public:
	SerialRowProbe() : calls(0), rows(0) {}
	virtual bool executeRows(unsigned begin, unsigned end)
	{
		++calls;
		rows += end-begin;
		return true;
	}
	unsigned calls, rows;
};

static int serialPolicy()
{
	CHECK(rts::SetPipelineExecutionMode("serial"));
	rts::JobSystem &system = rts::JobSystem::instance();
	CHECK(!system.isRunning());
	RadarTerrainPrepareService service;
	CHECK(service.initialize(2, 128));
	CHECK(service.warmup());
	CHECK(!system.isRunning());
	CHECK(particleParity(service, 65536) == 0);
	CHECK(waterParity(service, 512, 256) == 0);
	CHECK(terrainAdmission() == 0);
	CHECK(shroudOrder() == 0);
	SerialRowProbe work;
	CHECK(service.tryAcquire(77));
	bool parallel = true;
	CHECK(service.runRows(work, 0, 2048, &parallel));
	CHECK(!parallel && work.calls == 1 && work.rows == 2048);
	service.release(77);
	CHECK(!system.isRunning());
	CHECK(system.metrics().submittedJobCount == 0);
	service.shutdown();
	puts("effect preparation serial policy uses no compute jobs");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--serial") == 0)
		return serialPolicy();
	const unsigned workers[] = {1,2,4,8,16,0};
	const unsigned counts[] = {0,1,127,256,4096,65536};
	rts::JobSystem &system = rts::JobSystem::instance();
	for (unsigned worker = 0; worker < sizeof(workers)/sizeof(workers[0]); ++worker)
	{
		rts::JobSystemConfig config = rts::JobSystem::startupConfig();
		config.workerCount = workers[worker];
		config.queueCapacity = 1024;
		config.pinWorkers = false;
		CHECK(system.start(config));
		RadarTerrainPrepareService service;
		CHECK(service.initialize(2, 128));
		for (unsigned count = 0; count < sizeof(counts)/sizeof(counts[0]); ++count)
			CHECK(particleParity(service, counts[count]) == 0);
		CHECK(waterParity(service, 1, 1) == 0);
		CHECK(waterParity(service, 7, 3) == 0);
		CHECK(waterParity(service, 127, 257) == 0);
		CHECK(waterParity(service, 512, 256) == 0);
		CHECK(invalidAndRanges(service) == 0);
		CHECK(failureRecovery(service) == 0);
		CHECK(floatingPointParity(service) == 0);
		const rts::JobSystemMetrics metrics = system.metrics();
		printf("effect workers=%u actual=%u submitted=%u active=%u fallback=%u\n",
			workers[worker], system.workerCount(), static_cast<unsigned>(metrics.submittedJobCount),
			metrics.maximumActiveWorkers, static_cast<unsigned>(metrics.serialFallbackCount));
#if !defined(_MSC_VER) || _MSC_VER >= 1300
		if (workers[worker] >= 4) CHECK(metrics.maximumActiveWorkers > 2);
#endif
		service.shutdown();
		CHECK(system.outstandingJobCount() == 0);
		system.shutdown();
	}
	CHECK(shroudOrder() == 0);
	CHECK(terrainAdmission() == 0);
	puts("effect preparation parity, range, allocation, admission and lifetime checks passed");
	return 0;
}
