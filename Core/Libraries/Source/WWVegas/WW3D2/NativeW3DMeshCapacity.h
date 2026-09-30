#ifndef RTS_WW3D2_NATIVE_W3D_MESH_CAPACITY_H
#define RTS_WW3D2_NATIVE_W3D_MESH_CAPACITY_H

#include <limits.h>

namespace rts
{
namespace render
{
inline bool ComputeStaticPolygonIndexCount(unsigned int polygonCount,
	unsigned int passCount, unsigned int *requiredIndexCount)
{
	if (requiredIndexCount == 0)
	{
		return false;
	}
	*requiredIndexCount = 0;
	if (polygonCount > static_cast<unsigned int>(INT_MAX))
	{
		return false;
	}
	if (passCount == 0)
	{
		return true;
	}

	const unsigned int maximumIndexCount =
		static_cast<unsigned short>(~0U);
	const unsigned int maximumPolygonCount =
		(maximumIndexCount / 3U) / passCount;
	if (polygonCount > maximumPolygonCount)
	{
		return false;
	}

	// The division guard proves both multiplications remain within the
	// unsigned-short index-count contract before this product is evaluated.
	*requiredIndexCount = polygonCount * 3U * passCount;
	return true;
}

inline bool ComputeStaticMeshIndexCount(int modelPolygonCount,
	unsigned int gapPolygonCount, int passCount,
	unsigned int *requiredIndexCount)
{
	if (requiredIndexCount == 0)
	{
		return false;
	}
	*requiredIndexCount = 0;
	if (modelPolygonCount < 0 || passCount < 0)
	{
		return false;
	}

	const unsigned int modelPolygons =
		static_cast<unsigned int>(modelPolygonCount);
	if (gapPolygonCount > UINT_MAX - modelPolygons)
	{
		return false;
	}
	return ComputeStaticPolygonIndexCount(modelPolygons + gapPolygonCount,
		static_cast<unsigned int>(passCount), requiredIndexCount);
}
}
}

#endif
