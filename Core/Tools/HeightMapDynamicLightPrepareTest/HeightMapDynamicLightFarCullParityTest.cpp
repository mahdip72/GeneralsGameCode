#include "Lib/HeightMapDynamicLightKernel.h"
#include "../../GameEngineDevice/Source/W3DDevice/GameClient/HeightMapDynamicLightEnvelope.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <windows.h>

bool BaselinePrepareHeightMapDynamicLightRows(
    const HeightMapDynamicLightSnapshot &, HeightMapDynamicLightVertex *,
    unsigned, unsigned);

static unsigned state = 0x8F3A5B21u;
static unsigned nextRandom()
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void setupSnapshot(HeightMapDynamicLightSnapshot *snapshot,
    const std::vector<HeightMapDynamicLightVertex> &input,
    const std::vector<HeightMapDynamicLightSceneLight> &lights,
    unsigned width, unsigned height)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->width = width;
    snapshot->height = height;
    snapshot->inputStrideBytes = width * 4u * sizeof(input[0]);
    snapshot->outputStrideBytes = snapshot->inputStrideBytes;
    snapshot->vertexCount = width * height * 4u;
    snapshot->vertices = input.data();
    snapshot->lights = lights.empty() ? 0 : lights.data();
    snapshot->lightCount = static_cast<unsigned>(lights.size());
    snapshot->outputCapacityBytes = snapshot->outputStrideBytes * height;
    snapshot->batchBytes = 2u * snapshot->outputCapacityBytes +
        static_cast<unsigned>(lights.size() * sizeof(lights[0]));
}

static bool compare(const HeightMapDynamicLightSnapshot &snapshot,
    std::vector<HeightMapDynamicLightVertex> &baseline,
    std::vector<HeightMapDynamicLightVertex> &candidate,
    bool split)
{
    memset(baseline.data(), 0xA5, baseline.size() * sizeof(baseline[0]));
    memset(candidate.data(), 0xA5, candidate.size() * sizeof(candidate[0]));
    const unsigned middle = split ? snapshot.height / 2 : snapshot.height;
    if (!BaselinePrepareHeightMapDynamicLightRows(snapshot, baseline.data(),
            0, middle) ||
        !PrepareHeightMapDynamicLightRows(snapshot, candidate.data(),
            0, middle))
        return false;
    if (split &&
        (!BaselinePrepareHeightMapDynamicLightRows(snapshot, baseline.data(),
            middle, snapshot.height) ||
         !PrepareHeightMapDynamicLightRows(snapshot, candidate.data(),
            middle, snapshot.height)))
        return false;
    return memcmp(baseline.data(), candidate.data(),
        snapshot.outputCapacityBytes) == 0 &&
        ValidatePreparedHeightMapDynamicLightOutput(snapshot, candidate.data());
}

static HeightMapDynamicLightSceneLight pointLight(float x, float y,
    float z, double range, double midRange)
{
    HeightMapDynamicLightSceneLight light = {};
    light.type = HEIGHTMAP_DYNAMIC_LIGHT_POINT;
    light.enabled = 1;
    light.positionX = x;
    light.positionY = y;
    light.positionZ = z;
    light.range = range;
    light.midRange = midRange;
    light.diffuseRed = 0.5f;
    light.diffuseGreen = 0.25f;
    light.diffuseBlue = 0.75f;
    light.ambientRed = 0.1f;
    light.ambientGreen = 0.05f;
    light.ambientBlue = 0.025f;
    return light;
}

static HeightMapDynamicLightBounds boundsForLight(
    const HeightMapDynamicLightSceneLight &light)
{
    const float range = static_cast<float>(light.range);
    HeightMapDynamicLightBounds bounds = {};
    bounds.minX = static_cast<int>((light.positionX - range) / 10.0f);
    bounds.maxX = static_cast<int>((light.positionX + range) / 10.0f + 1.0f);
    bounds.minY = static_cast<int>((light.positionY - range) / 10.0f);
    bounds.maxY = static_cast<int>((light.positionY + range) / 10.0f + 1.0f);
    bounds.prevMinX = bounds.minX;
    bounds.prevMaxX = bounds.maxX;
    bounds.prevMinY = bounds.minY;
    bounds.prevMaxY = bounds.maxY;
    return bounds;
}

static bool tilePruningParity()
{
    const unsigned width = 8, height = 8;
    std::vector<HeightMapDynamicLightVertex> input(width * height * 4u);
    std::vector<HeightMapDynamicLightVertex> baseline(input.size());
    std::vector<HeightMapDynamicLightVertex> full(input.size());
    std::vector<HeightMapDynamicLightVertex> candidate(input.size());
    std::vector<HeightMapDynamicLightSceneLight> lights(6);
    std::vector<HeightMapDynamicLightSceneLight> selected;
    HeightMapDynamicLightBounds bounds[6];
    int xCoords[8], yCoords[8];
    unsigned selectedCount = 0;
    unsigned paritySelectedCount = 0;

    lights[0] = pointLight(15.0f, 15.0f, 2.0f, 22.0, 5.0);
    lights[1] = pointLight(1000.0f, 1000.0f, 2.0f, 20.0, 5.0);
    lights[2] = pointLight(65.0f, 65.0f, 4.0f, 18.0, 4.0);
    lights[2].type = HEIGHTMAP_DYNAMIC_LIGHT_SPOT;
    lights[3] = pointLight(200.0f, 200.0f, 0.0f, 10.0, 4.0);
    lights[3].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
    lights[3].directionZ = -1.0f;
    lights[4] = pointLight(40.0f, 40.0f, 2.0f, 16.0, 4.0);
    lights[4].enabled = 0;
    lights[5] = pointLight(1000.0f, 1000.0f, 2.0f, 20.0, 5.0);
    lights[5].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
    lights[5].enabled = 0;
    for (unsigned index = 0; index < lights.size(); ++index)
        bounds[index] = boundsForLight(lights[index]);
    // Moving/disabled previous footprints require rewriting, but do not make
    // the current point/spot a contributor to this tile.
    bounds[1].prevMinX -= 100;
    bounds[1].prevMaxX -= 100;
    bounds[1].prevMinY -= 100;
    bounds[1].prevMaxY -= 100;
    for (int x = 0; x < static_cast<int>(width); ++x)
        xCoords[x] = x;
    for (int y = 0; y < static_cast<int>(height); ++y)
        yCoords[y] = y;

    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            bool apply = false;
            for (unsigned light = 0; light < lights.size(); ++light)
                if (HeightMapDynamicLightCellHit(bounds[light],
                        xCoords[x], yCoords[y]))
                    apply = true;
            for (unsigned corner = 0; corner < 4; ++corner)
            {
                HeightMapDynamicLightVertex &vertex =
                    input[(y * width + x) * 4u + corner];
                memset(&vertex, 0, sizeof(vertex));
                vertex.x = static_cast<float>(x * 10u + (corner & 1u) * 10u);
                vertex.y = static_cast<float>(y * 10u + (corner >> 1u) * 10u);
                vertex.z = static_cast<float>((x + y + corner) % 5u);
                vertex.normalX = 0.15f;
                vertex.normalY = -0.2f;
                vertex.normalZ = 0.95f;
                vertex.diffuse = 0xA5704020u;
                vertex.applyLighting = apply ? 1 : 0;
            }
        }

    HeightMapDynamicLightSceneLight selectedStorage[6];
    if (!HeightMapSelectDynamicLightContributors(lights.data(), bounds,
            static_cast<unsigned>(lights.size()), xCoords, width, yCoords,
            height, selectedStorage, &selectedCount))
        return false;
    paritySelectedCount = selectedCount;
    const unsigned expectedIndices[] = {0, 2, 3, 5};
    if (selectedCount != sizeof(expectedIndices) / sizeof(expectedIndices[0]))
        return false;
    unsigned selectedIndex = 0;
    for (unsigned index = 0; index < lights.size(); ++index)
    {
        bool expected = false;
        for (unsigned n = 0; n < sizeof(expectedIndices) /
                sizeof(expectedIndices[0]); ++n)
            if (expectedIndices[n] == index)
                expected = true;
        if (expected && memcmp(&selectedStorage[selectedIndex++],
                &lights[index], sizeof(lights[index])) != 0)
            return false;
    }

    HeightMapDynamicLightSnapshot fullSnapshot, prunedSnapshot;
    setupSnapshot(&fullSnapshot, input, lights, width, height);
    selected.assign(selectedStorage, selectedStorage + selectedCount);
    setupSnapshot(&prunedSnapshot, input, selected, width, height);
    memset(baseline.data(), 0xA5, baseline.size() * sizeof(baseline[0]));
    memset(full.data(), 0xA5, full.size() * sizeof(full[0]));
    memset(candidate.data(), 0xA5, candidate.size() * sizeof(candidate[0]));
    if (!BaselinePrepareHeightMapDynamicLightRows(fullSnapshot,
            baseline.data(), 0, height) ||
        !PrepareHeightMapDynamicLightRows(fullSnapshot, full.data(), 0, height) ||
        !PrepareHeightMapDynamicLightRows(prunedSnapshot, candidate.data(),
            0, height) ||
        memcmp(baseline.data(), full.data(), fullSnapshot.outputCapacityBytes) != 0 ||
        memcmp(full.data(), candidate.data(), fullSnapshot.outputCapacityBytes) != 0 ||
        !ValidatePreparedHeightMapDynamicLightOutput(prunedSnapshot,
            candidate.data()))
        return false;

    // An unsupported or malformed light must still reject the full list so
    // production takes the original lighting fallback before pruning.
    lights[1].ambientRed = std::numeric_limits<float>::quiet_NaN();
    if (ValidateHeightMapDynamicLightSceneLights(lights.data(),
            static_cast<unsigned>(lights.size())))
        return false;
    lights[1].ambientRed = 0.1f;
    lights[1].type = 99;
    if (!HeightMapSelectDynamicLightContributors(lights.data(), bounds,
            static_cast<unsigned>(lights.size()), xCoords, width, yCoords,
            height, selectedStorage, &selectedCount) ||
        selectedCount != 5 ||
        selectedStorage[1].type != 99 ||
        ValidateHeightMapDynamicLightSceneLights(lights.data(),
            static_cast<unsigned>(lights.size())))
        return false;

    // Wrapped/non-monotonic coordinates are tested with exactly the same
    // inclusive axis predicate as the tile rewrite mask.
    {
        const int wrappedX[] = {30, 31, 0, 1};
        const int wrappedY[] = {8, 9};
        HeightMapDynamicLightBounds wrapped = {0, 8, 2, 10, 50, 50, 52, 52};
        if (!HeightMapDynamicLightCurrentBoundsHitTile(wrapped, wrappedX, 4,
                wrappedY, 2))
            return false;
        wrapped.minX = 10;
        wrapped.maxX = 12;
        if (HeightMapDynamicLightCurrentBoundsHitTile(wrapped, wrappedX, 4,
                wrappedY, 2))
            return false;
    }
    printf("tile_pruning lights=%u->%u byte_mismatches=0 fallback=preserved\n",
        static_cast<unsigned>(lights.size()), paritySelectedCount);
    return true;
}

struct TilePerfCase
{
    int xCoords[32];
    int yCoords[32];
    std::vector<HeightMapDynamicLightVertex> input;
    std::vector<HeightMapDynamicLightVertex> fullOutput;
    std::vector<HeightMapDynamicLightVertex> prunedOutput;
    HeightMapDynamicLightSnapshot snapshot;
};

static int tilePruningBenchmark(const char *name, bool dense)
{
    const unsigned width = 32, height = 32;
    std::vector<HeightMapDynamicLightSceneLight> lights(17);
    std::vector<HeightMapDynamicLightBounds> bounds(lights.size());
    std::vector<TilePerfCase> tiles(16);
    for (unsigned index = 0; index < 16; ++index)
    {
        const unsigned lightX = dense ? 64 : (index % 4) * 32 + 16;
        const unsigned lightY = dense ? 64 : (index / 4) * 32 + 16;
        const float range = dense ? 2000.0f : 80.0f;
        lights[index] = pointLight(static_cast<float>(lightX * 10),
            static_cast<float>(lightY * 10), 8.0f, range, range / 3.0f);
        if (index & 1u)
            lights[index].type = HEIGHTMAP_DYNAMIC_LIGHT_SPOT;
        bounds[index] = boundsForLight(lights[index]);
    }
    lights[16] = pointLight(640.0f, 640.0f, 0.0f, 10.0, 5.0);
    lights[16].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
    lights[16].directionZ = -1.0f;
    bounds[16] = boundsForLight(lights[16]);

    for (unsigned tileY = 0; tileY < 4; ++tileY)
        for (unsigned tileX = 0; tileX < 4; ++tileX)
        {
            TilePerfCase &tile = tiles[tileY * 4 + tileX];
            const unsigned x0 = tileX * width, y0 = tileY * height;
            tile.input.resize(width * height * 4u);
            tile.fullOutput.resize(tile.input.size());
            tile.prunedOutput.resize(tile.input.size());
            for (unsigned x = 0; x < width; ++x)
                tile.xCoords[x] = static_cast<int>(x0 + x);
            for (unsigned y = 0; y < height; ++y)
                tile.yCoords[y] = static_cast<int>(y0 + y);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    bool apply = false;
                    for (unsigned light = 0; light < lights.size(); ++light)
                        if (HeightMapDynamicLightCellHit(bounds[light],
                                tile.xCoords[x], tile.yCoords[y]))
                            apply = true;
                    for (unsigned corner = 0; corner < 4; ++corner)
                    {
                        HeightMapDynamicLightVertex &vertex =
                            tile.input[(y * width + x) * 4u + corner];
                        memset(&vertex, 0, sizeof(vertex));
                        vertex.x = static_cast<float>((x0 + x) * 10u +
                            (corner & 1u) * 10u);
                        vertex.y = static_cast<float>((y0 + y) * 10u +
                            (corner >> 1u) * 10u);
                        vertex.z = static_cast<float>((x + y + corner) % 13u);
                        vertex.normalZ = 1.0f;
                        vertex.diffuse = 0xFF403020u;
                        vertex.applyLighting = apply ? 1 : 0;
                    }
                }
            setupSnapshot(&tile.snapshot, tile.input, lights, width, height);
        }

    // Verify exact full-versus-pruned output before collecting timings.
    for (size_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex)
    {
        TilePerfCase &tile = tiles[tileIndex];
        HeightMapDynamicLightSceneLight selected[17];
        unsigned selectedCount = 0;
        if (!HeightMapSelectDynamicLightContributors(lights.data(),
                bounds.data(), static_cast<unsigned>(lights.size()),
                tile.xCoords, width, tile.yCoords, height, selected,
                &selectedCount))
            return 1;
        HeightMapDynamicLightSnapshot pruned = tile.snapshot;
        pruned.lights = selectedCount ? selected : 0;
        pruned.lightCount = selectedCount;
        pruned.batchBytes = 2u * pruned.outputCapacityBytes +
            selectedCount * static_cast<unsigned>(sizeof(selected[0]));
        if (!PrepareHeightMapDynamicLightRows(tile.snapshot,
                tile.fullOutput.data(), 0, height) ||
            !PrepareHeightMapDynamicLightRows(pruned,
                tile.prunedOutput.data(), 0, height) ||
            memcmp(tile.fullOutput.data(), tile.prunedOutput.data(),
                tile.snapshot.outputCapacityBytes) != 0)
        {
            printf("tile benchmark parity mismatch case=%s tile=%u\n",
                name, static_cast<unsigned>(tileIndex));
            return 2;
        }
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const unsigned iterations = 12;
    double fullTicks = 0.0, prunedTicks = 0.0;
    unsigned long long retained = 0;
    for (unsigned round = 0; round < 4; ++round)
        for (unsigned path = 0; path < 2; ++path)
        {
            const bool usePruned = ((round + path) & 1u) != 0;
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            for (unsigned iteration = 0; iteration < iterations; ++iteration)
                for (size_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex)
                {
                    TilePerfCase &tile = tiles[tileIndex];
                    if (!usePruned)
                    {
                        if (!PrepareHeightMapDynamicLightRows(tile.snapshot,
                                tile.fullOutput.data(), 0, height))
                            return 3;
                    }
                    else
                    {
                        HeightMapDynamicLightSceneLight selected[17];
                        HeightMapDynamicLightSceneLight batchLights[17];
                        unsigned selectedCount = 0;
                        if (!ValidateHeightMapDynamicLightSceneLights(
                                lights.data(),
                                static_cast<unsigned>(lights.size())) ||
                            !HeightMapSelectDynamicLightContributors(
                                lights.data(), bounds.data(),
                                static_cast<unsigned>(lights.size()),
                                tile.xCoords, width, tile.yCoords, height,
                                selected, &selectedCount))
                            return 4;
                        for (unsigned index = 0; index < selectedCount; ++index)
                            batchLights[index] = selected[index];
                        HeightMapDynamicLightSnapshot pruned = tile.snapshot;
                        pruned.lights = selectedCount ? batchLights : 0;
                        pruned.lightCount = selectedCount;
                        pruned.batchBytes = 2u * pruned.outputCapacityBytes +
                            selectedCount * static_cast<unsigned>(sizeof(selected[0]));
                        retained += selectedCount;
                        if (!PrepareHeightMapDynamicLightRows(pruned,
                                tile.prunedOutput.data(), 0, height))
                            return 5;
                    }
                }
            QueryPerformanceCounter(&end);
            (usePruned ? prunedTicks : fullTicks) +=
                static_cast<double>(end.QuadPart - start.QuadPart);
        }
    const double frames = iterations * 4.0;
    printf("tile_case=%s tiles=%u lights=%u average_retained=%.2f/17 "
        "full_ms/frame=%.3f pruned_ms/frame=%.3f speedup=%.3fx\n",
        name, static_cast<unsigned>(tiles.size()),
        static_cast<unsigned>(lights.size()),
        static_cast<double>(retained) / (frames * tiles.size()),
        fullTicks * 1000.0 / frequency.QuadPart / frames,
        prunedTicks * 1000.0 / frequency.QuadPart / frames,
        fullTicks / prunedTicks);
    return 0;
}

static unsigned long long potentialRejects(
    const std::vector<HeightMapDynamicLightVertex> &vertices,
    const std::vector<HeightMapDynamicLightSceneLight> &lights,
    unsigned long long *eligiblePairs)
{
    unsigned long long rejected = 0;
    *eligiblePairs = 0;
    for (size_t v = 0; v < vertices.size(); ++v)
    {
        if (!vertices[v].applyLighting)
            continue;
        for (size_t l = 0; l < lights.size(); ++l)
        {
            const HeightMapDynamicLightSceneLight &light = lights[l];
            if (!light.enabled || light.range < 1.0 ||
                light.type == HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL)
                continue;
            ++*eligiblePairs;
            const float dx = vertices[v].x - light.positionX;
            const float dy = vertices[v].y - light.positionY;
            const float dz = vertices[v].z - light.positionZ;
            const float squared = dx * dx + dy * dy + dz * dz;
            const double twice = light.range * 2.0;
            if (static_cast<double>(squared) > twice * twice)
                ++rejected;
        }
    }
    return rejected;
}

static int benchmark(const char *name, unsigned farLights)
{
    const unsigned width = 32, height = 32;
    std::vector<HeightMapDynamicLightVertex> input(width * height * 4u);
    std::vector<HeightMapDynamicLightVertex> baseline(input.size());
    std::vector<HeightMapDynamicLightVertex> candidate(input.size());
    std::vector<HeightMapDynamicLightSceneLight> lights(10);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            for (unsigned corner = 0; corner < 4; ++corner)
            {
                HeightMapDynamicLightVertex &v =
                    input[(y * width + x) * 4u + corner];
                memset(&v, 0, sizeof(v));
                v.x = static_cast<float>(x * 10 + (corner & 1));
                v.y = static_cast<float>(y * 10 + (corner >> 1));
                v.z = static_cast<float>((x + y + corner) % 9);
                v.normalZ = 1.0f;
                v.diffuse = 0xFF403020u;
                v.applyLighting = 1;
            }
    for (unsigned i = 0; i < lights.size(); ++i)
        lights[i] = i < farLights ?
            pointLight(static_cast<float>(1000 + i * 200), 1000.0f,
                10.0f, 80.0, 20.0) :
            pointLight(static_cast<float>(70 + i * 12), 150.0f,
                10.0f, 600.0, 100.0);
    if (strcmp(name, "menu89") == 0)
        lights[8] = pointLight(0.0f, -80.0f, 10.0f, 80.0, 20.0);
    HeightMapDynamicLightSnapshot snapshot;
    setupSnapshot(&snapshot, input, lights, width, height);
    if (!compare(snapshot, baseline, candidate, false) ||
        !compare(snapshot, baseline, candidate, true))
        return 1;
    unsigned long long pairs = 0;
    const unsigned long long rejects = potentialRejects(input, lights, &pairs);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const unsigned iterations = 80;
    double baselineTicks = 0.0, candidateTicks = 0.0;
    for (unsigned round = 0; round < 5; ++round)
    {
        for (unsigned path = 0; path < 2; ++path)
        {
            const bool useCandidate = ((round + path) & 1u) != 0;
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            for (unsigned i = 0; i < iterations; ++i)
            {
                const bool ok = useCandidate ?
                    PrepareHeightMapDynamicLightRows(snapshot,
                        candidate.data(), 0, height) :
                    BaselinePrepareHeightMapDynamicLightRows(snapshot,
                        baseline.data(), 0, height);
                if (!ok)
                    return 2;
            }
            QueryPerformanceCounter(&end);
            (useCandidate ? candidateTicks : baselineTicks) +=
                static_cast<double>(end.QuadPart - start.QuadPart);
        }
    }
    printf("case=%s eligible_pairs=%llu conservative_rejects=%llu "
        "reject_pct=%.2f baseline_ms=%.3f candidate_ms=%.3f speedup=%.3fx\n",
        name, pairs, rejects,
        pairs ? 100.0 * static_cast<double>(rejects) / pairs : 0.0,
        baselineTicks * 1000.0 / frequency.QuadPart / (iterations * 5),
        candidateTicks * 1000.0 / frequency.QuadPart / (iterations * 5),
        baselineTicks / candidateTicks);
    return 0;
}

static int edgeParity()
{
    std::vector<HeightMapDynamicLightVertex> input(4);
    std::vector<HeightMapDynamicLightVertex> baseline(4), candidate(4);
    std::vector<HeightMapDynamicLightSceneLight> lights(1);
    HeightMapDynamicLightSnapshot snapshot;
    const double ranges[] = {-0.0, 0.0, 1.0e-40, 0.9999999, 1.0,
        1.0000001, 20.0, 1.0e8};
    unsigned boundaryCases = 0;
    for (unsigned r = 0; r < sizeof(ranges) / sizeof(ranges[0]); ++r)
        for (unsigned place = 0; place < 9; ++place)
        {
            const float center = static_cast<float>(ranges[r] *
                (place < 3 ? 1.0 : place < 6 ? 2.0 : 3.0));
            const float x = (place % 3 == 0) ?
                std::nextafter(center, -INFINITY) :
                (place % 3 == 1) ? center :
                std::nextafter(center, INFINITY);
            if (std::fabs(x) > 1.0e8f)
                continue; // The kernel rejects positions outside this domain.
            ++boundaryCases;
            for (unsigned v = 0; v < 4; ++v)
            {
                HeightMapDynamicLightVertex &vertex = input[v];
                memset(&vertex, 0, sizeof(vertex));
                vertex.x = x;
                vertex.normalZ = 1.0f;
                vertex.diffuse = 0x807F3F1Fu;
                vertex.applyLighting = 1;
            }
            lights[0] = pointLight(0.0f, 0.0f, 0.0f,
                ranges[r], ranges[r] < 1.0 ? 0.0 : ranges[r] / 2.0);
            setupSnapshot(&snapshot, input, lights, 1, 1);
            if (!compare(snapshot, baseline, candidate, false))
            {
                printf("edge mismatch range=%.17g place=%u x=%g baseline=%08X candidate=%08X\n",
                    ranges[r], place, x, baseline[0].diffuse,
                    candidate[0].diffuse);
                return 3;
            }
        }
    lights.resize(3);
    lights[0] = pointLight(0.0f, 0.0f, 0.0f, 20.0, 5.0);
    lights[1] = pointLight(0.0f, 0.0f, 0.0f, 20.0, 5.0);
    lights[1].type = HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL;
    lights[1].directionZ = -1.0f;
    lights[2] = pointLight(0.0f, 0.0f, 0.0f, 20.0, 5.0);
    lights[2].enabled = 0;
    setupSnapshot(&snapshot, input, lights, 1, 1);
    if (!compare(snapshot, baseline, candidate, false))
        return 6;
    lights.resize(1);
    const double invalidRanges[] = {-1.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()};
    for (unsigned r = 0; r < sizeof(invalidRanges) /
            sizeof(invalidRanges[0]); ++r)
    {
        lights[0] = pointLight(0.0f, 0.0f, 0.0f,
            invalidRanges[r], 5.0);
        setupSnapshot(&snapshot, input, lights, 1, 1);
        memset(baseline.data(), 0xA5, baseline.size() * sizeof(baseline[0]));
        memset(candidate.data(), 0xA5, candidate.size() * sizeof(candidate[0]));
        if (BaselinePrepareHeightMapDynamicLightRows(snapshot,
                baseline.data(), 0, 1) ||
            PrepareHeightMapDynamicLightRows(snapshot,
                candidate.data(), 0, 1) ||
            memcmp(baseline.data(), candidate.data(),
                snapshot.outputCapacityBytes) != 0 ||
            reinterpret_cast<unsigned char *>(candidate.data())[0] != 0xA5)
            return 8;
    }
    for (unsigned iteration = 0; iteration < 100000; ++iteration)
    {
        const unsigned code = nextRandom();
        const float x = (code & 7u) == 0 ? 1.0e8f :
            static_cast<float>(static_cast<int>(code % 20001u) - 10000) /
                20.0f;
        for (unsigned v = 0; v < 4; ++v)
        {
            HeightMapDynamicLightVertex &vertex = input[v];
            memset(&vertex, 0, sizeof(vertex));
            vertex.x = x;
            vertex.y = static_cast<float>(nextRandom() % 1000u);
            vertex.z = static_cast<float>(nextRandom() % 100u);
            vertex.normalZ = 1.0f;
            vertex.diffuse = 0xA5704020u;
            vertex.applyLighting = static_cast<UnsignedByte>(nextRandom() & 1u);
        }
        const double range = (code & 15u) == 0 ? 1.0e-40 :
            (code & 15u) == 1 ? 1.0e8 :
            1.0 + static_cast<double>(code % 10000u) / 10.0;
        lights[0] = pointLight(static_cast<float>(nextRandom() % 2000u),
            static_cast<float>(nextRandom() % 2000u), 10.0f,
            range, range < 1.0 ? 0.0 : range / 2.0);
        lights[0].type = (code & 2u) ? HEIGHTMAP_DYNAMIC_LIGHT_SPOT :
            HEIGHTMAP_DYNAMIC_LIGHT_POINT;
        lights[0].enabled = static_cast<UnsignedByte>((code & 4u) != 0);
        if ((code & 31u) == 0)
            lights[0].midRange = 0.0;
        setupSnapshot(&snapshot, input, lights, 1, 1);
        if (!compare(snapshot, baseline, candidate, false))
        {
            printf("edge mismatch iteration=%u code=%u range=%.17g\n",
                iteration, code, range);
            return 4;
        }
    }
    /* Inputs outside the validated finite-vertex domain never write output.
     * Within that domain, three squared 2e8-coordinate deltas cannot
     * overflow Real; this exercises the rejection instead. */
    input[0].x = 3.0e38f;
    setupSnapshot(&snapshot, input, lights, 1, 1);
    memset(baseline.data(), 0xA5, baseline.size() * sizeof(baseline[0]));
    memset(candidate.data(), 0xA5, candidate.size() * sizeof(candidate[0]));
    if (BaselinePrepareHeightMapDynamicLightRows(snapshot, baseline.data(),
            0, 1) || PrepareHeightMapDynamicLightRows(snapshot,
            candidate.data(), 0, 1) ||
        memcmp(baseline.data(), candidate.data(),
            snapshot.outputCapacityBytes) != 0 ||
        reinterpret_cast<unsigned char *>(candidate.data())[0] != 0xA5)
        return 7;
    printf("edge_cases=%u byte_mismatches=0\n", 100002u + boundaryCases +
        static_cast<unsigned>(sizeof(invalidRanges) / sizeof(invalidRanges[0])));
    return 0;
}

int main(int argc, char **argv)
{
    if (!tilePruningParity())
        return 9;
    const int edge = edgeParity();
    if (edge)
        return edge;
    if (argc < 2 || strcmp(argv[1], "--benchmark") != 0)
        return 0;
    if (tilePruningBenchmark("menu-sparse", false) ||
        tilePruningBenchmark("dense-control", true))
        return 10;
    if (benchmark("menu89", 8) || benchmark("far", 10) || benchmark("mixed80", 8) ||
        benchmark("mixed50", 5) || benchmark("mixed20", 2) ||
        benchmark("near", 0))
        return 5;
    return 0;
}
