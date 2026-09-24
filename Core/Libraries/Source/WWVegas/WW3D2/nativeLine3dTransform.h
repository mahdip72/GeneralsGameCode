#ifndef RTS_WW3D2_NATIVE_LINE3D_TRANSFORM_H
#define RTS_WW3D2_NATIVE_LINE3D_TRANSFORM_H

#include "WWMath/matrix3d.h"

namespace rts
{
namespace render
{
// Matrix3D stores an affine transform as three rows whose fourth component
// is translation.  The native fixed-function shader consumes row vectors,
// so the legacy equivalent is the transposed 4x4 matrix with translation in
// the final row and an explicit homogeneous one.  Matrix3D has only rows
// 0..2; keeping every read explicit prevents an accidental row-3 OOB access.
inline bool Build_Native_Line3D_World_Matrix(const Matrix3D &transform,
	float *worldMatrix)
{
	if (worldMatrix == 0)
	{
		return false;
	}
	worldMatrix[0] = transform[0][0];
	worldMatrix[1] = transform[1][0];
	worldMatrix[2] = transform[2][0];
	worldMatrix[3] = 0.0f;
	worldMatrix[4] = transform[0][1];
	worldMatrix[5] = transform[1][1];
	worldMatrix[6] = transform[2][1];
	worldMatrix[7] = 0.0f;
	worldMatrix[8] = transform[0][2];
	worldMatrix[9] = transform[1][2];
	worldMatrix[10] = transform[2][2];
	worldMatrix[11] = 0.0f;
	worldMatrix[12] = transform[0][3];
	worldMatrix[13] = transform[1][3];
	worldMatrix[14] = transform[2][3];
	worldMatrix[15] = 1.0f;
	return true;
}
}
}

#endif
