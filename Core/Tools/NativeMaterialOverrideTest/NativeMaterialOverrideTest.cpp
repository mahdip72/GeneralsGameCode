// CPU-only mock environment. Product methods are included mechanically from
// tracked source; this file does not reimplement the override algorithm.
#include "Renderer/RendererDevice.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "contracts.inc"

struct Vector2 {
    union { float X; float U; }; union { float Y; float V; };
    Vector2(float x=0,float y=0):X(x),Y(y){}
};
struct Vector3 {
    float X,Y,Z; Vector3(float x=0,float y=0,float z=0):X(x),Y(y),Z(z){}
    Vector3 operator+(const Vector3 &v)const{return Vector3(X+v.X,Y+v.Y,Z+v.Z);}
};
struct Row4 { float X,Y,Z,W; };
class Matrix4x4 {
    Row4 rows[4];
public:
    Matrix4x4(){Make_Identity();}
    Row4 &operator[](int i){return rows[i];}
    const Row4 &operator[](int i)const{return rows[i];}
    void Make_Identity(){memset(rows,0,sizeof(rows));rows[0].X=rows[1].Y=rows[2].Z=rows[3].W=1;}
};
class Matrix3D : public Matrix4x4 {
public:
    explicit Matrix3D(bool = false){}
    void Get_Z_Vector(Vector3 *v)const{*v=Vector3(0,0,1);}
    void Get_Translation(Vector3 *v)const{*v=Vector3();}
    void Obj_Look_At(const Vector3 &,const Vector3 &,float){Make_Identity();}
};
namespace WWMath {
    float Floor(float x){return floorf(x);}
    float Clamp(float x,float low,float high){return x<low?low:(x>high?high:x);}
}
namespace WW3D {
    unsigned int syncTime=1000; bool sorting=true;
    unsigned int Get_Sync_Time(){return syncTime;}
    bool Is_Coloring_Enabled(){return false;}
    bool Is_Sorting_Enabled(){return sorting;}
}
class TextureClass {};
class LightEnvironmentClass {};
class TextureMapperClass {
public:
    enum { MAPPER_ID_LINEAR_OFFSET=1 };
    virtual ~TextureMapperClass(){}
    virtual int Mapper_ID()const=0;
    virtual void Apply(int)=0;
};
class ScaleTextureMapperClass : public TextureMapperClass {
public:
    Vector2 Scale; unsigned int Stage;
    ScaleTextureMapperClass():Scale(1,1),Stage(0){}
    void Apply(int);
#ifndef FIXTURE_GENERALS
    virtual void Calculate_Texture_Matrix(Matrix4x4 &)=0;
#endif
};
class LinearOffsetTextureMapperClass : public ScaleTextureMapperClass {
public:
    Vector2 CurrentUVOffset,UVOffsetDeltaPerMS;
    unsigned int LastUsedSyncTime; bool ClampFix; int mapperID;
    LinearOffsetTextureMapperClass():CurrentUVOffset(.1f,.2f),
        UVOffsetDeltaPerMS(.001f,.002f),LastUsedSyncTime(900),ClampFix(false),mapperID(MAPPER_ID_LINEAR_OFFSET){}
    int Mapper_ID()const{return mapperID;}
    void Set_LastUsedSyncTime(unsigned int t){LastUsedSyncTime=t;}
    unsigned int Get_LastUsedSyncTime(){return LastUsedSyncTime;}
    void Get_Current_UV_Offset(Vector2 &v){v=CurrentUVOffset;}
    void Set_Current_UV_Offset(const Vector2 &v){CurrentUVOffset=v;}
#ifdef FIXTURE_GENERALS
    void Apply(int);
#else
    void Calculate_Texture_Matrix(Matrix4x4 &);
#endif
};
class MeshBuilderClass { public: enum {MAX_STAGES=2}; };
class MeshMatDescClass { public: enum {MAX_TEX_STAGES=2}; };
class VertexMaterialClass {
public:
    rts::render::LegacyMaterialState Material; bool CRCDirty;
    TextureMapperClass *Mapper[2]; unsigned int UVSource[2];
    VertexMaterialClass():CRCDirty(false){Mapper[0]=Mapper[1]=0;UVSource[0]=1;UVSource[1]=1;
        Material.diffuse=rts::render::RenderFloat4(.2f,.4f,.6f,.8f);}
    const rts::render::LegacyMaterialState &Get_Renderer_Material_State()const{return Material;}
    bool Get_Lighting(){return true;}
    unsigned int Get_Ambient_Color_Source(){return rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;}
    unsigned int Get_Diffuse_Color_Source(){return rts::render::RENDER_MATERIAL_SOURCE_COLOR1;}
    unsigned int Get_Emissive_Color_Source(){return rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;}
    unsigned int Get_UV_Source(unsigned int i){return UVSource[i];}
    TextureMapperClass *Peek_Mapper(unsigned int i=0){return Mapper[i];}
    float Get_Opacity()const; void Set_Opacity(float);
    void Get_Diffuse(Vector3 *)const; void Set_Diffuse(float,float,float);
};
class ShaderClass {
public:
    enum {SRCBLEND_SRC_ALPHA=1,SRCBLEND_ZERO=2,DSTBLEND_ONE_MINUS_SRC_ALPHA=3,DSTBLEND_ZERO=4,DSTBLEND_SRC_COLOR=5};
    int source,dest; ShaderClass():source(0),dest(DSTBLEND_ZERO){}
    void Set_Src_Blend_Func(int v){source=v;}
    void Set_Dst_Blend_Func(int v){dest=v;}
    int Get_Dst_Blend_Func()const{return dest;}
};
class RenderObjClass {
public:
    enum {USER_DATA_MATERIAL_OVERRIDE=7};
    struct Material_Override {int type;Vector2 customUVOffset;
        Material_Override():type(USER_DATA_MATERIAL_OVERRIDE),customUVOffset(.3f,.7f){}};
};
class MeshGeometryClass {public:enum {SORT=1};};
class MeshModelClass : public MeshGeometryClass {
public:
    enum {ALIGNED=2,ORIENTED=4,SKIN=8};unsigned int flags;
    MeshModelClass():flags(0){} unsigned int Get_Flag(unsigned int f)const{return flags&f;}
};
struct Sphere {};
class MeshClass {
public:
    MeshModelClass model; float alpha,scale; bool additive,disabled;
    unsigned int base;void *user;Matrix3D transform;Sphere sphere;
    MeshClass():alpha(1),scale(1),additive(false),disabled(false),base(17),user(0){}
    unsigned int Get_Base_Vertex_Offset()const{return base;}
    MeshModelClass *Peek_Model(){return &model;}
    void *Get_User_Data(){return user;}
    float Get_Alpha_Override()const{return alpha;}
    bool Is_Additive()const{return additive;}
    float Get_ObjectScale()const{return scale;}
    bool Is_Transform_Identity()const{return true;}
    const Matrix3D &Get_Transform()const{return transform;}
    const Sphere &Get_Bounding_Sphere()const{return sphere;}
    LightEnvironmentClass *Get_Lighting_Environment(){return 0;}
    bool Is_Disabled_By_Debugger()const{return disabled;}
};
class Camera {Matrix3D transform;public:const Matrix3D &Get_Transform()const{return transform;}};
class MeshRenderer {Camera camera;public:Camera *Peek_Camera(){return &camera;}} TheDX8MeshRenderer;
class DX8RendererDebugger {public:static bool enabled;static bool Is_Enabled(){return enabled;}};
bool DX8RendererDebugger::enabled=false;
const unsigned int VERTEX_BUFFER_OVERFLOW=~0U;
bool m_gForceMultiply=false;
unsigned int failures=0,scopeDepth=0,scopeAdmissions=0;
#define CHECK(c) do{if(!(c)){++failures;fprintf(stderr,"line %u: %s\n",unsigned(__LINE__),#c);}}while(0)
#define WWASSERT(c) CHECK(c)
#define SNAPSHOT_SAY(x) ((void)0)

namespace rts {namespace render {
// Minimal command carrier and owner pin model, not an ABI/lifecycle test.
struct GameRenderCommand {GameRenderCommandType type;const void *input;size_t inputBytes;};
class IGameRenderClientNativeOwner {
public:
    bool operational,refuseCommand;unsigned int commands,failureCount;
    IGameRenderClientNativeOwner():operational(true),refuseCommand(false),commands(0),failureCount(0){}
    bool IsInitialized()const{return operational;} bool IsOperational()const{return operational;}
    void RecordGameFailure(RenderResult){++failureCount;}
    RenderResult ExecuteGameRenderCommand(const GameRenderCommand &);
} owner;
class NativeGameRenderOwnerScope {
public:
    NativeGameRenderOwnerScope(){++scopeDepth;++scopeAdmissions;}
    ~NativeGameRenderOwnerScope(){--scopeDepth;}
    IGameRenderClientNativeOwner *Get(){return &owner;}
};
void SetGameMaterial(const VertexMaterialClass *);
void SetGameTexture(unsigned int,TextureClass *){}
void SetGameLightEnvironment(LightEnvironmentClass *){}
void SetGameShader(const ShaderClass &){} // Pipeline-only; never refresh material.
void ApplyGameRenderStateChanges(){}     // Pipeline-only; never refresh material.
void SetGameRenderState(GameRenderState state,unsigned int value){
    LegacyPipelineState p;GetTrackedLegacyPipelineState(&p);
    if(state==GAME_RENDER_STATE_ALPHA_REFERENCE)p.alphaReference=value;
    if(state==GAME_RENDER_STATE_NORMALIZE_NORMALS)p.normalizeNormals=value!=0;
    TrackLegacyPipelineState(p);
}
void SetGameTextureStageState(unsigned int i,GameTextureStageState state,unsigned int value){
    LegacyPipelineState p;GetTrackedLegacyPipelineState(&p);
    if(state==GAME_TEXTURE_STAGE_COORDINATE_INDEX)p.textureStages[i].textureCoordinateIndex=value&255;
    if(state==GAME_TEXTURE_STAGE_TRANSFORM_FLAGS){p.textureStages[i].textureTransformEnable=value!=0;p.textureStages[i].textureTransformCount=value;}
    TrackLegacyPipelineState(p);
}
void SetGameTransform(GameRenderTransformSlot slot,const Matrix4x4 &m){
    float v[16];for(unsigned int r=0;r<4;++r){v[r]=m[r].X;v[4+r]=m[r].Y;v[8+r]=m[r].Z;v[12+r]=m[r].W;}
    CHECK(TrackLegacyTransform(static_cast<LegacyTransformSlot>(slot),v));
}
}}
#include "owner-material.inc"
#include "material-values.inc"
#include "mapper.inc"
#include "material-facade.inc"

struct CapturedDraw {rts::render::LegacyLogicalState state;bool sorted;};
std::vector<CapturedDraw> captured;
class DX8PolygonRendererClass {
public:
    void Capture(bool sorted){CapturedDraw draw;CHECK(rts::render::GetTrackedLegacyLogicalState(&draw.state));draw.sorted=sorted;captured.push_back(draw);}
    void Render(unsigned int){Capture(false);}
    void Render_Sorted(unsigned int,const Sphere &){Capture(true);}
};
class PolyRenderTaskClass {
public:
    DX8PolygonRendererClass *renderer;MeshClass *mesh;PolyRenderTaskClass *next;
    PolyRenderTaskClass(DX8PolygonRendererClass *r,MeshClass *m):renderer(r),mesh(m),next(0){}
    DX8PolygonRendererClass *Peek_Polygon_Renderer(){return renderer;}
    MeshClass *Peek_Mesh(){return mesh;}
    PolyRenderTaskClass *Get_Next_Visible(){return next;}
    void Set_Next_Visible(PolyRenderTaskClass *v){next=v;}
};
class DX8TextureCategoryClass {
public:
    VertexMaterialClass *material;ShaderClass shader;unsigned int pass,clears;PolyRenderTaskClass *render_task_head;
    explicit DX8TextureCategoryClass(VertexMaterialClass *m):material(m),pass(0),clears(0),render_task_head(0){}
    ~DX8TextureCategoryClass(){Clear_Render_List();}
    TextureClass *Peek_Texture(unsigned int){return 0;}
    const VertexMaterialClass *Peek_Material(){return material;}
    const ShaderClass &Get_Shader(){return shader;}
    void Add(DX8PolygonRendererClass *r,MeshClass *m){PolyRenderTaskClass **p=&render_task_head;while(*p)p=&(*p)->next;*p=new PolyRenderTaskClass(r,m);}
    void Clear_Render_List(){++clears;while(render_task_head){PolyRenderTaskClass *p=render_task_head;render_task_head=p->next;delete p;}}
    void Render();
};
#include "category.inc"

bool Near(float a,float b){return fabsf(a-b)<.00001f;}
void CheckMaterial(const CapturedDraw &d,float opacity,float x,float y,float z){
    CHECK(Near(d.state.constants.material.diffuse.w,opacity));
    CHECK(Near(d.state.constants.material.diffuse.x,x));
    CHECK(Near(d.state.constants.material.diffuse.y,y));
    CHECK(Near(d.state.constants.material.diffuse.z,z));
}
void CheckUV(const CapturedDraw &d,float u,float v){
    CHECK(Near(d.state.constants.textureTransforms[0].values[8],u));
    CHECK(Near(d.state.constants.textureTransforms[0].values[9],v));
    CHECK(d.state.pipeline.textureStages[0].textureTransformEnable);
    CHECK(d.state.pipeline.textureStages[0].textureTransformCount==2);
    CHECK(d.state.pipeline.textureStages[0].textureCoordinateIndex==1);
}
void TestInstances(bool additive,bool customUV,float alpha,bool sorting){
    using namespace rts::render;
    captured.clear();ResetTrackedLegacyState();WW3D::syncTime=1000;
    VertexMaterialClass material;LinearOffsetTextureMapperClass mapper;
    material.Mapper[0]=&mapper;
    MeshClass changed,ordinary,sorted;RenderObjClass::Material_Override overrideValue;
    changed.alpha=alpha;changed.additive=additive;
    if(customUV)changed.user=&overrideValue;
    sorted.model.flags=MeshGeometryClass::SORT;WW3D::sorting=sorting;
    DX8PolygonRendererClass renderer;DX8TextureCategoryClass category(&material);
    category.Add(&renderer,&changed);category.Add(&renderer,&ordinary);category.Add(&renderer,&sorted);
    category.Render();CHECK(captured.size()==3);CHECK(category.render_task_head==0);
    if(captured.size()==3){
        CheckMaterial(captured[0],alpha==1?.8f:alpha,additive&&alpha!=1?alpha:.2f,additive&&alpha!=1?alpha:.4f,additive&&alpha!=1?alpha:.6f);
        CheckUV(captured[0],customUV?.3f:.2f,customUV?.7f:.4f);
        CheckMaterial(captured[1],.8f,.2f,.4f,.6f);CheckUV(captured[1],.2f,.4f);
        CheckMaterial(captured[2],.8f,.2f,.4f,.6f);CheckUV(captured[2],.2f,.4f);
        CHECK(captured[2].sorted==sorting);CHECK(!captured[0].sorted && !captured[1].sorted);
        // Simulate later material/mapper/tracked-state changes: accepted draw
        // snapshots must remain immutable for both immediate and sorted lanes.
        material.Set_Opacity(.11f);material.Set_Diffuse(.9f,.8f,.7f);
        mapper.Set_Current_UV_Offset(Vector2(.88f,.99f));SetGameMaterial(&material);
        CheckMaterial(captured[0],alpha==1?.8f:alpha,additive&&alpha!=1?alpha:.2f,additive&&alpha!=1?alpha:.4f,additive&&alpha!=1?alpha:.6f);
        CheckUV(captured[0],customUV?.3f:.2f,customUV?.7f:.4f);
        CheckMaterial(captured[2],.8f,.2f,.4f,.6f);CheckUV(captured[2],.2f,.4f);
    }
    // Restoration before the deliberate later mutation above is also observed
    // by both following meshes; mapper sync must not accumulate custom delta.
    CHECK(mapper.Get_LastUsedSyncTime()==1000);CHECK(scopeDepth==0);
}
void TestNonLinearAndRefusal(){
    using namespace rts::render;
    captured.clear();ResetTrackedLegacyState();VertexMaterialClass material;
    LinearOffsetTextureMapperClass mapper;mapper.mapperID=99;material.Mapper[0]=&mapper;
    MeshClass mesh;RenderObjClass::Material_Override data;mesh.user=&data;
    DX8PolygonRendererClass renderer;DX8TextureCategoryClass category(&material);
    category.Add(&renderer,&mesh);category.Render();CHECK(captured.size()==1);
    if(!captured.empty()){CheckMaterial(captured[0],.8f,.2f,.4f,.6f);CheckUV(captured[0],.2f,.4f);}
    const unsigned int commands=owner.commands;owner.operational=false;SetGameMaterial(&material);
    CHECK(owner.commands==commands);CHECK(scopeDepth==0);owner.operational=true;
    mapper.Set_LastUsedSyncTime(123);owner.refuseCommand=true;SetGameMaterial(&material);
    CHECK(mapper.Get_LastUsedSyncTime()==123);CHECK(owner.failureCount>0);CHECK(scopeDepth==0);owner.refuseCommand=false;
}
void TestPendingAndDisabled(){
    captured.clear();VertexMaterialClass material;MeshClass mesh;mesh.alpha=.2f;
    DX8PolygonRendererClass renderer;
    {DX8TextureCategoryClass category(&material);mesh.base=VERTEX_BUFFER_OVERFLOW;
        category.Add(&renderer,&mesh);category.Render();CHECK(captured.empty());CHECK(category.render_task_head!=0);CHECK(category.clears==0);}
    {DX8TextureCategoryClass category(&material);mesh.base=17;mesh.disabled=true;DX8RendererDebugger::enabled=true;
        category.Add(&renderer,&mesh);category.Render();CHECK(captured.empty());CHECK(category.render_task_head==0);DX8RendererDebugger::enabled=false;}
    CHECK(Near(material.Get_Opacity(),.8f));CHECK(scopeDepth==0);
}
int main(){
    TestInstances(false,false,.25f,true);TestInstances(true,false,.4f,true);
    TestInstances(false,true,1,true);TestInstances(false,true,.5f,true);
    TestInstances(true,true,.6f,true);TestInstances(false,true,1,false);
    TestNonLinearAndRefusal();TestPendingAndDisabled();
    printf("failures=%u captured_owner_scopes=%u balanced=%u\n",failures,scopeAdmissions,scopeDepth);
    return failures?1:0;
}
