// Executes the real production backend and compares every captured pixel.
#include "Renderer/RendererDevice.h"
#include <windows.h>
#include <dxgi1_2.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using namespace rts::render;
namespace {
int Check(bool value, const char *message) {
    if (!value) fprintf(stderr, "FAIL: %s\n", message);
    return value ? 0 : 1;
}
struct Fixture {
    HWND window;
    IRenderDevice *device;
    IRenderContext *context;
    GpuHandle vb, vb36, vb32, ib, colorTexture, shadowTexture;
    LegacyVertexLayout layout, layout36, layout32;
    std::vector<unsigned char> vertexBytes32;
    Fixture() : window(CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
        0, 0, 96, 96, 0, 0, GetModuleHandleW(0), 0)),
        device(CreateD3D11RenderDevice()), context(0) {}
    ~Fixture() { delete device; if (window) DestroyWindow(window); }
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
        if (!device || !window || device->initialize(parameters) != RENDER_RESULT_OK)
            return false;
        printf("INSTANCING_BACKEND backend=D3D11 driver=explicit-hardware allow_software_fallback=0 adapter_index=%u vendor=0x%04x device=0x%04x luid=%08x:%08x software=%u description=%ls\n",
            parameters.adapterIndex,selected.VendorId,selected.DeviceId,
            static_cast<unsigned int>(selected.AdapterLuid.HighPart),selected.AdapterLuid.LowPart,
            (selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)?1U:0U,selected.Description);
        if (selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
        context = device->immediateContext();
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
        if (device->createTexture(texture,&pixels,1,&colorTexture)!=RENDER_RESULT_OK) return false;
        pixels.data=shadowPixels;
        if (device->createTexture(texture,&pixels,1,&shadowTexture)!=RENDER_RESULT_OK) return false;
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
        if (device->createBuffer(buffer, vertices, sizeof(vertices), &vb) != RENDER_RESULT_OK)
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
        if (device->createBuffer(buffer,uncolored,sizeof(uncolored),&vb32)!=RENDER_RESULT_OK) return false;
        buffer.usage=RENDER_USAGE_IMMUTABLE;
        buffer.byteCount = sizeof(indices); buffer.stride = 2; buffer.binding = RENDER_BUFFER_INDEX;
        if (device->createBuffer(buffer, indices, sizeof(indices), &ib) != RENDER_RESULT_OK)
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
    return state;
}
int Image(Fixture &f, unsigned int scenario, bool instanced, std::vector<unsigned char> &pixels, bool sampled=true, bool applyProjectedShadow=true, bool color1Defaults=false, bool emitGeometry=true) {
    LegacyLogicalState state=State(scenario);
    const bool uncolored=f.layout.stride==32;
    if (color1Defaults) {
        state.pipeline.diffuseMaterialSource=state.pipeline.ambientMaterialSource=
            state.pipeline.emissiveMaterialSource=state.pipeline.specularMaterialSource=RENDER_MATERIAL_SOURCE_COLOR1;
    }
    if (uncolored && Check(f.device->updateBufferResource(f.vb32,f.vertexBytes32.data(),
        f.vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD)==RENDER_RESULT_OK,"dynamic32 full image restored before frame")) return 1;
    const unsigned int textureMask=scenario>=5 && sampled ? 3U : 0U;
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
        result|=Check(f.context->setLegacyStateForLayout(state,f.layout,textureMask)==RENDER_RESULT_OK,
            "sentinel-only contribution baseline");
    }
    else if (instanced) {
        state.constants.world=worlds[count-1];
        result|=Check(f.context->setLegacyStateForLayout(state,f.layout,textureMask)==RENDER_RESULT_OK,"instance state");
        for (unsigned int batch=0;batch<repeats;++batch) {
            if (uncolored) result|=Check(f.context->updateBuffer(f.vb32,f.vertexBytes32.data(),
                f.vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD)==RENDER_RESULT_OK,"dynamic32 batch DISCARD");
            result|=Check(f.context->drawIndexedInstanced(3,0,0,worlds,count)==RENDER_RESULT_OK,"real instanced draw");
        }
    }
    else for (unsigned int batch=0;batch<repeats;++batch) for (unsigned int i=0;i<count;++i) {
        if (uncolored && i==0) result|=Check(f.context->updateBuffer(f.vb32,f.vertexBytes32.data(),
            f.vertexBytes32.size(),0,RENDER_BUFFER_UPDATE_DISCARD)==RENDER_RESULT_OK,"ordinary32 batch DISCARD");
        state.constants.world=worlds[i];
        result|=Check(f.context->setLegacyStateForLayout(state,f.layout,textureMask)==RENDER_RESULT_OK &&
            f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"ordinary reference draw");
    }
    // State-equal sentinel first, then changed ordinary state. It must restore
    // the ordinary input layout and VS even when logical-state caching hits.
    if (instanced) result|=Check(f.context->setLegacyStateForLayout(state,f.layout,textureMask)==RENDER_RESULT_OK,
        "equal-state ordinary sentinel binding");
    result|=Check(f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"equal-state ordinary sentinel");
    // The equal-state sentinel's world must match in both images.
    // Reset and cover a dedicated corner for a changed-state sentinel.
    state=State(0); state.constants.world.values[0]=state.constants.world.values[5]=.2f;
    state.constants.world.values[12]=-.7f; state.constants.world.values[13]=.7f;
    if (uncolored) result|=Check(f.context->setVertexBuffer(f.vb36,36,0)==RENDER_RESULT_OK,
        "switch from instanced32 to ordinary36 sentinel");
    result|=Check(f.context->setLegacyStateForLayout(state,uncolored?f.layout36:f.layout,0)==RENDER_RESULT_OK &&
        f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"changed-state ordinary sentinel");
    if (uncolored) {
        state.constants.world.values[12]=.7f;
        result|=Check(f.context->setVertexBuffer(f.vb32,32,0)==RENDER_RESULT_OK &&
            f.context->setLegacyStateForLayout(state,f.layout32,0)==RENDER_RESULT_OK &&
            f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"switch ordinary36 back to ordinary32 sentinel");
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
        result|=Check(f.context->setLegacyStateForLayout(state,f.layout,textureMask)==RENDER_RESULT_OK,"ordinary projected shadow state");
        if (instanced) result|=Check(f.context->drawIndexedInstanced(3,0,0,worlds,3)==RENDER_RESULT_UNSUPPORTED,
            "projected multiplicative shadow cleanly uses ordinary path");
        result|=Check(f.context->drawIndexed(3,0,0)==RENDER_RESULT_OK,"projected shadow ordinary draw once");
    }
    result|=Check(f.context->endFrame()==RENDER_RESULT_OK,"frame completion");
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
}
int main() {
    Fixture f;
    if (Check(f.Initialize(),"real D3D11 instancing fixture initialization")) return 1;
    int result=Rejections(f);
    for (unsigned int scenario=0;scenario<7 && !result;++scenario) {
        std::vector<unsigned char> reference(96*96*4), actual(reference.size());
        result|=Image(f,scenario,false,reference);
        result|=Image(f,scenario,true,actual);
        result|=Check(reference==actual,"all pixels ordinary/instanced parity");
        bool varied=false;
        for (size_t i=4;i<reference.size();i+=4)
            varied=varied || memcmp(reference.data(),reference.data()+i,4)!=0;
        result|=Check(varied,"render contains geometry beyond clear color");
        if (scenario>=5 && !result) {
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
        if (scenario>=5) printf("INSTANCING_TEXTURE_PARITY case=%s texture_stages=2 sampled_contribution_verified=%u full_pixel_parity=%u\n",
            scenario==5?"two-stage-add-signed":"opaque-shadow-mask-and-ordinary-projected-shadow",
            result==0?1U:0U,result==0?1U:0U);
    }
    f.Select32(true);
    for (unsigned int scenario=0;scenario<7 && !result;++scenario) {
        std::vector<unsigned char> reference(96*96*4), actual(reference.size());
        result|=Image(f,scenario,false,reference);
        result|=Image(f,scenario,true,actual);
        result|=Check(reference==actual,"exact32 all pixels ordinary/instanced parity");
        bool varied=false;
        for (size_t i=4;i<reference.size();i+=4)
            varied=varied || memcmp(reference.data(),reference.data()+i,4)!=0;
        result|=Check(varied,"exact32 geometry contributes visible pixels");
        if (!result) {
            std::vector<unsigned char> sentinelsOnly(reference.size());
            result|=Image(f,scenario,false,sentinelsOnly,true,true,false,false);
            result|=Check(sentinelsOnly!=reference,"exact32 batch contributes pixels beyond ordinary sentinels");
        }
        if (scenario>=5 && !result) {
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
    return result;
}
