/*
** Command & Conquer Generals(tm)
**
** Renderer-local replacements for the small subset of legacy matrix
** operations used by the product runtime.  The storage and multiplication
** convention intentionally match D3DMATRIX so existing shader
** constants and texture transforms remain byte-for-byte compatible at the
** API boundary without linking the deprecated helper library.
*/

#pragma once

#include "WWLib/win.h"
#include <d3d8types.h>
#include <math.h>
#include <string.h>

inline void LegacyD3DMatrixIdentity(D3DMATRIX *out)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 1.0f;
	out->_22 = 1.0f;
	out->_33 = 1.0f;
	out->_44 = 1.0f;
}

inline void LegacyD3DMatrixScaling(D3DMATRIX *out, float x, float y, float z)
{
	LegacyD3DMatrixIdentity(out);
	out->_11 = x;
	out->_22 = y;
	out->_33 = z;
}

inline void LegacyD3DMatrixTranslation(D3DMATRIX *out, float x, float y, float z)
{
	LegacyD3DMatrixIdentity(out);
	out->_41 = x;
	out->_42 = y;
	out->_43 = z;
}

inline void LegacyD3DMatrixMultiply(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
	D3DMATRIX result;
	int row;
	int column;
	for (row = 0; row < 4; ++row) {
		for (column = 0; column < 4; ++column) {
			result.m[row][column] =
				a->m[row][0] * b->m[0][column] +
				a->m[row][1] * b->m[1][column] +
				a->m[row][2] * b->m[2][column] +
				a->m[row][3] * b->m[3][column];
		}
	}
	*out = result;
}

inline void LegacyD3DMatrixTranspose(D3DMATRIX *out, const D3DMATRIX *in)
{
	D3DMATRIX result;
	int row;
	int column;
	for (row = 0; row < 4; ++row) {
		for (column = 0; column < 4; ++column) {
			result.m[row][column] = in->m[column][row];
		}
	}
	*out = result;
}

/*
** Invert a row-major 4x4 matrix using the same cofactor layout as the
** historical implementation.  The temporary result also makes the
** function safe when out aliases in, as the old API allowed.
*/
inline bool LegacyD3DMatrixInverse(D3DMATRIX *out, float *detOut, const D3DMATRIX *in)
{
	const float m00 = in->m[0][0], m01 = in->m[0][1], m02 = in->m[0][2], m03 = in->m[0][3];
	const float m10 = in->m[1][0], m11 = in->m[1][1], m12 = in->m[1][2], m13 = in->m[1][3];
	const float m20 = in->m[2][0], m21 = in->m[2][1], m22 = in->m[2][2], m23 = in->m[2][3];
	const float m30 = in->m[3][0], m31 = in->m[3][1], m32 = in->m[3][2], m33 = in->m[3][3];

	const float s0 = m00 * m11 - m10 * m01;
	const float s1 = m00 * m12 - m10 * m02;
	const float s2 = m00 * m13 - m10 * m03;
	const float s3 = m01 * m12 - m11 * m02;
	const float s4 = m01 * m13 - m11 * m03;
	const float s5 = m02 * m13 - m12 * m03;

	const float c5 = m22 * m33 - m32 * m23;
	const float c4 = m21 * m33 - m31 * m23;
	const float c3 = m21 * m32 - m31 * m22;
	const float c2 = m20 * m33 - m30 * m23;
	const float c1 = m20 * m32 - m30 * m22;
	const float c0 = m20 * m31 - m30 * m21;

	const float determinant = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
	if (detOut != 0) {
		*detOut = determinant;
	}
	if (determinant == 0.0f) {
		/* Keep ignored-return callers deterministic instead of exposing stale data. */
		LegacyD3DMatrixIdentity(out);
		return false;
	}

	const float invDet = 1.0f / determinant;
	D3DMATRIX result;
	result.m[0][0] = ( m11 * c5 - m12 * c4 + m13 * c3) * invDet;
	result.m[0][1] = (-m01 * c5 + m02 * c4 - m03 * c3) * invDet;
	result.m[0][2] = ( m31 * s5 - m32 * s4 + m33 * s3) * invDet;
	result.m[0][3] = (-m21 * s5 + m22 * s4 - m23 * s3) * invDet;

	result.m[1][0] = (-m10 * c5 + m12 * c2 - m13 * c1) * invDet;
	result.m[1][1] = ( m00 * c5 - m02 * c2 + m03 * c1) * invDet;
	result.m[1][2] = (-m30 * s5 + m32 * s2 - m33 * s1) * invDet;
	result.m[1][3] = ( m20 * s5 - m22 * s2 + m23 * s1) * invDet;

	result.m[2][0] = ( m10 * c4 - m11 * c2 + m13 * c0) * invDet;
	result.m[2][1] = (-m00 * c4 + m01 * c2 - m03 * c0) * invDet;
	result.m[2][2] = ( m30 * s4 - m31 * s2 + m33 * s0) * invDet;
	result.m[2][3] = (-m20 * s4 + m21 * s2 - m23 * s0) * invDet;

	result.m[3][0] = (-m10 * c3 + m11 * c1 - m12 * c0) * invDet;
	result.m[3][1] = ( m00 * c3 - m01 * c1 + m02 * c0) * invDet;
	result.m[3][2] = (-m30 * s3 + m31 * s1 - m32 * s0) * invDet;
	result.m[3][3] = ( m20 * s3 - m21 * s1 + m22 * s0) * invDet;

	*out = result;
	return true;
}
