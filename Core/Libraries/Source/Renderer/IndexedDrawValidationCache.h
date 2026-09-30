#ifndef RTS_INDEXED_DRAW_VALIDATION_CACHE_H
#define RTS_INDEXED_DRAW_VALIDATION_CACHE_H

#include "Renderer/RendererDevice.h"

namespace rts { namespace render { namespace detail {

struct IndexedDrawValidationKey
{
	GpuHandle vertexBuffer;
	GpuHandle indexBuffer;
	unsigned int vertexVersion;
	unsigned int indexVersion;
	unsigned int vertexOffset;
	unsigned int vertexStride;
	size_t firstIndexByte;
	unsigned int indexSize;
	unsigned int indexCount;
	int baseVertex;

	bool equals(const IndexedDrawValidationKey &other) const
	{
		return vertexBuffer == other.vertexBuffer && indexBuffer == other.indexBuffer &&
			vertexVersion == other.vertexVersion && indexVersion == other.indexVersion &&
			vertexOffset == other.vertexOffset && vertexStride == other.vertexStride &&
			firstIndexByte == other.firstIndexByte && indexSize == other.indexSize &&
			indexCount == other.indexCount && baseVertex == other.baseVertex;
	}

	unsigned int hash() const
	{
		unsigned int result = 2166136261U;
		const unsigned int values[] = {
			vertexBuffer.index(), vertexBuffer.generation(),
			indexBuffer.index(), indexBuffer.generation(), vertexVersion, indexVersion,
			vertexOffset, vertexStride, static_cast<unsigned int>(firstIndexByte),
			indexSize, indexCount, static_cast<unsigned int>(baseVertex)
		};
		for (unsigned int index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
			result = (result ^ values[index]) * 16777619U;
		return result ^ (result >> 16);
	}
};

// This cache retains only successful exact-address proofs. Direct mapping
// bounds lookup and storage; collisions cause validation, never acceptance.
class IndexedDrawValidationCache
{
public:
	enum { CAPACITY = 256 };

	bool contains(const IndexedDrawValidationKey &key) const
	{
		const Entry &entry = m_entries[key.hash() % CAPACITY];
		return entry.valid && entry.key.equals(key);
	}

	void store(const IndexedDrawValidationKey &key)
	{
		Entry &entry = m_entries[key.hash() % CAPACITY];
		entry.key = key;
		entry.valid = true;
	}

	void clear()
	{
		for (unsigned int index = 0; index < CAPACITY; ++index)
			m_entries[index].valid = false;
	}

private:
	struct Entry
	{
		Entry() : valid(false) {}
		IndexedDrawValidationKey key;
		bool valid;
	};
	Entry m_entries[CAPACITY];
};

} } }

#endif
