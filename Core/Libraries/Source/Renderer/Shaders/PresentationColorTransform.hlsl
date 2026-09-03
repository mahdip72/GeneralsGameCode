cbuffer PresentationColorTransform : register(b0)
{
	float4 Parameters;
};

Texture2D SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);

struct PresentationVertex
{
	float4 position : SV_POSITION;
	float2 textureCoordinate : TEXCOORD0;
};

PresentationVertex VSMain(uint vertexId : SV_VertexID)
{
	// A single oversized triangle avoids a vertex buffer and covers the full
	// viewport.  The y coordinates deliberately match D3D11's top-left texture
	// origin so the postprocess does not introduce a vertical flip.
	static const float2 positions[3] = {
		float2(-1.0f, -1.0f), float2(-1.0f, 3.0f), float2(3.0f, -1.0f)
	};
	static const float2 textureCoordinates[3] = {
		float2(0.0f, 1.0f), float2(0.0f, -1.0f), float2(2.0f, 1.0f)
	};
	PresentationVertex output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	output.textureCoordinate = textureCoordinates[vertexId];
	return output;
}

float4 PSMain(PresentationVertex input) : SV_TARGET
{
	const float4 source = SourceTexture.Sample(SourceSampler,
		input.textureCoordinate);
	float3 value = saturate(source.rgb - Parameters.w);
	value = pow(value, 1.0f / Parameters.x);
	value = saturate(value * Parameters.z + Parameters.y);
	return float4(value, source.a);
}
