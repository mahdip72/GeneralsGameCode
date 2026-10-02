#include "Renderer/RendererDevice.h"
#include <d3d11.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
using rts::render::LEGACY_TEXTURE_STAGE_COUNT;

int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}

// A cache owns one reference. Retain dead test allocations until teardown so
// a stale pointer is observable without dereferencing freed memory.
struct RecordingSampler
{
	RecordingSampler() : released(false), releaseCount(0) {}
	unsigned long Release()
	{
		released = true;
		++releaseCount;
		return 0;
	}
	bool released;
	unsigned int releaseCount;
};

struct RecordingDevice
{
	RecordingDevice() : failNextCreate(false) {}
	~RecordingDevice()
	{
		for (size_t index = 0; index < samplers.size(); ++index)
			delete samplers[index];
	}
	HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC *, RecordingSampler **state)
	{
		*state = 0;
		if (failNextCreate) { failNextCreate = false; return E_FAIL; }
		RecordingSampler *sampler = new RecordingSampler();
		samplers.push_back(sampler);
		*state = sampler;
		return S_OK;
	}
	bool failNextCreate;
	std::vector<RecordingSampler *> samplers;
};

struct RecordingContext
{
	RecordingContext() : bindCalls(0), boundReleasedSampler(false) {}
	void PSSetSamplers(unsigned int, unsigned int count, RecordingSampler **states)
	{
		++bindCalls;
		for (unsigned int index = 0; index < count; ++index)
			if (states[index] == 0 || states[index]->released)
				boundReleasedSampler = true;
	}
	unsigned int bindCalls;
	bool boundReleasedSampler;
};

#define ID3D11SamplerState RecordingSampler
#include "D3D11SamplerConstants.inc"
#include "D3D11SamplerEntry.inc"

struct RecencyEntry { unsigned int lastUsedSerial; };

class SamplerFixture
{
public:
	SamplerFixture() : m_stateUseSerial(0), m_device(&device), m_context(&context),
		m_pipelineHasTextures(false)
	{
		memset(m_boundSamplerStates, 0, sizeof(m_boundSamplerStates));
	}
	~SamplerFixture()
	{
		for (size_t index = 0; index < m_samplerStates.size(); ++index)
			m_samplerStates[index].state->Release();
	}

	HRESULT LookupSingle(const D3D11_SAMPLER_DESC &descriptor,
		RecordingSampler **state)
	{
		ID3D11SamplerState *samplerStates[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
		const unsigned int stage = 0;
		const D3D11_SAMPLER_DESC samplerDescriptor = descriptor;
		HRESULT result = S_OK;
#include "D3D11SamplerGatherCall.inc"
		*state = samplerStates[0];
		return result;
	}

	HRESULT Gather(const D3D11_SAMPLER_DESC *descriptors, RecordingSampler **output)
	{
		ID3D11SamplerState *samplerStates[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
		HRESULT result = S_OK;
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		{
			const D3D11_SAMPLER_DESC samplerDescriptor = descriptors[stage];
#include "D3D11SamplerGatherCall.inc"
		}
		const bool hasTextures = true;
		const bool forcePipeline = true;
#include "D3D11SamplerBindCall.inc"
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
			output[stage] = samplerStates[stage];
		return result;
	}

	HRESULT TranslateResult(HRESULT result) { return result; }

#include "D3D11SamplerSerial.inc"
#include "D3D11SamplerEviction.inc"
#include "D3D11SamplerLookup.inc"

	RecordingDevice device;
	RecordingContext context;
	unsigned int m_stateUseSerial;
	RecordingDevice *m_device;
	RecordingContext *m_context;
	bool m_pipelineHasTextures;
	RecordingSampler *m_boundSamplerStates[LEGACY_TEXTURE_STAGE_COUNT];
	std::vector<SamplerStateEntry> m_samplerStates;
	std::vector<RecencyEntry> m_blendStates, m_depthStates, m_rasterizerStates, m_inputLayouts;
};
#undef ID3D11SamplerState

D3D11_SAMPLER_DESC Descriptor(unsigned int tag)
{
	D3D11_SAMPLER_DESC descriptor;
	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	descriptor.AddressU = descriptor.AddressV = descriptor.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	descriptor.MaxAnisotropy = 1;
	descriptor.ComparisonFunc = D3D11_COMPARISON_NEVER;
	descriptor.MipLODBias = static_cast<float>(tag) * 0.001f;
	return descriptor;
}

int Seed(SamplerFixture &fixture)
{
	for (unsigned int index = 0; index < STATE_CACHE_CAPACITY; ++index)
	{
		RecordingSampler *state = 0;
		if (fixture.LookupSingle(Descriptor(index), &state) != S_OK || state == 0)
			return Check(false, "full sampler cache seeds through actual gather callsite");
	}
	fixture.m_boundSamplerStates[0] = fixture.m_samplerStates[3].state;
	return Check(fixture.m_samplerStates.size() == STATE_CACHE_CAPACITY,
		"sampler cache starts at production capacity");
}

int RunGatherCase(bool rollover, bool failCreate)
{
	SamplerFixture fixture;
	int result = Seed(fixture);
	if (result) return result;
	RecordingSampler *first = fixture.m_samplerStates[0].state;
	RecordingSampler *second = fixture.m_samplerStates[1].state;
	RecordingSampler *previouslyBound = fixture.m_boundSamplerStates[0];
	if (rollover) fixture.m_stateUseSerial = UINT_MAX - 2;
	fixture.device.failNextCreate = failCreate;
	D3D11_SAMPLER_DESC descriptors[LEGACY_TEXTURE_STAGE_COUNT];
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		descriptors[stage] = Descriptor(stage < 3 ? stage : 1000 + stage);
	RecordingSampler *gathered[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
	const HRESULT gatherResult = fixture.Gather(descriptors, gathered);
	result |= Check(gatherResult == (failCreate ? E_FAIL : S_OK),
		"production gather propagates expected creation result");
	result |= Check(!first->released && !second->released,
		"later cache miss retains previously gathered samplers across rollover");
	result |= Check(!previouslyBound->released,
		"current bound sampler remains protected while new set gathers");
	result |= Check(fixture.m_samplerStates.size() == STATE_CACHE_CAPACITY -
		(failCreate ? 1 : 0), "sampler cache stays bounded during lookup or failure");
	if (rollover)
		result |= Check(fixture.m_stateUseSerial < UINT_MAX / 2,
			"test actually traverses production use-serial rollover");
	if (failCreate)
		result |= Check(fixture.context.bindCalls == 0,
			"production gather failure returns before binding partial sampler set");
	else
	{
		result |= Check(fixture.context.bindCalls == 1 &&
			!fixture.context.boundReleasedSampler,
			"production PSSetSamplers call receives only live gathered states");
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
			result |= Check(gathered[stage] != 0 && !gathered[stage]->released,
				"every gathered stage retains a live cache reference until bind");
	}
	return result;
}
}

int main()
{
	int result = RunGatherCase(true, false);
	result |= RunGatherCase(true, true);
	result |= RunGatherCase(false, false);
	if (result == 0) printf("D3D11 sampler rollover lifetime tests passed.\n");
	return result;
}
