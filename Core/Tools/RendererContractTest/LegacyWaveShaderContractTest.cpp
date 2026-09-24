#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"

#include <stdio.h>
#include <string.h>

#if defined(RTS_RENDERER_WAVE_SHADER_CONTRACT_TEST)

namespace wave_shader_contract_test
{
struct Call
{
	const char *assetPath;
	bool vertexShader;
	const void *declarationWords;
	unsigned int declarationWordCount;
	unsigned int usage;
	unsigned int *handle;
	unsigned int firstWord;
	unsigned int handleValue;
};

Call calls[2];
unsigned int callCount = 0;

void Reset()
{
	memset(calls, 0, sizeof(calls));
	callCount = 0;
}
}

namespace rts { namespace render {
RenderResult CreateGameShaderFromAsset(const char *assetPath,
	bool vertexShader, const void *declarationWords,
	unsigned int declarationWordCount, unsigned int usage,
	unsigned int *handle)
{
	const unsigned int index = wave_shader_contract_test::callCount++;
	if (index < 2)
	{
		wave_shader_contract_test::Call &call =
			wave_shader_contract_test::calls[index];
		call.assetPath = assetPath;
		call.vertexShader = vertexShader;
		call.declarationWords = declarationWords;
		call.declarationWordCount = declarationWordCount;
		call.usage = usage;
		call.handle = handle;
		call.firstWord = declarationWords == 0 ? 0 :
			static_cast<const unsigned int *>(declarationWords)[0];
		call.handleValue = index + 1;
	}
	if (handle != 0)
	{
		*handle = index + 1;
	}
	return RENDER_RESULT_OK;
}

bool DeleteGameShader(bool, unsigned int)
{
	return true;
}
}}

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	fprintf(stderr, "FAIL: legacy wave shader contract: %s\n", message);
	return 1;
}

void ExecuteWaveShaderCalls()
{
	unsigned int m_wavePixelShader = 0;
	unsigned int m_waveVertexShader = 0;
	using namespace rts::render;
#include "LegacyWaveShaderCalls.inc"
}
}

int RunLegacyWaveShaderContractTests()
{
	wave_shader_contract_test::Reset();
	ExecuteWaveShaderCalls();

	int result = 0;
	result |= Check(wave_shader_contract_test::callCount == 2,
		"wave reacquisition submits exactly pixel and vertex shader assets");
	if (wave_shader_contract_test::callCount >= 2)
	{
		const wave_shader_contract_test::Call &pixel =
			wave_shader_contract_test::calls[0];
		const wave_shader_contract_test::Call &vertex =
			wave_shader_contract_test::calls[1];
		result |= Check(strcmp(pixel.assetPath, "shaders\\wave.pso") == 0 &&
			!pixel.vertexShader,
			"the first wave asset remains the pixel shader");
		result |= Check(strcmp(vertex.assetPath, "shaders\\wave.vso") == 0 &&
			vertex.vertexShader,
			"the second wave asset remains the vertex shader");
		result |= Check(pixel.declarationWords == 0 &&
			pixel.declarationWordCount == 0 && pixel.usage == 0,
			"pixel wave creation uses the neutral declaration boundary");
		result |= Check(vertex.declarationWords == 0 &&
			vertex.declarationWordCount == 0 && vertex.usage == 0,
			"vertex wave creation delegates declaration synthesis to the adapter");
		result |= Check(pixel.handleValue == 1 && vertex.handleValue == 2 &&
			pixel.handle != 0 && vertex.handle != 0,
			"both wave shader creations publish their returned handles");
	}
	return result;
}

#else

int RunLegacyWaveShaderContractTests()
{
	return 0;
}

#endif
