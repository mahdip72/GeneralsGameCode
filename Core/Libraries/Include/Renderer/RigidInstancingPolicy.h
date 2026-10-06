#ifndef RTS_RENDERER_RIGIDINSTANCINGPOLICY_H
#define RTS_RENDERER_RIGIDINSTANCINGPOLICY_H

#include "Renderer/NativeW3DRenderer.h"
#include <float.h>
#include <string.h>

namespace rts { namespace render {
namespace rigid_instancing_detail {
// Byte-compare scalar floats only, never aggregate padding. Preserve signed
// zero and NaN payloads in shared state, including disabled lights.
inline bool Equal(const float &a, const float &b) { return memcmp(&a, &b, sizeof(float)) == 0; }
template <class T> inline bool Equal(const T &a, const T &b) { return a == b; }
inline bool Equal(const RenderFloat4 &a, const RenderFloat4 &b)
{
	if (!Equal(a.x, b.x)) return false;
	if (!Equal(a.y, b.y)) return false;
	if (!Equal(a.z, b.z)) return false;
	if (!Equal(a.w, b.w)) return false;
	return true;
}
inline bool Equal(const RenderMatrix4 &a, const RenderMatrix4 &b)
{
	for (unsigned int i = 0; i < 16; ++i)
		if (!Equal(a.values[i], b.values[i])) return false;
	return true;
}
inline bool Equal(const LegacyBlendState &a, const LegacyBlendState &b)
{
	if (!Equal(a.blendEnable, b.blendEnable)) return false;
	if (!Equal(a.sourceColor, b.sourceColor)) return false;
	if (!Equal(a.destinationColor, b.destinationColor)) return false;
	if (!Equal(a.colorOperation, b.colorOperation)) return false;
	if (!Equal(a.sourceAlpha, b.sourceAlpha)) return false;
	if (!Equal(a.destinationAlpha, b.destinationAlpha)) return false;
	if (!Equal(a.alphaOperation, b.alphaOperation)) return false;
	if (!Equal(a.colorWriteMask, b.colorWriteMask)) return false;
	return true;
}
inline bool Equal(const LegacyDepthStencilState &a, const LegacyDepthStencilState &b)
{
	if (!Equal(a.depthEnable, b.depthEnable)) return false;
	if (!Equal(a.depthWrite, b.depthWrite)) return false;
	if (!Equal(a.depthFunction, b.depthFunction)) return false;
	if (!Equal(a.stencilEnable, b.stencilEnable)) return false;
	if (!Equal(a.stencilReadMask, b.stencilReadMask)) return false;
	if (!Equal(a.stencilWriteMask, b.stencilWriteMask)) return false;
	if (!Equal(a.stencilReference, b.stencilReference)) return false;
	if (!Equal(a.stencilFunction, b.stencilFunction)) return false;
	if (!Equal(a.stencilFail, b.stencilFail)) return false;
	if (!Equal(a.stencilDepthFail, b.stencilDepthFail)) return false;
	if (!Equal(a.stencilPass, b.stencilPass)) return false;
	return true;
}
inline bool Equal(const LegacyRasterizerState &a, const LegacyRasterizerState &b)
{
	if (!Equal(a.fillMode, b.fillMode)) return false;
	if (!Equal(a.cullMode, b.cullMode)) return false;
	if (!Equal(a.frontCounterClockwise, b.frontCounterClockwise)) return false;
	if (!Equal(a.scissorEnable, b.scissorEnable)) return false;
	if (!Equal(a.depthBias, b.depthBias)) return false;
	if (!Equal(a.slopeScaledDepthBias, b.slopeScaledDepthBias)) return false;
	return true;
}
inline bool Equal(const LegacySamplerState &a, const LegacySamplerState &b)
{
	if (!Equal(a.addressU, b.addressU)) return false;
	if (!Equal(a.addressV, b.addressV)) return false;
	if (!Equal(a.addressW, b.addressW)) return false;
	if (!Equal(a.minification, b.minification)) return false;
	if (!Equal(a.magnification, b.magnification)) return false;
	if (!Equal(a.mipmapping, b.mipmapping)) return false;
	if (!Equal(a.maximumAnisotropy, b.maximumAnisotropy)) return false;
	if (!Equal(a.maximumMipLevel, b.maximumMipLevel)) return false;
	if (!Equal(a.mipLodBias, b.mipLodBias)) return false;
	if (!Equal(a.borderColor, b.borderColor)) return false;
	return true;
}
inline bool Equal(const LegacyTextureStageState &a, const LegacyTextureStageState &b)
{
	if (!Equal(a.colorOperation, b.colorOperation)) return false;
	if (!Equal(a.colorArgument0, b.colorArgument0)) return false;
	if (!Equal(a.colorArgument1, b.colorArgument1)) return false;
	if (!Equal(a.colorArgument2, b.colorArgument2)) return false;
	if (!Equal(a.alphaOperation, b.alphaOperation)) return false;
	if (!Equal(a.alphaArgument0, b.alphaArgument0)) return false;
	if (!Equal(a.alphaArgument1, b.alphaArgument1)) return false;
	if (!Equal(a.alphaArgument2, b.alphaArgument2)) return false;
	if (!Equal(a.colorArgument0Complement, b.colorArgument0Complement)) return false;
	if (!Equal(a.colorArgument0AlphaReplicate, b.colorArgument0AlphaReplicate)) return false;
	if (!Equal(a.colorArgument1Complement, b.colorArgument1Complement)) return false;
	if (!Equal(a.colorArgument1AlphaReplicate, b.colorArgument1AlphaReplicate)) return false;
	if (!Equal(a.colorArgument2Complement, b.colorArgument2Complement)) return false;
	if (!Equal(a.colorArgument2AlphaReplicate, b.colorArgument2AlphaReplicate)) return false;
	if (!Equal(a.alphaArgument0Complement, b.alphaArgument0Complement)) return false;
	if (!Equal(a.alphaArgument0AlphaReplicate, b.alphaArgument0AlphaReplicate)) return false;
	if (!Equal(a.alphaArgument1Complement, b.alphaArgument1Complement)) return false;
	if (!Equal(a.alphaArgument1AlphaReplicate, b.alphaArgument1AlphaReplicate)) return false;
	if (!Equal(a.alphaArgument2Complement, b.alphaArgument2Complement)) return false;
	if (!Equal(a.alphaArgument2AlphaReplicate, b.alphaArgument2AlphaReplicate)) return false;
	if (!Equal(a.resultArgument, b.resultArgument)) return false;
	if (!Equal(a.textureCoordinateIndex, b.textureCoordinateIndex)) return false;
	if (!Equal(a.cameraSpacePosition, b.cameraSpacePosition)) return false;
	if (!Equal(a.cameraSpaceNormal, b.cameraSpaceNormal)) return false;
	if (!Equal(a.cameraSpaceReflectionVector, b.cameraSpaceReflectionVector)) return false;
	if (!Equal(a.textureTransformEnable, b.textureTransformEnable)) return false;
	if (!Equal(a.projectedCoordinates, b.projectedCoordinates)) return false;
	if (!Equal(a.textureTransformCount, b.textureTransformCount)) return false;
	if (!Equal(a.bumpEnvironmentMatrix00, b.bumpEnvironmentMatrix00)) return false;
	if (!Equal(a.bumpEnvironmentMatrix01, b.bumpEnvironmentMatrix01)) return false;
	if (!Equal(a.bumpEnvironmentMatrix10, b.bumpEnvironmentMatrix10)) return false;
	if (!Equal(a.bumpEnvironmentMatrix11, b.bumpEnvironmentMatrix11)) return false;
	if (!Equal(a.bumpEnvironmentLuminanceScale, b.bumpEnvironmentLuminanceScale)) return false;
	if (!Equal(a.bumpEnvironmentLuminanceOffset, b.bumpEnvironmentLuminanceOffset)) return false;
	if (!Equal(a.sampler, b.sampler)) return false;
	return true;
}
inline bool Equal(const LegacyPipelineState &a, const LegacyPipelineState &b)
{
	if (!Equal(a.shaderBits, b.shaderBits)) return false;
	if (!Equal(a.pixelProgram, b.pixelProgram)) return false;
	if (!Equal(a.vertexProgram, b.vertexProgram)) return false;
	if (!Equal(a.blend, b.blend)) return false;
	if (!Equal(a.depthStencil, b.depthStencil)) return false;
	if (!Equal(a.rasterizer, b.rasterizer)) return false;
	for (unsigned int i = 0; i < LEGACY_TEXTURE_STAGE_COUNT; ++i)
		if (!Equal(a.textureStages[i], b.textureStages[i])) return false;
	if (!Equal(a.fogMode, b.fogMode)) return false;
	if (!Equal(a.rangeFogEnable, b.rangeFogEnable)) return false;
	if (!Equal(a.secondaryGradientEnable, b.secondaryGradientEnable)) return false;
	if (!Equal(a.nPatchEnable, b.nPatchEnable)) return false;
	if (!Equal(a.lightingEnable, b.lightingEnable)) return false;
	if (!Equal(a.normalizeNormals, b.normalizeNormals)) return false;
	if (!Equal(a.alphaTestEnable, b.alphaTestEnable)) return false;
	if (!Equal(a.alphaFunction, b.alphaFunction)) return false;
	if (!Equal(a.alphaReference, b.alphaReference)) return false;
	if (!Equal(a.textureFactor, b.textureFactor)) return false;
	if (!Equal(a.clipPlaneEnableMask, b.clipPlaneEnableMask)) return false;
	if (!Equal(a.ambientMaterialSource, b.ambientMaterialSource)) return false;
	if (!Equal(a.diffuseMaterialSource, b.diffuseMaterialSource)) return false;
	if (!Equal(a.emissiveMaterialSource, b.emissiveMaterialSource)) return false;
	if (!Equal(a.specularMaterialSource, b.specularMaterialSource)) return false;
	return true;
}
inline bool Equal(const LegacyMaterialState &a, const LegacyMaterialState &b)
{
	if (!Equal(a.diffuse, b.diffuse)) return false;
	if (!Equal(a.ambient, b.ambient)) return false;
	if (!Equal(a.specular, b.specular)) return false;
	if (!Equal(a.emissive, b.emissive)) return false;
	if (!Equal(a.specularPower, b.specularPower)) return false;
	return true;
}
inline bool Equal(const LegacyLightState &a, const LegacyLightState &b)
{
	if (!Equal(a.enabled, b.enabled)) return false;
	if (!Equal(a.type, b.type)) return false;
	if (!Equal(a.diffuse, b.diffuse)) return false;
	if (!Equal(a.specular, b.specular)) return false;
	if (!Equal(a.ambient, b.ambient)) return false;
	if (!Equal(a.position, b.position)) return false;
	if (!Equal(a.direction, b.direction)) return false;
	if (!Equal(a.range, b.range)) return false;
	if (!Equal(a.falloff, b.falloff)) return false;
	if (!Equal(a.attenuation0, b.attenuation0)) return false;
	if (!Equal(a.attenuation1, b.attenuation1)) return false;
	if (!Equal(a.attenuation2, b.attenuation2)) return false;
	if (!Equal(a.theta, b.theta)) return false;
	if (!Equal(a.phi, b.phi)) return false;
	return true;
}
inline bool Equal(const LegacyFogConstants &a, const LegacyFogConstants &b)
{
	if (!Equal(a.enabled, b.enabled)) return false;
	if (!Equal(a.color, b.color)) return false;
	if (!Equal(a.start, b.start)) return false;
	if (!Equal(a.end, b.end)) return false;
	if (!Equal(a.density, b.density)) return false;
	return true;
}
inline bool Equal(const LegacyFixedFunctionConstants &a, const LegacyFixedFunctionConstants &b)
{
	if (!Equal(a.view, b.view)) return false;
	if (!Equal(a.projection, b.projection)) return false;
	for (unsigned int i = 0; i < LEGACY_TEXTURE_STAGE_COUNT; ++i)
		if (!Equal(a.textureTransforms[i], b.textureTransforms[i])) return false;
	if (!Equal(a.material, b.material)) return false;
	for (unsigned int i = 0; i < LEGACY_LIGHT_COUNT; ++i)
		if (!Equal(a.lights[i], b.lights[i])) return false;
	if (!Equal(a.fog, b.fog)) return false;
	if (!Equal(a.globalAmbient, b.globalAmbient)) return false;
	for (unsigned int i = 0; i < LEGACY_CLIP_PLANE_COUNT; ++i)
		if (!Equal(a.clipPlanes[i], b.clipPlanes[i])) return false;
	for (unsigned int i = 0; i < LEGACY_VERTEX_CONSTANT_COUNT; ++i)
		if (!Equal(a.vertexShaderConstants[i], b.vertexShaderConstants[i])) return false;
	for (unsigned int i = 0; i < LEGACY_PIXEL_CONSTANT_COUNT; ++i)
		if (!Equal(a.pixelShaderConstants[i], b.pixelShaderConstants[i])) return false;
	return true;
}
inline bool Equal(const LegacyLogicalState &a, const LegacyLogicalState &b)
{
	if (!Equal(a.pipeline, b.pipeline)) return false;
	if (!Equal(a.constants, b.constants)) return false;
	if (!Equal(a.texturePresenceMask, b.texturePresenceMask)) return false;
	return true;
}
inline bool Equal(const RenderVertexElement &a, const RenderVertexElement &b)
{
	if (!Equal(a.semantic, b.semantic)) return false;
	if (!Equal(a.semanticIndex, b.semanticIndex)) return false;
	if (!Equal(a.format, b.format)) return false;
	if (!Equal(a.byteOffset, b.byteOffset)) return false;
	return true;
}
inline bool Equal(const RenderVertexLayout &a, const RenderVertexLayout &b)
{
	if (!Equal(a.stride, b.stride)) return false;
	if (!Equal(a.elementCount, b.elementCount)) return false;
	if (!Equal(a.preTransformed, b.preTransformed)) return false;
	for (unsigned int i = 0; i < RenderVertexLayout::MAX_ELEMENT_COUNT; ++i)
		if (!Equal(a.elements[i], b.elements[i])) return false;
	return true;
}
inline bool Equal(const NativeDrawPacket &a, const NativeDrawPacket &b)
{
	if (!Equal(a.vertexBuffer, b.vertexBuffer)) return false;
	if (!Equal(a.indexBuffer, b.indexBuffer)) return false;
	for (unsigned int i = 0; i < LEGACY_TEXTURE_STAGE_COUNT; ++i)
		if (!Equal(a.textures[i], b.textures[i])) return false;
	if (!Equal(a.vertexStride, b.vertexStride)) return false;
	if (!Equal(a.vertexOffset, b.vertexOffset)) return false;
	if (!Equal(a.indexOffset, b.indexOffset)) return false;
	if (!Equal(a.indexFormat, b.indexFormat)) return false;
	if (!Equal(a.vertexFormat, b.vertexFormat)) return false;
	if (!Equal(a.vertexLayout, b.vertexLayout)) return false;
	if (!Equal(a.topology, b.topology)) return false;
	if (!Equal(a.texturePresenceMask, b.texturePresenceMask)) return false;
	if (!Equal(a.vertexCount, b.vertexCount)) return false;
	if (!Equal(a.startVertex, b.startVertex)) return false;
	if (!Equal(a.indexCount, b.indexCount)) return false;
	if (!Equal(a.startIndex, b.startIndex)) return false;
	if (!Equal(a.minimumVertexIndex, b.minimumVertexIndex)) return false;
	if (!Equal(a.baseVertex, b.baseVertex)) return false;
	if (!Equal(a.indexed, b.indexed)) return false;
	return true;
}
} // namespace rigid_instancing_detail

inline bool RigidInstancingPacketEqual(const NativeDrawPacket &a,
	const NativeDrawPacket &b)
{ return rigid_instancing_detail::Equal(a, b); }

inline bool RigidInstancingSharedStateEqual(const LegacyLogicalState &a,
	const LegacyLogicalState &b)
{ return rigid_instancing_detail::Equal(a, b); }

// Admit exactly the colored36 and no-diffuse32 unweighted declarations.
inline bool IsRigidInstancingLayout(const RenderVertexLayout &layout)
{
	const bool colored = layout.stride == 36 && layout.elementCount == 4;
	const bool noDiffuse = layout.stride == 32 && layout.elementCount == 3;
	if (layout.preTransformed || (!colored && !noDiffuse)) return false;
	const RenderVertexSemantic semantic[4] = { RENDER_VERTEX_SEMANTIC_POSITION,
		RENDER_VERTEX_SEMANTIC_NORMAL, RENDER_VERTEX_SEMANTIC_DIFFUSE,
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE };
	const RenderVertexDataFormat format[4] = { RENDER_VERTEX_DATA_FLOAT3,
		RENDER_VERTEX_DATA_FLOAT3, RENDER_VERTEX_DATA_COLOR_BGRA8,
		RENDER_VERTEX_DATA_FLOAT2 };
	const unsigned int offset[4] = { 0, 12, 24, 28 };
	for (unsigned int i = 0; i < layout.elementCount; ++i)
	{
		// The no-diffuse stream omits COLOR0 and packs TEXCOORD0 at24.
		const unsigned int declaration = noDiffuse && i == 2 ? 3 : i;
		const unsigned int byteOffset = noDiffuse && i == 2 ? 24 : offset[i];
		if (layout.elements[i].semantic != semantic[declaration] ||
			layout.elements[i].semanticIndex != 0 ||
			layout.elements[i].format != format[declaration] ||
			layout.elements[i].byteOffset != byteOffset) return false;
	}
	return true;
}

// Returns the exact padded ordinary normal upload while validating the world.
// Callers must use the output only on success. No arithmetic is duplicated or
// reordered relative to BuildLegacyInverseTransposeNormalMatrix.
inline bool RigidInstancingWorldValid(const RenderMatrix4 &world,
	float normal[12])
{
	if (normal == 0) return false;
	for (unsigned int i = 0; i < 16; ++i)
		if (!(world.values[i] <= FLT_MAX && world.values[i] >= -FLT_MAX))
			return false;
	if (!BuildLegacyInverseTransposeNormalMatrix(world, normal)) return false;
	for (unsigned int i = 0; i < 12; ++i)
		if (!(normal[i] <= FLT_MAX && normal[i] >= -FLT_MAX)) return false;
	return true;
}

inline bool RigidInstancingWorldValid(const RenderMatrix4 &world)
{
	float normal[12];
	return RigidInstancingWorldValid(world, normal);
}

inline bool RigidInstancingDrawEligible(const NativeDrawPacket &packet,
	const LegacyLogicalState &state)
{
	return packet.indexed && packet.indexCount != 0 &&
		packet.vertexCount != 0 && packet.vertexStride == packet.vertexLayout.stride &&
		packet.vertexFormat == RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1 &&
		packet.topology == RENDER_PRIMITIVE_TRIANGLE_LIST &&
		IsRigidInstancingLayout(packet.vertexLayout) &&
		state.pipeline.vertexProgram == RENDER_LEGACY_VERTEX_FIXED_FUNCTION &&
		state.pipeline.pixelProgram == RENDER_LEGACY_PIXEL_FIXED_FUNCTION &&
		!state.pipeline.blend.blendEnable && !state.pipeline.alphaTestEnable &&
		!state.pipeline.nPatchEnable && RigidInstancingWorldValid(state.constants.world);
}

inline bool RigidInstancingCompatible(const NativeDrawPacket &a,
	const LegacyLogicalState &stateA, uint64_t epochA,
	const NativeDrawPacket &b, const LegacyLogicalState &stateB, uint64_t epochB)
{
	return epochA == epochB && RigidInstancingPacketEqual(a, b) &&
		RigidInstancingSharedStateEqual(stateA, stateB);
}
} }
#endif
