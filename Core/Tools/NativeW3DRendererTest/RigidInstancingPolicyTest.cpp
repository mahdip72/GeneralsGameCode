#include "Renderer/RigidInstancingPolicy.h"
#include <cstdio>
#include <cstring>
#include <cstddef>
using namespace rts::render;
namespace {
int failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
NativeDrawPacket Packet()
{
	NativeDrawPacket packet;
	packet.indexed = true; packet.vertexCount = 3; packet.indexCount = 3;
	packet.vertexBuffer = GpuHandle(1, 1); packet.indexBuffer = GpuHandle(2, 1);
	packet.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	packet.vertexStride = packet.vertexLayout.stride = 36;
	packet.vertexLayout.elementCount = 4;
	const RenderVertexSemantic semantic[] = { RENDER_VERTEX_SEMANTIC_POSITION,
		RENDER_VERTEX_SEMANTIC_NORMAL, RENDER_VERTEX_SEMANTIC_DIFFUSE,
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE };
	const RenderVertexDataFormat format[] = { RENDER_VERTEX_DATA_FLOAT3,
		RENDER_VERTEX_DATA_FLOAT3, RENDER_VERTEX_DATA_COLOR_BGRA8, RENDER_VERTEX_DATA_FLOAT2 };
	const unsigned int offsets[] = { 0, 12, 24, 28 };
	for (unsigned int i = 0; i < 4; ++i) {
		packet.vertexLayout.elements[i].semantic = semantic[i];
		packet.vertexLayout.elements[i].format = format[i];
		packet.vertexLayout.elements[i].byteOffset = offsets[i];
	}
	return packet;
}
void TestEverySharedField()
{
	LegacyLogicalState original, changed;
#define CHANGE_STATE(field, value) changed = original; changed.field = value; \
	Check(!RigidInstancingSharedStateEqual(original, changed), #field)
	CHANGE_STATE(pipeline.shaderBits, static_cast<unsigned int>(original.pipeline.shaderBits + 1));
	CHANGE_STATE(pipeline.pixelProgram, static_cast<RenderLegacyPixelProgram>(original.pipeline.pixelProgram + 1));
	CHANGE_STATE(pipeline.vertexProgram, static_cast<RenderLegacyVertexProgram>(original.pipeline.vertexProgram + 1));
	CHANGE_STATE(pipeline.blend.blendEnable, !original.pipeline.blend.blendEnable);
	CHANGE_STATE(pipeline.blend.sourceColor, static_cast<RenderBlendFactor>(original.pipeline.blend.sourceColor + 1));
	CHANGE_STATE(pipeline.blend.destinationColor, static_cast<RenderBlendFactor>(original.pipeline.blend.destinationColor + 1));
	CHANGE_STATE(pipeline.blend.colorOperation, static_cast<RenderBlendOperation>(original.pipeline.blend.colorOperation + 1));
	CHANGE_STATE(pipeline.blend.sourceAlpha, static_cast<RenderBlendFactor>(original.pipeline.blend.sourceAlpha + 1));
	CHANGE_STATE(pipeline.blend.destinationAlpha, static_cast<RenderBlendFactor>(original.pipeline.blend.destinationAlpha + 1));
	CHANGE_STATE(pipeline.blend.alphaOperation, static_cast<RenderBlendOperation>(original.pipeline.blend.alphaOperation + 1));
	CHANGE_STATE(pipeline.blend.colorWriteMask, static_cast<unsigned int>(original.pipeline.blend.colorWriteMask + 1));
	CHANGE_STATE(pipeline.depthStencil.depthEnable, !original.pipeline.depthStencil.depthEnable);
	CHANGE_STATE(pipeline.depthStencil.depthWrite, !original.pipeline.depthStencil.depthWrite);
	CHANGE_STATE(pipeline.depthStencil.depthFunction, static_cast<RenderCompareFunction>(original.pipeline.depthStencil.depthFunction + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilEnable, !original.pipeline.depthStencil.stencilEnable);
	CHANGE_STATE(pipeline.depthStencil.stencilReadMask, static_cast<unsigned int>(original.pipeline.depthStencil.stencilReadMask + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilWriteMask, static_cast<unsigned int>(original.pipeline.depthStencil.stencilWriteMask + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilReference, static_cast<unsigned int>(original.pipeline.depthStencil.stencilReference + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilFunction, static_cast<RenderCompareFunction>(original.pipeline.depthStencil.stencilFunction + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilFail, static_cast<RenderStencilOperation>(original.pipeline.depthStencil.stencilFail + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilDepthFail, static_cast<RenderStencilOperation>(original.pipeline.depthStencil.stencilDepthFail + 1));
	CHANGE_STATE(pipeline.depthStencil.stencilPass, static_cast<RenderStencilOperation>(original.pipeline.depthStencil.stencilPass + 1));
	CHANGE_STATE(pipeline.rasterizer.fillMode, static_cast<RenderFillMode>(original.pipeline.rasterizer.fillMode + 1));
	CHANGE_STATE(pipeline.rasterizer.cullMode, static_cast<RenderCullMode>(original.pipeline.rasterizer.cullMode + 1));
	CHANGE_STATE(pipeline.rasterizer.frontCounterClockwise, !original.pipeline.rasterizer.frontCounterClockwise);
	CHANGE_STATE(pipeline.rasterizer.scissorEnable, !original.pipeline.rasterizer.scissorEnable);
	CHANGE_STATE(pipeline.rasterizer.depthBias, static_cast<int>(original.pipeline.rasterizer.depthBias + 1));
	CHANGE_STATE(pipeline.rasterizer.slopeScaledDepthBias, static_cast<float>(original.pipeline.rasterizer.slopeScaledDepthBias + 1));
	for (unsigned int i1 = 0; i1 < LEGACY_TEXTURE_STAGE_COUNT; ++i1) {
	CHANGE_STATE(pipeline.textureStages[i1].colorOperation, static_cast<RenderTextureOperation>(original.pipeline.textureStages[i1].colorOperation + 1));
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument0, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].colorArgument0 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument1, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].colorArgument1 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument2, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].colorArgument2 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].alphaOperation, static_cast<RenderTextureOperation>(original.pipeline.textureStages[i1].alphaOperation + 1));
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument0, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].alphaArgument0 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument1, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].alphaArgument1 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument2, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].alphaArgument2 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument0Complement, !original.pipeline.textureStages[i1].colorArgument0Complement);
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument0AlphaReplicate, !original.pipeline.textureStages[i1].colorArgument0AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument1Complement, !original.pipeline.textureStages[i1].colorArgument1Complement);
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument1AlphaReplicate, !original.pipeline.textureStages[i1].colorArgument1AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument2Complement, !original.pipeline.textureStages[i1].colorArgument2Complement);
	CHANGE_STATE(pipeline.textureStages[i1].colorArgument2AlphaReplicate, !original.pipeline.textureStages[i1].colorArgument2AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument0Complement, !original.pipeline.textureStages[i1].alphaArgument0Complement);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument0AlphaReplicate, !original.pipeline.textureStages[i1].alphaArgument0AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument1Complement, !original.pipeline.textureStages[i1].alphaArgument1Complement);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument1AlphaReplicate, !original.pipeline.textureStages[i1].alphaArgument1AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument2Complement, !original.pipeline.textureStages[i1].alphaArgument2Complement);
	CHANGE_STATE(pipeline.textureStages[i1].alphaArgument2AlphaReplicate, !original.pipeline.textureStages[i1].alphaArgument2AlphaReplicate);
	CHANGE_STATE(pipeline.textureStages[i1].resultArgument, static_cast<RenderTextureArgument>(original.pipeline.textureStages[i1].resultArgument + 1));
	CHANGE_STATE(pipeline.textureStages[i1].textureCoordinateIndex, static_cast<unsigned int>(original.pipeline.textureStages[i1].textureCoordinateIndex + 1));
	CHANGE_STATE(pipeline.textureStages[i1].cameraSpacePosition, !original.pipeline.textureStages[i1].cameraSpacePosition);
	CHANGE_STATE(pipeline.textureStages[i1].cameraSpaceNormal, !original.pipeline.textureStages[i1].cameraSpaceNormal);
	CHANGE_STATE(pipeline.textureStages[i1].cameraSpaceReflectionVector, !original.pipeline.textureStages[i1].cameraSpaceReflectionVector);
	CHANGE_STATE(pipeline.textureStages[i1].textureTransformEnable, !original.pipeline.textureStages[i1].textureTransformEnable);
	CHANGE_STATE(pipeline.textureStages[i1].projectedCoordinates, !original.pipeline.textureStages[i1].projectedCoordinates);
	CHANGE_STATE(pipeline.textureStages[i1].textureTransformCount, static_cast<unsigned int>(original.pipeline.textureStages[i1].textureTransformCount + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentMatrix00, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentMatrix00 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentMatrix01, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentMatrix01 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentMatrix10, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentMatrix10 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentMatrix11, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentMatrix11 + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentLuminanceScale, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentLuminanceScale + 1));
	CHANGE_STATE(pipeline.textureStages[i1].bumpEnvironmentLuminanceOffset, static_cast<float>(original.pipeline.textureStages[i1].bumpEnvironmentLuminanceOffset + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.addressU, static_cast<RenderTextureAddressMode>(original.pipeline.textureStages[i1].sampler.addressU + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.addressV, static_cast<RenderTextureAddressMode>(original.pipeline.textureStages[i1].sampler.addressV + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.addressW, static_cast<RenderTextureAddressMode>(original.pipeline.textureStages[i1].sampler.addressW + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.minification, static_cast<RenderTextureFilter>(original.pipeline.textureStages[i1].sampler.minification + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.magnification, static_cast<RenderTextureFilter>(original.pipeline.textureStages[i1].sampler.magnification + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.mipmapping, static_cast<RenderTextureFilter>(original.pipeline.textureStages[i1].sampler.mipmapping + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.maximumAnisotropy, static_cast<unsigned int>(original.pipeline.textureStages[i1].sampler.maximumAnisotropy + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.maximumMipLevel, static_cast<unsigned int>(original.pipeline.textureStages[i1].sampler.maximumMipLevel + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.mipLodBias, static_cast<float>(original.pipeline.textureStages[i1].sampler.mipLodBias + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.borderColor.x, static_cast<float>(original.pipeline.textureStages[i1].sampler.borderColor.x + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.borderColor.y, static_cast<float>(original.pipeline.textureStages[i1].sampler.borderColor.y + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.borderColor.z, static_cast<float>(original.pipeline.textureStages[i1].sampler.borderColor.z + 1));
	CHANGE_STATE(pipeline.textureStages[i1].sampler.borderColor.w, static_cast<float>(original.pipeline.textureStages[i1].sampler.borderColor.w + 1));
	}
	CHANGE_STATE(pipeline.fogMode, static_cast<RenderFogMode>(original.pipeline.fogMode + 1));
	CHANGE_STATE(pipeline.rangeFogEnable, !original.pipeline.rangeFogEnable);
	CHANGE_STATE(pipeline.secondaryGradientEnable, !original.pipeline.secondaryGradientEnable);
	CHANGE_STATE(pipeline.nPatchEnable, !original.pipeline.nPatchEnable);
	CHANGE_STATE(pipeline.lightingEnable, !original.pipeline.lightingEnable);
	CHANGE_STATE(pipeline.normalizeNormals, !original.pipeline.normalizeNormals);
	CHANGE_STATE(pipeline.alphaTestEnable, !original.pipeline.alphaTestEnable);
	CHANGE_STATE(pipeline.alphaFunction, static_cast<RenderCompareFunction>(original.pipeline.alphaFunction + 1));
	CHANGE_STATE(pipeline.alphaReference, static_cast<unsigned int>(original.pipeline.alphaReference + 1));
	CHANGE_STATE(pipeline.textureFactor, static_cast<unsigned int>(original.pipeline.textureFactor + 1));
	CHANGE_STATE(pipeline.clipPlaneEnableMask, static_cast<unsigned int>(original.pipeline.clipPlaneEnableMask + 1));
	CHANGE_STATE(pipeline.ambientMaterialSource, static_cast<RenderMaterialSource>(original.pipeline.ambientMaterialSource + 1));
	CHANGE_STATE(pipeline.diffuseMaterialSource, static_cast<RenderMaterialSource>(original.pipeline.diffuseMaterialSource + 1));
	CHANGE_STATE(pipeline.emissiveMaterialSource, static_cast<RenderMaterialSource>(original.pipeline.emissiveMaterialSource + 1));
	CHANGE_STATE(pipeline.specularMaterialSource, static_cast<RenderMaterialSource>(original.pipeline.specularMaterialSource + 1));
	for (unsigned int i2 = 0; i2 < 16; ++i2) {
	CHANGE_STATE(constants.view.values[i2], static_cast<float>(original.constants.view.values[i2] + 1));
	}
	for (unsigned int i2 = 0; i2 < 16; ++i2) {
	CHANGE_STATE(constants.projection.values[i2], static_cast<float>(original.constants.projection.values[i2] + 1));
	}
	for (unsigned int i1 = 0; i1 < LEGACY_TEXTURE_STAGE_COUNT; ++i1) {
	for (unsigned int i2 = 0; i2 < 16; ++i2) {
	CHANGE_STATE(constants.textureTransforms[i1].values[i2], static_cast<float>(original.constants.textureTransforms[i1].values[i2] + 1));
	}
	}
	CHANGE_STATE(constants.material.diffuse.x, static_cast<float>(original.constants.material.diffuse.x + 1));
	CHANGE_STATE(constants.material.diffuse.y, static_cast<float>(original.constants.material.diffuse.y + 1));
	CHANGE_STATE(constants.material.diffuse.z, static_cast<float>(original.constants.material.diffuse.z + 1));
	CHANGE_STATE(constants.material.diffuse.w, static_cast<float>(original.constants.material.diffuse.w + 1));
	CHANGE_STATE(constants.material.ambient.x, static_cast<float>(original.constants.material.ambient.x + 1));
	CHANGE_STATE(constants.material.ambient.y, static_cast<float>(original.constants.material.ambient.y + 1));
	CHANGE_STATE(constants.material.ambient.z, static_cast<float>(original.constants.material.ambient.z + 1));
	CHANGE_STATE(constants.material.ambient.w, static_cast<float>(original.constants.material.ambient.w + 1));
	CHANGE_STATE(constants.material.specular.x, static_cast<float>(original.constants.material.specular.x + 1));
	CHANGE_STATE(constants.material.specular.y, static_cast<float>(original.constants.material.specular.y + 1));
	CHANGE_STATE(constants.material.specular.z, static_cast<float>(original.constants.material.specular.z + 1));
	CHANGE_STATE(constants.material.specular.w, static_cast<float>(original.constants.material.specular.w + 1));
	CHANGE_STATE(constants.material.emissive.x, static_cast<float>(original.constants.material.emissive.x + 1));
	CHANGE_STATE(constants.material.emissive.y, static_cast<float>(original.constants.material.emissive.y + 1));
	CHANGE_STATE(constants.material.emissive.z, static_cast<float>(original.constants.material.emissive.z + 1));
	CHANGE_STATE(constants.material.emissive.w, static_cast<float>(original.constants.material.emissive.w + 1));
	CHANGE_STATE(constants.material.specularPower, static_cast<float>(original.constants.material.specularPower + 1));
	for (unsigned int i1 = 0; i1 < LEGACY_LIGHT_COUNT; ++i1) {
	CHANGE_STATE(constants.lights[i1].enabled, !original.constants.lights[i1].enabled);
	CHANGE_STATE(constants.lights[i1].type, static_cast<RenderLightType>(original.constants.lights[i1].type + 1));
	CHANGE_STATE(constants.lights[i1].diffuse.x, static_cast<float>(original.constants.lights[i1].diffuse.x + 1));
	CHANGE_STATE(constants.lights[i1].diffuse.y, static_cast<float>(original.constants.lights[i1].diffuse.y + 1));
	CHANGE_STATE(constants.lights[i1].diffuse.z, static_cast<float>(original.constants.lights[i1].diffuse.z + 1));
	CHANGE_STATE(constants.lights[i1].diffuse.w, static_cast<float>(original.constants.lights[i1].diffuse.w + 1));
	CHANGE_STATE(constants.lights[i1].specular.x, static_cast<float>(original.constants.lights[i1].specular.x + 1));
	CHANGE_STATE(constants.lights[i1].specular.y, static_cast<float>(original.constants.lights[i1].specular.y + 1));
	CHANGE_STATE(constants.lights[i1].specular.z, static_cast<float>(original.constants.lights[i1].specular.z + 1));
	CHANGE_STATE(constants.lights[i1].specular.w, static_cast<float>(original.constants.lights[i1].specular.w + 1));
	CHANGE_STATE(constants.lights[i1].ambient.x, static_cast<float>(original.constants.lights[i1].ambient.x + 1));
	CHANGE_STATE(constants.lights[i1].ambient.y, static_cast<float>(original.constants.lights[i1].ambient.y + 1));
	CHANGE_STATE(constants.lights[i1].ambient.z, static_cast<float>(original.constants.lights[i1].ambient.z + 1));
	CHANGE_STATE(constants.lights[i1].ambient.w, static_cast<float>(original.constants.lights[i1].ambient.w + 1));
	CHANGE_STATE(constants.lights[i1].position.x, static_cast<float>(original.constants.lights[i1].position.x + 1));
	CHANGE_STATE(constants.lights[i1].position.y, static_cast<float>(original.constants.lights[i1].position.y + 1));
	CHANGE_STATE(constants.lights[i1].position.z, static_cast<float>(original.constants.lights[i1].position.z + 1));
	CHANGE_STATE(constants.lights[i1].position.w, static_cast<float>(original.constants.lights[i1].position.w + 1));
	CHANGE_STATE(constants.lights[i1].direction.x, static_cast<float>(original.constants.lights[i1].direction.x + 1));
	CHANGE_STATE(constants.lights[i1].direction.y, static_cast<float>(original.constants.lights[i1].direction.y + 1));
	CHANGE_STATE(constants.lights[i1].direction.z, static_cast<float>(original.constants.lights[i1].direction.z + 1));
	CHANGE_STATE(constants.lights[i1].direction.w, static_cast<float>(original.constants.lights[i1].direction.w + 1));
	CHANGE_STATE(constants.lights[i1].range, static_cast<float>(original.constants.lights[i1].range + 1));
	CHANGE_STATE(constants.lights[i1].falloff, static_cast<float>(original.constants.lights[i1].falloff + 1));
	CHANGE_STATE(constants.lights[i1].attenuation0, static_cast<float>(original.constants.lights[i1].attenuation0 + 1));
	CHANGE_STATE(constants.lights[i1].attenuation1, static_cast<float>(original.constants.lights[i1].attenuation1 + 1));
	CHANGE_STATE(constants.lights[i1].attenuation2, static_cast<float>(original.constants.lights[i1].attenuation2 + 1));
	CHANGE_STATE(constants.lights[i1].theta, static_cast<float>(original.constants.lights[i1].theta + 1));
	CHANGE_STATE(constants.lights[i1].phi, static_cast<float>(original.constants.lights[i1].phi + 1));
	}
	CHANGE_STATE(constants.fog.enabled, !original.constants.fog.enabled);
	CHANGE_STATE(constants.fog.color.x, static_cast<float>(original.constants.fog.color.x + 1));
	CHANGE_STATE(constants.fog.color.y, static_cast<float>(original.constants.fog.color.y + 1));
	CHANGE_STATE(constants.fog.color.z, static_cast<float>(original.constants.fog.color.z + 1));
	CHANGE_STATE(constants.fog.color.w, static_cast<float>(original.constants.fog.color.w + 1));
	CHANGE_STATE(constants.fog.start, static_cast<float>(original.constants.fog.start + 1));
	CHANGE_STATE(constants.fog.end, static_cast<float>(original.constants.fog.end + 1));
	CHANGE_STATE(constants.fog.density, static_cast<float>(original.constants.fog.density + 1));
	CHANGE_STATE(constants.globalAmbient.x, static_cast<float>(original.constants.globalAmbient.x + 1));
	CHANGE_STATE(constants.globalAmbient.y, static_cast<float>(original.constants.globalAmbient.y + 1));
	CHANGE_STATE(constants.globalAmbient.z, static_cast<float>(original.constants.globalAmbient.z + 1));
	CHANGE_STATE(constants.globalAmbient.w, static_cast<float>(original.constants.globalAmbient.w + 1));
	for (unsigned int i1 = 0; i1 < LEGACY_CLIP_PLANE_COUNT; ++i1) {
	CHANGE_STATE(constants.clipPlanes[i1].x, static_cast<float>(original.constants.clipPlanes[i1].x + 1));
	CHANGE_STATE(constants.clipPlanes[i1].y, static_cast<float>(original.constants.clipPlanes[i1].y + 1));
	CHANGE_STATE(constants.clipPlanes[i1].z, static_cast<float>(original.constants.clipPlanes[i1].z + 1));
	CHANGE_STATE(constants.clipPlanes[i1].w, static_cast<float>(original.constants.clipPlanes[i1].w + 1));
	}
	for (unsigned int i1 = 0; i1 < LEGACY_VERTEX_CONSTANT_COUNT; ++i1) {
	CHANGE_STATE(constants.vertexShaderConstants[i1].x, static_cast<float>(original.constants.vertexShaderConstants[i1].x + 1));
	CHANGE_STATE(constants.vertexShaderConstants[i1].y, static_cast<float>(original.constants.vertexShaderConstants[i1].y + 1));
	CHANGE_STATE(constants.vertexShaderConstants[i1].z, static_cast<float>(original.constants.vertexShaderConstants[i1].z + 1));
	CHANGE_STATE(constants.vertexShaderConstants[i1].w, static_cast<float>(original.constants.vertexShaderConstants[i1].w + 1));
	}
	for (unsigned int i1 = 0; i1 < LEGACY_PIXEL_CONSTANT_COUNT; ++i1) {
	CHANGE_STATE(constants.pixelShaderConstants[i1].x, static_cast<float>(original.constants.pixelShaderConstants[i1].x + 1));
	CHANGE_STATE(constants.pixelShaderConstants[i1].y, static_cast<float>(original.constants.pixelShaderConstants[i1].y + 1));
	CHANGE_STATE(constants.pixelShaderConstants[i1].z, static_cast<float>(original.constants.pixelShaderConstants[i1].z + 1));
	CHANGE_STATE(constants.pixelShaderConstants[i1].w, static_cast<float>(original.constants.pixelShaderConstants[i1].w + 1));
	}
	CHANGE_STATE(texturePresenceMask, static_cast<unsigned int>(original.texturePresenceMask + 1));
#undef CHANGE_STATE
	changed = original; changed.constants.world.values[12] = 100;
	Check(RigidInstancingSharedStateEqual(original, changed), "world alone is per instance");
	changed = original; changed.constants.globalAmbient.x = -0.0f;
	original.constants.globalAmbient.x = 0.0f;
	Check(!RigidInstancingSharedStateEqual(original, changed), "signed zero bits differ");
	unsigned int nanBits = 0x7fc01234U;
	std::memcpy(&original.constants.lights[0].range, &nanBits, sizeof(float));
	changed = original;
	Check(RigidInstancingSharedStateEqual(original, changed), "identical disabled-light NaN payload compares equal");
	++nanBits; std::memcpy(&changed.constants.lights[0].range, &nanBits, sizeof(float));
	Check(!RigidInstancingSharedStateEqual(original, changed), "disabled-light NaN payload mismatch splits");
	LegacyBlendState a, b;
	unsigned char *bytesA = reinterpret_cast<unsigned char *>(&a);
	unsigned char *bytesB = reinterpret_cast<unsigned char *>(&b);
	for (size_t i = offsetof(LegacyBlendState, blendEnable) + sizeof(bool);
		i < offsetof(LegacyBlendState, sourceColor); ++i) {
		bytesA[i] = 0x19; bytesB[i] = 0x93;
	}
	Check(rigid_instancing_detail::Equal(a, b), "pipeline padding has no effect");
}
void TestEveryPacketField()
{
	NativeDrawPacket original = Packet(), changed;
#define CHANGE_PACKET(field, value) changed = original; changed.field = value; \
	Check(!RigidInstancingPacketEqual(original, changed), #field)
	CHANGE_PACKET(vertexBuffer, GpuHandle(original.vertexBuffer.index(), original.vertexBuffer.generation() + 1));
	CHANGE_PACKET(indexBuffer, GpuHandle(original.indexBuffer.index(), original.indexBuffer.generation() + 1));
	for (unsigned int i0 = 0; i0 < LEGACY_TEXTURE_STAGE_COUNT; ++i0) {
	CHANGE_PACKET(textures[i0], GpuHandle(original.textures[i0].index(), original.textures[i0].generation() + 1));
	}
	CHANGE_PACKET(vertexStride, static_cast<unsigned int>(original.vertexStride + 1));
	CHANGE_PACKET(vertexOffset, static_cast<unsigned int>(original.vertexOffset + 1));
	CHANGE_PACKET(indexOffset, static_cast<unsigned int>(original.indexOffset + 1));
	CHANGE_PACKET(indexFormat, static_cast<RenderFormat>(original.indexFormat + 1));
	CHANGE_PACKET(vertexFormat, static_cast<RenderVertexFormat>(original.vertexFormat + 1));
	CHANGE_PACKET(vertexLayout.stride, static_cast<unsigned int>(original.vertexLayout.stride + 1));
	CHANGE_PACKET(vertexLayout.elementCount, static_cast<unsigned int>(original.vertexLayout.elementCount + 1));
	CHANGE_PACKET(vertexLayout.preTransformed, !original.vertexLayout.preTransformed);
	for (unsigned int i1 = 0; i1 < RenderVertexLayout::MAX_ELEMENT_COUNT; ++i1) {
	CHANGE_PACKET(vertexLayout.elements[i1].semantic, static_cast<RenderVertexSemantic>(original.vertexLayout.elements[i1].semantic + 1));
	CHANGE_PACKET(vertexLayout.elements[i1].semanticIndex, static_cast<unsigned int>(original.vertexLayout.elements[i1].semanticIndex + 1));
	CHANGE_PACKET(vertexLayout.elements[i1].format, static_cast<RenderVertexDataFormat>(original.vertexLayout.elements[i1].format + 1));
	CHANGE_PACKET(vertexLayout.elements[i1].byteOffset, static_cast<unsigned int>(original.vertexLayout.elements[i1].byteOffset + 1));
	}
	CHANGE_PACKET(topology, static_cast<RenderPrimitiveTopology>(original.topology + 1));
	CHANGE_PACKET(texturePresenceMask, static_cast<unsigned int>(original.texturePresenceMask + 1));
	CHANGE_PACKET(vertexCount, static_cast<unsigned int>(original.vertexCount + 1));
	CHANGE_PACKET(startVertex, static_cast<unsigned int>(original.startVertex + 1));
	CHANGE_PACKET(indexCount, static_cast<unsigned int>(original.indexCount + 1));
	CHANGE_PACKET(startIndex, static_cast<unsigned int>(original.startIndex + 1));
	CHANGE_PACKET(minimumVertexIndex, static_cast<unsigned int>(original.minimumVertexIndex + 1));
	CHANGE_PACKET(baseVertex, static_cast<int>(original.baseVertex + 1));
	CHANGE_PACKET(indexed, !original.indexed);
#undef CHANGE_PACKET
}
void TestValidatedNormalOutput()
{
	RenderMatrix4 worlds[4];
	// Distinct valid transforms exercise the accepted ordinary arithmetic and
	// padding, including nonsymmetric cofactors and translated finite lanes.
	worlds[1].values[1] = 0.25f;
	worlds[1].values[6] = -0.5f;
	worlds[1].values[8] = 0.125f;
	worlds[2].values[0] = 2.0f;
	worlds[2].values[5] = 4.0f;
	worlds[2].values[10] = 0.5f;
	worlds[3].values[12] = -7.25f;
	worlds[3].values[13] = 3.5f;
	worlds[3].values[14] = 125.0f;
	for (unsigned int i = 0; i < 4; ++i)
	{
		float expected[12];
		struct GuardedOutput { float before; float normal[12]; float after; };
		GuardedOutput actual;
		actual.before = 37.0f; actual.after = -91.0f;
		for (unsigned int lane = 0; lane < 12; ++lane)
			actual.normal[lane] = 123.0f;
		Check(BuildLegacyInverseTransposeNormalMatrix(worlds[i], expected),
			"ordinary builder accepts identity/shear/nonuniform/translation");
		Check(RigidInstancingWorldValid(worlds[i], actual.normal) &&
			RigidInstancingWorldValid(worlds[i]),
			"both validator signatures accept valid ordinary transforms");
		Check(std::memcmp(expected, actual.normal, sizeof(expected)) == 0,
			"validated output matches all twelve ordinary float bytes");
		Check(actual.normal[3] == 0.0f && actual.normal[7] == 0.0f &&
			actual.normal[11] == 0.0f,
			"validated upload preserves all three normal padding lanes");
		Check(actual.before == 37.0f && actual.after == -91.0f,
			"overload writes exactly twelve floats");
		if (i == 0)
			Check(actual.normal[0] == 1.0f && actual.normal[5] == 1.0f &&
				actual.normal[10] == 1.0f, "identity has unit normal diagonal");
		if (i == 2)
			Check(actual.normal[0] == 0.5f && actual.normal[5] == 0.25f &&
				actual.normal[10] == 2.0f, "nonuniform scale produces inverse diagonal");
	}
	float normal[12];
	RenderMatrix4 singular;
	singular.values[0] = 0.0f;
	Check(!RigidInstancingWorldValid(singular, normal) &&
		!RigidInstancingWorldValid(singular),
		"both signatures reject singular transforms");
	RenderMatrix4 identity;
	Check(!RigidInstancingWorldValid(identity, 0), "null output is rejected");
	// Include homogeneous and translation lanes: the normal builder itself
	// only reads the upper 3x3, but admission checks the entire source world.
	const unsigned int invalidBits[] = { 0x7fc01234U, 0x7f800000U, 0xff800000U };
	for (unsigned int pattern = 0; pattern < 3; ++pattern)
	{
		for (unsigned int lane = 0; lane < 16; ++lane)
		{
			RenderMatrix4 invalid;
			std::memcpy(&invalid.values[lane], &invalidBits[pattern], sizeof(float));
			Check(!RigidInstancingWorldValid(invalid, normal) &&
				!RigidInstancingWorldValid(invalid),
				"both signatures reject NaN and signed infinity in each of sixteen lanes");
		}
	}
	RenderMatrix4 overflow;
	overflow.values[0] = FLT_MAX;
	overflow.values[5] = FLT_MAX;
	Check(!RigidInstancingWorldValid(overflow, normal) &&
		!RigidInstancingWorldValid(overflow),
		"finite worlds producing nonfinite normal output remain rejected");
}

void TestExactNoDiffuseDeclaration()
{
	NativeDrawPacket packet = Packet();
	packet.vertexStride = packet.vertexLayout.stride = 32;
	packet.vertexLayout.elementCount = 3;
	packet.vertexLayout.elements[2] = packet.vertexLayout.elements[3];
	packet.vertexLayout.elements[2].byteOffset = 24;
	LegacyLogicalState state;
	Check(IsRigidInstancingLayout(packet.vertexLayout) &&
		RigidInstancingDrawEligible(packet, state),
		"exact position normal UV1 no-diffuse32 is admitted with producer compatibility enum");
	NativeDrawPacket colored = Packet();
	Check(IsRigidInstancingLayout(colored.vertexLayout) &&
		RigidInstancingDrawEligible(colored, state), "colored36 admission preserved");
	Check(!RigidInstancingPacketEqual(packet, colored) &&
		!RigidInstancingCompatible(packet, state, 1, colored, state, 1),
		"distinct admitted declarations never share a batch");
	for (unsigned int element = 0; element < 3; ++element)
	{
		NativeDrawPacket bad = packet;
		bad.vertexLayout.elements[element].semantic = RENDER_VERTEX_SEMANTIC_SPECULAR;
		Check(!IsRigidInstancingLayout(bad.vertexLayout), "every no-diffuse semantic is exact");
		bad = packet; bad.vertexLayout.elements[element].semanticIndex = 1;
		Check(!IsRigidInstancingLayout(bad.vertexLayout), "every no-diffuse semantic index must be zero");
		bad = packet; bad.vertexLayout.elements[element].format = RENDER_VERTEX_DATA_FLOAT4;
		Check(!IsRigidInstancingLayout(bad.vertexLayout), "every no-diffuse data format is exact");
		bad = packet; ++bad.vertexLayout.elements[element].byteOffset;
		Check(!IsRigidInstancingLayout(bad.vertexLayout), "every no-diffuse byte offset is exact");
	}
	NativeDrawPacket bad = packet;
	bad.vertexLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
	Check(!RigidInstancingDrawEligible(bad, state), "no-diffuse extension excludes weights");
	bad = packet; bad.vertexLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_BLEND_INDEX;
	Check(!RigidInstancingDrawEligible(bad, state), "no-diffuse extension excludes blend indices");
	bad = packet; bad.vertexLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	Check(!RigidInstancingDrawEligible(bad, state), "no-normal declaration remains excluded");
	bad = packet; bad.vertexLayout.elements[3] = packet.vertexLayout.elements[2];
	bad.vertexLayout.elements[3].semanticIndex = 1;
	bad.vertexLayout.elements[3].byteOffset = 32;
	bad.vertexStride = bad.vertexLayout.stride = 40; bad.vertexLayout.elementCount = 4;
	Check(!RigidInstancingDrawEligible(bad, state), "TEX2 remains excluded");
	bad = packet; bad.vertexStride = bad.vertexLayout.stride = 36;
	Check(!RigidInstancingDrawEligible(bad, state), "padded no-diffuse36 near miss is excluded");
	bad = packet; bad.vertexStride = bad.vertexLayout.stride = 31;
	Check(!RigidInstancingDrawEligible(bad, state), "truncated no-diffuse near miss is excluded");
	bad = packet; bad.vertexStride = 36;
	Check(!RigidInstancingDrawEligible(bad, state), "packet stride must match exact admitted declaration");
	bad = colored; bad.vertexStride = 32;
	Check(!RigidInstancingDrawEligible(bad, state), "colored packet stride mismatch remains excluded");
	bad = packet; bad.vertexLayout.elementCount = 2;
	Check(!RigidInstancingDrawEligible(bad, state), "missing UV declaration remains excluded");
	bad = packet; bad.vertexLayout.preTransformed = true;
	Check(!RigidInstancingDrawEligible(bad, state), "no-diffuse pretransformed declaration remains excluded");
	for (unsigned int family = 0; family < 2; ++family)
	{
		const NativeDrawPacket admitted = family == 0 ? packet : colored;
		bad = admitted; bad.indexed = false;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts still require indexed draws");
		bad = admitted; bad.indexCount = 0;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts still require nonempty indices");
		bad = admitted; bad.vertexCount = 0;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts still require nonempty declared vertices");
		bad = admitted; bad.vertexFormat = RENDER_VERTEX_POSITION3_COLOR;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts retain producer compatibility format gate");
		bad = admitted; bad.topology = RENDER_PRIMITIVE_TRIANGLE_STRIP;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts still exclude strips");
		bad = admitted; bad.topology = RENDER_PRIMITIVE_LINE_LIST;
		Check(!RigidInstancingDrawEligible(bad, state), "both layouts still exclude lines");
		LegacyLogicalState changed = state; changed.pipeline.blend.blendEnable = true;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude blending");
		changed = state; changed.pipeline.alphaTestEnable = true;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude alpha testing");
		changed = state; changed.pipeline.nPatchEnable = true;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude NPatch");
		changed = state; changed.pipeline.vertexProgram = RENDER_LEGACY_VERTEX_TREES;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude custom vertex programs");
		changed = state; changed.pipeline.pixelProgram = RENDER_LEGACY_PIXEL_WATER_FLAT;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude custom pixel programs");
		changed = state; changed.constants.world.values[0] = 0.0f;
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude singular worlds");
		changed = state;
		unsigned int infinity = 0x7f800000U;
		std::memcpy(&changed.constants.world.values[12], &infinity, sizeof(float));
		Check(!RigidInstancingDrawEligible(admitted, changed), "both layouts still exclude nonfinite worlds");
		changed = state; changed.constants.material.diffuse.x = 0.5f;
		Check(!RigidInstancingCompatible(admitted, state, 1, admitted, changed, 1),
			"both layouts still split material state mismatch");
		Check(!RigidInstancingCompatible(admitted, state, 1, admitted, state, 2),
			"both layouts still split context epoch mismatch");
	}
}

void TestEligibilityAndEpoch()
{
	NativeDrawPacket packet = Packet(), other = packet;
	LegacyLogicalState state, otherState = state;
	Check(RigidInstancingDrawEligible(packet, state), "canonical rigid opaque draw is eligible");
	otherState.constants.world.values[12] = 2;
	Check(RigidInstancingCompatible(packet, state, 1, other, otherState, 1),
		"distinct worlds with common final state are compatible");
	Check(!RigidInstancingCompatible(packet, state, 1, other, otherState, 2),
		"context epoch mutation splits otherwise identical packets");
	otherState.pipeline.blend.blendEnable = true;
	Check(!RigidInstancingDrawEligible(packet, otherState), "transparent draw rejected");
	otherState = state; otherState.pipeline.alphaTestEnable = true;
	Check(!RigidInstancingDrawEligible(packet, otherState), "alpha test rejected");
	otherState = state; otherState.pipeline.vertexProgram = RENDER_LEGACY_VERTEX_TREES;
	Check(!RigidInstancingDrawEligible(packet, otherState), "custom vertex program rejected");
	otherState = state; otherState.constants.world.values[0] = 0;
	Check(!RigidInstancingDrawEligible(packet, otherState), "singular world rejected");
	otherState = state; otherState.constants.world.values[0] = FLT_MAX;
	otherState.constants.world.values[5] = FLT_MAX;
	Check(!RigidInstancingWorldValid(otherState.constants.world), "overflowing normal calculation rejected");
	unsigned int infinity = 0x7f800000U;
	std::memcpy(&otherState.constants.world.values[12], &infinity, sizeof(float));
	Check(!RigidInstancingWorldValid(otherState.constants.world), "nonfinite translation rejected");
	other = packet; other.vertexLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
	Check(!RigidInstancingDrawEligible(other, state), "weighted declaration rejected");
	other = packet; other.vertexLayout.preTransformed = true;
	Check(!RigidInstancingDrawEligible(other, state), "pretransformed declaration rejected");
	other = packet; other.topology = RENDER_PRIMITIVE_TRIANGLE_STRIP;
	Check(!RigidInstancingDrawEligible(other, state), "strip rejected");
}
}
int main()
{
	TestEverySharedField(); TestEveryPacketField(); TestValidatedNormalOutput(); TestExactNoDiffuseDeclaration(); TestEligibilityAndEpoch();
	return failures == 0 ? 0 : 1;
}
