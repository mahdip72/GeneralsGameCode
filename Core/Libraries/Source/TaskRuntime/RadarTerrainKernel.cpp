#include "Lib/RadarTerrainKernel.h"

#include <limits.h>

static unsigned radarTerrainBytesPerPixelForFormat(unsigned formatCode)
{
	switch (formatCode)
	{
	case RADAR_TERRAIN_FORMAT_R8G8B8:
		return 3;

	case RADAR_TERRAIN_FORMAT_X8R8G8B8:
		return 4;

	case RADAR_TERRAIN_FORMAT_R5G6B5:
	case RADAR_TERRAIN_FORMAT_X1R5G5B5:
		return 2;

	default:
		return 0;
	}
}

unsigned RadarTerrainBytesPerPixel(unsigned formatCode)
{
	return radarTerrainBytesPerPixelForFormat(formatCode);
}

void InterpolateRadarColorForHeight(RadarTerrainRgb *color,
	Real height, Real hiZ, Real midZ, Real loZ)
{
	const Real howBright = 0.95f;
	const Real howDark = 0.60f;
	Real t;
	RadarTerrainRgb colorTarget;

	if (color == 0)
	{
		return;
	}

	/* Preserve the legacy flat-map guards and argument order. */
	if (hiZ == midZ)
	{
		hiZ = midZ + 0.1f;
	}
	if (midZ == loZ)
	{
		loZ = midZ - 0.1f;
	}
	if (hiZ == loZ)
	{
		hiZ = loZ + 0.2f;
	}

	if (height >= midZ)
	{
		t = (height - midZ) / (hiZ - midZ);
		colorTarget.red = color->red + (1.0f - color->red) * howBright;
		colorTarget.green = color->green + (1.0f - color->green) * howBright;
		colorTarget.blue = color->blue + (1.0f - color->blue) * howBright;
	}
	else
	{
		t = (midZ - height) / (midZ - loZ);
		colorTarget.red = color->red + (0.0f - color->red) * howDark;
		colorTarget.green = color->green + (0.0f - color->green) * howDark;
		colorTarget.blue = color->blue + (0.0f - color->blue) * howDark;
	}

	color->red = color->red + (colorTarget.red - color->red) * t;
	color->green = color->green + (colorTarget.green - color->green) * t;
	color->blue = color->blue + (colorTarget.blue - color->blue) * t;

	if (color->red < 0.0f)
	{
		color->red = 0.0f;
	}
	if (color->red > 1.0f)
	{
		color->red = 1.0f;
	}
	if (color->green < 0.0f)
	{
		color->green = 0.0f;
	}
	if (color->green > 1.0f)
	{
		color->green = 1.0f;
	}
	if (color->blue < 0.0f)
	{
		color->blue = 0.0f;
	}
	if (color->blue > 1.0f)
	{
		color->blue = 1.0f;
	}
}

static unsigned radarTerrainColorByte(Real value)
{
	return static_cast<unsigned>(static_cast<unsigned char>(value * 255.0f));
}

static unsigned radarTerrainPackColor(unsigned formatCode, unsigned red,
	unsigned green, unsigned blue, unsigned alpha)
{
	switch (formatCode)
	{
	case RADAR_TERRAIN_FORMAT_R8G8B8:
		return (red << 16) | (green << 8) | blue;

	case RADAR_TERRAIN_FORMAT_A8R8G8B8:
		return (alpha << 24) | (red << 16) | (green << 8) | blue;

	case RADAR_TERRAIN_FORMAT_X8R8G8B8:
		return (0xFFu << 24) | (red << 16) | (green << 8) | blue;

	case RADAR_TERRAIN_FORMAT_R5G6B5:
		return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);

	case RADAR_TERRAIN_FORMAT_X1R5G5B5:
		return (1u << 15) | ((red >> 3) << 10) |
			((green >> 3) << 5) | (blue >> 3);

	case RADAR_TERRAIN_FORMAT_A1R5G5B5:
		return ((alpha >> 7) << 15) | ((red >> 3) << 10) |
			((green >> 3) << 5) | (blue >> 3);

	case RADAR_TERRAIN_FORMAT_A4R4G4B4:
		return ((alpha >> 4) << 12) | ((red >> 4) << 8) |
			((green >> 4) << 4) | (blue >> 4);

	case RADAR_TERRAIN_FORMAT_R3G3B2:
		return ((red >> 5) << 5) | ((green >> 5) << 2) | (blue >> 6);

	case RADAR_TERRAIN_FORMAT_A8:
		return alpha;

	case RADAR_TERRAIN_FORMAT_A8R3G3B2:
		return (alpha << 8) | ((red >> 5) << 5) |
			((green >> 5) << 2) | (blue >> 6);

	case RADAR_TERRAIN_FORMAT_X4R4G4B4:
		return (0xFu << 12) | ((red >> 4) << 8) |
			((green >> 4) << 4) | (blue >> 4);

	case RADAR_TERRAIN_FORMAT_L8:
		return (red * 77 + green * 150 + blue * 29) >> 8;

	case RADAR_TERRAIN_FORMAT_A8L8:
		return (alpha << 8) |
			((red * 77 + green * 150 + blue * 29) >> 8);

	case RADAR_TERRAIN_FORMAT_A4L4:
		return ((alpha >> 4) << 4) |
			(((red * 77 + green * 150 + blue * 29) >> 8) >> 4);

	default:
		/* Palettized, bump-map, and compressed formats have no ARGB color. */
		return 0;
	}
}

static void radarTerrainWritePixel(const RadarTerrainSnapshot &snapshot,
	const RadarTerrainRgb &color, unsigned char *pixelBytes)
{
	const unsigned red = radarTerrainColorByte(color.red);
	const unsigned green = radarTerrainColorByte(color.green);
	const unsigned blue = radarTerrainColorByte(color.blue);
	const unsigned packed = radarTerrainPackColor(snapshot.formatCode,
		red, green, blue, 255);
	unsigned byteIndex;

	for (byteIndex = 0; byteIndex < snapshot.bytesPerPixel; ++byteIndex)
	{
		pixelBytes[byteIndex] = static_cast<unsigned char>(
			(packed >> (byteIndex * 8)) & 0xFFu);
	}
}

static void radarTerrainAddSample(RadarTerrainRgb &sampleColor,
	const RadarTerrainRgb &color)
{
	sampleColor.red += color.red;
	sampleColor.green += color.green;
	sampleColor.blue += color.blue;
}

void ShadeRadarPixel(const RadarTerrainSnapshot &snapshot,
	unsigned x, unsigned y,
	const RadarTerrainCellInput *neighbors[3][3],
	unsigned char *pixelBytes)
{
	const RadarTerrainCellInput *center;
	RadarTerrainRgb sampleColor;
	RadarTerrainRgb color;
	unsigned samples = 0;
	unsigned matrixY;
	unsigned matrixX;

	if (neighbors == 0 || pixelBytes == 0 || x >= snapshot.width ||
		y >= snapshot.height || snapshot.bytesPerPixel == 0 ||
		snapshot.bytesPerPixel !=
		radarTerrainBytesPerPixelForFormat(snapshot.formatCode))
	{
		return;
	}

	center = neighbors[1][1];
	if (center == 0)
	{
		return;
	}

	sampleColor.red = 0.0f;
	sampleColor.green = 0.0f;
	sampleColor.blue = 0.0f;

	/* Match buildTerrainTexture: center branch first, then water, then terrain. */
	if (center->workingBridge)
	{
		for (matrixY = 0; matrixY < 3; ++matrixY)
		{
			for (matrixX = 0; matrixX < 3; ++matrixX)
			{
				if (neighbors[matrixY][matrixX] != 0)
				{
					color = center->bridgeColor;
					InterpolateRadarColorForHeight(&color, center->bridgeHeight,
						snapshot.terrainAverageZ, snapshot.mapHighZ,
						snapshot.mapLowZ);
					radarTerrainAddSample(sampleColor, color);
					++samples;
				}
			}
		}
	}
	else if (center->centerUnderwater)
	{
		for (matrixY = 0; matrixY < 3; ++matrixY)
		{
			for (matrixX = 0; matrixX < 3; ++matrixX)
			{
				const RadarTerrainCellInput *neighbor =
					neighbors[matrixY][matrixX];
				if (neighbor != 0 && neighbor->neighborUnderwater)
				{
					color = snapshot.waterColor;
					InterpolateRadarColorForHeight(&color,
						neighbor->neighborWaterBottomZ,
						center->centerWaterSurfaceZ,
						center->centerWaterSurfaceZ,
						snapshot.mapLowZ);
					radarTerrainAddSample(sampleColor, color);
					++samples;
				}
			}
		}
	}
	else
	{
		for (matrixY = 0; matrixY < 3; ++matrixY)
		{
			for (matrixX = 0; matrixX < 3; ++matrixX)
			{
				const RadarTerrainCellInput *neighbor =
					neighbors[matrixY][matrixX];
				if (neighbor != 0)
				{
					color = neighbor->terrainColor;
					InterpolateRadarColorForHeight(&color, neighbor->groundZ,
						snapshot.terrainAverageZ, snapshot.mapHighZ,
						snapshot.mapLowZ);
					radarTerrainAddSample(sampleColor, color);
					++samples;
				}
			}
		}
	}

	if (samples == 0)
	{
		samples = 1;
	}
	color.red = sampleColor.red / (Real)samples;
	color.green = sampleColor.green / (Real)samples;
	color.blue = sampleColor.blue / (Real)samples;
	radarTerrainWritePixel(snapshot, color, pixelBytes);
}

bool ShadeRadarRows(const RadarTerrainSnapshot &snapshot,
	unsigned char *output, unsigned yBegin, unsigned yEnd)
{
	const unsigned bytesPerPixel =
		radarTerrainBytesPerPixelForFormat(snapshot.formatCode);
	unsigned requiredRowBytes;
	unsigned y;

	if (output == 0 || snapshot.cells == 0 || snapshot.width == 0 ||
		snapshot.height == 0 || bytesPerPixel == 0 ||
		snapshot.bytesPerPixel != bytesPerPixel || yBegin > yEnd ||
		yEnd > snapshot.height)
	{
		return false;
	}

	if (snapshot.width > UINT_MAX / bytesPerPixel)
	{
		return false;
	}
	requiredRowBytes = snapshot.width * bytesPerPixel;
	if (snapshot.rowBytes < requiredRowBytes ||
		snapshot.width > UINT_MAX / snapshot.height ||
		snapshot.rowBytes > UINT_MAX / snapshot.height)
	{
		return false;
	}

	for (y = yBegin; y < yEnd; ++y)
	{
		unsigned x;
		unsigned char *row = output + y * snapshot.rowBytes;
		for (x = 0; x < snapshot.width; ++x)
		{
			const RadarTerrainCellInput *neighbors[3][3];
			unsigned matrixY;
			unsigned matrixX;

			for (matrixY = 0; matrixY < 3; ++matrixY)
			{
				for (matrixX = 0; matrixX < 3; ++matrixX)
				{
					neighbors[matrixY][matrixX] = 0;
				}
			}

			for (matrixY = 0; matrixY < 3; ++matrixY)
			{
				unsigned sourceY;
				if (matrixY == 0)
				{
					if (y == 0)
					{
						continue;
					}
					sourceY = y - 1;
				}
				else if (matrixY == 1)
				{
					sourceY = y;
				}
				else
				{
					if (y >= snapshot.height - 1)
					{
						continue;
					}
					sourceY = y + 1;
				}

				for (matrixX = 0; matrixX < 3; ++matrixX)
				{
					unsigned sourceX;
					if (matrixX == 0)
					{
						if (x == 0)
						{
							continue;
						}
						sourceX = x - 1;
					}
					else if (matrixX == 1)
					{
						sourceX = x;
					}
					else
					{
						if (x >= snapshot.width - 1)
						{
							continue;
						}
						sourceX = x + 1;
					}

					neighbors[matrixY][matrixX] =
						&snapshot.cells[sourceY * snapshot.width + sourceX];
				}
			}

			ShadeRadarPixel(snapshot, x, y, neighbors,
				row + x * snapshot.bytesPerPixel);
		}
	}

	return true;
}
