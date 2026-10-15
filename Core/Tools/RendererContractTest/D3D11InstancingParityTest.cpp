// Executes the real production backend and compares every captured pixel.
#include "Renderer/RendererDevice.h"
#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"
#include "Renderer/ThreadedRenderDevice.h"
namespace rts { namespace render {
// Reuse the existing friend seam, without adding a production injection API.
class NativeW3DRecoveryTestAccess {
public:
    static RenderResult Attach(NativeW3DRenderer &renderer,
        NativeW3DRenderState *state, IRenderDevice *device) {
        RenderResult result=state->AttachBackend(device,device->immediateContext());
        return result==RENDER_RESULT_OK ? renderer.AttachBorrowedState(state) : result;
    }
    static RenderResult Detach(NativeW3DRenderer &renderer, NativeW3DRenderState *state) {
        RenderResult result=renderer.DetachBorrowedState();
        return result==RENDER_RESULT_OK ? state->DetachBackend() : result;
    }
};
} }
#include <windows.h>
#include <dxgi1_2.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <memory>
using namespace rts::render;
namespace {
int Check(bool value, const char *message) {
    if (!value) fprintf(stderr, "FAIL: %s\n", message);
    return value ? 0 : 1;
}
struct Fixture {
    struct WindowLifetime {
        explicit WindowLifetime(HWND value) : value(value) {}
        ~WindowLifetime() { if (value) DestroyWindow(value); }
        HWND value;
    };
    HWND window;
    WindowLifetime windowLifetime;
    IRenderDevice *device;
    std::unique_ptr<IRenderDevice> deviceLifetime;
    IRenderContext *context;
    GpuHandle vb, vb36, vb32, ib, colorTexture, shadowTexture;
    LegacyVertexLayout layout, layout36, layout32;
    std::vector<unsigned char> vertexBytes32;
    // 0 is the unchanged low-level reference; 1/2/3 are direct/queued/serial facade.
    unsigned int routeMode, completedFrames;
    NativeW3DRenderer facade;
    NativeW3DResources resources;
    NativeW3DRenderState *borrowed;
    bool facadeFrameOpen;
    LegacyLogicalState submittedState;
    LegacyVertexLayout submittedLayout;
    unsigned int submittedMask, submittedStride;
    GpuHandle submittedVertex;
    LUID adapterLuid;
    static IRenderDevice *MakeDevice(unsigned int route) {
        if (route<2) return CreateD3D11RenderDevice();
        ThreadedRenderOptions options; options.serial=route==3;
        return CreateThreadedD3D11RenderDevice(options);
    }
    const char *RouteName() const {
        return routeMode==1 ? "direct" : routeMode==2 ? "queued" : "serial";
    }
    explicit Fixture(unsigned int route=0) : window(CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
        0, 0, 96, 96, 0, 0, GetModuleHandleW(0), 0)),
        windowLifetime(window), device(MakeDevice(route)), deviceLifetime(device), context(0), routeMode(route), completedFrames(0),
        borrowed(0), facadeFrameOpen(false), submittedMask(0), submittedStride(0) {
        adapterLuid.LowPart=0; adapterLuid.HighPart=0;
    }
    // Member destruction is resources -> facade -> backend owner -> window.
    // Even a failed explicit cleanup cannot leave a destructor using a freed device.
    ~Fixture() { CloseRoute(); }
    int CloseRoute() {
        if (!borrowed) return 0;
        int result=0;
        if (facadeFrameOpen) result|=Finish();
        if (routeMode>=2) result|=DrainAndPublishCleanup();
        result|=Check(resources.Shutdown()==RENDER_RESULT_OK,"route resource authority shutdown");
        if (routeMode>=2) result|=Check(DrainThreadedRenderDevice(device)==RENDER_RESULT_OK,
            "route cleanup drains resource retirements");
        result|=Check(borrowed->BeginShutdown()==RENDER_RESULT_OK &&
            NativeW3DRecoveryTestAccess::Detach(facade,borrowed)==RENDER_RESULT_OK,
            "route closes cleanup authority before device destruction");
        borrowed->Release(); borrowed=0;
        return result;
    }
    RenderResult CreateBuffer(const BufferDescriptor &d, const void *data, size_t bytes, GpuHandle *out) {
        return routeMode ? resources.CreateBuffer(d,data,bytes,out) : device->createBuffer(d,data,bytes,out);
    }
    RenderResult CreateTexture(const TextureDescriptor &d, const TextureSubresourceData *data,
        unsigned int count, GpuHandle *out) {
        return routeMode ? resources.CreateTexture(d,data,count,out) : device->createTexture(d,data,count,out);
    }
    RenderResult SetVertex(GpuHandle buffer, unsigned int stride, unsigned int offset) {
        submittedVertex=buffer; submittedStride=stride;
        return routeMode ? (offset==0 ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT) :
            context->setVertexBuffer(buffer,stride,offset);
    }
    RenderResult SetState(const LegacyLogicalState &state, const LegacyVertexLayout &layout,
        unsigned int mask, bool instanced=false) {
        submittedState=state; submittedLayout=layout; submittedMask=mask;
        if (routeMode) return RENDER_RESULT_OK;
        return instanced ? context->setLegacyStateForInstancedDraw(state,layout,mask) :
            context->setLegacyStateForLayout(state,layout,mask);
    }
    NativeDrawPacket Packet(unsigned int count, unsigned int first, int base) const {
        NativeDrawPacket packet; packet.indexed=true; packet.vertexCount=3;
        packet.indexCount=count; packet.startIndex=first; packet.baseVertex=base;
        packet.vertexBuffer=submittedVertex; packet.vertexStride=submittedStride;
        packet.indexBuffer=ib; packet.indexFormat=RENDER_FORMAT_R16_UINT;
        packet.vertexFormat=RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
        packet.vertexLayout.stride=submittedLayout.stride;
        packet.vertexLayout.elementCount=submittedLayout.elementCount;
        packet.vertexLayout.preTransformed=submittedLayout.preTransformed;
        for (unsigned int i=0;i<submittedLayout.elementCount;++i) {
            packet.vertexLayout.elements[i].semantic=submittedLayout.elements[i].semantic;
            packet.vertexLayout.elements[i].semanticIndex=submittedLayout.elements[i].semanticIndex;
            packet.vertexLayout.elements[i].format=submittedLayout.elements[i].format;
            packet.vertexLayout.elements[i].byteOffset=submittedLayout.elements[i].byteOffset;
        }
        packet.texturePresenceMask=submittedMask;
        packet.textures[0]=(submittedMask&1) ? colorTexture : GpuHandle();
        packet.textures[1]=(submittedMask&2) ? shadowTexture : GpuHandle();
        return packet;
    }
    RenderResult Draw(unsigned int count, unsigned int first, int base) {
        return routeMode ? facade.Submit(resources,submittedState,Packet(count,first,base)) :
            context->drawIndexed(count,first,base);
    }
    RenderResult DrawInstanced(unsigned int count, unsigned int first, int base,
        const RenderMatrix4 *worlds, unsigned int instances) {
        return routeMode ? facade.SubmitInstanced(resources,submittedState,
            Packet(count,first,base),worlds,instances) :
            context->drawIndexedInstanced(count,first,base,worlds,instances);
    }
    RenderResult Update32(bool beforeFrame) {
        if (routeMode) return resources.UpdateBuffer(vb32,vertexBytes32.data(),
            vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD);
        return beforeFrame ? device->updateBufferResource(vb32,vertexBytes32.data(),
            vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD) :
            context->updateBuffer(vb32,vertexBytes32.data(),vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD);
    }
    int PublishOwnerCompletion(const ThreadedRenderFrameCompletion &completion) {
        int result=Check(completion.sequence==completedFrames+1,
            "route completion publication preserves exact owner sequence");
        const RenderResult published=resources.PublishThreadedCompletion(
            completion.sequence,completion.resourceFailure);
        result|=Check(published==RENDER_RESULT_OK,"route publishes consumed completion into resource authority");
        if (published==RENDER_RESULT_OK) completedFrames=static_cast<unsigned int>(completion.sequence);
        return result;
    }
    int DrainAndPublishCleanup() {
        int result=Check(DrainThreadedRenderDevice(device)==RENDER_RESULT_OK,
            "route cleanup drains accepted owner commands");
        // MakeDevice retains the production default of at most three frame slots.
        // A fourth poll detects overflow; any consumed completion is still published.
        for (unsigned int count=0;count<4;++count) {
            ThreadedRenderFrameCompletion completion;
            if (!PollThreadedRenderCompletion(device,&completion)) return result;
            result|=PublishOwnerCompletion(completion);
            result|=Check(count<3,"route cleanup completion count respects three frame slots");
            result|=Check(completion.result==RENDER_RESULT_OK && completion.operational &&
                !completion.resourceFailure && !completion.outcome.hasCommandFailure(),
                "route cleanup preserves reported owner failures");
        }
        return result;
    }
    int Finish() {
        if (!routeMode) return Check(context->endFrame()==RENDER_RESULT_OK,"frame completion");
        const RenderResult ended=facade.EndFrame(false); facadeFrameOpen=false;
        if (Check(ended==RENDER_RESULT_OK,"route facade ends frame")) return 1;
        int result=Check(facade.FinalizeEndedFrame(false)==RENDER_RESULT_OK,"route seals non-present frame");
        if (routeMode>=2) {
            result|=Check(DrainThreadedRenderDevice(device)==RENDER_RESULT_OK,"route drains real owner before readback");
            ThreadedRenderFrameCompletion completion;
            const bool found=PollThreadedRenderCompletion(device,&completion);
            result|=Check(found,"route retains the exact frame completion");
            if (found) {
                result|=Check(completion.result==RENDER_RESULT_OK && completion.operational &&
                    !completion.resourceFailure && !completion.presented && !completion.outcome.hasCommandFailure(),
                    "exact successful non-present route completion before capture");
                // Polling acknowledges the mailbox, not NativeW3DResources uploads.
                // Publish even a failed completion with its actual resource flag.
                result|=PublishOwnerCompletion(completion);
            }
            if (PollThreadedRenderCompletion(device,&completion)) {
                result|=PublishOwnerCompletion(completion);
                result|=Check(false,"route consumes exactly one frame completion");
            }
        }
        return result;
    }
    bool Initialize() {
        RenderDeviceParameters parameters;
        parameters.backend = RENDER_BACKEND_D3D11; parameters.window = window;
        parameters.width = parameters.height = 96; parameters.enableVsync = false;
        parameters.allowSoftwareFallback = false;
        // Bind the production device to this exact verified hardware adapter.
        // Its explicit-index path uses D3D_DRIVER_TYPE_UNKNOWN and cannot WARP-retry.
        IDXGIFactory1 *factory=0;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void **>(&factory))))
            return false;
        DXGI_ADAPTER_DESC1 selected = {};
        bool hardware=false;
        for (UINT index=0;;++index) {
            IDXGIAdapter1 *adapter=0;
            if (FAILED(factory->EnumAdapters1(index,&adapter))) break;
            DXGI_ADAPTER_DESC1 description = {};
            const HRESULT described=adapter->GetDesc1(&description);
            adapter->Release();
            if (SUCCEEDED(described) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected=description; parameters.adapterIndex=index; hardware=true; break;
            }
        }
        factory->Release();
        if (!hardware) { fprintf(stderr,"FAIL: no hardware DXGI adapter for qualification\n"); return false; }
        if (routeMode && parameters.adapterIndex!=0) return false;
        adapterLuid=selected.AdapterLuid;
        if (!device || !window || device->initialize(parameters) != RENDER_RESULT_OK)
            return false;
        printf(routeMode ? "INSTANCING_ROUTE_BACKEND backend=D3D11 driver=explicit-hardware allow_software_fallback=0 adapter_index=%u vendor=0x%04x device=0x%04x luid=%08x:%08x software=%u description=%ls\n" : "INSTANCING_BACKEND backend=D3D11 driver=explicit-hardware allow_software_fallback=0 adapter_index=%u vendor=0x%04x device=0x%04x luid=%08x:%08x software=%u description=%ls\n",
            parameters.adapterIndex,selected.VendorId,selected.DeviceId,
            static_cast<unsigned int>(selected.AdapterLuid.HighPart),selected.AdapterLuid.LowPart,
            (selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)?1U:0U,selected.Description);
        if (selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
        context = device->immediateContext();
        if (routeMode) {
            borrowed=NativeW3DRenderState::Create();
            if (!borrowed || borrowed->BindOwner()!=RENDER_RESULT_OK ||
                NativeW3DRecoveryTestAccess::Attach(facade,borrowed,device)!=RENDER_RESULT_OK ||
                resources.Bind(&facade)!=RENDER_RESULT_OK) return false;
        }
        unsigned int colorPixels[64], shadowPixels[64];
        for (unsigned int y=0;y<8;++y) for (unsigned int x=0;x<8;++x) {
            const unsigned int cell=y*8+x;
            colorPixels[cell]=0xff000000U | ((35+x*27)<<16) | ((45+y*25)<<8) |
                (((x+y)&1)?210U:55U);
            const int dx=static_cast<int>(x)-3, dy=static_cast<int>(y)-3;
            const unsigned int shade=dx*dx+dy*dy<9 ? 65U : 220U;
            shadowPixels[cell]=0xff000000U | (shade<<16) | (shade<<8) | shade;
        }
        TextureDescriptor texture;
        texture.width=texture.height=8; texture.mipCount=texture.arrayCount=1;
        texture.dimension=RENDER_TEXTURE_2D; texture.format=RENDER_FORMAT_B8G8R8A8_UNORM;
        texture.binding=RENDER_TEXTURE_SHADER_RESOURCE; texture.usage=RENDER_USAGE_IMMUTABLE;
        TextureSubresourceData pixels; pixels.rowPitch=8*4; pixels.slicePitch=sizeof(colorPixels);
        pixels.data=colorPixels;
        if (CreateTexture(texture,&pixels,1,&colorTexture)!=RENDER_RESULT_OK) return false;
        pixels.data=shadowPixels;
        if (CreateTexture(texture,&pixels,1,&shadowTexture)!=RENDER_RESULT_OK) return false;
        struct Vertex { float position[3], normal[3]; unsigned int color; float uv[2]; };
        const Vertex vertices[] = {
            {{-.75f,-.65f,.35f},{.3f,.4f,-.85f},0xffd0a080U,{0,1}},
            {{0,.75f,.35f},{-.2f,.6f,-.75f},0xff80c0e0U,{.5f,0}},
            {{.75f,-.65f,.35f},{.5f,-.2f,-.8f},0xffe080b0U,{1,1}}
        };
        const unsigned short indices[] = {0,1,2};
        BufferDescriptor buffer;
        buffer.byteCount = sizeof(vertices); buffer.stride = sizeof(Vertex);
        buffer.binding = RENDER_BUFFER_VERTEX; buffer.usage = RENDER_USAGE_IMMUTABLE;
        if (CreateBuffer(buffer, vertices, sizeof(vertices), &vb) != RENDER_RESULT_OK)
            return false;
        vb36=vb;
        struct Vertex32 { float position[3], normal[3], uv[2]; };
        Vertex32 uncolored[3];
        for (unsigned int i=0;i<3;++i) {
            memcpy(uncolored[i].position,vertices[i].position,sizeof(uncolored[i].position));
            memcpy(uncolored[i].normal,vertices[i].normal,sizeof(uncolored[i].normal));
            memcpy(uncolored[i].uv,vertices[i].uv,sizeof(uncolored[i].uv));
        }
        vertexBytes32.assign(reinterpret_cast<const unsigned char *>(uncolored),
            reinterpret_cast<const unsigned char *>(uncolored)+sizeof(uncolored));
        buffer.byteCount=sizeof(uncolored); buffer.stride=sizeof(Vertex32);
        buffer.usage=RENDER_USAGE_DYNAMIC;
        if (CreateBuffer(buffer,uncolored,sizeof(uncolored),&vb32)!=RENDER_RESULT_OK) return false;
        buffer.usage=RENDER_USAGE_IMMUTABLE;
        buffer.byteCount = sizeof(indices); buffer.stride = 2; buffer.binding = RENDER_BUFFER_INDEX;
        if (CreateBuffer(buffer, indices, sizeof(indices), &ib) != RENDER_RESULT_OK)
            return false;
        layout.stride = sizeof(Vertex); layout.elementCount = 4;
        const LegacyVertexSemantic semantics[] = {RENDER_VERTEX_SEMANTIC_POSITION,
            RENDER_VERTEX_SEMANTIC_NORMAL, RENDER_VERTEX_SEMANTIC_DIFFUSE,
            RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE};
        const LegacyVertexDataFormat formats[] = {RENDER_VERTEX_DATA_FLOAT3,
            RENDER_VERTEX_DATA_FLOAT3, RENDER_VERTEX_DATA_COLOR_BGRA8, RENDER_VERTEX_DATA_FLOAT2};
        const unsigned int offsets[] = {0,12,24,28};
        for (unsigned int i=0;i<4;++i) {
            layout.elements[i].semantic=semantics[i]; layout.elements[i].format=formats[i];
            layout.elements[i].byteOffset=offsets[i];
        }
        layout36=layout; layout32=layout;
        layout32.stride=32; layout32.elementCount=3;
        layout32.elements[2]=layout32.elements[3]; layout32.elements[2].byteOffset=24;
        layout32.elements[3]=LegacyVertexElement();
        return context && device->supportsRigidInstancing();
    }
    void Select32(bool uncolored) {
        layout=uncolored?layout32:layout36; vb=uncolored?vb32:vb36;
    }
    int Begin(const LegacyLogicalState &state, unsigned int textureMask=0) {
        if (routeMode) {
            if (facade.BeginFrame()!=RENDER_RESULT_OK) return Check(false,"route facade begin");
            facadeFrameOpen=true;
            return Check(context->clear(RenderFloat4(.03f,.07f,.11f,1),1,0)==RENDER_RESULT_OK &&
                SetVertex(vb,layout.stride,0)==RENDER_RESULT_OK &&
                SetState(state,layout,textureMask)==RENDER_RESULT_OK,"route clear and packet values");
        }
        return Check(context->beginFrame()==RENDER_RESULT_OK &&
            context->clear(RenderFloat4(.03f,.07f,.11f,1),1,0)==RENDER_RESULT_OK &&
            context->setVertexBuffer(vb,layout.stride,0)==RENDER_RESULT_OK &&
            context->setIndexBuffer(ib,RENDER_FORMAT_R16_UINT,0)==RENDER_RESULT_OK &&
            context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST)==RENDER_RESULT_OK &&
            context->setTexture(0,(textureMask&1)?colorTexture:GpuHandle())==RENDER_RESULT_OK &&
            context->setTexture(1,(textureMask&2)?shadowTexture:GpuHandle())==RENDER_RESULT_OK &&
            context->setLegacyStateForLayout(state,layout,textureMask)==RENDER_RESULT_OK,"frame bindings");
    }
};
LegacyLogicalState State(unsigned int scenario) {
    LegacyLogicalState state;
    state.pipeline.rasterizer.cullMode=RENDER_CULL_NONE;
    state.pipeline.normalizeNormals=true;
    if (scenario>=2) {
        state.pipeline.lightingEnable=true;
        state.pipeline.diffuseMaterialSource=RENDER_MATERIAL_SOURCE_MATERIAL;
        state.constants.material.diffuse=RenderFloat4(.65f,.45f,.85f,1);
        state.constants.material.ambient=RenderFloat4(.3f,.4f,.2f,1);
        state.constants.material.specular=RenderFloat4(.25f,.2f,.3f,1);
        state.constants.material.specularPower=9;
        state.constants.globalAmbient=RenderFloat4(.15f,.2f,.1f,1);
        state.constants.lights[0].enabled=true;
        state.constants.lights[0].type=RENDER_LIGHT_DIRECTIONAL;
        state.constants.lights[0].direction=RenderFloat4(.3f,.4f,.85f,0);
        state.constants.lights[0].diffuse=RenderFloat4(.8f,.65f,.9f,1);
        state.constants.lights[0].specular=RenderFloat4(.2f,.3f,.4f,1);
        state.constants.view.values[1]=.075f;
        state.constants.view.values[14]=.12f;
    }
    if (scenario>=3) {
        state.pipeline.clipPlaneEnableMask=1;
        state.constants.clipPlanes[0]=RenderFloat4(1,.25f,0,.15f);
        state.pipeline.fogMode=RENDER_FOG_LINEAR;
        state.pipeline.rangeFogEnable=true;
        state.constants.fog.enabled=true;
        state.constants.fog.start=.05f; state.constants.fog.end=1.2f;
        state.constants.fog.color=RenderFloat4(.18f,.32f,.5f,1);
    }
    if (scenario>=5) {
        // Two sampled channels, real material/light/normal/fog/clip state, and
        // a nontrivial second-stage combiner. Both stages use mesh UV0.
        for (unsigned int stage=0;stage<2;++stage) {
            LegacyTextureStageState &t=state.pipeline.textureStages[stage];
            t.textureCoordinateIndex=0;
            t.sampler.addressU=t.sampler.addressV=RENDER_TEXTURE_ADDRESS_CLAMP;
            t.sampler.minification=t.sampler.magnification=RENDER_TEXTURE_FILTER_POINT;
            t.sampler.mipmapping=RENDER_TEXTURE_FILTER_NONE;
            t.colorArgument1=RENDER_TEXTURE_ARG_TEXTURE;
            t.colorArgument2=stage==0?RENDER_TEXTURE_ARG_DIFFUSE:RENDER_TEXTURE_ARG_CURRENT;
            t.colorOperation=stage==0?RENDER_TEXTURE_OP_MODULATE:RENDER_TEXTURE_OP_ADD_SIGNED;
            t.alphaOperation=RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
            t.alphaArgument1=RENDER_TEXTURE_ARG_CURRENT;
        }
        state.pipeline.textureStages[2].colorOperation=RENDER_TEXTURE_OP_DISABLE;
    }
    if (scenario==6) {
        // Eligible opaque shadow-mask receiver: sample its attenuation mask,
        // modulate the existing textured surface, and retain depth/stencil.
        state.pipeline.textureStages[1].colorOperation=RENDER_TEXTURE_OP_MODULATE;
        state.pipeline.depthStencil.depthEnable=true;
        state.pipeline.depthStencil.depthWrite=true;
        state.pipeline.depthStencil.depthFunction=RENDER_COMPARE_LESS_EQUAL;
        state.pipeline.depthStencil.stencilEnable=true;
        state.pipeline.depthStencil.stencilFunction=RENDER_COMPARE_ALWAYS;
        state.pipeline.depthStencil.stencilReference=1;
        state.pipeline.depthStencil.stencilPass=RENDER_STENCIL_REPLACE;
    }
    if (scenario==7) {
        state=State(2);
        state.pipeline.secondaryGradientEnable=true;
        state.constants.material.specular=RenderFloat4(.8f,.6f,.7f,1);
        state.constants.material.specularPower=4;
        state.constants.lights[0].specular=RenderFloat4(.6f,.8f,.7f,1);
        state.constants.view.setIdentity();
        state.constants.view.values[0]=-.8f; state.constants.view.values[2]=.6f;
        state.constants.view.values[8]=-.6f; state.constants.view.values[10]=-.8f;
        state.constants.view.values[14]=.7f;
    }
    return state;
}
int Image(Fixture &f, unsigned int scenario, bool instanced, std::vector<unsigned char> &pixels, bool sampled=true, bool applyProjectedShadow=true, bool color1Defaults=false, bool emitGeometry=true, bool specular=true, bool atomicState=true) {
    LegacyLogicalState state=State(scenario);
    if (!specular) state.pipeline.secondaryGradientEnable=false;
    const bool uncolored=f.layout.stride==32;
    if (color1Defaults) {
        state.pipeline.diffuseMaterialSource=state.pipeline.ambientMaterialSource=
            state.pipeline.emissiveMaterialSource=state.pipeline.specularMaterialSource=RENDER_MATERIAL_SOURCE_COLOR1;
    }
    if (uncolored && Check(f.Update32(true)==RENDER_RESULT_OK,"dynamic32 full image restored before frame")) return 1;
    const unsigned int textureMask=(scenario==5 || scenario==6) && sampled ? 3U : 0U;
    int result=f.Begin(state,textureMask); if (result) return result;
    const unsigned int count=scenario==4 ? 32 : 3;
    RenderMatrix4 worlds[32];
    for (unsigned int i=0;i<count;++i) {
        worlds[i].setIdentity();
        worlds[i].values[0]=scenario==0 ? .32f : .63f;
        worlds[i].values[5]=scenario==0 ? .45f : .55f;
        worlds[i].values[10]=.75f + .07f*i;
        worlds[i].values[4]=.09f*(i+1);
        worlds[i].values[12]=scenario==0 ? -.6f+.6f*i : -.15f+.12f*(i%3);
        worlds[i].values[13]=scenario==4 ? -.3f+.12f*(i%5) : -.1f+.1f*i;
        worlds[i].values[14]=.006f*i;
    }
    // Scenario 4 issues multiple admitted batches in one frame to exercise
    // DISCARD renaming while prior GPU reads may still be in flight.
    const unsigned int repeats=scenario==4 ? 4 : 1;
    if (!emitGeometry) {
        state.constants.world=worlds[count-1];
        result|=Check(f.SetState(state,f.layout,textureMask)==RENDER_RESULT_OK,
            "sentinel-only contribution baseline");
    }
    else if (instanced) {
        state.constants.world=worlds[count-1];
        result|=Check((atomicState ? f.SetState(state,f.layout,textureMask,true) : f.SetState(state,f.layout,textureMask))==RENDER_RESULT_OK,"instance state");
        for (unsigned int batch=0;batch<repeats;++batch) {
            if (uncolored) result|=Check(f.Update32(false)==RENDER_RESULT_OK,"dynamic32 batch DISCARD");
            result|=Check(f.DrawInstanced(3,0,0,worlds,count)==RENDER_RESULT_OK,"real instanced draw");
        }
    }
    else for (unsigned int batch=0;batch<repeats;++batch) for (unsigned int i=0;i<count;++i) {
        if (uncolored && i==0) result|=Check(f.Update32(false)==RENDER_RESULT_OK,"ordinary32 batch DISCARD");
        state.constants.world=worlds[i];
        result|=Check(f.SetState(state,f.layout,textureMask)==RENDER_RESULT_OK &&
            f.Draw(3,0,0)==RENDER_RESULT_OK,"ordinary reference draw");
    }
    // State-equal sentinel first, then changed ordinary state. It must restore
    // the ordinary input layout and VS even when logical-state caching hits.
    if (instanced && scenario!=7) result|=Check(f.SetState(state,f.layout,textureMask)==RENDER_RESULT_OK,
        "equal-state ordinary sentinel binding");
    result|=Check(f.Draw(3,0,0)==RENDER_RESULT_OK,"equal-state ordinary sentinel");
    // The equal-state sentinel's world must match in both images.
    // Reset and cover a dedicated corner for a changed-state sentinel.
    state=State(0); state.constants.world.values[0]=state.constants.world.values[5]=.2f;
    state.constants.world.values[12]=-.7f; state.constants.world.values[13]=.7f;
    if (uncolored) result|=Check(f.SetVertex(f.vb36,36,0)==RENDER_RESULT_OK,
        "switch from instanced32 to ordinary36 sentinel");
    result|=Check(f.SetState(state,uncolored?f.layout36:f.layout,0)==RENDER_RESULT_OK &&
        f.Draw(3,0,0)==RENDER_RESULT_OK,"changed-state ordinary sentinel");
    if (uncolored) {
        state.constants.world.values[12]=.7f;
        result|=Check(f.SetVertex(f.vb32,32,0)==RENDER_RESULT_OK &&
            f.SetState(state,f.layout32,0)==RENDER_RESULT_OK &&
            f.Draw(3,0,0)==RENDER_RESULT_OK,"switch ordinary36 back to ordinary32 sentinel");
    }
    if (scenario==6 && applyProjectedShadow) {
        // Production projected-shadow multiplicative blending is outside the
        // opaque whitelist. A clean rejection is followed by exactly one
        // ordinary shadow-shading draw; never retry an accepted batch.
        state=State(6); state.constants.world=worlds[0];
        state.constants.world.values[14]-=.002f;
        state.pipeline.blend.blendEnable=true;
        state.pipeline.blend.sourceColor=RENDER_BLEND_DESTINATION_COLOR;
        state.pipeline.blend.destinationColor=RENDER_BLEND_ZERO;
        state.pipeline.depthStencil.depthWrite=false;
        state.pipeline.depthStencil.stencilFunction=RENDER_COMPARE_EQUAL;
        state.pipeline.depthStencil.stencilPass=RENDER_STENCIL_KEEP;
        result|=Check(f.SetState(state,f.layout,textureMask)==RENDER_RESULT_OK,"ordinary projected shadow state");
        if (instanced) result|=Check(f.DrawInstanced(3,0,0,worlds,3)==RENDER_RESULT_UNSUPPORTED,
            "projected multiplicative shadow cleanly uses ordinary path");
        result|=Check(f.Draw(3,0,0)==RENDER_RESULT_OK,"projected shadow ordinary draw once");
    }
    result|=f.Finish();
    if (f.routeMode && result) return result; // No capture or later upload after failed publication.
    RenderFormat format=RENDER_FORMAT_UNKNOWN;
    result|=Check(f.device->captureBackBuffer(pixels.data(),pixels.size(),96*4,&format)==RENDER_RESULT_OK &&
        format==RENDER_FORMAT_B8G8R8A8_UNORM,"actual GPU full-frame capture");
    return result;
}
int Rejections(Fixture &f) {
    LegacyLogicalState state=State(0);
    int result=f.Begin(state); if (result) return result;
    RenderMatrix4 worlds[33]; for (unsigned int i=0;i<33;++i) worlds[i].setIdentity();
    result|=Check(f.context->drawIndexedInstanced(3,0,0,worlds,1)==RENDER_RESULT_INVALID_ARGUMENT &&
        f.context->drawIndexedInstanced(3,0,0,worlds,33)==RENDER_RESULT_INVALID_ARGUMENT &&
        f.context->drawIndexedInstanced(3,0,0,0,2)==RENDER_RESULT_INVALID_ARGUMENT,"count and pointer rejection");
    result|=Check(f.context->drawIndexedInstanced(4,0,0,worlds,2)==RENDER_RESULT_INVALID_ARGUMENT &&
        f.context->drawIndexedInstanced(3,0,-1,worlds,2)==RENDER_RESULT_INVALID_ARGUMENT,"shared complete indexed validation");
    worlds[0].values[5]=0;
    result|=Check(f.context->drawIndexedInstanced(3,0,0,worlds,2)==RENDER_RESULT_INVALID_ARGUMENT,"singular normal rejection");
    worlds[0].setIdentity();
    LegacyVertexLayout reordered=f.layout;
    const LegacyVertexElement position=reordered.elements[0];
    reordered.elements[0]=reordered.elements[1]; reordered.elements[1]=position;
    result|=Check(f.context->setLegacyStateForLayout(state,reordered,0)==RENDER_RESULT_OK &&
        f.device->supportsRigidInstancing() &&
        f.context->drawIndexedInstanced(3,0,0,worlds,2)==RENDER_RESULT_UNSUPPORTED &&
        f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"noncanonical ordinary layout keeps eager capability intact");
    state.pipeline.alphaTestEnable=true;
    result|=Check(f.context->setLegacyStateForLayout(state,f.layout,0)==RENDER_RESULT_OK &&
        f.context->drawIndexedInstanced(3,0,0,worlds,2)==RENDER_RESULT_UNSUPPORTED,"unsupported alpha-test state");
    result|=Check(f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK &&
        f.context->endFrame()==RENDER_RESULT_OK,"rejected batch leaves ordinary state usable");
    // Empty and partial vertex images are rejected by the same initialized
    // byte proof as ordinary draws, even after another buffer seeded caches.
    BufferDescriptor empty;
    empty.byteCount=108; empty.stride=36; empty.binding=RENDER_BUFFER_VERTEX;
    empty.usage=RENDER_USAGE_DYNAMIC;
    GpuHandle incomplete;
    result|=Check(f.device->createBuffer(empty,0,0,&incomplete)==RENDER_RESULT_OK,"empty vertex resource");
    state=State(0);
    result|=f.Begin(state);
    // Empty content is rejected at binding. A rejected binding preserves the
    // previous valid stream; do not mistake that old stream for this fixture.
    result|=Check(f.context->setVertexBuffer(incomplete,36,0)==RENDER_RESULT_INVALID_ARGUMENT,
        "uninitialized vertex binding rejection");
    unsigned char firstVertex[36]={0};
    result|=Check(f.context->updateBuffer(incomplete,firstVertex,sizeof(firstVertex),0,
        RENDER_BUFFER_UPDATE_DISCARD)==RENDER_RESULT_OK &&
        f.context->setVertexBuffer(incomplete,36,0)==RENDER_RESULT_OK &&
        f.context->drawIndexedInstanced(3,0,0,worlds,2)==RENDER_RESULT_INVALID_ARGUMENT,"partial vertex rejection");
    result|=Check(f.context->endFrame()==RENDER_RESULT_OK &&
        f.device->destroyResource(incomplete),"partial fixture cleanup");
    return result;
}
int ReadViewPixels(Fixture &f, std::vector<unsigned char> &pixels) {
    RenderFormat format=RENDER_FORMAT_UNKNOWN;
    return Check(f.device->captureBackBuffer(pixels.data(),pixels.size(),96*4,&format)==RENDER_RESULT_OK &&
        format==RENDER_FORMAT_B8G8R8A8_UNORM,"View transition full-frame hardware capture");
}
int OrdinaryViewImage(Fixture &f, LegacyLogicalState state, const RenderMatrix4 *worlds,
    std::vector<unsigned char> &pixels) {
    if (f.layout.stride==32 && Check(f.Update32(true)==RENDER_RESULT_OK,"View oracle restores32 stream")) return 1;
    int result=f.Begin(state); if (result) return result;
    for (unsigned int i=0;i<3;++i) {
        state.constants.world=worlds[i];
        result|=Check(f.SetState(state,f.layout,0)==RENDER_RESULT_OK &&
            f.Draw(3,0,0)==RENDER_RESULT_OK,"independent ordinary View oracle draw");
    }
    result|=f.Finish();
    return result ? result : ReadViewPixels(f,pixels);
}
int ViewTransitionImage(Fixture &f, const LegacyLogicalState &prime,
    const LegacyLogicalState &viewChanged, const LegacyLogicalState &worldChanged,
    const RenderMatrix4 *worlds, const RenderMatrix4 *nextWorlds,
    bool includeWorldStep, std::vector<unsigned char> &pixels) {
    if (f.layout.stride==32 && Check(f.Update32(true)==RENDER_RESULT_OK,"View route restores32 authority")) return 1;
    int result=f.Begin(prime); if (result) return result;
    result|=Check(f.SetState(prime,f.layout,0,true)==RENDER_RESULT_OK &&
        f.DrawInstanced(3,0,0,worlds,3)==RENDER_RESULT_OK,"prime real valid instanced publication");
    // Clear removes priming pixels without changing the live legacy state proof.
    // No ordinary setter intervenes:07 itself chooses its strict View-only
    // ordinary-domain path inside the next specialized setter.
    result|=Check(f.context->clear(RenderFloat4(.03f,.07f,.11f,1),1,0)==RENDER_RESULT_OK &&
        f.SetState(viewChanged,f.layout,0,true)==RENDER_RESULT_OK &&
        f.DrawInstanced(3,0,0,worlds,3)==RENDER_RESULT_OK,"View-only refresh then actual instanced draw");
    if (includeWorldStep) {
        // Same frame, immediately following that View-only path:stable View,
        // only original World changes before the next canonical admission/draw.
        result|=Check(f.context->clear(RenderFloat4(.03f,.07f,.11f,1),1,0)==RENDER_RESULT_OK &&
            f.SetState(worldChanged,f.layout,0,true)==RENDER_RESULT_OK &&
            f.DrawInstanced(3,0,0,nextWorlds,3)==RENDER_RESULT_OK,"stable View changed World canonical draw");
    }
    result|=f.Finish();
    return result ? result : ReadViewPixels(f,pixels);
}
int ViewTransitionCases(Fixture &referenceDevice, Fixture &actualDevice) {
    int result=0;
    for (unsigned int layout=0;layout<2 && !result;++layout) {
        referenceDevice.Select32(layout!=0); actualDevice.Select32(layout!=0);
        RenderMatrix4 worlds[3], nextWorlds[3];
        for (unsigned int i=0;i<3;++i) {
            worlds[i].setIdentity(); worlds[i].values[0]=.63f; worlds[i].values[5]=.55f;
            worlds[i].values[10]=.75f+.07f*i; worlds[i].values[4]=.09f*(i+1);
            worlds[i].values[12]=-.15f+.12f*i; worlds[i].values[13]=-.1f+.1f*i;
            worlds[i].values[14]=.006f*i;
            nextWorlds[i]=worlds[i]; nextWorlds[i].values[12]+=.14f;
        }
        LegacyLogicalState prime=State(7); prime.constants.world=worlds[2];
        LegacyLogicalState changedView, changedWorld, proof;
        memcpy(&changedView,&prime,sizeof(prime));
        changedView.constants.view.values[0]=-.6f; changedView.constants.view.values[2]=.8f;
        changedView.constants.view.values[8]=-.8f; changedView.constants.view.values[10]=-.6f;
        memcpy(&proof,&changedView,sizeof(proof)); proof.constants.view=prime.constants.view;
        result|=Check(memcmp(&proof,&prime,sizeof(prime))==0 &&
            memcmp(&changedView.constants.view,&prime.constants.view,sizeof(prime.constants.view))!=0,
            "strict input transition changes only View bytes");
        memcpy(&changedWorld,&changedView,sizeof(changedWorld)); changedWorld.constants.world=nextWorlds[2];
        memcpy(&proof,&changedWorld,sizeof(proof)); proof.constants.world=changedView.constants.world;
        result|=Check(memcmp(&proof,&changedView,sizeof(proof))==0 &&
            memcmp(&changedWorld.constants.world,&changedView.constants.world,sizeof(prime.constants.world))!=0,
            "next input changes only World and retains the same View bytes");
        std::vector<unsigned char> viewReference(96*96*4), oldView(viewReference.size()), noSpecular(viewReference.size());
        result|=OrdinaryViewImage(referenceDevice,changedView,worlds,viewReference);
        result|=OrdinaryViewImage(referenceDevice,prime,worlds,oldView);
        LegacyLogicalState withoutSpecular; memcpy(&withoutSpecular,&changedView,sizeof(withoutSpecular));
        withoutSpecular.pipeline.secondaryGradientEnable=false;
        result|=OrdinaryViewImage(referenceDevice,withoutSpecular,worlds,noSpecular);
        result|=Check(viewReference!=oldView && viewReference!=noSpecular,
            "targeted rotated View and active specular each contribute visible pixels");
        bool visible=false;
        for (size_t i=4;i<viewReference.size();i+=4)
            visible=visible || memcmp(viewReference.data(),viewReference.data()+i,4)!=0;
        result|=Check(visible,"View transition oracle contains visible geometry");
        for (unsigned int phase=0;phase<2 && !result;++phase) {
            std::vector<unsigned char> expected(viewReference), actual(viewReference.size());
            if (phase) result|=OrdinaryViewImage(referenceDevice,changedWorld,nextWorlds,expected);
            result|=ViewTransitionImage(actualDevice,prime,changedView,changedWorld,
                worlds,nextWorlds,phase!=0,actual);
            result|=Check(expected==actual,"strict View transition all-pixel ordinary/production-route parity");
            printf("INSTANCING_ROUTE_VIEW_GPU route=%s stride=%u phase=%s view_visible=%u specular_visible=%u full_pixel_parity=%u\n",
                actualDevice.RouteName(),actualDevice.layout.stride,phase?"stable-view-world":"view-only",
                result==0?1U:0U,result==0?1U:0U,result==0?1U:0U);
        }
    }
    return result;
}
int RouteImages(Fixture &referenceDevice) {
    int result=0;
    for (unsigned int route=1;route<=3 && !result;++route) {
        Fixture actualDevice(route);
        if (Check(actualDevice.Initialize(),"production hardware facade route initialization")) return 1;
        result|=Check(actualDevice.adapterLuid.LowPart==referenceDevice.adapterLuid.LowPart &&
            actualDevice.adapterLuid.HighPart==referenceDevice.adapterLuid.HighPart,
            "route and independent ordinary oracle use the same explicit hardware adapter0");
        for (unsigned int layout=0;layout<2 && !result;++layout) {
            referenceDevice.Select32(layout!=0); actualDevice.Select32(layout!=0);
            for (unsigned int scenario=0;scenario<8 && !result;++scenario) {
                std::vector<unsigned char> reference(96*96*4), actual(reference.size());
                // Ordinary low-level renderer remains the independent pixel oracle.
                result|=Image(referenceDevice,scenario,false,reference);
                result|=Image(actualDevice,scenario,true,actual);
                result|=Check(reference==actual,"combined production-route all-pixel parity");
                printf("INSTANCING_ROUTE_GPU route=%s stride=%u scenario=%u instances=%u batches=%u full_pixel_parity=%u\n",
                    actualDevice.RouteName(),actualDevice.layout.stride,scenario,
                    scenario==4?32U:3U,scenario==4?4U:1U,result==0?1U:0U);
            }
        }
        if (!result) result|=ViewTransitionCases(referenceDevice,actualDevice);
        result|=actualDevice.CloseRoute();
    }
    return result;
}
}
int main() {
    Fixture f;
    if (Check(f.Initialize(),"real D3D11 instancing fixture initialization")) return 1;
    int result=Rejections(f);
    for (unsigned int scenario=0;scenario<8 && !result;++scenario) {
        std::vector<unsigned char> reference(96*96*4), actual(reference.size());
        result|=Image(f,scenario,false,reference);
        result|=Image(f,scenario,true,actual);
        result|=Check(reference==actual,"all pixels ordinary/instanced parity");
        if (scenario==7 && !result) {
            std::vector<unsigned char> noSpecular(reference.size()), oldApi(reference.size());
            result|=Image(f,scenario,false,noSpecular,true,true,false,true,false);
            result|=Check(noSpecular!=reference,"secondary gradient specular has visible contribution with rotated live View");
            result|=Image(f,scenario,true,oldApi,true,true,false,true,true,false);
            result|=Check(oldApi==reference,"old ordinary state then instanced draw keeps full pixel parity");
        }
        bool varied=false;
        for (size_t i=4;i<reference.size();i+=4)
            varied=varied || memcmp(reference.data(),reference.data()+i,4)!=0;
        result|=Check(varied,"render contains geometry beyond clear color");
        if (scenario>=5 && scenario<=6 && !result) {
            std::vector<unsigned char> noSamples(reference.size());
            result|=Image(f,scenario,false,noSamples,false);
            result|=Check(noSamples!=reference,"sampled texture/shadow combiner changes complete image");
            if (scenario==6 && !result) {
                std::vector<unsigned char> noProjectedShadow(reference.size());
                result|=Image(f,scenario,false,noProjectedShadow,true,false);
                result|=Check(noProjectedShadow!=reference,"ordinary projected shadow makes a visible pixel contribution");
            }
        }
        if (scenario==2 && !result) {
            result|=Check(f.device->recoverDevice()==RENDER_RESULT_OK &&
                f.device->supportsRigidInstancing(),"device recovery recreates instanced resources");
            f.context=f.device->immediateContext();
            std::vector<unsigned char> recovered(reference.size());
            result|=Image(f,scenario,true,recovered);
            result|=Check(recovered==reference,"recovered instanced pipeline pixel parity");
        }
        printf("INSTANCING_GPU path=DrawIndexedInstanced scenario=%u instances=%u batches=%u full_pixel_parity=%u\n",
            scenario,scenario==4?32U:3U,scenario==4?4U:1U,result==0?1U:0U);
        if (scenario>=5 && scenario<=6) printf("INSTANCING_TEXTURE_PARITY case=%s texture_stages=2 sampled_contribution_verified=%u full_pixel_parity=%u\n",
            scenario==5?"two-stage-add-signed":"opaque-shadow-mask-and-ordinary-projected-shadow",
            result==0?1U:0U,result==0?1U:0U);
    }
    f.Select32(true);
    for (unsigned int scenario=0;scenario<8 && !result;++scenario) {
        std::vector<unsigned char> reference(96*96*4), actual(reference.size());
        result|=Image(f,scenario,false,reference);
        result|=Image(f,scenario,true,actual);
        result|=Check(reference==actual,"exact32 all pixels ordinary/instanced parity");
        if (scenario==7 && !result) {
            std::vector<unsigned char> noSpecular(reference.size()), oldApi(reference.size());
            result|=Image(f,scenario,false,noSpecular,true,true,false,true,false);
            result|=Check(noSpecular!=reference,"secondary gradient specular has visible contribution with rotated live View");
            result|=Image(f,scenario,true,oldApi,true,true,false,true,true,false);
            result|=Check(oldApi==reference,"old ordinary state then instanced draw keeps full pixel parity");
        }
        bool varied=false;
        for (size_t i=4;i<reference.size();i+=4)
            varied=varied || memcmp(reference.data(),reference.data()+i,4)!=0;
        result|=Check(varied,"exact32 geometry contributes visible pixels");
        if (!result) {
            std::vector<unsigned char> sentinelsOnly(reference.size());
            result|=Image(f,scenario,false,sentinelsOnly,true,true,false,false);
            result|=Check(sentinelsOnly!=reference,"exact32 batch contributes pixels beyond ordinary sentinels");
        }
        if (scenario>=5 && scenario<=6 && !result) {
            std::vector<unsigned char> noSamples(reference.size());
            result|=Image(f,scenario,false,noSamples,false);
            result|=Check(noSamples!=reference,"exact32 sampled combiner visible contribution");
        }
        if (scenario==2 && !result) {
            std::vector<unsigned char> fallbackOrdinary(reference.size()), fallbackInstanced(reference.size());
            result|=Image(f,scenario,false,fallbackOrdinary,true,true,true);
            result|=Image(f,scenario,true,fallbackInstanced,true,true,true);
            result|=Check(fallbackOrdinary==reference && fallbackInstanced==reference,
                "missing COLOR1 sources preserve exact material/ambient/emissive/specular defaults");
            result|=Check(f.device->recoverDevice()==RENDER_RESULT_OK && f.device->supportsRigidInstancing(),
                "recovery recreates both eagerly admitted layouts");
            f.context=f.device->immediateContext();
            std::vector<unsigned char> recovered(reference.size());
            result|=Image(f,scenario,true,recovered);
            result|=Check(recovered==reference,"exact32 recovered full pixel parity");
            // Recovered colored path remains usable as well, with its own oracle.
            f.Select32(false);
            std::vector<unsigned char> coloredOrdinary(reference.size()), coloredInstanced(reference.size());
            result|=Image(f,scenario,false,coloredOrdinary);
            result|=Image(f,scenario,true,coloredInstanced);
            result|=Check(coloredOrdinary==coloredInstanced,"recovered32/36 pipeline swap full pixel parity");
            f.Select32(true);
        }
        printf("INSTANCING_GPU32 path=DrawIndexedInstanced stride=32 diffuse_present=0 scenario=%u instances=%u batches=%u dynamic_discards=%u full_pixel_parity=%u\n",
            scenario,scenario==4?32U:3U,scenario==4?4U:1U,scenario==4?4U:1U,result==0?1U:0U);
        if (scenario==2) printf("INSTANCING_DEFAULT32 color1_material_fallback=%u ambient_lighting=%u recovery_and_layout_swap=%u\n",
            result==0?1U:0U,result==0?1U:0U,result==0?1U:0U);
    }
    if (!result) result|=RouteImages(f);
    return result;
}
