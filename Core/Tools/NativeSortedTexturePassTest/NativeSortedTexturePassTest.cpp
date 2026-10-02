#include "Utility/CppMacros.h"
#include "Renderer/RenderGameTexturePass.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include <cstdio>
#include <stdexcept>

using namespace rts::render;
class TextureClass {};
class ZTextureClass {};
typedef float Real;
typedef int Int;
enum { WATER_TYPE_2_PVSHADER = 2, TIME_OF_DAY_NIGHT = 1, WW3D_ERROR_OK = 0 };
#ifndef FALSE
#define FALSE 0
#endif

namespace {
unsigned int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "line %u: %s\n", \
    static_cast<unsigned int>(__LINE__), #x); } } while (0)
struct Control
{
    Control() : native(true), target(false), frame(false), select(RENDER_RESULT_OK),
        begin(RENDER_RESULT_OK), clear(RENDER_RESULT_OK), flush(RENDER_RESULT_OK),
        restore(RENDER_RESULT_OK), completion(RENDER_RESULT_OK), renderError(false),
        renderThrows(false), selects(0), begins(0), clears(0), flushes(0),
        restores(0), ends(0), renders(0), visibleBegins(0), sky(0), body(0),
        passedFirst(RENDER_RESULT_OK), endResult(RENDER_RESULT_OK), visibleSea(true),
        frameFailures(0), seaReachedSampling(0) {}
    bool native, target, frame;
    RenderResult select, begin, clear, flush, restore, completion;
    bool renderError, renderThrows;
    unsigned int selects, begins, clears, flushes, restores, ends, renders, visibleBegins, sky, body;
    RenderResult passedFirst, endResult;
    bool visibleSea;
    unsigned int frameFailures, seaReachedSampling;
} control;
}

class CameraClass
{
public:
    CameraClass() : transform(true), minimum(.1f, .2f), maximum(.8f, .9f), applies(0)
    { transform.Set_Translation(Vector3(2, 3, 10)); }
    const Matrix3D &Get_Transform() const { return transform; }
    void Set_Transform(const Matrix3D &value) { transform = value; }
    void Get_Viewport(Vector2 &a, Vector2 &b) const { a = minimum; b = maximum; }
    void Set_Viewport(const Vector2 &a, const Vector2 &b) { minimum = a; maximum = b; }
    void Apply() { ++applies; }
    Matrix3D transform;
    Vector2 minimum, maximum;
    unsigned int applies;
};

class ShaderClass
{
public:
    static bool Is_Backface_Culling_Inverted() { return inverted; }
    static void Invert_Backface_Culling(bool value) { inverted = value; }
    static bool inverted;
};
bool ShaderClass::inverted = false;

class AABoxClass {};
struct RenderInfoClass { CameraClass &Camera; };
class BufferMock { public: bool Is_Valid() const { return true; } };

class TerrainMock { public: void *getMap() const { return reinterpret_cast<void *>(1); } };
TerrainMock terrain;
TerrainMock *TheTerrainRenderObject = &terrain;
class WW3D
{
public:
    static int Render(void *, CameraClass *)
    {
        ++control.renders;
        if (control.renderThrows) throw std::runtime_error("mock scene failure");
        return control.renderError ? 1 : WW3D_ERROR_OK;
    }
    static int Begin_Render(bool, bool, const Vector3 &, float)
    {
        ++control.visibleBegins;
        return WW3D_ERROR_OK;
    }
};

class WaterRenderObjClass
{
public:
    WaterRenderObjClass() : m_waterType(WATER_TYPE_2_PVSHADER),
        m_pReflectionTexture(&color), m_pReflectionDepthTexture(&depth),
        m_level(4), m_tod(TIME_OF_DAY_NIGHT), m_parentScene(0), m_nativeReflectionReady(false),
        m_fBumpFrame(0), m_vertexBuffer(&vertex), m_waterIndexBuffer(&index),
        m_waveVertexShader(1), m_wavePixelShader(1), m_numIndices(3), m_numVertices(4)
    { for (unsigned int i = 0; i < 32; ++i) m_pBumpTexture[i] = &color; }
    bool updateRenderTargetTextures(CameraClass *);
    bool renderMirror(CameraClass *);
    void drawSea(RenderInfoClass &);
    bool getClippedWaterPlane(CameraClass *, void *) const { return control.visibleSea; }
    void renderSky() { ++control.sky; }
    void renderSkyBody(Matrix3D *) { ++control.body; }
    int m_waterType;
    TextureClass *m_pReflectionTexture;
    ZTextureClass *m_pReflectionDepthTexture;
    float m_level;
    int m_tod;
    void *m_parentScene;
    bool m_nativeReflectionReady;
    TextureClass color;
    ZTextureClass depth;
    float m_fBumpFrame;
    BufferMock *m_vertexBuffer, *m_waterIndexBuffer;
    unsigned int m_waveVertexShader, m_wavePixelShader;
    int m_numIndices, m_numVertices;
    TextureClass *m_pBumpTexture[32]; // Verified against extracted actual constant.
    BufferMock vertex, index;
};

namespace rts { namespace render {
bool IsNativeGameRendererActive() { return control.native; }
bool IsGameRenderingToTexture() { return control.target; }
void RecordGameRenderFailure(RenderResult result)
{ CHECK(result == RENDER_RESULT_FAILED); ++control.frameFailures; }
RenderResult SetGameRenderTargetChecked(TextureClass *color, ZTextureClass *, bool)
{
    if (color)
    {
        ++control.selects;
        if (control.select == RENDER_RESULT_OK) control.target = true;
        return control.select;
    }
    ++control.restores;
    if (control.restore == RENDER_RESULT_OK) control.target = false;
    return control.restore;
}
void SetGameRenderTarget(TextureClass *color, ZTextureClass *depth, bool useDefault)
{ (void)SetGameRenderTargetChecked(color, depth, useDefault); }
RenderResult BeginGameRender(bool, bool, const GameRenderColor &, float)
{
    ++control.begins;
    if (control.begin == RENDER_RESULT_OK) control.frame = true;
    return control.begin;
}
RenderResult ClearGameRenderTargets(bool, bool, const GameRenderColor &, float)
{ ++control.clears; return control.clear; }
RenderResult FlushGameSortedTriangles() { ++control.flushes; return control.flush; }
RenderResult EndGameTextureRenderPass(RenderResult first)
{
    CHECK(control.frame);
    ++control.ends;
    control.frame = false;
    control.passedFirst = first;
    control.endResult = first != RENDER_RESULT_OK ? first : control.completion;
    return control.endResult;
}
RenderResult EndGameRender(bool present)
{ CHECK(!present); ++control.ends; control.frame = false; return RENDER_RESULT_OK; }
} }

#include "water.inc"
static_assert(NUM_BUMP_FRAMES == 32, "Fixture carrier must match the extracted production bound");

struct GlobalDataMock { int m_waterType = 2; int m_breakTheMovie = FALSE; bool m_disableRender = false; };
struct TransparencyMock { float m_minWaterOpacity = .4f; };
struct ViewMock { CameraClass *camera; CameraClass *get3DCamera() const { return camera; } };
GlobalDataMock globalData;
TransparencyMock transparency;
GlobalDataMock *TheGlobalData = &globalData;
TransparencyMock *TheWaterTransparency = &transparency;
WaterRenderObjClass *TheWaterRenderObj = 0;
ViewMock *primaryW3DView = 0;
#include "Generals-display.inc"
#include "GeneralsMD-display.inc"

namespace {
bool SameMatrix(const Matrix3D &a, const Matrix3D &b)
{
    for (unsigned int row = 0; row < 3; ++row)
        for (unsigned int column = 0; column < 4; ++column)
            if (a[row][column] != b[row][column]) return false;
    return true;
}

void TestStickyHeaderResult()
{
    control = Control();
    TextureClass color;
    ZTextureClass depth;
    {
        GameTextureRenderPass pass(&color, &depth, false);
        CHECK(pass.IsReady());
        pass.Fail(RENDER_RESULT_UNSUPPORTED);
        control.restore = RENDER_RESULT_FAILED;
        control.completion = RENDER_RESULT_DEVICE_REMOVED;
        CHECK(pass.Finish() == RENDER_RESULT_UNSUPPORTED);
        CHECK(pass.Finish() == RENDER_RESULT_UNSUPPORTED);
        CHECK(control.ends == 1 && control.passedFirst == RENDER_RESULT_UNSUPPORTED);
    }
    CHECK(control.ends == 1 && !control.frame);
    control = Control();
    control.select = RENDER_RESULT_INVALID_ARGUMENT;
    {
        GameTextureRenderPass pass(&color, &depth, false);
        CHECK(!pass.IsReady());
        CHECK(pass.Finish() == RENDER_RESULT_INVALID_ARGUMENT);
    }
    CHECK(control.begins == 0 && control.ends == 0 && !control.target);
}

void SetFault(unsigned int fault)
{
    if (fault == 1) control.select = RENDER_RESULT_FAILED;
    if (fault == 2) control.begin = RENDER_RESULT_FAILED;
    if (fault == 3) control.clear = RENDER_RESULT_FAILED;
    if (fault == 4) control.renderError = true;
    if (fault == 5) control.renderThrows = true;
    if (fault == 6) control.flush = RENDER_RESULT_FAILED;
    if (fault == 7) control.restore = RENDER_RESULT_FAILED;
    if (fault == 8) control.completion = RENDER_RESULT_FAILED;
}

void TestWaterAndBothAdmissions()
{
    for (unsigned int title = 0; title < 2; ++title)
        for (unsigned int cull = 0; cull < 2; ++cull)
            for (unsigned int fault = 0; fault < 9; ++fault)
            {
                control = Control();
                SetFault(fault);
                CameraClass camera;
                const Matrix3D original = camera.Get_Transform();
                WaterRenderObjClass water;
                ViewMock view = { &camera };
                TheWaterRenderObj = &water;
                primaryW3DView = &view;
                ShaderClass::inverted = cull != 0;
                const bool admitted = title == 0 ? DisplayAdmissionGenerals() : DisplayAdmissionGeneralsMD();
                CHECK(admitted == (fault == 0));
                CHECK(control.visibleBegins == (fault == 0 ? 1U : 0U));
                CHECK(water.m_nativeReflectionReady == (fault == 0));
                CHECK(!control.frame);
                CHECK(control.ends == (fault == 1 || fault == 2 ? 0U : 1U));
                CHECK(SameMatrix(camera.Get_Transform(), original));
                CHECK(camera.minimum.X == .1f && camera.minimum.Y == .2f &&
                    camera.maximum.X == .8f && camera.maximum.Y == .9f);
                CHECK(ShaderClass::inverted == (cull != 0));
                if (fault != 7) CHECK(!control.target);
                if (fault == 0) CHECK(control.renders == 1 && control.sky == 1 && control.body == 1);
                // No sticky 'ready' resurrection: a new complete healthy pass
                // is required; the backend mock is reset, not output authority.
                control = Control();
                CHECK(water.updateRenderTargetTextures(&camera));
                CHECK(water.m_nativeReflectionReady && control.renders == 1 && control.ends == 1);
                CHECK(SameMatrix(camera.Get_Transform(), original));
                std::printf("WATER_PASS title=%u cull=%u fault=%u checked\n", title, cull, fault);
            }
    TheWaterRenderObj = 0;
    primaryW3DView = 0;
}

void TestWaterFirstErrorBlocksAdmissionAndSampling()
{
    for (unsigned int title = 0; title < 2; ++title)
    {
        control = Control();
        control.renderError = true;
        control.restore = RENDER_RESULT_DEVICE_REMOVED;
        control.completion = RENDER_RESULT_UNSUPPORTED;
        globalData = GlobalDataMock();
        ShaderClass::inverted = false;
        CameraClass camera;
        WaterRenderObjClass water;
        ViewMock view = { &camera };
        RenderInfoClass info = { camera };
        TheWaterRenderObj = &water;
        primaryW3DView = &view;

        const bool admitted = title == 0 ? DisplayAdmissionGenerals() :
            DisplayAdmissionGeneralsMD();
        CHECK(!admitted);
        CHECK(control.begins == 1 && control.clears == 1 && control.renders == 1 &&
            control.ends == 1 &&
            control.passedFirst == RENDER_RESULT_FAILED &&
            control.endResult == RENDER_RESULT_FAILED);
        CHECK(control.restores > 0 && control.restore == RENDER_RESULT_DEVICE_REMOVED &&
            control.completion == RENDER_RESULT_UNSUPPORTED && control.target);
        CHECK(!water.m_nativeReflectionReady && control.visibleBegins == 0 &&
            !control.frame);

        water.drawSea(info);
        CHECK(control.frameFailures == 1 && control.seaReachedSampling == 0 &&
            control.visibleBegins == 0 && !control.frame);
    }
    TheWaterRenderObj = 0;
    primaryW3DView = 0;
    globalData = GlobalDataMock();
}

void TestClosedTitleAdmissionGates()
{
    for (unsigned int title = 0; title < 2; ++title)
        for (unsigned int gate = 0; gate < 2; ++gate)
        {
            control = Control();
            globalData = GlobalDataMock();
            if (gate == 0)
                globalData.m_breakTheMovie = 1;
            else
                globalData.m_disableRender = true;
            ShaderClass::inverted = false;
            CameraClass camera;
            WaterRenderObjClass water;
            ViewMock view = { &camera };
            TheWaterRenderObj = &water;
            primaryW3DView = &view;

            const bool admitted = title == 0 ? DisplayAdmissionGenerals() :
                DisplayAdmissionGeneralsMD();
            CHECK(!admitted && water.m_nativeReflectionReady);
            CHECK(control.renders == 1 && control.ends == 1 &&
                control.passedFirst == RENDER_RESULT_OK &&
                control.endResult == RENDER_RESULT_OK);
            CHECK(control.visibleBegins == 0 && !control.frame);
        }
    TheWaterRenderObj = 0;
    primaryW3DView = 0;
    globalData = GlobalDataMock();
}
}

void TestSeaAdmissionAfterReacquire()
{
    for (unsigned int title = 0; title < 2; ++title)
    {
        control = Control();
        CameraClass camera;
        WaterRenderObjClass water; // Like ctor/reacquire: ready remains false.
        RenderInfoClass info = {camera};
        ViewMock view = {&camera};
        TheWaterRenderObj = &water;
        primaryW3DView = &view;
        control.visibleSea = false;
        CHECK(title == 0 ? DisplayAdmissionGenerals() : DisplayAdmissionGeneralsMD());
        water.drawSea(info);
        CHECK(!water.m_nativeReflectionReady && control.frameFailures == 0 &&
            control.seaReachedSampling == 0 && control.renders == 0);
        control.visibleSea = true;
        water.m_pReflectionTexture = 0;
        water.drawSea(info);
        CHECK(!water.m_nativeReflectionReady && control.frameFailures == 0 && control.seaReachedSampling == 0);
        water.m_pReflectionTexture = &water.color;
        water.m_vertexBuffer = 0;
        water.drawSea(info);
        CHECK(control.frameFailures == 0 && control.seaReachedSampling == 0);
        water.m_vertexBuffer = &water.vertex;
        water.drawSea(info);
        CHECK(!water.m_nativeReflectionReady && control.frameFailures == 1 && control.seaReachedSampling == 0);
        CHECK(water.updateRenderTargetTextures(&camera));
        water.drawSea(info);
        CHECK(water.m_nativeReflectionReady && control.frameFailures == 1 && control.seaReachedSampling == 1);
    }
    TheWaterRenderObj = 0;
    primaryW3DView = 0;
}

int main()
{
    TestStickyHeaderResult();
    TestWaterAndBothAdmissions();
    TestWaterFirstErrorBlocksAdmissionAndSampling();
    TestClosedTitleAdmissionGates();
    TestSeaAdmissionAfterReacquire();
    std::printf("actual water/header/admission control failures=%u\n", failures);
    return failures == 0 ? 0 : 1;
}
