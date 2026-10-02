#ifndef RTS_RENDERER_POINT_GROUP_COLOR_PACKING_H
#define RTS_RENDERER_POINT_GROUP_COLOR_PACKING_H

#include "Renderer/LegacyColorPacking.h"

namespace rts
{
namespace render
{

// Point-group vertex colors duplicate each source point color across every
// vertex in its primitive. Cache by point index while writing one VB chunk.
template <class PointColor>
inline bool PackPointGroupColorForVertex(const PointColor *pointColors,
	int vertexIndex, int verticesPerPoint, int &cachedPointIndex,
	unsigned int &packedColor)
{
	const int pointIndex = vertexIndex / verticesPerPoint;
	if (pointIndex == cachedPointIndex)
		return false;

	const PointColor &color = pointColors[pointIndex];
	packedColor = PackLegacyARGB(color[0], color[1], color[2], color[3]);
	cachedPointIndex = pointIndex;
	return true;
}

} // namespace render
} // namespace rts

#endif // RTS_RENDERER_POINT_GROUP_COLOR_PACKING_H
