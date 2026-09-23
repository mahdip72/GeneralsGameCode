#include "W3DDevice/GameClient/W3DWaterTrackTextureBindCache.h"

#include <stdio.h>

namespace
{
	unsigned int failures = 0;

	struct TrackTextureCase
	{
		int type;
		const void *texture;
		bool expectedBind;
	};

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), \
		#condition); } } while (0)

	void TestActualTextureIdentityAndRetainedTrackOrder()
	{
		int textureA = 0;
		int textureB = 0;
		const TrackTextureCase modules[] = {
			{0, &textureA, true},
			{0, &textureA, false},
			{0, &textureB, true},  // Same type, different texture must bind.
			{1, &textureB, false}, // Different type, same texture need not bind.
			{1, &textureA, true},
			{3, &textureA, false},
			{3, &textureB, true},
			{2, &textureB, false}
		};
		CHECK(modules[1].type == modules[2].type);
		CHECK(modules[1].texture != modules[2].texture);
		CHECK(modules[2].type != modules[3].type);
		CHECK(modules[2].texture == modules[3].texture);
		W3DWaterTrackTextureBindCache cache;
		unsigned int renderedModules = 0;
		unsigned int textureBinds = 0;
		for (unsigned int index = 0;
			index < sizeof(modules) / sizeof(modules[0]); ++index)
		{
			++renderedModules;
			const bool shouldBind = cache.ShouldBind(modules[index].texture);
			CHECK(shouldBind == modules[index].expectedBind);
			if (shouldBind)
				++textureBinds;
		}
		CHECK(renderedModules == sizeof(modules) / sizeof(modules[0]));
		CHECK(textureBinds == 4);
	}

	void TestTextureTransitionRebindsWhenIdentityReturns()
	{
		int textureA = 0;
		int textureB = 0;
		W3DWaterTrackTextureBindCache cache;
		CHECK(cache.ShouldBind(&textureA));
		CHECK(!cache.ShouldBind(&textureA));
		CHECK(cache.ShouldBind(&textureB));
		CHECK(!cache.ShouldBind(&textureB));
		CHECK(cache.ShouldBind(&textureA));
	}

	void TestNullTextureAndA_Null_ASequence()
	{
		int textureA = 0;
		W3DWaterTrackTextureBindCache cache;
		CHECK(cache.ShouldBind(&textureA));
		CHECK(!cache.ShouldBind(&textureA));
		CHECK(cache.ShouldBind(nullptr));
		CHECK(!cache.ShouldBind(nullptr));
		CHECK(cache.ShouldBind(&textureA));
	}

	void TestEachFlushStartsWithUnknownTexture()
	{
		int textureA = 0;
		W3DWaterTrackTextureBindCache firstFlush;
		CHECK(firstFlush.ShouldBind(&textureA));
		CHECK(!firstFlush.ShouldBind(&textureA));

		W3DWaterTrackTextureBindCache nextFlush;
		CHECK(nextFlush.ShouldBind(&textureA));
	}
}

int main()
{
	TestActualTextureIdentityAndRetainedTrackOrder();
	TestTextureTransitionRebindsWhenIdentityReturns();
	TestNullTextureAndA_Null_ASequence();
	TestEachFlushStartsWithUnknownTexture();
	return failures == 0 ? 0 : 1;
}
