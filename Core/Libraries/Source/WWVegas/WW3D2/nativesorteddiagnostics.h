#ifndef RTS_WW3D2_NATIVESORTEDDIAGNOSTICS_H
#define RTS_WW3D2_NATIVESORTEDDIAGNOSTICS_H

#include "Renderer/RendererDevice.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <chrono>
#endif

namespace rts { namespace render { namespace sorted_diagnostics {

// Fixed raw-bit receipt. No float arithmetic, geometry ownership, or live pointers.
// Only present is initialized on the disabled path; the payload is cleared on capture.
struct DepthSnapshot
{
	DepthSnapshot() : present(false) {}
	bool present;
	unsigned int bounds, stride, minimum, vertexCount, vertexFormat;
	unsigned int layoutCount, preTransformed, commonZ;
	unsigned int vertexHandleIndex, vertexHandleGeneration, indexHandleIndex, indexHandleGeneration, shaderBits;
	unsigned int kernelResult, batchCount, descriptorCount, submittedJobs;
	unsigned int completedJobs, workerThreadsUsed, scratchOutputCount, scratchCapacityBytes;
	uint64_t vertexAddress, vertexBytes, indexAddress, indexCount;
	uint64_t outputAddress, outputCount, scratchAddress, descriptorAddress;
	uint64_t firstIndex, outputSlot;
	unsigned int sourceIndices[3], relativeIndices[3], xyzBits[9], zBits[4];
	unsigned int outputIndices[3], outputNode, outputZ;
	unsigned int layoutSemantic[16], layoutIndex[16], layoutFormat[16], layoutOffset[16];
};

// Capture the same XYZ-at-byte-zero view used by the kernel, not an alternate
// layout-derived position. Layout offsets are recorded separately for diagnosis.
inline void ReadDepthSource(DepthSnapshot &depth, const unsigned char *vertices,
	size_t vertexBytes, const unsigned short *indices, size_t indexCount,
	size_t firstIndex)
{
	depth.vertexAddress = reinterpret_cast<size_t>(vertices);
	depth.vertexBytes = vertexBytes;
	depth.indexAddress = reinterpret_cast<size_t>(indices);
	depth.indexCount = indexCount;
	depth.firstIndex = firstIndex;
	if (indices == 0 || firstIndex > indexCount || indexCount - firstIndex < 3)
		return;
	depth.bounds |= 1U;
	for (unsigned int corner = 0; corner < 3; ++corner)
	{
		const unsigned int value = indices[firstIndex + corner];
		depth.sourceIndices[corner] = value;
		if (value < depth.minimum || value - depth.minimum >= depth.vertexCount)
			continue;
		const size_t relative = value - depth.minimum;
		depth.relativeIndices[corner] = static_cast<unsigned int>(relative);
		if (vertices == 0 || depth.stride < sizeof(float) * 3 ||
			relative > static_cast<size_t>(-1) / depth.stride)
			continue;
		const size_t offset = relative * depth.stride;
		if (offset > vertexBytes || vertexBytes - offset < sizeof(float) * 3)
			continue;
		memcpy(&depth.xyzBits[corner * 3], vertices + offset, sizeof(float) * 3);
		depth.bounds |= 2U << corner;
	}
}
// Synchronous owner-thread metadata only. Never retain geometry or worker data.
struct Origin
{
	Origin() : checked(false), enabled(false), present(false), result(RENDER_RESULT_OK),
		caller(0), reason(0), site(0), qpc(0), clock(0)
	{
		for (unsigned int i = 0; i < 6; ++i) values[i] = 0;
	}
	DepthSnapshot depth;
	bool checked, enabled, present;
	RenderResult result;
	const char *caller, *reason;
	unsigned int site;
	uint64_t qpc, values[6];
	// Private deterministic clock seam for CPU contract tests.
	uint64_t (*clock)();
};

inline Origin *&ActiveOrigin()
{
	static thread_local Origin *origin = 0;
	return origin;
}

class Scope
{
public:
	explicit Scope(uint64_t (*clock)() = 0) : m_previous(ActiveOrigin()),
		m_active(m_previous != 0 ? m_previous : &m_origin)
	{
		if (m_previous == 0) m_origin.clock = clock;
		ActiveOrigin() = m_active;
	}
	~Scope() { ActiveOrigin() = m_previous; }
	const Origin &origin() const { return *m_active; }
private:
	Origin m_origin;
	Origin *m_previous, *m_active;
	Scope(const Scope &);
	Scope &operator=(const Scope &);
};

inline RenderResult NoteFailure(RenderResult result, const char *caller,
	unsigned int site, const char *reason, uint64_t a = 0, uint64_t b = 0,
	uint64_t c = 0, uint64_t d = 0, uint64_t e = 0, uint64_t f = 0)
{
	Origin *origin = ActiveOrigin();
	if (result == RENDER_RESULT_OK || origin == 0 || origin->present) return result;
	if (!origin->checked)
	{
		origin->checked = true;
		const char *path = getenv("RTS_RENDER_FAILURE_TRACE");
		origin->enabled = path != 0 && path[0] != '\0';
	}
	if (!origin->enabled) return result;
	origin->present = true;
	origin->result = result;
	origin->caller = caller;
	origin->reason = reason;
	origin->site = site;
	origin->values[0] = a; origin->values[1] = b; origin->values[2] = c;
	origin->values[3] = d; origin->values[4] = e; origin->values[5] = f;
	if (origin->clock != 0) origin->qpc = origin->clock();
	else
	{
#if defined(_WIN32)
		LARGE_INTEGER qpc = {};
		QueryPerformanceCounter(&qpc);
		origin->qpc = static_cast<uint64_t>(qpc.QuadPart);
#else
		origin->qpc = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
	}
	return result;
}

} } }
#endif
