// CPU seam: mechanically included production geometry/facade/handler and the
// linked actual sorter. Title objects and owner admission are explicitly mocked.
#include "Utility/CppMacros.h"
#include "nativew3dsorting.h"
#include "Renderer/LegacyColorPacking.h"
#include "Lib/JobSystem.h"
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <climits>
#include <vector>
#include <algorithm>

using namespace rts::render;
static unsigned int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr,"line %u: %s\n",unsigned(__LINE__),#x); } } while (0)
#define WWASSERT(x) assert(x)
#define W3DNEWARRAY new
#define REF_PTR_RELEASE(p) do { if (p) { (p)->Release_Ref(); (p)=0; } } while (0)
static const float WWMATH_PI = 3.14159265358979323846f;

struct Vector3 {
    float X,Y,Z;
    Vector3(float x=0,float y=0,float z=0):X(x),Y(y),Z(z){}
    void Set(const Vector3 &v){*this=v;}
    Vector3 operator+(const Vector3 &v)const{return Vector3(X+v.X,Y+v.Y,Z+v.Z);}
};
static Vector3 operator*(float s,const Vector3 &v){return Vector3(s*v.X,s*v.Y,s*v.Z);}
struct Vector4 { float X,Y,Z,W; Vector4(float x=0,float y=0,float z=0,float w=0):X(x),Y(y),Z(z),W(w){} };
class Matrix4x4 {
public:
    float m[4][4];
    explicit Matrix4x4(bool=true){std::memset(m,0,sizeof(m));for(int i=0;i<4;++i)m[i][i]=1;}
    float *operator[](unsigned int r){return m[r];}
    const float *operator[](unsigned int r)const{return m[r];}
};
class Matrix3D : public Matrix4x4 {
public:
    void Set_Translation(const Vector3 &v){m[0][3]=v.X;m[1][3]=v.Y;m[2][3]=v.Z;}
    void Get_Orthogonal_Inverse(Matrix3D &out)const {
        Matrix3D copy=*this;
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)out.m[r][c]=copy.m[c][r];
        out.Set_Translation(Vector3());
    }
    static void Transform_Vector(const Matrix3D &m,const Vector3 &v,Vector3 *out) {
        *out=Vector3(m[0][0]*v.X+m[0][1]*v.Y+m[0][2]*v.Z+m[0][3],
            m[1][0]*v.X+m[1][1]*v.Y+m[1][2]*v.Z+m[1][3],
            m[2][0]*v.X+m[2][1]*v.Y+m[2][2]*v.Z+m[2][3]);
    }
};
namespace WWMath { static float Cos(float x){return std::cos(x);} static float Sin(float x){return std::sin(x);} }
namespace WW3D { static bool sorting=true; bool Is_Sorting_Enabled(){return sorting;} }
class CameraClass { public: Matrix3D transform; const Matrix3D &Get_Transform()const{return transform;} };
class RenderInfoClass { public: CameraClass &Camera; explicit RenderInfoClass(CameraClass &c):Camera(c){} };
template<class T> class ShareBufferClass {
public:
    std::vector<T> values;
    const T *Get_Array()const{return values.data();}
    const T &Get_Element(int i)const{return values.at(static_cast<size_t>(i));}
    void Release_Ref(){}
};
class TextureBaseClass { public: GpuHandle handle; TextureBaseClass():handle(17,2){} void Release_Ref(){} };
class TextureClass : public TextureBaseClass {};
class VertexMaterialClass {
public:
    enum { PRELIT_DIFFUSE=1 };
    LegacyMaterialState material;
    VertexMaterialClass(){material.diffuse=RenderFloat4(.2f,.4f,.6f,.7f);}
    static VertexMaterialClass *Get_Preset(int){return new VertexMaterialClass;}
    void Release_Ref(){delete this;}
};
class ShaderClass {
public:
    enum { CULL_MODE_ENABLE=1, GRADIENT_MODULATE=2, GRADIENT_DISABLE=3,
        TEXTURING_ENABLE=4, TEXTURING_DISABLE=5, DSTBLEND_ZERO=0, ALPHATEST_DISABLE=0 };
    static const ShaderClass _PresetAdditiveSpriteShader;
    int destination,alphaTest; unsigned int bits;
    ShaderClass():destination(1),alphaTest(0),bits(123){}
    void Set_Cull_Mode(int){}
    void Set_Primary_Gradient(int){}
    void Set_Texturing(int){}
    int Get_Dst_Blend_Func()const{return destination;}
    int Get_Alpha_Test()const{return alphaTest;}
};
const ShaderClass ShaderClass::_PresetAdditiveSpriteShader;

struct Triangle {
    LegacyLogicalState state;
    GpuHandle texture;
    std::vector<unsigned char> vertices;
};
static bool SameTriangle(const Triangle &a,const Triangle &b) {
    return a.vertices==b.vertices && a.texture==b.texture &&
        a.state.pipeline.shaderBits==b.state.pipeline.shaderBits &&
        a.state.texturePresenceMask==b.state.texturePresenceMask &&
        a.state.constants.material.diffuse.x==b.state.constants.material.diffuse.x &&
        a.state.constants.material.diffuse.y==b.state.constants.material.diffuse.y &&
        a.state.constants.material.diffuse.z==b.state.constants.material.diffuse.z &&
        a.state.constants.material.diffuse.w==b.state.constants.material.diffuse.w &&
        a.state.constants.lights[0].diffuse.x==b.state.constants.lights[0].diffuse.x &&
        a.state.constants.fog.density==b.state.constants.fog.density &&
        std::memcmp(a.state.constants.world.values,b.state.constants.world.values,sizeof(RenderMatrix4))==0 &&
        std::memcmp(a.state.constants.view.values,b.state.constants.view.values,sizeof(RenderMatrix4))==0;
}
class Sink : public NativeSortedGeometrySink {
public:
    unsigned int failCall=0,acceptedOnFailure=0,calls=0;
    bool retainStateTriangles=true;
    unsigned int acceptedTriangleCount=0;
    std::vector<unsigned char> acceptedGeometryBytes;
    std::vector<Triangle> triangles;
    RenderResult SubmitNativeSortedBatch(const NativeSortedDraw *draws,unsigned int count,
        const void *vertices,size_t vertexBytes,const void *indices,size_t indexBytes,unsigned int *accepted)override {
        ++calls;*accepted=0;
        const unsigned int limit=failCall==calls?std::min(count,acceptedOnFailure):count;
        for(unsigned int d=0;d<limit;++d) {
            const NativeDrawPacket &p=draws[d].packet;
            CHECK(p.indexCount%3==0 && p.vertexStride!=0);
            CHECK(size_t(p.startIndex)+p.indexCount<=indexBytes/sizeof(unsigned short));
            for(unsigned int t=0;t<p.indexCount;t+=3) {
                ++acceptedTriangleCount;
                Triangle out;out.state=draws[d].state;out.texture=p.textures[0];
                for(unsigned int corner=0;corner<3;++corner) {
                    unsigned short index=0;
                    std::memcpy(&index,static_cast<const unsigned char *>(indices)+(p.startIndex+t+corner)*sizeof(index),sizeof(index));
                    size_t offset=size_t(p.vertexOffset)+size_t(index)*p.vertexStride;
                    CHECK(offset<=vertexBytes && p.vertexStride<=vertexBytes-offset);
                    if(offset>vertexBytes || p.vertexStride>vertexBytes-offset)return RENDER_RESULT_INVALID_ARGUMENT;
                    const unsigned char *v=static_cast<const unsigned char *>(vertices)+offset;
                    if(retainStateTriangles)out.vertices.insert(out.vertices.end(),v,v+p.vertexStride);
                    else acceptedGeometryBytes.insert(acceptedGeometryBytes.end(),v,v+p.vertexStride);
                }
                if(retainStateTriangles)triangles.push_back(out);
            }
            ++*accepted;
        }
        return failCall==calls?RENDER_RESULT_FAILED:RENDER_RESULT_OK;
    }
};
class FixtureOwner : public IGameRenderClientNativeOwner {
public:
    bool operational=true,correctThread=true;
    unsigned int commands=0,queued=0,failureCount=0;
    RenderResult lastFailure=RENDER_RESULT_OK,queueResult=RENDER_RESULT_OK;
    GpuHandle m_gameTextures[LEGACY_TEXTURE_STAGE_COUNT];
    NativeSortingRenderer sorter;
    std::vector<Triangle> immediate;
    bool IsInitialized()const override{return true;}
    bool IsOperational()const override{return operational;}
    GameRenderTargetKind ActiveRenderTargetKind()const override{return GAME_RENDER_TARGET_BACK_BUFFER;}
    void RecordGameFailure(RenderResult r)override{lastFailure=r;++failureCount;}
    RenderResult ExecuteSortedCase(const GameRenderCommand &command);
    RenderResult QueueGameSortedTriangles(const LegacyLogicalState &state,const NativeDrawPacket &packet,
        const void *vertices,size_t vertexBytes,const void *indices,size_t indexBytes,const GameBoundingSphere *sphere)override {
        CHECK(sphere==0);CHECK(!packet.vertexBuffer.isValid() && !packet.indexBuffer.isValid());
        CHECK(packet.vertexOffset==0 && packet.indexOffset==0 && packet.minimumVertexIndex==0 && packet.startIndex==0 && packet.baseVertex==0);
        if(queueResult!=RENDER_RESULT_OK)return queueResult;
        RenderResult r=sorter.Queue(state,packet,vertices,vertexBytes,indices,indexBytes,sphere);
        if(r==RENDER_RESULT_OK)++queued;return r;
    }
    RenderResult ExecuteGameRenderCommand(const GameRenderCommand &command)override {
        ++commands;
        if(!operational || !correctThread){RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);return RENDER_RESULT_INVALID_ARGUMENT;}
        if(command.type==GAME_RENDER_COMMAND_SET_TRANSFORM) {
            const RenderMatrix4 &m=*static_cast<const RenderMatrix4 *>(command.input);
            return TrackLegacyTransform(static_cast<LegacyTransformSlot>(command.value0),m.values)?RENDER_RESULT_OK:RENDER_RESULT_INVALID_ARGUMENT;
        }
        if(command.type==GAME_RENDER_COMMAND_GET_TRANSFORM) {
            return GetTrackedLegacyTransform(static_cast<LegacyTransformSlot>(command.value0),
                static_cast<RenderMatrix4 *>(command.output))?RENDER_RESULT_OK:RENDER_RESULT_INVALID_ARGUMENT;
        }
        if(command.type==GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP) {
            CHECK(command.value0==GAME_PRIMITIVE_TRIANGLE_LIST);
            LegacyLogicalState state;CHECK(GetTrackedLegacyLogicalState(&state));
            const unsigned char *v=static_cast<const unsigned char *>(command.input);
            for(unsigned int t=0;t<command.value1;++t) {
                Triangle out;out.state=state;out.texture=m_gameTextures[0];
                out.vertices.assign(v+size_t(t)*3*command.value2,v+size_t(t+1)*3*command.value2);
                immediate.push_back(out);
            }
            return RENDER_RESULT_OK;
        }
        return ExecuteSortedCase(command);
    }
};
static FixtureOwner *currentOwner=0;
static unsigned int scopeDepth=0;
namespace rts { namespace render {
NativeGameRenderOwnerScope::NativeGameRenderOwnerScope():m_owner(currentOwner),m_locked(true){++scopeDepth;}
NativeGameRenderOwnerScope::~NativeGameRenderOwnerScope(){--scopeDepth;}
IGameRenderClientNativeOwner *NativeGameRenderOwnerScope::Get()const{return m_owner;}
void SetGameMaterial(const VertexMaterialClass *material){TrackLegacyMaterial(material->material);}
void SetGameShader(const ShaderClass &shader){TrackLegacyShaderBits(shader.bits);}
void SetGameTexture(unsigned int stage,TextureBaseClass *texture){
    CHECK(currentOwner!=0);currentOwner->m_gameTextures[stage]=texture?texture->handle:GpuHandle();
    TrackLegacyTexturePresence(stage,texture!=0);
}
} }
namespace {
#include "command-helpers.inc"
}
#include "owner-sorted.inc"
namespace rts { namespace render {
#include "command-facade.inc"
} }
#include "linegroup.inc"

class Group : public LineGroupClass {
public:
    void Configure(ShareBufferClass<Vector3> *start,ShareBufferClass<Vector3> *end,
        ShareBufferClass<Vector4> *diffuse,ShareBufferClass<Vector4> *tail,
        ShareBufferClass<unsigned int> *alt,ShareBufferClass<float> *sizes,ShareBufferClass<float> *u,
        TextureClass *texture,bool prism,bool transform) {
        StartLineLoc=start;EndLineLoc=end;LineDiffuse=diffuse;TailDiffuse=tail;ALT=alt;
        LineSize=sizes;LineUCoord=u;Texture=texture;LineCount=static_cast<int>(start->values.size());
        Flags=transform?1:0;LineMode=prism?PRISM:TETRAHEDRON;
    }
    void MakeOpaque(){Shader.destination=ShaderClass::DSTBLEND_ZERO;}
    void EnableAlphaTest(){Shader.alphaTest=1;}
    void OverrideCount(int n){LineCount=n;}
};
struct Input {
    ShareBufferClass<Vector3> start,end;
    ShareBufferClass<Vector4> diffuse,tail;
    ShareBufferClass<unsigned int> alt;
    ShareBufferClass<float> size,u;
    TextureClass texture;
    Input(){start.values={Vector3(1,2,2),Vector3(3,4,8)};end.values={Vector3(1,2,5),Vector3(3,4,10)};
        diffuse.values={Vector4(.2f,.3f,.4f,.5f),Vector4(.6f,.7f,.8f,.9f)};
        tail.values={Vector4(.1f,.2f,.3f,.4f),Vector4(.4f,.3f,.2f,.1f)};
        alt.values={1,0};size.values={.25f,.5f};u.values={.2f,.8f};}
    void Configure(Group &g,bool prism,bool transform){g.Configure(&start,&end,&diffuse,&tail,&alt,&size,&u,&texture,prism,transform);}
};
static void Initialize(FixtureOwner &owner,CameraClass &camera) {
    currentOwner=&owner;ResetTrackedLegacyState();LegacyPipelineState pipeline;
    pipeline.shaderBits=77;TrackLegacyPipelineState(pipeline);
    LegacyLightState light;light.enabled=true;light.diffuse.x=.35f;CHECK(TrackLegacyLight(0,light));
    LegacyFogConstants fog;fog.density=.125f;TrackLegacyFog(fog);
    camera.transform[0][0]=0;camera.transform[0][1]=-1;
    camera.transform[1][0]=1;camera.transform[1][1]=0;
    Matrix4x4 view;view[2][0]=.25f;view[2][1]=.5f;view[2][3]=3;
    SetGameTransform(GAME_TRANSFORM_VIEW,view);
}
static void QueueReference(NativeSortingRenderer &sorter,const std::vector<Triangle> &triangles) {
    for(const Triangle &triangle:triangles) {
        NativeDrawPacket p;p.vertexStride=sizeof(LineGroupVertex);p.vertexCount=3;p.indexCount=3;
        p.indexed=true;p.indexFormat=RENDER_FORMAT_R16_UINT;p.topology=RENDER_PRIMITIVE_TRIANGLE_LIST;
        p.vertexFormat=RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;p.texturePresenceMask=triangle.state.texturePresenceMask;
        p.textures[0]=triangle.texture;CHECK(DecodeLegacyFvfVertexLayout(GAME_VERTEX_XYZNDUV2,p.vertexStride,&p.vertexLayout));
        const unsigned short indices[3]={0,1,2};
        CHECK(sorter.Queue(triangle.state,p,triangle.vertices.data(),triangle.vertices.size(),indices,sizeof(indices),0)==RENDER_RESULT_OK);
    }
}
static bool SameStream(const std::vector<Triangle> &a,const std::vector<Triangle> &b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(!SameTriangle(a[i],b[i]))return false;
    return true;
}
static void PrintMismatchTriangle(const char *label,const Triangle &t) {
    const RenderFloat4 &m=t.state.constants.material.diffuse;
    std::fprintf(stderr,"%s shader=%u mask=%u texture=%u:%u material=%.9g,%.9g,%.9g,%.9g light=%.9g fog=%.9g bytes=%zu\n",
        label,t.state.pipeline.shaderBits,t.state.texturePresenceMask,t.texture.index(),t.texture.generation(),
        m.x,m.y,m.z,m.w,t.state.constants.lights[0].diffuse.x,t.state.constants.fog.density,t.vertices.size());
    std::fprintf(stderr,"%s world/view:",label);
    for(float value:t.state.constants.world.values)std::fprintf(stderr," %.9g",value);
    std::fprintf(stderr," /");
    for(float value:t.state.constants.view.values)std::fprintf(stderr," %.9g",value);
    std::fprintf(stderr,"\n");
    if(t.vertices.size()==3*sizeof(LineGroupVertex))for(unsigned int c=0;c<3;++c) {
        LineGroupVertex v;std::memcpy(&v,t.vertices.data()+c*sizeof(v),sizeof(v));
        std::fprintf(stderr,"%s corner%u xyz=%.9g,%.9g,%.9g normal=%.9g,%.9g,%.9g color=%08x uv=%.9g,%.9g,%.9g,%.9g\n",
            label,c,v.x,v.y,v.z,v.nx,v.ny,v.nz,v.diffuse,v.u1,v.v1,v.u2,v.v2);
    }
}
static void PrintStreamMismatch(bool prism,bool transform,int acceptedPrefix,
    const std::vector<Triangle> &expected,const std::vector<Triangle> &actual) {
    size_t first=0;
    while(first<expected.size() && first<actual.size() && SameTriangle(expected[first],actual[first]))++first;
    std::fprintf(stderr,"STREAM_MISMATCH prism=%d transform=%d acceptedPrefix=%d expected=%zu actual=%zu first=%zu\n",
        int(prism),int(transform),acceptedPrefix,expected.size(),actual.size(),first);
    if(first>=expected.size() || first>=actual.size())return;
    size_t expectedInActual=0,actualInExpected=0;
    while(expectedInActual<actual.size() && !SameTriangle(expected[first],actual[expectedInActual]))++expectedInActual;
    while(actualInExpected<expected.size() && !SameTriangle(actual[first],expected[actualInExpected]))++actualInExpected;
    std::fprintf(stderr,"MATCH_LOCATIONS expected_first_in_actual=%zu actual_first_in_expected=%zu (stream size means absent)\n",
        expectedInActual,actualInExpected);
    PrintMismatchTriangle("expected",expected[first]);PrintMismatchTriangle("actual",actual[first]);
}
static Triangle Marker(float z,unsigned int shader) {
    Triangle t;CHECK(GetTrackedLegacyLogicalState(&t.state));t.state.pipeline.shaderBits=shader;
    t.texture=GpuHandle(31,shader);LineGroupVertex v[3]={};
    for(int i=0;i<3;++i){v[i].x=float(i);v[i].z=z;v[i].diffuse=0xff00ff00;}
    t.vertices.resize(sizeof(v));std::memcpy(t.vertices.data(),v,sizeof(v));return t;
}
static void TestGeometryStateAndGlobalOrder(bool prism,bool transform,int acceptedPrefix) {
    CameraClass camera;Input source;FixtureOwner referenceOwner;Initialize(referenceOwner,camera);
    Group direct;source.Configure(direct,prism,transform);RenderInfoClass info(camera);
    WW3D::sorting=false;direct.Render(info);
    const size_t expected=prism?16:8;CHECK(referenceOwner.immediate.size()==expected);
    NativeSortingRenderer reference;Sink expectedSink;
    std::vector<Triangle> markers={Marker(1,501),Marker(14,502)};
    QueueReference(reference,{markers[0]});QueueReference(reference,referenceOwner.immediate);QueueReference(reference,{markers[1]});
    CHECK(reference.Flush(expectedSink)==RENDER_RESULT_OK);
    FixtureOwner candidate;Initialize(candidate,camera);Group sorted;source.Configure(sorted,prism,transform);
    QueueReference(candidate.sorter,{markers[0]});WW3D::sorting=true;
    RenderMatrix4 savedView;CHECK(GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW,&savedView));
    sorted.Render(info);CHECK(candidate.queued==1);CHECK(candidate.immediate.empty());
    RenderMatrix4 restored;CHECK(GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW,&restored));
    CHECK(std::memcmp(savedView.values,restored.values,sizeof(savedView))==0);
    QueueReference(candidate.sorter,{markers[1]});
    source.start.values[0]=Vector3(999,999,999);source.diffuse.values[0]=Vector4();source.texture.handle=GpuHandle(99,1);
    LegacyMaterialState changed;changed.diffuse.w=.01f;TrackLegacyMaterial(changed);
    LegacyLightState light;light.diffuse.x=.99f;TrackLegacyLight(0,light);
    LegacyFogConstants fog;fog.density=.99f;TrackLegacyFog(fog);
    TrackLegacyShaderBits(999);Matrix4x4 changedView;changedView[2][3]=99;SetGameTransform(GAME_TRANSFORM_VIEW,changedView);
    candidate.m_gameTextures[0]=GpuHandle(99,1);
    Sink actual;if(acceptedPrefix>=0){actual.failCall=1;actual.acceptedOnFailure=static_cast<unsigned int>(acceptedPrefix);}
    CHECK(candidate.sorter.Flush(actual)==(acceptedPrefix>=0?RENDER_RESULT_FAILED:RENDER_RESULT_OK));
    if(acceptedPrefix>=0){CHECK(!candidate.sorter.Empty());actual.failCall=0;CHECK(candidate.sorter.Flush(actual)==RENDER_RESULT_OK);}
    if(!SameStream(expectedSink.triangles,actual.triangles))
        PrintStreamMismatch(prism,transform,acceptedPrefix,expectedSink.triangles,actual.triangles);
    CHECK(candidate.sorter.Empty());CHECK(SameStream(expectedSink.triangles,actual.triangles));
    size_t found=0;for(const Triangle &t:actual.triangles)if(t.state.pipeline.shaderBits==123){++found;
        CHECK(t.texture==GpuHandle(17,2));CHECK(t.state.constants.material.diffuse.w==.7f);
        for(size_t c=0;c<3;++c){LineGroupVertex v;std::memcpy(&v,t.vertices.data()+c*sizeof(v),sizeof(v));
            CHECK(v.nx==0 && v.ny==0 && v.nz==0 && v.u2==0 && v.v2==0);}}
    CHECK(found==expected);CHECK(scopeDepth==0);
}
static void TestRefusalAndImmediateCases() {
    FixtureOwner owner;CameraClass camera;Initialize(owner,camera);LineGroupVertex v[3]={};unsigned short indices[3]={0,1,2};
    auto submit=[&](){return DrawGameSortedIndexedTrianglesUP(1,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices));};
    CHECK(submit()==RENDER_RESULT_OK);CHECK(owner.queued==1);owner.sorter.Clear();
    CHECK(DrawGameSortedIndexedTrianglesUP(1,0,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(1,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,0,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(0,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    indices[2]=3;CHECK(submit()==RENDER_RESULT_INVALID_ARGUMENT);CHECK(owner.queued==1);indices[2]=2;
    CHECK(DrawGameSortedIndexedTrianglesUP(1,v,65536,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(UINT_MAX,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(1,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v)-1,sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(1,v,3,sizeof(v[0]),GAME_VERTEX_XYZNDUV2,indices,sizeof(v),sizeof(indices)-1)==RENDER_RESULT_INVALID_ARGUMENT);
    CHECK(DrawGameSortedIndexedTrianglesUP(1,v,3,sizeof(v[0]),1,indices,sizeof(v),sizeof(indices))==RENDER_RESULT_INVALID_ARGUMENT);
    owner.operational=false;CHECK(submit()==RENDER_RESULT_INVALID_ARGUMENT);owner.operational=true;
    owner.correctThread=false;CHECK(submit()==RENDER_RESULT_INVALID_ARGUMENT);owner.correctThread=true;
    owner.queueResult=RENDER_RESULT_OUT_OF_MEMORY;CHECK(submit()==RENDER_RESULT_OUT_OF_MEMORY);owner.queueResult=RENDER_RESULT_OK;
    currentOwner=0;CHECK(submit()==RENDER_RESULT_INVALID_ARGUMENT);currentOwner=&owner;
    GameRenderCommand command={};command.type=GAME_RENDER_COMMAND_DRAW_SORTED_INDEXED_TRIANGLES_UP;
    CHECK(owner.ExecuteGameRenderCommand(command)==RENDER_RESULT_INVALID_ARGUMENT);CHECK(owner.sorter.Empty());
    GameSortedIndexedTrianglesUPData data={v,sizeof(v),indices,sizeof(indices)};
    command.input=&data;command.inputBytes=sizeof(data)-1;command.value0=1;command.value1=3;
    command.value2=sizeof(v[0]);command.value3=GAME_VERTEX_XYZNDUV2;
    CHECK(owner.ExecuteGameRenderCommand(command)==RENDER_RESULT_INVALID_ARGUMENT);
    command.inputBytes=sizeof(data);data.indexBytes=sizeof(indices)-1;
    CHECK(owner.ExecuteGameRenderCommand(command)==RENDER_RESULT_INVALID_ARGUMENT);CHECK(owner.sorter.Empty());
    for(int mode=0;mode<3;++mode){Input input;Group group;input.Configure(group,false,false);owner.immediate.clear();
        WW3D::sorting=mode!=0;if(mode==1)group.MakeOpaque();if(mode==2)group.EnableAlphaTest();
        RenderInfoClass info(camera);group.Render(info);CHECK(owner.immediate.size()==8);CHECK(owner.sorter.Empty());}
    Input input;Group empty;input.Configure(empty,false,false);empty.OverrideCount(0);owner.immediate.clear();
    WW3D::sorting=true;RenderInfoClass info(camera);empty.Render(info);CHECK(owner.immediate.empty());CHECK(owner.sorter.Empty());
    Group tooLarge;input.Configure(tooLarge,false,false);tooLarge.OverrideCount(16384);
    const unsigned int beforeFailure=owner.failureCount;tooLarge.Render(info);
    CHECK(owner.failureCount>beforeFailure);CHECK(owner.sorter.Empty());CHECK(owner.immediate.empty());
    CHECK(scopeDepth==0);
}
static void TestIndexChunkBoundaryAndRetry() {
    const unsigned int count=5462; // 65,544 R16 indices: two existing flush chunks.
    Input input;input.start.values.resize(count);input.end.values.resize(count);
    input.diffuse.values.resize(count,Vector4(.2f,.3f,.4f,.5f));
    input.tail.values.resize(count,Vector4(.6f,.7f,.8f,.9f));
    input.alt.values.resize(count);input.size.values.resize(count,.25f);input.u.values.resize(count,.4f);
    for(unsigned int i=0;i<count;++i){input.start.values[i]=Vector3(float(i),0,float(i));
        input.end.values[i]=Vector3(float(i),0,float(i)+.5f);input.alt.values[i]=i;}
    CameraClass camera;FixtureOwner once;Initialize(once,camera);Group first;input.Configure(first,false,false);
    WW3D::sorting=true;RenderInfoClass info(camera);first.Render(info);Sink expected;
    expected.retainStateTriangles=false;
    CHECK(once.sorter.Flush(expected)==RENDER_RESULT_OK);CHECK(expected.calls==2);
    CHECK(expected.acceptedTriangleCount==count*4U);
    FixtureOwner retried;Initialize(retried,camera);Group second;input.Configure(second,false,false);second.Render(info);
    Sink actual;actual.retainStateTriangles=false;actual.failCall=2;actual.acceptedOnFailure=0;
    CHECK(retried.sorter.Flush(actual)==RENDER_RESULT_FAILED);CHECK(!retried.sorter.Empty());
    actual.failCall=0;CHECK(retried.sorter.Flush(actual)==RENDER_RESULT_OK);
    CHECK(retried.sorter.Empty());CHECK(actual.calls==3);
    CHECK(expected.acceptedTriangleCount==actual.acceptedTriangleCount);
    CHECK(expected.acceptedGeometryBytes==actual.acceptedGeometryBytes);
}
static void TestMixedLayoutInterleaving() {
    struct CompactVertex {float x,y,z;unsigned int diffuse;};
    FixtureOwner owner;CameraClass camera;Initialize(owner,camera);Input input;Group group;input.Configure(group,false,false);
    LegacyLogicalState state;CHECK(GetTrackedLegacyLogicalState(&state));state.pipeline.shaderBits=801;
    NativeDrawPacket p;p.vertexCount=3;p.indexCount=3;p.vertexStride=sizeof(CompactVertex);p.indexed=true;
    p.indexFormat=RENDER_FORMAT_R16_UINT;p.topology=RENDER_PRIMITIVE_TRIANGLE_LIST;p.vertexFormat=RENDER_VERTEX_POSITION3_COLOR;
    CHECK(DecodeLegacyFvfVertexLayout(LEGACY_FVF_XYZ|LEGACY_FVF_DIFFUSE,p.vertexStride,&p.vertexLayout));
    CompactVertex v[3]={{0,0,1,1},{1,0,1,2},{0,1,1,3}};unsigned short indices[3]={0,1,2};
    CHECK(owner.sorter.Queue(state,p,v,sizeof(v),indices,sizeof(indices),0)==RENDER_RESULT_OK);
    WW3D::sorting=true;RenderInfoClass info(camera);group.Render(info);Sink sink;
    CHECK(owner.sorter.Flush(sink)==RENDER_RESULT_OK);CHECK(sink.triangles.size()==9);CHECK(sink.calls>=2);
    unsigned int compactCount=0,lineCount=0;
    for(const Triangle &t:sink.triangles){if(t.state.pipeline.shaderBits==801){++compactCount;CHECK(t.vertices.size()==sizeof(v));}
        else if(t.state.pipeline.shaderBits==123){++lineCount;CHECK(t.vertices.size()==3*sizeof(LineGroupVertex));}}
    CHECK(compactCount==1 && lineCount==8);
}
int main() {
    rts::JobSystem::setStartupWorkerCount(6);
    for(int prism=0;prism<2;++prism)for(int transform=0;transform<2;++transform){
        TestGeometryStateAndGlobalOrder(prism!=0,transform!=0,-1);
        TestGeometryStateAndGlobalOrder(prism!=0,transform!=0,0);
        TestGeometryStateAndGlobalOrder(prism!=0,transform!=0,2);}
    TestRefusalAndImmediateCases();TestIndexChunkBoundaryAndRetry();TestMixedLayoutInterleaving();currentOwner=0;
    std::printf("failures=%u outstanding_owner_scopes=%u\n",failures,scopeDepth);
    return failures?1:0;
}
