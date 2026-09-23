#include "AudioDevice/AudioChannelPolicy.h"
#include "W3DDevice/GameClient/TerrainDrawSizing.h"

#include <stdio.h>
#include <float.h>

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

static void TestShellTerrainDrawSizing()
{
	rts::TerrainDrawSizingInput input = MakeTerrainInput();
	input.cameraHeight = 618.125f;
	input.cameraToPivotDistance = 782.0f;
	input.mapWidth = 315;
	input.mapHeight = 315;
	input.verticalFovRadians = 0.672870f;
	int width = 0, height = 0;

	// Two recorded shell camera headings at 4:3 and the first at 16:9.
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	CHECK(width == 225 && height == 193);
	rts::StabilizeTerrainDrawSizeForMap(129, 129, 315, 315, width, height);
	CHECK(width == 225 && height == 225);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.611570f, 0.505363f, width, height));
	CHECK(width == 225 && height == 225);
	rts::StabilizeTerrainDrawSizeForMap(225, 225, 315, 315, width, height);
	CHECK(width == 225 && height == 225);
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	rts::StabilizeTerrainDrawSizeForMap(225, 225, 315, 315, width, height);
	CHECK(width == 225 && height == 225);

	// A newly constructed map starts with a new 129-cell draw area, not the
	// previous map's grow-only floor.
	input.verticalFovRadians = 0.513039f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	CHECK(width == 193 && height == 161);
	rts::StabilizeTerrainDrawSizeForMap(129, 129, 315, 315, width, height);
	CHECK(width == 193 && height == 193);

	// A smaller map and an extreme finite view clamp before float-to-int.
	input.mapWidth = 160;
	input.mapHeight = 140;
	input.pitchRadians = 0.30f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	CHECK(width == 160 && height == 140);
	input.mapWidth = 315;
	input.mapHeight = 315;
	input.cameraHeight = FLT_MAX / 2.0f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	CHECK(width == 315 && height == 315);
	input.cameraHeight = 618.125f;
	input.pitchRadians = 0.65449846f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		FLT_MAX, FLT_MAX, width, height));
	CHECK(width == 257 && height == 257);

	// A near-horizon view keeps the existing full-map fallback.
	input.pitchRadians = 0.20f;
	CHECK(rts::CalculateTerrainDrawSizeForCameraDirection(input,
		0.053675f, 0.791536f, width, height));
	CHECK(width == 315 && height == 315);
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
	TestAudioChannelPolicy();

	if (s_failures != 0)
	{
		printf("%d camera/audio policy test(s) failed.\n", s_failures);
		return 1;
	}

	printf("Camera/audio policy tests passed.\n");
	return 0;
}
