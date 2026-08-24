#include "Renderer/LegacyRenderState.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
using rts::render::RenderTextureOperation;

struct Color
{
	float r;
	float g;
	float b;
	float a;

	Color() : r(0.0f), g(0.0f), b(0.0f), a(0.0f) {}
	Color(float red, float green, float blue, float alpha) :
		r(red), g(green), b(blue), a(alpha) {}
};

enum Channel
{
	CHANNEL_COLOR,
	CHANNEL_ALPHA
};

struct OperationInfo
{
	bool colorOnly;
	bool bump;
	bool premodulate;
};

Color Add(const Color &left, const Color &right)
{
	return Color(left.r + right.r, left.g + right.g, left.b + right.b,
		left.a + right.a);
}

Color Subtract(const Color &left, const Color &right)
{
	return Color(left.r - right.r, left.g - right.g, left.b - right.b,
		left.a - right.a);
}

Color Multiply(const Color &left, const Color &right)
{
	return Color(left.r * right.r, left.g * right.g, left.b * right.b,
		left.a * right.a);
}

Color Scale(const Color &value, float scale)
{
	return Color(value.r * scale, value.g * scale, value.b * scale,
		value.a * scale);
}

Color Lerp(const Color &second, const Color &first, float amount)
{
	return Add(second, Scale(Subtract(first, second), amount));
}

Color PremodulateCurrent(const Color &previous, const Color &nextTexture)
{
	return Multiply(previous, nextTexture);
}

OperationInfo GetOperationInfo(RenderTextureOperation operation)
{
	OperationInfo info;
	info.colorOnly = false;
	info.bump = false;
	info.premodulate = false;
	switch (operation)
	{
	case rts::render::RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR:
	case rts::render::RENDER_TEXTURE_OP_MODULATE_COLOR_ADD_ALPHA:
	case rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_ALPHA_ADD_COLOR:
	case rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_COLOR_ADD_ALPHA:
		info.colorOnly = true;
		break;
	case rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT:
	case rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE:
		info.bump = true;
		info.colorOnly = true;
		break;
	case rts::render::RENDER_TEXTURE_OP_PREMODULATE:
		info.premodulate = true;
		break;
	default:
		break;
	}
	return info;
}

Color Evaluate(RenderTextureOperation operation, const Color &argument1,
	const Color &argument2, const Color &argument0, const Color &current,
	const Color &diffuse, const Color &textureSample,
	const Color &textureFactor, Channel channel)
{
	const bool alpha = channel == CHANNEL_ALPHA;
	const OperationInfo info = GetOperationInfo(operation);
	if (info.bump)
	{
		// Bump operations perturb the following stage's coordinates and are
		// handled by PSTextured before the normal combiner operation.
		return current;
	}
	if (info.colorOnly && alpha)
	{
		// Direct3D documents these operations as COLOROP-only. The separate
		// alpha channel therefore remains the prior stage's value in our
		// channel-complete model.
		return current;
	}

	switch (operation)
	{
	case rts::render::RENDER_TEXTURE_OP_DISABLE:
		return current;
	case rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1:
		return argument1;
	case rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2:
		return argument2;
	case rts::render::RENDER_TEXTURE_OP_MODULATE:
		return Multiply(argument1, argument2);
	case rts::render::RENDER_TEXTURE_OP_MODULATE_2X:
		return Scale(Multiply(argument1, argument2), 2.0f);
	case rts::render::RENDER_TEXTURE_OP_MODULATE_4X:
		return Scale(Multiply(argument1, argument2), 4.0f);
	case rts::render::RENDER_TEXTURE_OP_ADD:
		return Add(argument1, argument2);
	case rts::render::RENDER_TEXTURE_OP_ADD_SIGNED:
		return Subtract(Add(argument1, argument2), Color(0.5f, 0.5f,
			0.5f, 0.5f));
	case rts::render::RENDER_TEXTURE_OP_ADD_SIGNED_2X:
		return Scale(Subtract(Add(argument1, argument2), Color(0.5f,
			0.5f, 0.5f, 0.5f)), 2.0f);
	case rts::render::RENDER_TEXTURE_OP_SUBTRACT:
		return Subtract(argument1, argument2);
	case rts::render::RENDER_TEXTURE_OP_ADD_SMOOTH:
		return Add(argument1, Multiply(argument2, Color(1.0f - argument1.r,
			1.0f - argument1.g, 1.0f - argument1.b, 1.0f - argument1.a)));
	case rts::render::RENDER_TEXTURE_OP_BLEND_DIFFUSE_ALPHA:
		return Lerp(argument2, argument1, diffuse.a);
	case rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA:
		return Lerp(argument2, argument1, textureSample.a);
	case rts::render::RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA:
		return Lerp(argument2, argument1, current.a);
	case rts::render::RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR:
		return Color(argument1.r + argument1.a * argument2.r,
			argument1.g + argument1.a * argument2.g,
			argument1.b + argument1.a * argument2.b, current.a);
	case rts::render::RENDER_TEXTURE_OP_DOT_PRODUCT_3:
	{
		const float dotProduct = 4.0f *
			((argument1.r - 0.5f) * (argument2.r - 0.5f) +
			(argument1.g - 0.5f) * (argument2.g - 0.5f) +
			(argument1.b - 0.5f) * (argument2.b - 0.5f));
		return alpha ? Color(current.r, current.g, current.b, dotProduct) :
			Color(dotProduct, dotProduct, dotProduct, current.a);
	}
	case rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT:
	case rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE:
		return current;
	case rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA_PREMULTIPLIED:
		return Add(argument1, Scale(argument2, 1.0f - textureSample.a));
	case rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_FACTOR_ALPHA:
		return Lerp(argument2, argument1, textureFactor.a);
	case rts::render::RENDER_TEXTURE_OP_PREMODULATE:
		return argument1;
	case rts::render::RENDER_TEXTURE_OP_MODULATE_COLOR_ADD_ALPHA:
		return Color(argument1.r * argument2.r + argument1.a,
			argument1.g * argument2.g + argument1.a,
			argument1.b * argument2.b + argument1.a, current.a);
	case rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_ALPHA_ADD_COLOR:
		return Color(argument1.r + (1.0f - argument1.a) * argument2.r,
			argument1.g + (1.0f - argument1.a) * argument2.g,
			argument1.b + (1.0f - argument1.a) * argument2.b, current.a);
	case rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_COLOR_ADD_ALPHA:
		return Color((1.0f - argument1.r) * argument2.r + argument1.a,
			(1.0f - argument1.g) * argument2.g + argument1.a,
			(1.0f - argument1.b) * argument2.b + argument1.a, current.a);
	case rts::render::RENDER_TEXTURE_OP_MULTIPLY_ADD:
		return Add(argument0, Multiply(argument1, argument2));
	case rts::render::RENDER_TEXTURE_OP_LINEAR_INTERPOLATE:
		return Color(argument0.r * argument1.r + (1.0f - argument0.r) * argument2.r,
			argument0.g * argument1.g + (1.0f - argument0.g) * argument2.g,
			argument0.b * argument1.b + (1.0f - argument0.b) * argument2.b,
			argument0.a * argument1.a + (1.0f - argument0.a) * argument2.a);
	default:
		return current;
	}
}

bool Near(float actual, float expected)
{
	return std::fabs(actual - expected) < 0.0001f;
}

bool Same(const Color &actual, const Color &expected)
{
	return Near(actual.r, expected.r) && Near(actual.g, expected.g) &&
		Near(actual.b, expected.b) && Near(actual.a, expected.a);
}

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int CheckOperation(RenderTextureOperation operation, Channel channel,
	const Color &actual, const Color &expected, const char *name)
{
	if (Same(actual, expected))
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s (%u) channel %u: got %.6f %.6f %.6f %.6f, "
		"expected %.6f %.6f %.6f %.6f\n", name,
		static_cast<unsigned int>(operation), static_cast<unsigned int>(channel),
		actual.r, actual.g, actual.b, actual.a, expected.r, expected.g,
		expected.b, expected.a);
	return 1;
}

int CheckShaderSource()
{
	std::string path = RTS_SOURCE_ROOT;
	path += "/Core/Libraries/Source/Renderer/Shaders/LegacyFixedFunction.hlsl";
	std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
	if (!input)
	{
		return Check(false, "could not open LegacyFixedFunction.hlsl");
	}
	std::ostringstream contents;
	contents << input.rdbuf();
	const std::string source = contents.str();
	const std::string::size_type premodulateCase = source.find("case 20:");
	const std::string::size_type premodulateReturn = source.find(
		"return argument1;", premodulateCase);
	const std::string::size_type followingCase = source.find(
		"case 21:", premodulateCase);
	int result = 0;
	result |= Check(source.find("float4 textureSample, bool alphaOperation") !=
		std::string::npos, "shader combiner must distinguish alpha operations");
	result |= Check(source.find("argument1.rgb + argument1.a * argument2.rgb") !=
		std::string::npos, "shader MODULATEALPHA_ADDCOLOR RGB formula");
	result |= Check(source.find("argument1.rgb * argument2.rgb + argument1.a") !=
		std::string::npos, "shader MODULATECOLOR_ADDALPHA RGB formula");
	result |= Check(source.find("(1.0f - argument1.a) * argument2.rgb") !=
		std::string::npos, "shader MODULATEINVALPHA_ADDCOLOR RGB formula");
	result |= Check(source.find("(1.0f - argument1.rgb) * argument2.rgb + argument1.a") !=
		std::string::npos, "shader MODULATEINVCOLOR_ADDALPHA RGB formula");
	result |= Check(premodulateCase != std::string::npos &&
		premodulateReturn != std::string::npos &&
		followingCase != std::string::npos &&
		premodulateCase < premodulateReturn && premodulateReturn < followingCase,
		"shader PREMODULATE must output argument 1");
	result |= Check(source.find("float4(current.rgb, dotProduct)") !=
		std::string::npos, "shader DOTPRODUCT3 alpha routing");
	result |= Check(source.find("current * textureSample") !=
		std::string::npos, "shader PREMODULATE current propagation");
	result |= Check(source.find("premodulateCurrent = colorOperation == 20 || alphaOperation == 20") !=
		std::string::npos, "shader PREMODULATE stage flag");
	result |= Check(source.find("stageResult.a = current.a") !=
		std::string::npos, "shader preserves alpha without ALPHAOP");
	return result;
}
}

int main()
{
	const Color argument1(0.2f, 0.4f, 0.7f, 0.3f);
	const Color argument2(0.8f, 0.1f, 0.5f, 0.6f);
	const Color argument0(0.4f, 0.9f, 0.25f, 0.8f);
	const Color current(0.3f, 0.6f, 0.2f, 0.45f);
	const Color diffuse(0.9f, 0.2f, 0.7f, 0.35f);
	const Color textureSample(0.1f, 0.8f, 0.4f, 0.65f);
	const Color textureFactor(0.7f, 0.4f, 0.9f, 0.55f);
	int result = CheckShaderSource();

	struct ExpectedOperation
	{
		RenderTextureOperation operation;
		Color expected;
		const char *name;
	};
	const ExpectedOperation expected[] = {
		{ rts::render::RENDER_TEXTURE_OP_DISABLE, Color(0.3f, 0.6f, 0.2f, 0.45f), "DISABLE" },
		{ rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1, Color(0.2f, 0.4f, 0.7f, 0.3f), "SELECTARG1" },
		{ rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2, Color(0.8f, 0.1f, 0.5f, 0.6f), "SELECTARG2" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE, Color(0.16f, 0.04f, 0.35f, 0.18f), "MODULATE" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_2X, Color(0.32f, 0.08f, 0.7f, 0.36f), "MODULATE2X" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_4X, Color(0.64f, 0.16f, 1.4f, 0.72f), "MODULATE4X" },
		{ rts::render::RENDER_TEXTURE_OP_ADD, Color(1.0f, 0.5f, 1.2f, 0.9f), "ADD" },
		{ rts::render::RENDER_TEXTURE_OP_ADD_SIGNED, Color(0.5f, 0.0f, 0.7f, 0.4f), "ADDSIGNED" },
		{ rts::render::RENDER_TEXTURE_OP_ADD_SIGNED_2X, Color(1.0f, 0.0f, 1.4f, 0.8f), "ADDSIGNED2X" },
		{ rts::render::RENDER_TEXTURE_OP_SUBTRACT, Color(-0.6f, 0.3f, 0.2f, -0.3f), "SUBTRACT" },
		{ rts::render::RENDER_TEXTURE_OP_ADD_SMOOTH, Color(0.84f, 0.46f, 0.85f, 0.72f), "ADDSMOOTH" },
		{ rts::render::RENDER_TEXTURE_OP_BLEND_DIFFUSE_ALPHA, Color(0.59f, 0.205f, 0.57f, 0.495f), "BLENDDIFFUSEALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA, Color(0.41f, 0.295f, 0.63f, 0.405f), "BLENDTEXTUREALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA, Color(0.53f, 0.235f, 0.59f, 0.465f), "BLENDCURRENTALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR, Color(0.44f, 0.43f, 0.85f, 0.45f), "MODULATEALPHA_ADDCOLOR" },
		{ rts::render::RENDER_TEXTURE_OP_DOT_PRODUCT_3, Color(-0.2f, -0.2f, -0.2f, 0.45f), "DOTPRODUCT3" },
		{ rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT, Color(0.3f, 0.6f, 0.2f, 0.45f), "BUMPENVMAP" },
		{ rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE, Color(0.3f, 0.6f, 0.2f, 0.45f), "BUMPENVMAPLUMINANCE" },
		{ rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA_PREMULTIPLIED, Color(0.48f, 0.435f, 0.875f, 0.51f), "BLENDTEXTUREALPHAPM" },
		{ rts::render::RENDER_TEXTURE_OP_BLEND_TEXTURE_FACTOR_ALPHA, Color(0.47f, 0.265f, 0.61f, 0.435f), "BLENDFACTORALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_PREMODULATE, Color(0.2f, 0.4f, 0.7f, 0.3f), "PREMODULATE" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_COLOR_ADD_ALPHA, Color(0.46f, 0.34f, 0.65f, 0.45f), "MODULATECOLOR_ADDALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_ALPHA_ADD_COLOR, Color(0.76f, 0.47f, 1.05f, 0.45f), "MODULATEINVALPHA_ADDCOLOR" },
		{ rts::render::RENDER_TEXTURE_OP_MODULATE_INVERSE_COLOR_ADD_ALPHA, Color(0.94f, 0.36f, 0.45f, 0.45f), "MODULATEINVCOLOR_ADDALPHA" },
		{ rts::render::RENDER_TEXTURE_OP_MULTIPLY_ADD, Color(0.56f, 0.94f, 0.6f, 0.98f), "MULTIPLYADD" },
		{ rts::render::RENDER_TEXTURE_OP_LINEAR_INTERPOLATE, Color(0.56f, 0.37f, 0.55f, 0.36f), "LERP" }
	};
	const unsigned int expectedCount = static_cast<unsigned int>(
		sizeof(expected) / sizeof(expected[0]));
	result |= Check(expectedCount == 26U, "oracle must enumerate every operation");
	for (unsigned int index = 0; index < expectedCount; ++index)
	{
		const ExpectedOperation &entry = expected[index];
		result |= CheckOperation(entry.operation, CHANNEL_COLOR,
			Evaluate(entry.operation, argument1, argument2, argument0, current,
				diffuse, textureSample, textureFactor, CHANNEL_COLOR),
			entry.expected, entry.name);
		const OperationInfo info = GetOperationInfo(entry.operation);
		if (info.colorOnly)
		{
			result |= CheckOperation(entry.operation, CHANNEL_ALPHA,
				Evaluate(entry.operation, argument1, argument2, argument0, current,
					diffuse, textureSample, textureFactor, CHANNEL_ALPHA), current,
				entry.name);
		}
	}

	const Color dotAlpha = Evaluate(
		rts::render::RENDER_TEXTURE_OP_DOT_PRODUCT_3, argument1, argument2,
		argument0, current, diffuse, textureSample, textureFactor,
		CHANNEL_ALPHA);
	result |= CheckOperation(rts::render::RENDER_TEXTURE_OP_DOT_PRODUCT_3,
		CHANNEL_ALPHA, dotAlpha, Color(current.r, current.g, current.b, -0.2f),
		"DOTPRODUCT3 alpha");
	result |= Check(GetOperationInfo(
		rts::render::RENDER_TEXTURE_OP_PREMODULATE).premodulate,
		"PREMODULATE must publish a next-stage side effect");
	result |= CheckOperation(rts::render::RENDER_TEXTURE_OP_PREMODULATE,
		CHANNEL_COLOR, PremodulateCurrent(current, textureSample),
		Color(0.03f, 0.48f, 0.08f, 0.2925f),
		"PREMODULATE next-stage CURRENT");
	if (result == 0)
	{
		std::printf("legacy texture operation semantics: PASS\n");
	}
	return result;
}
