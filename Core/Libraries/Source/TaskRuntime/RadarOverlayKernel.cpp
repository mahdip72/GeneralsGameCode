#include "Lib/RadarOverlayKernel.h"

#include <limits.h>

static unsigned radarOverlayBytesPerPixelForFormat(unsigned formatCode)
{
	switch (formatCode)
	{
	case RADAR_OVERLAY_FORMAT_A8R8G8B8:
		return 4;

	case RADAR_OVERLAY_FORMAT_A4R4G4B4:
		return 2;

	default:
		return 0;
	}
}

unsigned RadarOverlayBytesPerPixel(unsigned formatCode)
{
	return radarOverlayBytesPerPixelForFormat(formatCode);
}

static bool radarOverlayCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool radarOverlayValidateRows(unsigned width, unsigned height,
	unsigned bytesPerPixel, unsigned formatCode, unsigned rowBytes,
	unsigned char *output, unsigned rowBegin, unsigned rowEnd)
{
	unsigned requiredRowBytes;
	unsigned outputBytes;
	const unsigned expectedBytesPerPixel =
		radarOverlayBytesPerPixelForFormat(formatCode);

	if (output == 0 || width == 0 || height == 0 || width > INT_MAX ||
		height > INT_MAX || rowBegin > rowEnd ||
		rowEnd > height || expectedBytesPerPixel == 0 ||
		bytesPerPixel != expectedBytesPerPixel ||
		!radarOverlayCheckedMultiply(width, bytesPerPixel,
			&requiredRowBytes) || rowBytes < requiredRowBytes ||
		!radarOverlayCheckedMultiply(rowBytes, height, &outputBytes))
	{
		return false;
	}

	/* Keep the checked result live so the multiplication is an explicit
	 * validation of the last row's address arithmetic. */
	return outputBytes >= requiredRowBytes;
}

static bool radarOverlayValidateObjectSnapshot(
	const RadarObjectOverlaySnapshot &snapshot, unsigned rowBegin,
	unsigned rowEnd)
{
	unsigned commandBytes;
	if (!radarOverlayValidateRows(snapshot.width, snapshot.height,
		snapshot.bytesPerPixel, snapshot.formatCode, snapshot.rowBytes,
		snapshot.output, rowBegin, rowEnd))
	{
		return false;
	}

	return snapshot.commandCount <= snapshot.commandCapacity &&
		radarOverlayCheckedMultiply(snapshot.commandCapacity,
			static_cast<unsigned>(sizeof(RadarObjectOverlayCommand)),
			&commandBytes) &&
		(snapshot.commandCount == 0 || snapshot.commands != 0);
}

static bool radarOverlayValidateShroudSnapshot(
	const RadarShroudOverlaySnapshot &snapshot, unsigned rowBegin,
	unsigned rowEnd)
{
	unsigned commandBytes;
	if (!radarOverlayValidateRows(snapshot.width, snapshot.height,
		snapshot.bytesPerPixel, snapshot.formatCode, snapshot.rowBytes,
		snapshot.output, rowBegin, rowEnd))
	{
		return false;
	}

	return snapshot.commandCount <= snapshot.commandCapacity &&
		radarOverlayCheckedMultiply(snapshot.commandCapacity,
			static_cast<unsigned>(sizeof(RadarShroudOverlayCommand)),
			&commandBytes) &&
		(snapshot.commandCount == 0 || snapshot.commands != 0);
}

static void radarOverlayWritePixel(unsigned packedColor, unsigned bytesPerPixel,
	unsigned char *output,
	unsigned rowBytes, unsigned x, unsigned y)
{
	unsigned byteIndex;
	unsigned char *destination;
	destination = output + y * rowBytes + x * bytesPerPixel;
	for (byteIndex = 0; byteIndex < bytesPerPixel; ++byteIndex)
	{
		destination[byteIndex] = static_cast<unsigned char>(
			(packedColor >> (byteIndex * 8)) & 0xFFu);
	}
}

/*
 * Add one of the two footprint offsets without ever evaluating a signed
 * x + 1/y + 1 expression.  -1 + 1 is the one negative edge coordinate that
 * can become legal; all other negative bases remain clipped.
 */
static bool radarOverlayObjectCoordinate(Int base, unsigned offset,
	unsigned limit, unsigned *coordinate)
{
	unsigned value;

	if (coordinate == 0 || offset > 1)
	{
		return false;
	}

	if (base < 0)
	{
		if (base != -1 || offset != 1)
		{
			return false;
		}
		*coordinate = 0;
		return limit > 0;
	}

	value = static_cast<unsigned>(base);
	if (offset > UINT_MAX - value)
	{
		return false;
	}
	value += offset;
	if (value >= limit)
	{
		return false;
	}
	*coordinate = value;
	return true;
}

static void radarOverlayWriteObjectPoint(
	const RadarObjectOverlaySnapshot &snapshot,
	const RadarObjectOverlayCommand &command, unsigned offsetX,
	unsigned offsetY, unsigned rowBegin, unsigned rowEnd)
{
	unsigned x;
	unsigned y;
	if (radarOverlayObjectCoordinate(command.x, offsetX, snapshot.width, &x) &&
		radarOverlayObjectCoordinate(command.y, offsetY, snapshot.height, &y) &&
		y >= rowBegin && y < rowEnd)
	{
		radarOverlayWritePixel(command.packedColor,
			snapshot.bytesPerPixel, snapshot.output, snapshot.rowBytes, x, y);
	}
}

static bool radarOverlayCoordinateInRange(unsigned coordinate, Int minimum,
	Int maximum)
{
	if (coordinate > static_cast<unsigned>(INT_MAX))
	{
		return false;
	}

	return static_cast<Int>(coordinate) >= minimum &&
		static_cast<Int>(coordinate) <= maximum;
}

bool PackRadarObjectRows(const RadarObjectOverlaySnapshot &snapshot,
	unsigned rowBegin, unsigned rowEnd)
{
	if (!radarOverlayValidateObjectSnapshot(snapshot, rowBegin, rowEnd))
	{
		return false;
	}

	/* Each worker owns disjoint rows.  Walk the ordered command stream once per
	 * stripe rather than once per row, preserving last-writer-wins while keeping
	 * preparation proportional to commands times workers, not image height. */
	unsigned commandIndex;
	for (commandIndex = 0; commandIndex < snapshot.commandCount;
		++commandIndex)
	{
		const RadarObjectOverlayCommand &command =
			snapshot.commands[commandIndex];
		/* Keep the four writes in the exact owner order. */
		radarOverlayWriteObjectPoint(snapshot, command, 0, 0, rowBegin, rowEnd);
		radarOverlayWriteObjectPoint(snapshot, command, 0, 1, rowBegin, rowEnd);
		radarOverlayWriteObjectPoint(snapshot, command, 1, 1, rowBegin, rowEnd);
		radarOverlayWriteObjectPoint(snapshot, command, 1, 0, rowBegin, rowEnd);
	}

	return true;
}

bool PackRadarShroudRows(const RadarShroudOverlaySnapshot &snapshot,
	unsigned rowBegin, unsigned rowEnd)
{
	unsigned row;

	if (!radarOverlayValidateShroudSnapshot(snapshot, rowBegin, rowEnd))
	{
		return false;
	}

	for (row = rowBegin; row < rowEnd; ++row)
	{
		unsigned commandIndex;
		for (commandIndex = 0; commandIndex < snapshot.commandCount;
			++commandIndex)
		{
			const RadarShroudOverlayCommand &command =
				snapshot.commands[commandIndex];
			unsigned x;
			unsigned xBegin;
			unsigned xEnd;

			if (command.minX > command.maxX || command.minY > command.maxY ||
				!radarOverlayCoordinateInRange(row, command.minY,
					command.maxY))
			{
				continue;
			}

			if (command.maxX < 0 || command.minX >=
				static_cast<Int>(snapshot.width))
			{
				continue;
			}
			xBegin = command.minX < 0 ? 0u :
				static_cast<unsigned>(command.minX);
			xEnd = command.maxX >= static_cast<Int>(snapshot.width) ?
				snapshot.width - 1 : static_cast<unsigned>(command.maxX);

			for (x = xBegin; x <= xEnd; ++x)
			{
				radarOverlayWritePixel(command.packedColor,
					snapshot.bytesPerPixel, snapshot.output,
					snapshot.rowBytes, x, row);
			}
		}
	}

	return true;
}
