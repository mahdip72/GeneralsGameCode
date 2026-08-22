#include "Renderer/LegacyD3DMath.h"

#include <math.h>
#include <stdio.h>

namespace
{
	bool Near(float a, float b)
	{
		return fabsf(a - b) <= 1.0e-5f;
	}

	bool MatrixNear(const D3DMATRIX &a, const D3DMATRIX &b)
	{
		int row;
		int column;
		for (row = 0; row < 4; ++row) {
			for (column = 0; column < 4; ++column) {
				if (!Near(a.m[row][column], b.m[row][column])) {
					return false;
				}
			}
		}
		return true;
	}

	bool Check(bool condition, const char *description)
	{
		if (!condition) {
			fprintf(stderr, "LegacyD3DMathTest failed: %s\n", description);
		}
		return condition;
	}
}

int main()
{
	D3DMATRIX identity;
	LegacyD3DMatrixIdentity(&identity);
	if (!Check(identity._11 == 1.0f && identity._22 == 1.0f && identity._33 == 1.0f && identity._44 == 1.0f,
		"identity diagonal")) {
		return 1;
	}

	D3DMATRIX scale;
	D3DMATRIX translation;
	D3DMATRIX transform;
	LegacyD3DMatrixScaling(&scale, 2.0f, 3.0f, 4.0f);
	LegacyD3DMatrixTranslation(&translation, 5.0f, -6.0f, 7.0f);
	LegacyD3DMatrixMultiply(&transform, &scale, &translation);
	if (!Check(transform._11 == 2.0f && transform._22 == 3.0f && transform._33 == 4.0f &&
		transform._41 == 5.0f && transform._42 == -6.0f && transform._43 == 7.0f,
		"scale/translation multiplication")) {
		return 1;
	}

	D3DMATRIX transpose;
	LegacyD3DMatrixTranspose(&transpose, &transform);
	if (!Check(transpose._14 == transform._41 && transpose._24 == transform._42 && transpose._34 == transform._43,
		"transpose")) {
		return 1;
	}

	D3DMATRIX inverse;
	float determinant = 0.0f;
	if (!Check(LegacyD3DMatrixInverse(&inverse, &determinant, &transform), "affine inverse succeeds") ||
		!Check(determinant != 0.0f, "affine determinant")) {
		return 1;
	}
	D3DMATRIX roundTrip;
	LegacyD3DMatrixMultiply(&roundTrip, &transform, &inverse);
	if (!Check(MatrixNear(roundTrip, identity), "affine inverse round-trip")) {
		return 1;
	}

	D3DMATRIX general;
	general.m[0][0] = 1.0f; general.m[0][1] = 2.0f; general.m[0][2] = 3.0f; general.m[0][3] = 4.0f;
	general.m[1][0] = 0.0f; general.m[1][1] = 1.0f; general.m[1][2] = 4.0f; general.m[1][3] = 2.0f;
	general.m[2][0] = 5.0f; general.m[2][1] = 6.0f; general.m[2][2] = 0.0f; general.m[2][3] = 1.0f;
	general.m[3][0] = 1.0f; general.m[3][1] = 0.0f; general.m[3][2] = 2.0f; general.m[3][3] = 1.0f;
	if (!Check(LegacyD3DMatrixInverse(&inverse, &determinant, &general), "general inverse succeeds")) {
		return 1;
	}
	LegacyD3DMatrixMultiply(&roundTrip, &general, &inverse);
	if (!Check(MatrixNear(roundTrip, identity), "general inverse round-trip")) {
		return 1;
	}

	D3DMATRIX nearSingular;
	LegacyD3DMatrixIdentity(&nearSingular);
	nearSingular._22 = 1.0e-5f;
	if (!Check(LegacyD3DMatrixInverse(&inverse, &determinant, &nearSingular), "near-singular inverse is not rejected by an arbitrary epsilon") ||
		!Check(determinant != 0.0f, "near-singular determinant")) {
		return 1;
	}

	D3DMATRIX singular;
	LegacyD3DMatrixIdentity(&singular);
	singular._22 = 0.0f;
	LegacyD3DMatrixIdentity(&inverse);
	if (!Check(!LegacyD3DMatrixInverse(&inverse, &determinant, &singular), "singular inverse rejected") ||
		!Check(MatrixNear(inverse, identity), "singular output is deterministic identity")) {
		return 1;
	}

	return 0;
}
