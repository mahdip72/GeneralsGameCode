#ifndef RTS_RENDER_NORMAL_MATRIX_SUBGROUP_CACHE_H
#define RTS_RENDER_NORMAL_MATRIX_SUBGROUP_CACHE_H

#include "Renderer/LegacyRenderState.h"
#include <string.h>
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace rts { namespace render { namespace detail {

// Only the native MSVC x64 builder has the audited SSE floating-point domain.
inline unsigned int ReadNormalMatrixCacheControls()
{
#if defined(_MSC_VER) && defined(_M_X64)
	return _mm_getcsr();
#else
	return 0;
#endif
}

class NormalMatrixSubgroupCache
{
public:
	struct Entry
	{
		Entry() : controls(0), buildResult(false), valid(false) {}
		unsigned int words[9];
		float values[12];
		unsigned int controls;
		bool buildResult;
		bool valid;
	};
	enum Lookup { Hit, Miss, Bypass };

	void invalidate() { m_entry.valid = false; }

	// Staging is independent of GPU-buffer validity. Only a successful complete
	// constant publication may commit the pending entry. Hits copy padding too.
	Lookup stage(const RenderMatrix4 &matrix, float *output, Entry &pending,
		unsigned int controls) const
	{
		pending.valid = false;
		bool admissible = false;
#if defined(_MSC_VER) && defined(_M_X64)
		admissible = (controls & 0x1f80U) == 0x1f80U;
#endif
		pending.controls = controls & 0xe040U; // RC, DAZ and FTZ; not sticky flags.
		if (admissible)
		{
			const unsigned int indices[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
			for (unsigned int index = 0; index < 9; ++index)
			{
				memcpy(&pending.words[index], &matrix.values[indices[index]],
					sizeof(unsigned int));
				if (!finiteWord(pending.words[index])) admissible = false;
			}
		}
		if (admissible && m_entry.valid &&
			m_entry.controls == pending.controls &&
			memcmp(m_entry.words, pending.words, sizeof(pending.words)) == 0)
		{
			memcpy(output, m_entry.values, sizeof(m_entry.values));
			pending.buildResult = m_entry.buildResult;
			return Hit;
		}
		// Keep the original builder, input, expression order and singular output.
		pending.buildResult = BuildLegacyInverseTransposeNormalMatrix(matrix, output);
		if (!admissible) return Bypass;
		for (unsigned int index = 0; index < 12; ++index)
		{
			unsigned int word;
			memcpy(&word, &output[index], sizeof(word));
			if (!finiteWord(word)) return Bypass;
		}
		memcpy(pending.values, output, sizeof(pending.values));
		pending.valid = true;
		return Miss;
	}

	void commit(const Entry &pending)
	{
		if (pending.valid) m_entry = pending;
	}

private:
	static bool finiteWord(unsigned int word)
	{
		return (word & 0x7f800000U) != 0x7f800000U;
	}
	// IEEE binary32 and 32-bit unsigned int are native renderer contracts.
	typedef char WordAndFloatMustBe32Bits[
		(sizeof(unsigned int) == 4 && sizeof(float) == 4) ? 1 : -1];
	Entry m_entry;
};

} } }
#endif
