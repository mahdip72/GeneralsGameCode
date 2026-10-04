/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Device-free C++98 fixture executing source-extracted production decisions.
#include "WWMath/matrix4.h"
#include "WWMath/matrix3d.h"
#include "WWMath/quat.h"
#include "Lib/BaseTypeCore.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace GeneralsThreshold {
#include "GeneralsShadowThreshold.inc"
}
namespace ZeroHourThreshold {
#include "GeneralsMDShadowThreshold.inc"
}

static bool GeneralsOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = GeneralsThreshold::cosAngleToCare;
#include "GeneralsShadowOrientation.inc"
    return isMeshRotating;
}
static bool ZeroHourOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = ZeroHourThreshold::cosAngleToCare;
#include "GeneralsMDShadowOrientation.inc"
    return isMeshRotating;
}
#define CNC3
static bool NormalizedOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = ZeroHourThreshold::cosAngleToCare;
#include "GeneralsMDShadowOrientation.inc"
    return isMeshRotating;
}
#undef CNC3
#define ASSUME_NEAR_LIGHTSOURCE
static bool GeneralsNearOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = GeneralsThreshold::cosAngleToCare;
    (void)cosAngleToCare;
#include "GeneralsShadowOrientation.inc"
    return isMeshRotating;
}
static bool ZeroHourNearOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = ZeroHourThreshold::cosAngleToCare;
    (void)cosAngleToCare;
#include "GeneralsMDShadowOrientation.inc"
    return isMeshRotating;
}
#undef ASSUME_NEAR_LIGHTSOURCE
#define CNC3
#define ASSUME_NEAR_LIGHTSOURCE
static bool NormalizedNearOrientation(Matrix4x4 previous, Matrix4x4 objectToWorld)
{
    Matrix4x4 *prevXForm = &previous;
    Bool isMeshRotating = false;
    const Real cosAngleToCare = ZeroHourThreshold::cosAngleToCare;
#include "GeneralsMDShadowOrientation.inc"
    return isMeshRotating;
}
#undef ASSUME_NEAR_LIGHTSOURCE
#undef CNC3

typedef bool (*Decision)(Matrix4x4, Matrix4x4);
static int Check(bool condition, const char *mode, const char *message)
{
    if (!condition) fprintf(stderr, "FAIL: %s: %s\n", mode, message);
    return condition ? 0 : 1;
}

static Matrix4x4 Rotation(int axis, float degrees)
{
    Matrix4x4 result(true);
    const float radians = degrees * static_cast<float>(PI / 180.0);
    const float c = static_cast<float>(cos(radians));
    const float s = static_cast<float>(sin(radians));
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    result[a][a] = c; result[a][b] = -s;
    result[b][a] = s; result[b][b] = c;
    return result;
}

static int CheckMode(Decision decision, const char *name, bool nearLight, bool normalized)
{
    int failures = 0;
    const Matrix4x4 identity(true);
    failures += Check(!decision(identity, identity), name, "identity retained");
    for (int axis = 0; axis < 3; ++axis)
    {
        failures += Check(decision(identity, Rotation(axis, 0.1f)) == nearLight,
            name, "subthreshold orientation contract");
        failures += Check(decision(identity, Rotation(axis, 0.3f)), name, "above threshold");
        failures += Check(decision(identity, Rotation(axis, 90.0f)), name, "quarter turn");
        failures += Check(decision(identity, Rotation(axis, 180.0f)), name, "half turn");
        failures += Check(decision(identity, Rotation(axis, 179.9f)), name, "near half turn");
        failures += Check(decision(identity, Rotation(axis, -180.0f)), name, "negative half turn");
        Matrix4x4 previous = Rotation(axis, 90.0f);
        failures += Check(!decision(previous, previous), name, "unchanged nonidentity retained");
    }
    Matrix4x4 translated(identity);
    translated[0].W = 10.0f; translated[1].W = -4.0f; translated[2].W = 3.0f;
    failures += Check(decision(identity, translated) == nearLight, name, "translation contract");
    Matrix4x4 scaled(identity);
    for (int axis = 0; axis < 3; ++axis) scaled[axis][axis] = 0.5f;
    failures += Check(decision(scaled, scaled) == (!normalized && !nearLight),
        name, "existing unnormalized versus CNC3 normalization contract");
    return failures;
}

static Matrix3D Compose(const Matrix3D &left, const Matrix3D &right)
{
    Matrix3D result;
    Matrix3D::Multiply(left, right, &result);
    return result;
}

static Vector3 TransformPoint(const Matrix3D &transform, const Vector3 &point)
{
    Vector3 result;
    Matrix3D::Transform_Vector(transform, point, &result);
    return result;
}

static int CheckHelixExtrusion(Decision decision, const char *name)
{
    // Frozen reference vectors transcribed from NVHELIX_PROPS02 in NVHelix.W3D.
    // Source-asset SHA-256 (provenance only; this test does not load the proprietary
    // runtime asset): 20fa9ead5be843146aefb211da53850e3eaa4c4dcbe93c0abc56602b8845003d.
    // Actual child inherits animated PROPELLER02; its blade cards are asymmetric.
    Matrix3D parent(Quaternion(-0.740167975f, -0.671784580f,
        0.020225441f, 0.021159338f), Vector3(-0.563622475f, -0.188373744f, 23.917997f));
    Matrix3D child(Quaternion(-0.000181539f, 0.002280675f,
        0.005349963f, 0.999983072f), Vector3(6.760917187f, 0.793124795f, -0.115654409f));
    Matrix3D halfTurn(true);
    halfTurn.Rotate_Z(static_cast<float>(PI));
    // Synthetic world placement adds 100 units of airborne height; all
    // serialized parent/child transforms and blade vertices remain unchanged.
    Matrix3D aircraft(true);
    aircraft.Set_Translation(Vector3(0.0f, 0.0f, 100.0f));
    const Matrix3D before = Compose(aircraft, Compose(parent, child));
    // A whole-aircraft world-yaw half turn preserves the actual rotor tilt
    // and child geometry. It negates world X/Y rows, exposing the old cache
    // predicate independently of slight exporter tilt in the rotor pivots.
    const Matrix3D after = Compose(aircraft, Compose(halfTurn, Compose(parent, child)));
    const bool rebuild = decision(Matrix4x4(before), Matrix4x4(after));
    Matrix3D oldInverse, newInverse;
    before.Get_Orthogonal_Inverse(oldInverse);
    after.Get_Orthogonal_Inverse(newInverse);
    // Oblique light remains stationary. Reusing the old object-space extrusion
    // and submitting with the new mesh transform rotates its world direction.
    const Vector3 light(1000.0f, 700.0f, 5000.0f);
    const Vector3 oldLocal = TransformPoint(oldInverse, light);
    const Vector3 newLocal = TransformPoint(newInverse, light);
    const Vector3 selectedLocal = rebuild ? newLocal : oldLocal;
    const Vector3 selectedWorld = TransformPoint(after, selectedLocal);
    const float error = (selectedWorld - light).Length();
    int failures = Check((oldLocal - newLocal).Length() > 100.0f,
        name, "actual rotor half turn changes local light/extrusion");
    failures += Check(rebuild, name, "actual inherited rotor half turn invalidates cache");
    failures += Check(error < 0.02f, name, "selected extrusion remains aligned to stationary light");
    // One serialized outer vertex from each of the three asymmetric cards.
    const Vector3 bladeVertices[] = {
        Vector3(40.241859436f, -11.060291290f, -0.188567132f),
        Vector3(-19.483293533f, 45.575180054f, -0.144752398f),
        Vector3(-40.303874969f, -34.976665497f, 0.774066329f)
    };
    const Vector3 staleWorldLight = TransformPoint(after, oldLocal);
    for (int blade = 0; blade < 3; ++blade)
    {
        const Vector3 worldVertex = TransformPoint(after, bladeVertices[blade]);
        const Vector3 correctGround = worldVertex + (worldVertex - light) *
            (worldVertex.Z / (light.Z - worldVertex.Z));
        const Vector3 staleGround = worldVertex + (worldVertex - staleWorldLight) *
            (worldVertex.Z / (staleWorldLight.Z - worldVertex.Z));
        const Vector3 selectedGround = worldVertex + (worldVertex - selectedWorld) *
            (worldVertex.Z / (selectedWorld.Z - worldVertex.Z));
        failures += Check((staleGround - correctGround).Length() > 1.0f,
            name, "actual blade ground extrusion differs when cache is stale");
        failures += Check((selectedGround - correctGround).Length() < 0.02f,
            name, "actual blade selected extrusion reaches correct ground point");
    }
    return failures;
}

int main()
{
    Decision decisions[] = { GeneralsOrientation, ZeroHourOrientation, NormalizedOrientation,
        GeneralsNearOrientation, ZeroHourNearOrientation, NormalizedNearOrientation };
    const char *names[] = { "Generals", "ZeroHour", "ZeroHour CNC3", "Generals near", "ZeroHour near",
        "ZeroHour CNC3 near" };
    int failures = 0;
    for (int i = 0; i < 6; ++i)
    {
        failures += CheckMode(decisions[i], names[i], i == 3 || i == 4, i == 2 || i == 5);
        failures += CheckHelixExtrusion(decisions[i], names[i]);
    }
    if (!failures) printf("production shadow orientation cache contracts passed\n");
    return failures ? 1 : 0;
}
