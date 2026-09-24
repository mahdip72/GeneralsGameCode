#include "Lib/HeightMapDynamicLightKernel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
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
    const double ranges[] = {0.0, 1.0e-40, 0.9999999, 1.0,
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
    printf("edge_cases=%u byte_mismatches=0\n", 100002u + boundaryCases);
    return 0;
}

int main(int argc, char **argv)
{
    const int edge = edgeParity();
    if (edge)
        return edge;
    if (argc < 2 || strcmp(argv[1], "--benchmark") != 0)
        return 0;
    if (benchmark("menu89", 8) || benchmark("far", 10) || benchmark("mixed80", 8) ||
        benchmark("mixed50", 5) || benchmark("mixed20", 2) ||
        benchmark("near", 0))
        return 5;
    return 0;
}
