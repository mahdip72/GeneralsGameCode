#include "Common/BezierSegment.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
#include <float.h>
#endif

namespace
{

struct PointBits
{
	unsigned long x;
	unsigned long y;
	unsigned long z;
};

static const PointBits LEGACY_SCORPION_SHELL_PATH[] = {
	{0x44D63D79UL, 0x44D0C5F2UL, 0x41CFAA2EUL},
	{0x44D86F43UL, 0x44CFE669UL, 0x41E73DCAUL},
	{0x44DA81C9UL, 0x44CF1351UL, 0x41F8211EUL},
	{0x44DC6F5DUL, 0x44CE4EEDUL, 0x4200C875UL},
	{0x44DE324FUL, 0x44CD9B7FUL, 0x420164F9UL},
	{0x44DFC4F0UL, 0x44CCFB4BUL, 0x41FB08F6UL},
	{0x44E12192UL, 0x44CC7093UL, 0x41E98AB8UL},
	{0x44E24285UL, 0x44CBFD9BUL, 0x41CD8BF9UL},
	{0x44E3221AUL, 0x44CBA4A5UL, 0x41A6497AUL},
	{0x44E3BAA2UL, 0x44CB67F5UL, 0x4165FFFAUL},
};

float FloatFromBits(unsigned long bits)
{
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

unsigned long BitsFromFloat(float value)
{
	unsigned long bits = 0;
	memcpy(&bits, &value, sizeof(value));
	return bits;
}

Coord3D PointFromBits(unsigned long x, unsigned long y, unsigned long z)
{
	Coord3D point;
	point.x = FloatFromBits(x);
	point.y = FloatFromBits(y);
	point.z = FloatFromBits(z);
	return point;
}

PointBits BitsFromPoint(const Coord3D &point)
{
	PointBits bits;
	bits.x = BitsFromFloat(point.x);
	bits.y = BitsFromFloat(point.y);
	bits.z = BitsFromFloat(point.z);
	return bits;
}

int CheckPoint(const char *label, Int index, const Coord3D &point, const PointBits &expected)
{
	const PointBits actual = BitsFromPoint(point);
	if (actual.x == expected.x && actual.y == expected.y && actual.z == expected.z) {
		return 0;
	}

	fprintf(stderr,
		"FAIL: %s %d expected %08lX,%08lX,%08lX, got %08lX,%08lX,%08lX\n",
		label, index, expected.x, expected.y, expected.z, actual.x, actual.y, actual.z);
	return 1;
}

BezierSegment MakeScorpionSegment()
{
	Coord3D controlPoints[4];
	controlPoints[0] = PointFromBits(0x44D63D79UL, 0x44D0C5F2UL, 0x41CFAA2EUL);
	controlPoints[1] = PointFromBits(0x44DCFC0CUL, 0x44CE16F3UL, 0x420FD517UL);
	controlPoints[2] = PointFromBits(0x44E2614EUL, 0x44CBF15AUL, 0x420FD517UL);
	controlPoints[3] = PointFromBits(0x44E3BA9FUL, 0x44CB67F4UL, 0x41660000UL);
	return BezierSegment(controlPoints[0], controlPoints[1], controlPoints[2], controlPoints[3]);
}

BezierSegment MakeEvaluationSegment()
{
	Coord3D controlPoints[4];
	controlPoints[0] = PointFromBits(0x4488B624UL, 0x454061DDUL, 0xBD9D8E8AUL);
	controlPoints[1] = PointFromBits(0x446C513BUL, 0x40D78C40UL, 0x3D7C1829UL);
	controlPoints[2] = PointFromBits(0xC3A9A0C6UL, 0xC2454627UL, 0x3EFB519CUL);
	controlPoints[3] = PointFromBits(0x413F8EB5UL, 0xC1FB1842UL, 0xC436A953UL);
	return BezierSegment(controlPoints[0], controlPoints[1], controlPoints[2], controlPoints[3]);
}

int CheckLegacyPath()
{
	const BezierSegment segment = MakeScorpionSegment();
	const Int expectedCount = static_cast<Int>(
		sizeof(LEGACY_SCORPION_SHELL_PATH) / sizeof(LEGACY_SCORPION_SHELL_PATH[0]));
	VecCoord3D path;
	segment.getSegmentPoints(expectedCount, &path);
	if (static_cast<Int>(path.size()) != expectedCount) {
		fprintf(stderr, "FAIL: expected %d path points, got %d\n",
			expectedCount, static_cast<Int>(path.size()));
		return 1;
	}

	for (Int index = 0; index < expectedCount; ++index) {
		if (CheckPoint("path point", index, path[index], LEGACY_SCORPION_SHELL_PATH[index]) != 0) {
			return 1;
		}
	}
	return 0;
}

int CheckLegacyEvaluation()
{
	const BezierSegment segment = MakeEvaluationSegment();
	Coord3D point;
	segment.evaluateBezSegmentAtT(FloatFromBits(0x3EC79E96UL), &point);
	const PointBits expected = {0x440D9198UL, 0x442B9B97UL, 0xC22C9F9EUL};
	return CheckPoint("evaluated point", 0, point, expected);
}

#if defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
int CheckPrecisionControlRestoration()
{
	const unsigned int originalControl = _control87(0, 0);
	_control87(_PC_53, _MCW_PC);

	const BezierSegment segment = MakeEvaluationSegment();
	Coord3D point;
	segment.evaluateBezSegmentAtT(FloatFromBits(0x3EC79E96UL), &point);
	unsigned int actualControl = _control87(0, 0);
	if ((actualControl & _MCW_PC) != _PC_53) {
		_control87(originalControl, _MCW_PC);
		fprintf(stderr, "FAIL: evaluateBezSegmentAtT leaked its x87 precision mode\n");
		return 1;
	}

	VecCoord3D path;
	segment.getSegmentPoints(10, &path);
	actualControl = _control87(0, 0);
	_control87(originalControl, _MCW_PC);
	if ((actualControl & _MCW_PC) != _PC_53) {
		fprintf(stderr, "FAIL: getSegmentPoints leaked its x87 precision mode\n");
		return 1;
	}
	return 0;
}
#endif

} // namespace

int main()
{
	if (CheckLegacyEvaluation() != 0 || CheckLegacyPath() != 0) {
		return 1;
	}
#if defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
	if (CheckPrecisionControlRestoration() != 0) {
		return 1;
	}
#endif
	return 0;
}
