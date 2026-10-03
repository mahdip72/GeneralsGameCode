// Device-free C++98 fixture: real interval selector and production quaternion math.
#include "WW3D2/RawAnimationFrame.h"
#include "WWMath/matrix3d.h"
#include "WWMath/quat.h"
#include <stdio.h>
#include <string.h>

namespace
{
int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}

float AdjacentPositiveFloat(float value, bool above)
{
	unsigned int bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	if (above) ++bits; else --bits;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

int CheckPhase(float frame, int expected)
{
	const int selected = rts::animation::SelectRawAnimationFrame0(frame);
#if defined(_WIN64)
	const float ratio = frame - static_cast<float>(selected);
	int failures = Check(selected == expected, "native enclosing interval");
	failures += Check(ratio >= 0.0f && ratio < 1.0f, "native ratio in [0,1)");
	return failures;
#else
	(void)expected;
	return Check(selected == static_cast<int>(
		WWMath::Float_To_Long(frame - 0.499999f)), "unchanged non-native selection");
#endif
}

// Serialized active keys from NVHelix.W3D (SHA256
// 20fa9ead5be843146aefb211da53850e3eaa4c4dcbe93c0abc56602b8845003d).
// Channels on pivots 1/3 declare frames 1..14; frames 0/15 are the existing
// loader's identity fallback, not the exporter's ignored trailing padding.
const float HelixZW[2][16][2] = {
	{{0,1}, {-.195090339f,.980785251f}, {-.382683396f,.923879564f},
	 {-.555570185f,.831469655f}, {-.707106769f,.707106769f},
	 {-.831469595f,.555570245f}, {-.923879504f,.382683486f},
	 {-.980785310f,.195090294f}, {-1,0}, {-.974927902f,-.222520888f},
	 {-.900968909f,-.433883637f}, {.781831563f,.623489678f},
	 {.623489857f,.781831443f}, {.433883816f,.900968790f},
	 {.222521067f,.974927902f}, {0,1}},
	{{0,1}, {-.195090339f,.980785251f}, {-.382683456f,.923879504f},
	 {-.555570245f,.831469595f}, {-.707106769f,.707106769f},
	 {-.831469655f,.555570185f}, {-.923879564f,.382683396f},
	 {-.980785310f,.195090309f}, {-1,0}, {-.974927962f,-.222520843f},
	 {-.900968909f,-.433883667f}, {.781831503f,.623489738f},
	 {.623489857f,.781831443f}, {.433883816f,.900968790f},
	 {.222521052f,.974927902f}, {0,1}}
};

int CheckHelixPhase(unsigned int channel, float frame, double degrees)
{
	const int first = rts::animation::SelectRawAnimationFrame0(frame);
	const int second = first + 1;
	if (first < 0 || second > 15)
		return Check(false, "Helix fixture phase bounds");
	const Quaternion q0(0, 0, HelixZW[channel][first][0], HelixZW[channel][first][1]);
	const Quaternion q1(0, 0, HelixZW[channel][second][0], HelixZW[channel][second][1]);
	Quaternion sampled;
	Fast_Slerp(sampled, q0, q1, frame - static_cast<float>(first));
	Matrix3D rotation;
	Build_Matrix3D(sampled, rotation);
	const double radians = degrees * 3.14159265358979323846 / 180.0;
	// An analytical rotated unit point is the expected result, not a copied
	// Slerp implementation. Tolerance allows production fast-trig lookup error.
	int failures = Check(fabs(rotation[0][0] - cos(radians)) < .001,
		"Helix production Slerp X");
	failures += Check(fabs(rotation[1][0] - sin(radians)) < .001,
		"Helix production Slerp Y / hemisphere");
	return failures;
}
}

int main()
{
	if (sizeof(float) != 4 || sizeof(unsigned int) != 4)
		return Check(false, "fixture requires binary32 / 32-bit unsigned int");
	WWMath::Init();
	int failures = 0;
	const float phases[] = {0, .25f, .49f, .5f, 1, 8.25f, 8.49f, 8.5f,
		8.999f, 9, 10.25f, 14.99f, 15};
	const int expected[] = {0,0,0,0,1,8,8,8,8,9,10,14,15};
	unsigned int i;
	for (i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i)
		failures += CheckPhase(phases[i], expected[i]);
	const float integers[] = {1,8,9,15};
	for (i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
		failures += CheckPhase(AdjacentPositiveFloat(integers[i], false),
			static_cast<int>(integers[i]) - 1);
		failures += CheckPhase(AdjacentPositiveFloat(integers[i], true),
			static_cast<int>(integers[i]));
	}
	const float negative[] = {-.25f, -.5f, -1, -1.25f, -8.49f};
	for (i = 0; i < sizeof(negative) / sizeof(negative[0]); ++i)
		failures += Check(rts::animation::SelectRawAnimationFrame0(negative[i]) ==
			static_cast<int>(WWMath::Float_To_Long(negative[i] - .499999f)),
			"unchanged negative legacy edge");
#if defined(_WIN64)
	for (i = 0; i < 2; ++i) {
		failures += CheckHelixPhase(i, 0, 0);
		failures += CheckHelixPhase(i, 8.25f, -186.42857142857143);
		failures += CheckHelixPhase(i, 8.49f, -192.6);
		failures += CheckHelixPhase(i, 8.5f, -192.85714285714286);
		failures += CheckHelixPhase(i, 10.25f, -237.85714285714286);
		failures += CheckHelixPhase(i, 10.5f, -244.28571428571429);
		failures += CheckHelixPhase(i, 14.99f, -359.74285714285714);
		failures += Check(HelixZW[i][10][0] * HelixZW[i][11][0] +
			HelixZW[i][10][1] * HelixZW[i][11][1] < 0,
			"serialized Helix hemisphere sign transition");
	}
	// The existing LOOP caller wraps NumFrames-1 (15) to zero before sampling.
	// Selector(15)==15 is not permission to read key 16 or change loop policy.
	const float wrapped = 15.0f - 15.0f;
	failures += CheckPhase(wrapped, 0);
#endif
	WWMath::Shutdown();
	if (!failures) printf("raw animation interval / Helix math contracts passed\n");
	return failures ? 1 : 0;
}
