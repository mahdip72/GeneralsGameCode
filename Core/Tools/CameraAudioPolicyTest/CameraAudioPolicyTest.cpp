#include "MilesAudioDevice/AudioChannelPolicy.h"
#include "W3DDevice/GameClient/TerrainDrawSizing.h"

#include <stdio.h>

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

	input.worldUnitsPerCell = 0.0f;
	CHECK(!rts::CalculateTerrainDrawSize(input, width, height));
}

static void TestAudioChannelPolicy()
{
	CHECK(rts::GetAdaptive3DChannelTarget(25) == 64);
	CHECK(rts::GetAdaptive3DChannelTarget(64) == 64);
	CHECK(rts::GetAdaptive3DChannelTarget(80) == 80);

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
	TestAudioChannelPolicy();

	if (s_failures != 0)
	{
		printf("%d camera/audio policy test(s) failed.\n", s_failures);
		return 1;
	}

	printf("Camera/audio policy tests passed.\n");
	return 0;
}
