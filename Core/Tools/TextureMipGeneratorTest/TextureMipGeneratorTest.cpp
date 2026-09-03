#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "WWVegas/WW3D2/texturemipgenerator.h"

static int s_failures = 0;

static void expectTrue(bool condition, const char* message)
{
    if (!condition)
    {
        ++s_failures;
        printf("FAIL: %s\n", message);
    }
}

static void fillBytes(unsigned char* bytes, unsigned count, unsigned char value)
{
    unsigned i;
    for (i = 0; i < count; ++i)
    {
        bytes[i] = value;
    }
}

static void testBoxFilterHonorsPitchAndAlpha()
{
    static const unsigned char source[24] = {
        3, 8, 12, 64,  8, 12, 16, 128,  0, 0, 0, 0,
        12, 16, 20, 192,  16, 20, 24, 252,  0, 0, 0, 0
    };
    unsigned char destination[12];

    fillBytes(destination, sizeof(destination), 0xcd);
    expectTrue(Generate_Texture_Mip_Level_Box(
        source, 12, 2, 2, destination, 6, WW3D_FORMAT_A8R8G8B8),
        "A8R8G8B8 box filter accepts padded rows");
    expectTrue(destination[0] == 10 && destination[1] == 14 &&
        destination[2] == 18 && destination[3] == 159,
        "A8R8G8B8 box filter rounds color and alpha bytes like D3DX8");
    expectTrue(destination[4] == 0xcd && destination[5] == 0xcd &&
        destination[6] == 0xcd && destination[11] == 0xcd,
        "box filter preserves destination row padding");
}

static void testBoxFilterHandlesOddAndOneDimensionalLevels()
{
    static const unsigned char oddSource[3 * 2 * 4] = {
        4, 8, 12, 64,  8, 12, 16, 64,  24, 28, 32, 64,
        12, 16, 20, 64,  16, 20, 24, 64,  28, 32, 36, 64
    };
    static const unsigned char horizontalSource[3 * 4] = {
        4, 8, 12, 64,  8, 12, 16, 128,  24, 28, 32, 192
    };
    static const unsigned char verticalSource[3 * 4] = {
        4, 8, 12, 64,
        8, 12, 16, 128,
        24, 28, 32, 192
    };
    unsigned char oddDestination[4];
    unsigned char horizontalDestination[4];
    unsigned char verticalDestination[4];

    fillBytes(oddDestination, sizeof(oddDestination), 0xcd);
    fillBytes(horizontalDestination, sizeof(horizontalDestination), 0xcd);
    fillBytes(verticalDestination, sizeof(verticalDestination), 0xcd);
    expectTrue(Generate_Texture_Mip_Level_Box(
        oddSource, 12, 3, 2, oddDestination, 4, WW3D_FORMAT_A8R8G8B8),
        "box filter accepts odd dimensions");
    expectTrue(oddDestination[0] == 10 && oddDestination[1] == 14 &&
        oddDestination[2] == 18 && oddDestination[3] == 64,
        "odd box filter level ignores the unpaired third column");
    expectTrue(Generate_Texture_Mip_Level_Box(
        horizontalSource, 12, 3, 1, horizontalDestination, 4,
        WW3D_FORMAT_A8R8G8B8),
        "box filter accepts a one-row level");
    expectTrue(horizontalDestination[0] == 6 && horizontalDestination[1] == 10 &&
        horizontalDestination[2] == 14 && horizontalDestination[3] == 96,
        "one-row box filter duplicates the only source row");
    expectTrue(Generate_Texture_Mip_Level_Box(
        verticalSource, 4, 1, 3, verticalDestination, 4,
        WW3D_FORMAT_A8R8G8B8),
        "box filter accepts a one-column level");
    expectTrue(verticalDestination[0] == 6 && verticalDestination[1] == 10 &&
        verticalDestination[2] == 14 && verticalDestination[3] == 96,
        "one-column box filter duplicates the only source column");
}

static void testProductionFormatAlphaRules()
{
    // A tiny D3DX8 reference probe (min-dx8-sdk d3dx8.lib) observed A1 alpha
    // outputs 0/0/1/1/1 for 0/1/2/3/4 opaque samples and byte/X bytes rounded
    // with (sum + 2) / 4. Keep these cases here as the production-format rule.
    static const unsigned char xSource[8] = {
        4, 8, 12, 2,  8, 12, 16, 0
    };
    static const unsigned char a1OpaqueSource[8] = {
        0x00, 0x80,  0x00, 0x80,  0x00, 0x80,  0x00, 0x80
    };
    static const unsigned char a1ColorSource[8] = {
        0x00, 0x88,  0x00, 0x80,  0x00, 0x80,  0x00, 0x80
    };
    static const unsigned char a1HalfAlphaSource[8] = {
        0x00, 0x80,  0x00, 0x80,  0x00, 0x00,  0x00, 0x00
    };
    static const unsigned char a1MostlyTransparentSource[8] = {
        0x00, 0x80,  0x00, 0x00,  0x00, 0x00,  0x00, 0x00
    };
    unsigned char xDestination[4];
    unsigned char a1OpaqueDestination[2];
    unsigned char a1ColorDestination[2];
    unsigned char a1HalfAlphaDestination[2];
    unsigned char a1MostlyTransparentDestination[2];

    fillBytes(xDestination, sizeof(xDestination), 0xcd);
    fillBytes(a1OpaqueDestination, sizeof(a1OpaqueDestination), 0xcd);
    fillBytes(a1ColorDestination, sizeof(a1ColorDestination), 0xcd);
    fillBytes(a1HalfAlphaDestination, sizeof(a1HalfAlphaDestination), 0xcd);
    fillBytes(a1MostlyTransparentDestination,
        sizeof(a1MostlyTransparentDestination), 0xcd);
    expectTrue(Generate_Texture_Mip_Level_Box(
        xSource, 8, 2, 1, xDestination, 4, WW3D_FORMAT_X8R8G8B8),
        "X8R8G8B8 box filter is supported");
    expectTrue(xDestination[0] == 6 && xDestination[1] == 10 &&
        xDestination[2] == 14 && xDestination[3] == 1,
        "X8R8G8B8 box-averages its unused high byte like D3DX8");
    expectTrue(Generate_Texture_Mip_Level_Box(
        a1OpaqueSource, 4, 2, 2, a1OpaqueDestination, 2,
        WW3D_FORMAT_A1R5G5B5),
        "A1R5G5B5 box filter is supported");
    expectTrue((a1OpaqueDestination[1] & 0x80) != 0,
        "A1R5G5B5 retains alpha when every sample is opaque");
    expectTrue(Generate_Texture_Mip_Level_Box(
        a1ColorSource, 4, 2, 2, a1ColorDestination, 2,
        WW3D_FORMAT_A1R5G5B5),
        "A1R5G5B5 rounds five-bit color components like D3DX8");
    expectTrue(((a1ColorDestination[0] | (a1ColorDestination[1] << 8)) >> 10 & 0x1f) == 1,
        "A1R5G5B5 rounds a half-step red component up");
    expectTrue(Generate_Texture_Mip_Level_Box(
        a1HalfAlphaSource, 4, 2, 2, a1HalfAlphaDestination, 2,
        WW3D_FORMAT_A1R5G5B5),
        "A1R5G5B5 half-alpha box filter completes");
    expectTrue((a1HalfAlphaDestination[1] & 0x80) != 0,
        "A1R5G5B5 rounds alpha up at two opaque samples like D3DX8");
    expectTrue(Generate_Texture_Mip_Level_Box(
        a1MostlyTransparentSource, 4, 2, 2,
        a1MostlyTransparentDestination, 2, WW3D_FORMAT_A1R5G5B5),
        "A1R5G5B5 mostly-transparent box filter completes");
    expectTrue((a1MostlyTransparentDestination[1] & 0x80) == 0,
        "A1R5G5B5 rounds alpha down below two opaque samples like D3DX8");
}

static void testByteExactMipChain()
{
    unsigned char level0[4 * 4 * 4];
    unsigned char level1[2 * 2 * 4];
    unsigned char level2[4];
    static const unsigned char expectedLevel1[2 * 2 * 4] = {
        3, 3, 3, 3, 5, 5, 5, 5,
        11, 11, 11, 11, 13, 13, 13, 13
    };
    static const unsigned char expectedLevel2[4] = { 8, 8, 8, 8 };
    unsigned pixel;

    for (pixel = 0; pixel < 16; ++pixel)
    {
        level0[pixel * 4] = (unsigned char)pixel;
        level0[pixel * 4 + 1] = (unsigned char)pixel;
        level0[pixel * 4 + 2] = (unsigned char)pixel;
        level0[pixel * 4 + 3] = (unsigned char)pixel;
    }
    fillBytes(level1, sizeof(level1), 0xcd);
    fillBytes(level2, sizeof(level2), 0xcd);
    expectTrue(Generate_Texture_Mip_Level_Box(level0, 16, 4, 4,
        level1, 8, WW3D_FORMAT_A8R8G8B8),
        "first byte-image mip level generates without a texture object");
    expectTrue(memcmp(level1, expectedLevel1, sizeof(level1)) == 0,
        "first mip level matches the byte-exact legacy box averages");
    expectTrue(Generate_Texture_Mip_Level_Box(level1, 8, 2, 2,
        level2, 4, WW3D_FORMAT_A8R8G8B8),
        "second byte-image mip level generates without a texture object");
    expectTrue(memcmp(level2, expectedLevel2, sizeof(level2)) == 0,
        "second mip level preserves the legacy rounding across the chain");
}

static void testRejectsUnsupportedFormatsAndOverflow()
{
    static const unsigned char source[4] = { 0, 0, 0, 0 };
    unsigned char destination[4];

    fillBytes(destination, sizeof(destination), 0xcd);
    expectTrue(!Generate_Texture_Mip_Level_Box(
        source, 4, 1, 1, destination, 4, WW3D_FORMAT_R5G6B5),
		"uncharacterized R5G6B5 mip generation fails closed");
    expectTrue(!Generate_Texture_Mip_Level_Box(
        source, UINT_MAX, UINT_MAX, 1, destination, 4,
        WW3D_FORMAT_A8R8G8B8),
        "source width times bytes-per-pixel overflow is rejected");
}

int main()
{
    testBoxFilterHonorsPitchAndAlpha();
    testBoxFilterHandlesOddAndOneDimensionalLevels();
    testProductionFormatAlphaRules();
    testByteExactMipChain();
    testRejectsUnsupportedFormatsAndOverflow();

    if (s_failures != 0)
    {
        return s_failures;
    }

    return 0;
}
