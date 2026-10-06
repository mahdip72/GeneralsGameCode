// Actual shared mesh-category Render/Add/Clear and pooled task lifetime code are
// mechanically included below. Geometry/material/renderer interfaces are doubles.
#include <cassert>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstddef>

static unsigned failures=0, flushCalls=0, failAt=0, retains=0, releases=0;
static bool stickyFailure=false;
static std::vector<int> rendered;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr,"line %u: %s\n",unsigned(__LINE__),#x); } } while(0)
#define WWASSERT(x) assert(x)
#define SNAPSHOT_SAY(x) do {} while(0)
#define VERTEX_BUFFER_OVERFLOW 0xffffffffU

template<class T,int Count> class AutoPoolClass {};
struct Vector2 { float X=0,Y=0; };
struct Vector3 { float X=0,Y=0,Z=0; Vector3 operator+(const Vector3&)const{return *this;} };
struct Matrix3D {
    void Make_Identity(){} void Get_Translation(Vector3*)const{} void Get_Z_Vector(Vector3*)const{}
    void Obj_Look_At(const Vector3&,const Vector3&,float){}
};
struct CameraClass { Matrix3D transform; const Matrix3D& Get_Transform()const{return transform;} };
struct MeshRendererDouble { CameraClass camera; CameraClass* Peek_Camera(){return &camera;} } TheDX8MeshRenderer;
struct LightEnvironmentClass {};
struct TextureClass {};
struct TextureMapperClass { enum { MAPPER_ID_LINEAR_OFFSET=1 }; int Mapper_ID()const{return 1;} };
struct LinearOffsetTextureMapperClass:TextureMapperClass {
    unsigned Get_LastUsedSyncTime()const{return 0;} void Set_LastUsedSyncTime(unsigned){}
    void Get_Current_UV_Offset(Vector2&)const{} void Set_Current_UV_Offset(const Vector2&){}
};
struct VertexMaterialClass {
    float opacity=1; Vector3 diffuse; TextureMapperClass* Peek_Mapper(unsigned=0)const{return nullptr;}
    float Get_Opacity()const{return opacity;} void Set_Opacity(float v){opacity=v;}
    void Get_Diffuse(Vector3* p)const{*p=diffuse;} void Set_Diffuse(float,float,float){}
};
struct ShaderClass {
    enum { SRCBLEND_ONE=1,SRCBLEND_SRC_ALPHA=2,SRCBLEND_ZERO=0,
        DSTBLEND_ZERO=0,DSTBLEND_ONE_MINUS_SRC_ALPHA=2,DSTBLEND_SRC_COLOR=3,ALPHATEST_DISABLE=0 };
    unsigned Get_Bits()const{return 0;} int Get_Dst_Blend_Func()const{return DSTBLEND_ZERO;}
    int Get_Src_Blend_Func()const{return SRCBLEND_ONE;} int Get_Alpha_Test()const{return ALPHATEST_DISABLE;}
    void Set_Src_Blend_Func(int){} void Set_Dst_Blend_Func(int){}
};
struct MeshMatDescClass { enum {MAX_TEX_STAGES=2}; };
struct MeshGeometryClass { enum {SORT=8}; };
struct MeshModelClass {
    enum { SKIN=1,ALIGNED=2,ORIENTED=4 };
    unsigned flags=0,passes=1; unsigned Get_Pass_Count()const{return passes;}
    bool Get_Flag(unsigned f)const{return (flags&f)!=0;}
};
struct RenderObjClass {
    enum {USER_DATA_MATERIAL_OVERRIDE=1};
    struct Material_Override { int tag=1; Vector2 customUVOffset; };
};
struct MeshClass {
    MeshModelClass model; Matrix3D transform; unsigned offset=0; int refs=1,id;
    float scale=1,alpha=1; void* user=nullptr;
    explicit MeshClass(int value):id(value){}
    void Add_Ref(){++refs;++retains;} void Release_Ref(){CHECK(refs>1);--refs;++releases;}
    MeshModelClass* Peek_Model(){return &model;} float Get_ObjectScale()const{return scale;}
    float Get_Alpha_Override()const{return alpha;} void* Get_User_Data()const{return user;}
    unsigned Get_Base_Vertex_Offset()const{return offset;} const char* Get_Name()const{return "fixture";}
    LightEnvironmentClass* Get_Lighting_Environment()const{return nullptr;}
    const Matrix3D& Get_Transform()const{return transform;} bool Is_Transform_Identity()const{return true;}
    int Get_Bounding_Sphere()const{return 0;} bool Is_Disabled_By_Debugger()const{return false;}
    bool Is_Additive()const{return false;}
};
struct DX8RendererDebugger { static bool Is_Enabled(){return false;} };
namespace WW3D { bool Is_Sorting_Enabled(){return true;} unsigned Get_Sync_Time(){return 0;} }
struct DX8PolygonRendererClass {
    int id; explicit DX8PolygonRendererClass(int v):id(v){}
    void Render(unsigned){rendered.push_back(id);} void Render_Sorted(unsigned,int){rendered.push_back(id);}
};
class DX8TextureCategoryClass;
struct CategoryList {
    std::vector<DX8TextureCategoryClass*> entries;
    void Add(DX8TextureCategoryClass* p){if(std::find(entries.begin(),entries.end(),p)==entries.end())entries.push_back(p);}
    DX8TextureCategoryClass* Remove_Head(){if(entries.empty())return nullptr;auto* p=entries.front();entries.erase(entries.begin());return p;}
};
struct DX8FVFCategoryContainer {
    unsigned passes=1; bool AnythingToRender=false;
    CategoryList visible_texture_category_list[2];
    void Add_Visible_Texture_Category(DX8TextureCategoryClass* p,unsigned pass){AnythingToRender=true;visible_texture_category_list[pass].Add(p);}
};
struct DX8RigidFVFCategoryContainer:DX8FVFCategoryContainer {
    void* vertex_buffer=nullptr; void* index_buffer=nullptr;
    bool Anything_To_Render()const{return AnythingToRender;}
    void Render_Procedural_Material_Passes(){} void Render();
};
namespace rts { namespace render {
    enum RenderResult {RENDER_RESULT_OK,RENDER_RESULT_FAILED};
    enum {GAME_RENDER_STATE_SOURCE_BLEND,GAME_RENDER_STATE_NORMALIZE_NORMALS,GAME_RENDER_STATE_ALPHA_REFERENCE,
        RENDER_BLEND_DESTINATION_COLOR,GAME_TRANSFORM_WORLD};
    RenderResult FlushGameRigidDraws(){++flushCalls;if(failAt&&flushCalls==failAt)stickyFailure=true;return stickyFailure?RENDER_RESULT_FAILED:RENDER_RESULT_OK;}
    void SetGameTexture(unsigned,TextureClass*){} void SetGameMaterial(VertexMaterialClass*){}
    void SetGameShader(const ShaderClass&){} void ApplyGameRenderStateChanges(){}
    void SetGameRenderState(int,unsigned){} void SetGameTransform(int,const Matrix3D&){}
    void SetGameLightEnvironment(LightEnvironmentClass*){} void SetGameVertexBuffer(void*){} void SetGameIndexBuffer(void*,int){}
    struct GameRigidDrawHintScope {GameRigidDrawHintScope(bool,const void*,const void*){}};
} }
#include "mesh-task-class.inc"
class DX8TextureCategoryClass {
public:
    static bool m_gForceMultiply;
    PolyRenderTaskClass* render_task_head=nullptr;
    DX8FVFCategoryContainer* container; unsigned pass=0;
    VertexMaterialClass material; ShaderClass shader;
    explicit DX8TextureCategoryClass(DX8FVFCategoryContainer& c):container(&c){}
    TextureClass* Peek_Texture(unsigned)const{return nullptr;}
    VertexMaterialClass* Peek_Material()const{return const_cast<VertexMaterialClass*>(&material);}
    ShaderClass Get_Shader()const{return shader;}
    void Render(); void Clear_Render_List(); void Add_Render_Task(DX8PolygonRendererClass*,MeshClass*);
    std::vector<int> Pending()const {std::vector<int> ids;for(auto* p=render_task_head;p;p=p->Get_Next_Visible())ids.push_back(p->Peek_Mesh()->id);return ids;}
};
bool DX8TextureCategoryClass::m_gForceMultiply=false;
#include "mesh-task-methods.inc"

static void Reset(unsigned fail=0){flushCalls=0;failAt=fail;stickyFailure=false;retains=releases=0;rendered.clear();}
static void Queue(DX8TextureCategoryClass& category,MeshClass& mesh,DX8PolygonRendererClass& polygon){category.Add_Render_Task(&polygon,&mesh);}
static void EntryFailureAndRecovery(){
    Reset(1);DX8RigidFVFCategoryContainer c;DX8TextureCategoryClass cat(c);
    MeshClass old1(1),old2(2),fresh(3);DX8PolygonRendererClass p1(1),p2(2),p3(3);
    Queue(cat,old2,p2);Queue(cat,old1,p1);c.Render();
    CHECK(cat.Pending().empty());CHECK(rendered.empty());CHECK(old1.refs==1&&old2.refs==1);CHECK(retains==releases);
    CHECK(c.visible_texture_category_list[0].entries.empty());
    stickyFailure=false;failAt=0;Queue(cat,fresh,p3);c.Render();
    CHECK(rendered==std::vector<int>{3});CHECK(cat.Pending().empty());CHECK(fresh.refs==1);CHECK(retains==releases);
    cat.Clear_Render_List();CHECK(retains==releases);
}
static void MidFailureCancelsOverflowPrefix(){
    Reset(3);DX8RigidFVFCategoryContainer c;DX8TextureCategoryClass cat(c);
    MeshClass overflow(10),ready(11),ordinary(12),later(13);overflow.offset=VERTEX_BUFFER_OVERFLOW;ordinary.model.passes=2;
    DX8PolygonRendererClass p0(10),p1(11),p2(12),p3(13);
    Queue(cat,later,p3);Queue(cat,ordinary,p2);Queue(cat,ready,p1);Queue(cat,overflow,p0);c.Render();
    CHECK(rendered==std::vector<int>{11});CHECK(cat.Pending().empty());CHECK(retains==releases);
    CHECK(overflow.refs==1&&ready.refs==1&&ordinary.refs==1&&later.refs==1);
    stickyFailure=false;failAt=0;c.AnythingToRender=true;c.visible_texture_category_list[0].Add(&cat);c.Render();
    CHECK(rendered==std::vector<int>{11});CHECK(cat.Pending().empty());
}
static void FinalFailureCancelsOverflowAndOtherCategory(){
    Reset(3);DX8RigidFVFCategoryContainer c;DX8TextureCategoryClass first(c),second(c);
    MeshClass overflow(20),ready(21),next(22);overflow.offset=VERTEX_BUFFER_OVERFLOW;
    DX8PolygonRendererClass po(20),pr(21),pn(22);
    Queue(first,ready,pr);Queue(first,overflow,po);Queue(second,next,pn);c.Render();
    CHECK(flushCalls==4);CHECK(rendered==std::vector<int>{21});CHECK(first.Pending().empty()&&second.Pending().empty());
    CHECK(overflow.refs==1&&ready.refs==1&&next.refs==1);CHECK(retains==releases);
    CHECK(c.visible_texture_category_list[0].entries.empty());
}
static void HealthyOverflowPreservesOrder(){
    Reset();DX8RigidFVFCategoryContainer c;DX8TextureCategoryClass cat(c);
    MeshClass a(30),b(31),d(32),e(33);a.offset=d.offset=VERTEX_BUFFER_OVERFLOW;
    DX8PolygonRendererClass pa(30),pb(31),pd(32),pe(33);
    Queue(cat,e,pe);Queue(cat,d,pd);Queue(cat,b,pb);Queue(cat,a,pa);c.Render();
    CHECK(rendered==(std::vector<int>{31,33}));CHECK(cat.Pending()==(std::vector<int>{30,32}));
    CHECK(a.refs==2&&d.refs==2&&b.refs==1&&e.refs==1);CHECK(retains==4&&releases==2);
    a.offset=d.offset=0;c.AnythingToRender=true;c.visible_texture_category_list[0].Add(&cat);c.Render();
    CHECK(rendered==(std::vector<int>{31,33,30,32}));CHECK(cat.Pending().empty());CHECK(retains==releases);
    CHECK(a.refs==1&&d.refs==1);cat.Clear_Render_List();CHECK(retains==releases);
}
static void HealthyAllOverflowAndEmpty(){
    Reset();DX8RigidFVFCategoryContainer c;DX8TextureCategoryClass cat(c);
    MeshClass a(40),b(41);a.offset=b.offset=VERTEX_BUFFER_OVERFLOW;DX8PolygonRendererClass pa(40),pb(41);
    Queue(cat,b,pb);Queue(cat,a,pa);c.Render();CHECK(rendered.empty());CHECK(cat.Pending()==(std::vector<int>{40,41}));
    CHECK(releases==0&&a.refs==2&&b.refs==2);
    a.offset=b.offset=0;c.AnythingToRender=true;c.visible_texture_category_list[0].Add(&cat);c.Render();
    CHECK(rendered==(std::vector<int>{40,41}));CHECK(cat.Pending().empty());CHECK(retains==releases);
    cat.Render();cat.Clear_Render_List();CHECK(retains==releases);
}
int main(){
    EntryFailureAndRecovery();MidFailureCancelsOverflowPrefix();FinalFailureCancelsOverflowAndOtherCategory();
    HealthyOverflowPreservesOrder();HealthyAllOverflowAndEmpty();
    if(failures){std::fprintf(stderr,"FAIL mesh task lifetime: %u checks\n",failures);return 1;}
    std::puts("PASS actual category failure cleanup, mesh refs, stale visibility recovery, and healthy overflow order");return 0;
}
