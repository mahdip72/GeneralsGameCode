/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "W3DDevice/GameClient/W3DScreenshotCodec.h"

#include <limits.h>

unsigned BuildScreenshotRowRanges(unsigned height, unsigned workerCount,
	ScreenshotRowRange *ranges, unsigned rangeCapacity)
{
	unsigned rangeCount = 1;
	unsigned rowsPerRange;
	unsigned remainder;
	unsigned y = 0;
	unsigned index;

	if (ranges == 0 || rangeCapacity == 0 || height == 0)
	{
		return 0;
	}

	if (workerCount > 1 && height >= 128)
	{
		const unsigned rangesForWorkers = workerCount > UINT_MAX / 2
			? UINT_MAX : workerCount * 2;
		const unsigned rangesForRows = height / 64 + (height % 64 != 0 ? 1 : 0);
		rangeCount = rangesForWorkers < rangesForRows ? rangesForWorkers : rangesForRows;
		if (rangeCount > rangeCapacity)
		{
			rangeCount = rangeCapacity;
		}
	}

	rowsPerRange = height / rangeCount;
	remainder = height % rangeCount;
	for (index = 0; index < rangeCount; ++index)
	{
		const unsigned rowCount = rowsPerRange + (index < remainder ? 1 : 0);
		ranges[index].yBegin = y;
		y += rowCount;
		ranges[index].yEnd = y;
	}

	return rangeCount;
}

void ConvertScreenshotRows(const ScreenshotPixelSource &source,
	unsigned yBegin, unsigned yEnd, unsigned char *rgbDestination)
{
	unsigned y;

	for (y = yBegin; y < yEnd; ++y)
	{
		unsigned x;
		const unsigned char *sourceRow = source.pixels + y * source.pitch;

		if (source.format == SCREENSHOT_SOURCE_ARGB32)
		{
			const unsigned int *argbRow = reinterpret_cast<const unsigned int *>(sourceRow);
			for (x = 0; x < source.width; ++x)
			{
				const unsigned int argb = argbRow[x];
				const unsigned index = 3 * (x + y * source.width);
				rgbDestination[index + 0] = (unsigned char)(argb >> 16);
				rgbDestination[index + 1] = (unsigned char)(argb >> 8);
				rgbDestination[index + 2] = (unsigned char)argb;
			}
		}
		else
		{
			const unsigned short *rgbRow = reinterpret_cast<const unsigned short *>(sourceRow);
			for (x = 0; x < source.width; ++x)
			{
				const unsigned short rgb = rgbRow[x];
				const unsigned index = 3 * (x + y * source.width);
				rgbDestination[index + 0] = (unsigned char)((rgb & 0xF800) >> 8);
				rgbDestination[index + 1] = (unsigned char)((rgb & 0x07E0) >> 3);
				rgbDestination[index + 2] = (unsigned char)((rgb & 0x001F) << 3);
			}
		}
	}
}
