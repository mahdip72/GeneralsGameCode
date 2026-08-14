#include "WW3D2/texturemipbuffer.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int s_failures = 0;

static void expectTrue(bool value, const char* message)
{
	if (!value)
	{
		printf("FAIL: %s\n", message);
		++s_failures;
	}
}

static void expectSize(size_t actual, size_t expected, const char* message)
{
	if (actual != expected)
	{
		printf("FAIL: %s (expected %u, got %u)\n", message,
			(unsigned)expected, (unsigned)actual);
		++s_failures;
	}
}

static void testUncompressedLayouts()
{
	TextureMipLayout layout;

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, 7, 3, 1, layout),
		"A8R8G8B8 layout is accepted");
	expectSize(layout.rowPitch, 28, "A8R8G8B8 row pitch");
	expectSize(layout.rowCount, 3, "A8R8G8B8 row count");
	expectSize(layout.slicePitch, 84, "A8R8G8B8 slice pitch");
	expectSize(layout.dataSize, 84, "A8R8G8B8 data size");

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_R5G6B5, 5, 2, 3, layout),
		"R5G6B5 volume layout is accepted");
	expectSize(layout.rowPitch, 10, "R5G6B5 row pitch");
	expectSize(layout.rowCount, 2, "R5G6B5 row count");
	expectSize(layout.slicePitch, 20, "R5G6B5 slice pitch");
	expectSize(layout.dataSize, 60, "R5G6B5 data size");

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_R8G8B8, 3, 4, 1, layout),
		"R8G8B8 layout is accepted");
	expectSize(layout.rowPitch, 9, "R8G8B8 row pitch");

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_L8, 9, 1, 1, layout),
		"L8 layout is accepted");
	expectSize(layout.rowPitch, 9, "L8 row pitch");
}

static void testCompressedLayouts()
{
	TextureMipLayout layout;

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_DXT1, 1, 1, 1, layout),
		"sub-block DXT1 layout is accepted");
	expectSize(layout.rowPitch, 8, "sub-block DXT1 row pitch");
	expectSize(layout.rowCount, 1, "sub-block DXT1 row count");
	expectSize(layout.dataSize, 8, "sub-block DXT1 data size");

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_DXT1, 9, 7, 1, layout),
		"odd DXT1 layout is accepted");
	expectSize(layout.rowPitch, 24, "odd DXT1 row pitch");
	expectSize(layout.rowCount, 2, "odd DXT1 row count");
	expectSize(layout.dataSize, 48, "odd DXT1 data size");

	expectTrue(CalculateTextureMipLayout(WW3D_FORMAT_DXT5, 9, 7, 2, layout),
		"odd DXT5 volume layout is accepted");
	expectSize(layout.rowPitch, 48, "odd DXT5 row pitch");
	expectSize(layout.rowCount, 2, "odd DXT5 row count");
	expectSize(layout.slicePitch, 96, "odd DXT5 slice pitch");
	expectSize(layout.dataSize, 192, "odd DXT5 data size");
}

static void testInvalidLayouts()
{
	TextureMipLayout layout;

	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_UNKNOWN, 4, 4, 1, layout),
		"unknown format is rejected");
	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, 0, 4, 1, layout),
		"zero width is rejected");
	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, 4, 0, 1, layout),
		"zero height is rejected");
	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, 4, 4, 0, layout),
		"zero depth is rejected");
	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, UINT_MAX, 2, 1, layout),
		"row pitch overflow is rejected");
}

static void testPaddedCopyPreservesGuards()
{
	unsigned char sourceBytes[16];
	unsigned char destinationBytes[32];
	TextureMipLayout source;
	TextureMipLayout destination;
	unsigned i;

	for (i = 0; i < sizeof(sourceBytes); ++i)
	{
		sourceBytes[i] = (unsigned char)(i + 1);
	}
	memset(destinationBytes, 0xcd, sizeof(destinationBytes));

	source.rowPitch = 4;
	source.rowCount = 2;
	source.slicePitch = 8;
	source.dataSize = 16;
	destination.rowPitch = 6;
	destination.rowCount = 2;
	destination.slicePitch = 14;
	destination.dataSize = 28;

	expectTrue(CopyTextureMipData(sourceBytes, source, destinationBytes + 2,
		destination, 2), "two padded slices copy successfully");

	for (i = 0; i < 4; ++i)
	{
		expectTrue(destinationBytes[2 + i] == sourceBytes[i], "first row bytes match");
		expectTrue(destinationBytes[8 + i] == sourceBytes[4 + i], "second row bytes match");
		expectTrue(destinationBytes[16 + i] == sourceBytes[8 + i], "third row bytes match");
		expectTrue(destinationBytes[22 + i] == sourceBytes[12 + i], "fourth row bytes match");
	}
	expectTrue(destinationBytes[0] == 0xcd && destinationBytes[1] == 0xcd,
		"leading guard bytes are unchanged");
	expectTrue(destinationBytes[6] == 0xcd && destinationBytes[7] == 0xcd,
		"first row padding is unchanged");
	expectTrue(destinationBytes[12] == 0xcd && destinationBytes[13] == 0xcd,
		"first slice padding is unchanged");
	expectTrue(destinationBytes[30] == 0xcd && destinationBytes[31] == 0xcd,
		"trailing guard bytes are unchanged");
}

int main()
{
	testUncompressedLayouts();
	testCompressedLayouts();
	testInvalidLayouts();
	testPaddedCopyPreservesGuards();

	if (s_failures != 0)
	{
		printf("%d texture mip buffer test(s) failed\n", s_failures);
		return 1;
	}

	printf("All texture mip buffer tests passed\n");
	return 0;
}
