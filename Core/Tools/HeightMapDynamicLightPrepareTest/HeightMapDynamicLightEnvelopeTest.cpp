#include "../../GameEngineDevice/Source/W3DDevice/GameClient/HeightMapDynamicLightEnvelope.h"
#include <cstdio>
#include <cstring>
#include <ctime>

static unsigned seed = 0x6d2b79f5u;
static unsigned nextRandom()
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static int mapped(int cell, int origin, int extent, int drawOrigin, int border)
{
	int value = cell - origin;
	if (value < 0) value += extent;
	if (value >= extent) value -= extent;
	return value + drawOrigin - border;
}

// Independent oracle: the inline current/previous test used by capture before
// the envelope change. Keep this expression separate from the shared helper.
static bool originalCellHit(const HeightMapDynamicLightBounds &light,
	int x, int y)
{
	return (light.minX <= x + 1 && light.maxX >= x &&
		light.minY <= y + 1 && light.maxY >= y) ||
		(light.prevMinX <= x + 1 && light.prevMaxX >= x &&
		light.prevMinY <= y + 1 && light.prevMaxY >= y);
}

static bool check(int width, int height, int xOrigin, int yOrigin,
	int xExtent, int yExtent, int drawX, int drawY, int border,
	const HeightMapDynamicLightBounds *lights, int lightCount)
{
	int xs[32], ys[32];
	for (int x = 0; x < width; ++x)
		xs[x] = mapped(x, xOrigin, xExtent, drawX, border);
	for (int y = 0; y < height; ++y)
		ys[y] = mapped(y, yOrigin, yExtent, drawY, border);
	HeightMapDynamicLightEnvelope result;
	const bool found = HeightMapFindDynamicLightEnvelope(xs, width, ys,
		height, lights, lightCount, result);
	int x0 = width, y0 = height, x1 = 0, y1 = 0;
	unsigned char full[32 * 32 * 4];
	unsigned char partial[32 * 32 * 4];
	memset(full, 0x5a, sizeof(full));
	memset(partial, 0x5a, sizeof(partial));
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			bool hit = false;
			for (int l = 0; l < lightCount; ++l)
			{
				const bool oracle = originalCellHit(lights[l], xs[x], ys[y]);
				if (oracle != HeightMapDynamicLightCellHit(lights[l],
					xs[x], ys[y]))
					return false;
				if (oracle)
					hit = true;
			}
			if (!hit) continue;
			if (x < x0) x0 = x;
			if (y < y0) y0 = y;
			if (x + 1 > x1) x1 = x + 1;
			if (y + 1 > y1) y1 = y + 1;
			for (int corner = 0; corner < 4; ++corner)
				full[(y * width + x) * 4 + corner] =
					static_cast<unsigned char>((x * 17 + y * 31 + corner) & 255);
		}
	if (found != (x1 > x0 && y1 > y0) ||
		(found && (result.x0 != x0 || result.y0 != y0 ||
			result.x1 != x1 || result.y1 != y1)))
		return false;
	if (found)
		for (int y = result.y0; y < result.y1; ++y)
			for (int x = result.x0; x < result.x1; ++x)
				for (int l = 0; l < lightCount; ++l)
					if (HeightMapDynamicLightCellHit(lights[l], xs[x], ys[y]))
					{
						for (int corner = 0; corner < 4; ++corner)
							partial[(y * width + x) * 4 + corner] =
								static_cast<unsigned char>((x * 17 + y * 31 + corner) & 255);
						break;
					}
	return memcmp(full, partial, sizeof(full)) == 0;
}

static HeightMapDynamicLightBounds rectangle(int x0, int y0, int x1, int y1,
	int px0, int py0, int px1, int py1)
{
	HeightMapDynamicLightBounds light =
		{x0, y0, x1, y1, px0, py0, px1, py1};
	return light;
}

static void benchmark(const char *name, const HeightMapDynamicLightBounds &light)
{
	int xs[32], ys[32];
	for (int n = 0; n < 32; ++n) xs[n] = ys[n] = n;
	volatile unsigned checksum = 0;
	const int repeats = 20000;
	clock_t start = clock();
	for (int repeat = 0; repeat < repeats; ++repeat)
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
				checksum += HeightMapDynamicLightCellHit(light, xs[x], ys[y]) ? 4 : 0;
	const double fullMs = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
	start = clock();
	HeightMapDynamicLightEnvelope envelope;
	for (int repeat = 0; repeat < repeats; ++repeat)
	{
		if (HeightMapFindDynamicLightEnvelope(xs, 32, ys, 32, &light, 1,
			envelope))
			for (int y = envelope.y0; y < envelope.y1; ++y)
				for (int x = envelope.x0; x < envelope.x1; ++x)
					checksum += HeightMapDynamicLightCellHit(light, xs[x], ys[y]) ? 4 : 0;
	}
	const double partialMs = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
	printf("%s: full %.2f ms, envelope+partial %.2f ms, area %d/%d, checksum %u\n",
		name, fullMs, partialMs,
		(envelope.x1 - envelope.x0) * (envelope.y1 - envelope.y0),
		32 * 32, checksum);
}

int main()
{
	// Both inclusive boundaries, including the previous footprint, matter.
	const HeightMapDynamicLightBounds boundary = rectangle(11, 11, 11, 11,
		21, 21, 21, 21);
	if (!originalCellHit(boundary, 10, 10) ||
		!originalCellHit(boundary, 11, 11) ||
		!originalCellHit(boundary, 20, 20) ||
		!originalCellHit(boundary, 21, 21) ||
		originalCellHit(boundary, 9, 10) ||
		originalCellHit(boundary, 12, 11) ||
		originalCellHit(boundary, 19, 20) ||
		originalCellHit(boundary, 22, 21))
		return 3;
	HeightMapDynamicLightBounds cases[] = {
		boundary, // exact current and previous inclusive boundaries
		rectangle(10, 10, 14, 14, 10, 10, 14, 14), // normal
		rectangle(0, 0, 2, 2, 0, 0, 2, 2),       // edge
		rectangle(20, 20, 23, 23, 2, 2, 4, 4), // moving
		rectangle(12, 12, 12, 12, 3, 3, 8, 8), // shrinking
		rectangle(99, 99, 100, 100, 5, 5, 8, 8), // disabled old footprint
		// Exact-empty case: the caller retains its full-tile lock/Commit.
		rectangle(99, 99, 100, 100, 99, 99, 100, 100),
		rectangle(-10, -10, 100, 100, -10, -10, 100, 100) // all lit
	};
	for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n)
		if (!check(32, 32, 0, 0, 63, 63, 0, 0, 0, cases + n, 1) ||
			!check(32, 32, 17, 25, 40, 40, 7, 3, 4, cases + n, 1) ||
			!check(7, 5, 3, 2, 7, 5, 2, 1, 3, cases + n, 1))
			return 1;
	for (int n = 0; n < 4000; ++n)
	{
		HeightMapDynamicLightBounds lights[3];
		for (int l = 0; l < 3; ++l)
		{
			const int x = static_cast<int>(nextRandom() % 64) - 16;
			const int y = static_cast<int>(nextRandom() % 64) - 16;
			const int px = static_cast<int>(nextRandom() % 64) - 16;
			const int py = static_cast<int>(nextRandom() % 64) - 16;
			lights[l] = rectangle(x, y, x + static_cast<int>(nextRandom() % 16),
				y + static_cast<int>(nextRandom() % 16), px, py,
				px + static_cast<int>(nextRandom() % 16),
				py + static_cast<int>(nextRandom() % 16));
		}
		if (!check(32, 32, static_cast<int>(nextRandom() % 32),
			static_cast<int>(nextRandom() % 32), 40, 40,
			static_cast<int>(nextRandom() % 9),
			static_cast<int>(nextRandom() % 9),
			static_cast<int>(nextRandom() % 5), lights, 3))
			return 2;
	}
	benchmark("sparse", cases[1]);
	benchmark("all-lit", cases[sizeof(cases) / sizeof(cases[0]) - 1]);
	puts("envelope mask and modeled diffuse bytes match");
	return 0;
}
