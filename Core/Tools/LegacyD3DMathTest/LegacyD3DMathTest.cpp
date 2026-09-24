#include "Renderer/RenderMatrixMath.h"

#include <math.h>
#include <stdio.h>

namespace
{
	bool Near(float a, float b)
	{
		return fabsf(a - b) <= 1.0e-5f;
	}

	bool MatrixNear(const RenderMatrix4x4 &a, const RenderMatrix4x4 &b)
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
			fprintf(stderr, "RenderMatrixMathTest failed: %s\n", description);
		}
		return condition;
	}
}

int main()
{
	RenderMatrix4x4 identity;
	RenderMatrixIdentity(&identity);
	if (!Check(identity.m[0][0] == 1.0f && identity.m[1][1] == 1.0f &&
		identity.m[2][2] == 1.0f && identity.m[3][3] == 1.0f,
		"identity diagonal")) {
		return 1;
	}

	RenderMatrix4x4 scale;
	RenderMatrix4x4 translation;
	RenderMatrix4x4 transform;
	RenderMatrixScaling(&scale, 2.0f, 3.0f, 4.0f);
	RenderMatrixTranslation(&translation, 5.0f, -6.0f, 7.0f);
	RenderMatrixMultiply(&transform, &scale, &translation);
	if (!Check(transform.m[0][0] == 2.0f && transform.m[1][1] == 3.0f &&
		transform.m[2][2] == 4.0f && transform.m[3][0] == 5.0f &&
		transform.m[3][1] == -6.0f && transform.m[3][2] == 7.0f,
		"scale/translation multiplication")) {
		return 1;
	}

	RenderMatrix4x4 transpose;
	RenderMatrixTranspose(&transpose, &transform);
	if (!Check(transpose.m[0][3] == transform.m[3][0] &&
		transpose.m[1][3] == transform.m[3][1] &&
		transpose.m[2][3] == transform.m[3][2],
		"transpose")) {
		return 1;
	}

	RenderMatrix4x4 inverse;
	float determinant = 0.0f;
	if (!Check(RenderMatrixInverse(&inverse, &determinant, &transform), "affine inverse succeeds") ||
		!Check(determinant != 0.0f, "affine determinant")) {
		return 1;
	}
	RenderMatrix4x4 roundTrip;
	RenderMatrixMultiply(&roundTrip, &transform, &inverse);
	if (!Check(MatrixNear(roundTrip, identity), "affine inverse round-trip")) {
		return 1;
	}

	RenderMatrix4x4 general;
	general.m[0][0] = 1.0f; general.m[0][1] = 2.0f; general.m[0][2] = 3.0f; general.m[0][3] = 4.0f;
	general.m[1][0] = 0.0f; general.m[1][1] = 1.0f; general.m[1][2] = 4.0f; general.m[1][3] = 2.0f;
	general.m[2][0] = 5.0f; general.m[2][1] = 6.0f; general.m[2][2] = 0.0f; general.m[2][3] = 1.0f;
	general.m[3][0] = 1.0f; general.m[3][1] = 0.0f; general.m[3][2] = 2.0f; general.m[3][3] = 1.0f;
	if (!Check(RenderMatrixInverse(&inverse, &determinant, &general), "general inverse succeeds")) {
		return 1;
	}
	RenderMatrixMultiply(&roundTrip, &general, &inverse);
	if (!Check(MatrixNear(roundTrip, identity), "general inverse round-trip")) {
		return 1;
	}

	RenderMatrix4x4 nearSingular;
	RenderMatrixIdentity(&nearSingular);
	nearSingular.m[1][1] = 1.0e-5f;
	if (!Check(RenderMatrixInverse(&inverse, &determinant, &nearSingular), "near-singular inverse is not rejected by an arbitrary epsilon") ||
		!Check(determinant != 0.0f, "near-singular determinant")) {
		return 1;
	}

	RenderMatrix4x4 singular;
	RenderMatrixIdentity(&singular);
	singular.m[1][1] = 0.0f;
	RenderMatrixIdentity(&inverse);
	if (!Check(!RenderMatrixInverse(&inverse, &determinant, &singular), "singular inverse rejected") ||
		!Check(MatrixNear(inverse, identity), "singular output is deterministic identity")) {
		return 1;
	}

	return 0;
}
