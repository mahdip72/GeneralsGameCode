#include "WW3D2/texturemipbuffer.h"
#include "WW3D2/bitmaphandler.h"
#include "Lib/TaskRuntime.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int s_failures = 0;

unsigned Get_Bytes_Per_Pixel(WW3DFormat format)
{
	if (format == WW3D_FORMAT_A8R8G8B8 || format == WW3D_FORMAT_X8R8G8B8)
	{
		return 4;
	}
	if (format == WW3D_FORMAT_R8G8B8)
	{
		return 3;
	}
	return 0;
}

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
	expectTrue(!CalculateTextureMipLayout(WW3D_FORMAT_DXT5, UINT_MAX, UINT_MAX, UINT_MAX, layout),
		"compressed dimension overflow is rejected");
}

static void testMipLevelCounts()
{
	unsigned width = 8;
	unsigned height = 4;

	expectSize(CalculateTextureMipLevelCount(1, 1), 1, "one by one has one mip level");
	expectSize(CalculateTextureMipLevelCount(8, 4), 4, "rectangular texture reaches one by one");
	expectSize(CalculateTextureMipLevelCount(1, 8), 4, "single-width texture reaches one by one");
	expectSize(CalculateTextureMipLevelCount(0, 8), 0, "zero-width texture has no mip levels");
	ReduceTextureMipDimensions(width, height);
	expectSize(width, 4, "first rectangular reduction width");
	expectSize(height, 2, "first rectangular reduction height");
	ReduceTextureMipDimensions(width, height);
	expectSize(width, 2, "second rectangular reduction width");
	expectSize(height, 1, "second rectangular reduction clamps height");
	ReduceTextureMipDimensions(width, height);
	expectSize(width, 1, "rectangular reduction reaches one-wide");
	expectSize(height, 1, "rectangular reduction remains one-high");
	ReduceTextureMipDimensions(width, height);
	expectSize(width, 1, "one-wide stays clamped");
	expectSize(height, 1, "one-high stays clamped");
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

static void testHorizontalMipTailGeneratesPixels()
{
	unsigned source[2] = { 0x10101010U, 0x30303030U };
	unsigned destination[2] = { 0xcdcdcdcdU, 0xcdcdcdcdU };

	BitmapHandlerClass::Copy_Image(
		reinterpret_cast<unsigned char *>(destination), 2, 1, 8, WW3D_FORMAT_A8R8G8B8,
		reinterpret_cast<unsigned char *>(source), 2, 1, 8, WW3D_FORMAT_A8R8G8B8,
		nullptr, 0, true);

	expectTrue(destination[0] == 0x10101010U && destination[1] == 0x30303030U,
		"two by one level copies every source pixel");
	expectTrue(source[0] == 0x20202020U,
		"two by one level generates its one by one successor");
}

static void testVerticalMipTailGeneratesPixels()
{
	unsigned source[2] = { 0x10101010U, 0x30303030U };
	unsigned destination[2] = { 0xcdcdcdcdU, 0xcdcdcdcdU };

	BitmapHandlerClass::Copy_Image(
		reinterpret_cast<unsigned char *>(destination), 1, 2, 4, WW3D_FORMAT_A8R8G8B8,
		reinterpret_cast<unsigned char *>(source), 1, 2, 4, WW3D_FORMAT_A8R8G8B8,
		nullptr, 0, true);

	expectTrue(destination[0] == 0x10101010U && destination[1] == 0x30303030U,
		"one by two level copies every source pixel");
	expectTrue(source[0] == 0x20202020U,
		"one by two level generates its one by one successor");
}

static void testGenericMipTailsPreservePitches()
{
	unsigned char horizontalSource[8] = {
		0x10, 0x10, 0x10, 0x30, 0x30, 0x30, 0xcd, 0xcd
	};
	unsigned horizontalDestination[2] = { 0xcdcdcdcdU, 0xcdcdcdcdU };
	unsigned char verticalSource[16];
	unsigned verticalDestination[4];

	BitmapHandlerClass::Copy_Image(
		reinterpret_cast<unsigned char *>(horizontalDestination), 2, 1, 8, WW3D_FORMAT_A8R8G8B8,
		horizontalSource, 2, 1, 8, WW3D_FORMAT_R8G8B8,
		nullptr, 0, true);
	expectTrue(horizontalDestination[0] == 0xff101010U &&
		horizontalDestination[1] == 0xff303030U,
		"generic two by one level copies converted pixels");
	expectTrue(horizontalSource[0] == 0x20 && horizontalSource[1] == 0x20 &&
		horizontalSource[2] == 0x20 && horizontalSource[6] == 0xcd && horizontalSource[7] == 0xcd,
		"generic horizontal tail generates its successor without touching padding");

	memset(verticalSource, 0xcd, sizeof(verticalSource));
	memset(verticalDestination, 0xcd, sizeof(verticalDestination));
	verticalSource[0] = verticalSource[1] = verticalSource[2] = 0x10;
	verticalSource[8] = verticalSource[9] = verticalSource[10] = 0x30;
	BitmapHandlerClass::Copy_Image(
		reinterpret_cast<unsigned char *>(verticalDestination), 1, 2, 8, WW3D_FORMAT_A8R8G8B8,
		verticalSource, 1, 2, 8, WW3D_FORMAT_R8G8B8,
		nullptr, 0, true);
	expectTrue(verticalDestination[0] == 0xff101010U &&
		verticalDestination[2] == 0xff303030U,
		"generic one by two level honors destination pitch");
	expectTrue(verticalSource[0] == 0x20 && verticalSource[1] == 0x20 &&
		verticalSource[2] == 0x20 && verticalSource[3] == 0xcd &&
		verticalSource[7] == 0xcd && verticalSource[11] == 0xcd,
		"generic vertical tail honors source pitch and padding");
}

class MipCopyTask : public rts::Task
{
public:
	MipCopyTask(const unsigned char* source, const TextureMipLayout& sourceLayout,
		TextureMipBuffer* destination, bool* copied)
		: m_source(source), m_sourceLayout(sourceLayout), m_destination(destination), m_copied(copied)
	{
	}

	virtual void execute()
	{
		*m_copied = m_destination->copyFrom(m_source, m_sourceLayout, 1);
	}

private:
	const unsigned char* m_source;
	TextureMipLayout m_sourceLayout;
	TextureMipBuffer* m_destination;
	bool* m_copied;
};

static void testTwoWorkerOwnedBuffersAreIndependent()
{
	static const unsigned char sourceOne[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const unsigned char sourceTwo[8] = { 21, 22, 23, 24, 25, 26, 27, 28 };
	TextureMipLayout sourceLayout;
	TextureMipBuffer destinationOne;
	TextureMipBuffer destinationTwo;
	TextureMipBuffer serialOne;
	TextureMipBuffer serialTwo;
	rts::TaskRuntime runtime;
	rts::Task* tasks[2];
	bool copiedOne = false;
	bool copiedTwo = false;
	unsigned i;

	sourceLayout.rowPitch = 4;
	sourceLayout.rowCount = 2;
	sourceLayout.slicePitch = 8;
	sourceLayout.dataSize = 8;

	expectTrue(destinationOne.allocate(WW3D_FORMAT_R5G6B5, 2, 2, 1),
		"first owned buffer allocates");
	expectTrue(destinationTwo.allocate(WW3D_FORMAT_R5G6B5, 2, 2, 1),
		"second owned buffer allocates");
	expectTrue(serialOne.allocate(WW3D_FORMAT_R5G6B5, 2, 2, 1) &&
		serialOne.copyFrom(sourceOne, sourceLayout, 1), "first serial baseline prepares");
	expectTrue(serialTwo.allocate(WW3D_FORMAT_R5G6B5, 2, 2, 1) &&
		serialTwo.copyFrom(sourceTwo, sourceLayout, 1), "second serial baseline prepares");
	expectTrue(runtime.start(2, 2), "two-worker runtime starts");

	tasks[0] = new MipCopyTask(sourceOne, sourceLayout, &destinationOne, &copiedOne);
	tasks[1] = new MipCopyTask(sourceTwo, sourceLayout, &destinationTwo, &copiedTwo);
	expectTrue(runtime.trySubmitBatch(tasks, 2), "both owned buffer tasks are accepted atomically");
	runtime.waitUntilIdle();
	runtime.shutdown();
	expectTrue(copiedOne && copiedTwo, "both worker copies report success");

	for (i = 0; i < 8; ++i)
	{
		expectTrue(destinationOne.data()[i] == serialOne.data()[i],
			"first worker output matches serial preparation");
		expectTrue(destinationTwo.data()[i] == serialTwo.data()[i],
			"second worker output matches serial preparation");
	}
}

int main()
{
	testUncompressedLayouts();
	testCompressedLayouts();
	testInvalidLayouts();
	testMipLevelCounts();
	testPaddedCopyPreservesGuards();
	testHorizontalMipTailGeneratesPixels();
	testVerticalMipTailGeneratesPixels();
	testGenericMipTailsPreservePitches();
	testTwoWorkerOwnedBuffersAreIndependent();

	if (s_failures != 0)
	{
		printf("%d texture mip buffer test(s) failed\n", s_failures);
		return 1;
	}

	printf("All texture mip buffer tests passed\n");
	return 0;
}
