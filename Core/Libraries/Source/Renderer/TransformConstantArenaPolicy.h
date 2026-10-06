#ifndef RTS_TRANSFORM_CONSTANT_ARENA_POLICY_H
#define RTS_TRANSFORM_CONSTANT_ARENA_POLICY_H

#include <string.h>

namespace rts { namespace render {
class IRenderDevice;
namespace detail {

// D3D11.1 ranges have 256-byte granularity (16 16-byte constants).
class TransformConstantArenaPolicy
{
public:
	enum { PAYLOAD_BYTES = 2976, SLICE_BYTES = 3072, PAGE_BYTES = 65536,
		SLICE_COUNT = PAGE_BYTES / SLICE_BYTES, RANGE_CONSTANTS = SLICE_BYTES / 16 };
	struct Range
	{
		unsigned int slot, byteOffset, firstConstant, constantCount;
		bool discard;
	};
	TransformConstantArenaPolicy() : m_nextSlot(0) {}
	static bool Supported(bool context1, bool offsetting, bool noOverwrite)
	{ return context1 && offsetting && noOverwrite; }
	Range reserve() const
	{
		Range result;
		result.slot = m_nextSlot;
		result.byteOffset = m_nextSlot * SLICE_BYTES;
		result.firstConstant = result.byteOffset / 16;
		result.constantCount = RANGE_CONSTANTS;
		result.discard = m_nextSlot == 0;
		return result;
	}
	// Failed uploads never consume a range. Reset always schedules DISCARD.
	bool commit(const Range &range)
	{
		const Range expected = reserve();
		if (range.slot != expected.slot || range.byteOffset != expected.byteOffset ||
			range.firstConstant != expected.firstConstant ||
			range.constantCount != expected.constantCount || range.discard != expected.discard)
			return false;
		m_nextSlot = (m_nextSlot + 1) % SLICE_COUNT;
		return true;
	}
	void reset() { m_nextSlot = 0; }
	static void WriteSlice(void *destination, const void *payload)
	{
		memcpy(destination, payload, PAYLOAD_BYTES);
		memset(static_cast<unsigned char *>(destination) + PAYLOAD_BYTES, 0,
			SLICE_BYTES - PAYLOAD_BYTES);
	}
private:
	unsigned int m_nextSlot;
};

// Read-only coverage query for devices returned by CreateD3D11RenderDevice.
// No mode forcing, environment switch, or per-draw diagnostic counter.
bool D3D11TransformConstantArenaEnabled(IRenderDevice *device);

} } }
#endif
