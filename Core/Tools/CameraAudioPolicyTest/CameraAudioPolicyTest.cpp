#include "AudioDevice/AudioChannelPolicy.h"
#include "W3DDevice/GameClient/TerrainDrawSizing.h"

#include <stdio.h>
#include <float.h>
#include <math.h>

static int s_failures = 0;

#define CHECK(expression) Check((expression), #expression, __LINE__)

static void Check(bool result, const char *expression, int line)
{
	if (!result)
	{
		printf("FAIL line %d: %s\n", line, expression);
		++s_failures;
	}
}

static rts::TerrainDrawSizingInput MakeTerrainInput()
{
	rts::TerrainDrawSizingInput input;
	input.cameraHeight = 200.0f;
	input.cameraHeightAboveMax = 200.0f;
	input.cameraToPivotDistance = 260.645f;
	input.pitchRadians = 0.65449846f;
	input.horizontalFovRadians = 0.87266463f;
	input.verticalFovRadians = 0.65f;
	input.worldUnitsPerCell = 10.0f;
	input.mapWidth = 512;
	input.mapHeight = 512;
	input.minimumWidth = 129;
	input.minimumHeight = 129;
	input.tileLength = 32;
	return input;
}

static void TestTerrainDrawSizing()
{
	rts::TerrainDrawSizingInput input = MakeTerrainInput();
	int width = 0;
	int height = 0;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width == 129);
	CHECK(height == 129);

	input.cameraHeight = 600.0f;
	input.cameraToPivotDistance = 781.935f;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width >= 289);
	CHECK(height >= 289);
	CHECK((width - 1) % input.tileLength == 0);
	CHECK((height - 1) % input.tileLength == 0);

	input.horizontalFovRadians = 1.3962634f;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width >= 257);

	input.pitchRadians = 0.20f;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width == input.mapWidth);
	CHECK(height == input.mapHeight);

	input.mapWidth = 160;
	input.mapHeight = 140;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width == 160);
	CHECK(height == 140);

	// The scripted Zero Hour shell camera exposes more than the legacy 129-cell
	// window at 4:3, while a narrower vertical FOV needs less terrain at 16:9.
	input = MakeTerrainInput();
	input.cameraHeight = 618.125f;
	input.cameraHeightAboveMax = 518.125f;
	input.cameraToPivotDistance = 782.0f;
	input.mapWidth = 315;
	input.mapHeight = 315;
	input.verticalFovRadians = 0.672870f;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width == 315);
	CHECK(height == 315);
	input.verticalFovRadians = 0.513039f;
	CHECK(rts::CalculateTerrainDrawSize(input, width, height));
	CHECK(width == 257);
	CHECK(height == 257);

	input.worldUnitsPerCell = 0.0f;
	CHECK(!rts::CalculateTerrainDrawSize(input, width, height));
}

static rts::TerrainCameraBasis MakeTerrainBasis(float forwardX, float forwardY,
	float pitchRadians)
{
	rts::TerrainCameraBasis basis;
	const float horizontalLength = (float)sqrt(forwardX * forwardX + forwardY * forwardY);
	const float sinPitch = (float)sin(pitchRadians);
	const float cosPitch = (float)cos(pitchRadians);
	basis.forwardX = forwardX;
	basis.forwardY = forwardY;
	basis.forwardZ = -sinPitch;
	basis.rightX = forwardY / horizontalLength;
	basis.rightY = -forwardX / horizontalLength;
	basis.rightZ = 0.0f;
	basis.upX = sinPitch * forwardX / horizontalLength;
	basis.upY = sinPitch * forwardY / horizontalLength;
	basis.upZ = cosPitch;
	return basis;
}

static void TestShellTerrainDrawSizing()
{
	rts::TerrainDrawSizingInput input = MakeTerrainInput();
	input.cameraHeight = 618.125f;
	input.cameraHeightAboveMax = 518.125f;
	input.cameraToPivotDistance = 782.0f;
	input.mapWidth = 315;
	input.mapHeight = 315;
	input.verticalFovRadians = 0.672870f;
	int width = 0, height = 0;
	rts::TerrainCameraBasis basis = MakeTerrainBasis(0.053675f, 0.791536f,
		input.pitchRadians);

	// The first 4:3 pose projects both extrema of a 100-unit terrain range.
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	CHECK(width == 225 && height == 225);
	// Roll rotates the screen edges even when forward remains unchanged.
	const float roll = 0.087266f;
	const float rollCos = (float)cos(roll), rollSin = (float)sin(roll);
	rts::TerrainCameraBasis rolled = basis;
	rolled.rightX = basis.rightX * rollCos + basis.upX * rollSin;
	rolled.rightY = basis.rightY * rollCos + basis.upY * rollSin;
	rolled.rightZ = basis.rightZ * rollCos + basis.upZ * rollSin;
	rolled.upX = basis.upX * rollCos - basis.rightX * rollSin;
	rolled.upY = basis.upY * rollCos - basis.rightY * rollSin;
	rolled.upZ = basis.upZ * rollCos - basis.rightZ * rollSin;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		rolled, width, height));
	CHECK(width == 225 && height == 225);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	rts::StabilizeTerrainDrawSizeForMap(129, 129, 315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	rts::StabilizeTerrainDrawSizeForMap(315, 315, 315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	rts::StabilizeTerrainDrawSizeForMap(315, 315, 315, 315, true, width, height);
	CHECK(width == 315 && height == 315);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	rts::StabilizeTerrainDrawSizeForMap(315, 315, 315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	width = 315; height = 315;
	rts::StabilizeTerrainDrawSizeForMap(225, 225, 315, 315, false, width, height);
	CHECK(width == 315 && height == 315);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		MakeTerrainBasis(0.611570f, 0.505363f, input.pitchRadians), width, height));
	CHECK(width == 225 && height == 225);
	rts::StabilizeTerrainDrawSizeForMap(225, 225, 315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	rts::StabilizeTerrainDrawSizeForMap(225, 225, 315, 315, false, width, height);
	CHECK(width == 225 && height == 225);

	// A newly constructed map starts with a new 129-cell draw area, not the
	// previous map's grow-only floor.
	input.verticalFovRadians = 0.513039f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	CHECK(width == 193 && height == 161);
	rts::StabilizeTerrainDrawSizeForMap(129, 129, 315, 315, false, width, height);
	CHECK(width == 193 && height == 193);

	// A high ridge still intersects every corner ray before its min-height
	// endpoint; a camera at or under the maximum terrain is conservatively full.
	input.verticalFovRadians = 0.672870f;
	input.cameraHeightAboveMax = 1.0f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		MakeTerrainBasis(0.611570f, 0.505363f, input.pitchRadians), width, height));
	CHECK(width == 257 && height == 225);
	input.cameraHeightAboveMax = 0.0f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	CHECK(width == 315 && height == 315);
	input.cameraHeightAboveMax = 518.125f;

	// A smaller map and an extreme finite view clamp before float-to-int.
	input.mapWidth = 160;
	input.mapHeight = 140;
	input.pitchRadians = 0.30f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	CHECK(width == 160 && height == 140);
	input.mapWidth = 315;
	input.mapHeight = 315;
	input.cameraHeight = FLT_MAX / 2.0f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		basis, width, height));
	CHECK(width == 315 && height == 315);
	input.cameraHeight = 618.125f;
	input.pitchRadians = 0.65449846f;
	rts::TerrainCameraBasis invalid = basis;
	invalid.rightZ = FLT_MAX;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		invalid, width, height));
	CHECK(width == 315 && height == 315);

	// A near-horizon view keeps the existing full-map fallback.
	input.pitchRadians = 0.20f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		MakeTerrainBasis(0.053675f, 0.791536f, input.pitchRadians), width, height));
	CHECK(width == 315 && height == 315);
}

static void TestShellTerrainFloorPolicy()
{
	int shellWidth = 0, shellHeight = 0;
	int width = 193, height = 161;

	// The first shell request must not inherit the user camera's 257-cell
	// draw area at 16:9; only shell-chosen dimensions become its floor.
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 193 && height == 193);
	shellWidth = width; shellHeight = height;
	width = 225; height = 193;
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	shellWidth = width; shellHeight = height;
	width = 193; height = 161;
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 225 && height == 225);

	// A map change or a return to user control clears the shell floor.
	shellWidth = 0; shellHeight = 0;
	width = 193; height = 161;
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 193 && height == 193);

	// The same first-shell rule contracts a 4:3 startup draw of 315.
	shellWidth = 0; shellHeight = 0;
	width = 225; height = 193;
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
	shellWidth = width; shellHeight = height;
	rts::StabilizeTerrainDrawSizeForMap(315, 315,
		315, 315, true, width, height);
	CHECK(width == 315 && height == 315);
	width = 225; height = 193;
	rts::StabilizeTerrainDrawSizeForMap(shellWidth, shellHeight,
		315, 315, false, width, height);
	CHECK(width == 225 && height == 225);
}

static void TestAudioChannelPolicy()
{
	CHECK(rts::GetAdaptive3DChannelTarget(25) == 64);
	CHECK(rts::GetAdaptive3DChannelTarget(64) == 64);
	CHECK(rts::GetAdaptive3DChannelTarget(80) == 80);

	CHECK(rts::ShouldGrow3DChannelPool(0, 25, 64, 7));
	CHECK(!rts::ShouldGrow3DChannelPool(1, 25, 64, 7));
	CHECK(rts::ShouldGrow3DChannelPool(0, 56, 64, 7));
	CHECK(!rts::ShouldGrow3DChannelPool(0, 57, 64, 7));
	CHECK(!rts::ShouldGrow3DChannelPool(0, 64, 64, 7));
	CHECK(!rts::ShouldGrow3DChannelPool(0, 80, 64, 7));

	CHECK(rts::CanReplace3DChannel(false, 2, 1, false, false, false, false, false));
	CHECK(!rts::CanReplace3DChannel(false, 2, 2, false, false, false, false, false));
	CHECK(rts::CanReplace3DChannel(true, 2, 2, false, false, false, false, false));
	CHECK(!rts::CanReplace3DChannel(true, 2, 3, false, false, false, false, false));
	CHECK(!rts::CanReplace3DChannel(true, 4, 2, true, false, false, false, false));
	CHECK(!rts::CanReplace3DChannel(true, 3, 2, false, true, false, false, false));
	CHECK(!rts::CanReplace3DChannel(true, 3, 2, false, false, true, false, false));
	CHECK(!rts::CanReplace3DChannel(true, 3, 2, false, false, false, true, false));
	CHECK(!rts::CanReplace3DChannel(true, 3, 2, false, false, false, false, true));

	CHECK(rts::IsPreferred3DChannelReplacement(1, 2));
	CHECK(!rts::IsPreferred3DChannelReplacement(2, 2));
	CHECK(!rts::IsPreferred3DChannelReplacement(3, 2));
}

int main()
{
	TestTerrainDrawSizing();
	TestShellTerrainDrawSizing();
	TestShellTerrainFloorPolicy();
	TestAudioChannelPolicy();

	if (s_failures != 0)
	{
		printf("%d camera/audio policy test(s) failed.\n", s_failures);
		return 1;
	}

	printf("Camera/audio policy tests passed.\n");
	return 0;
}
