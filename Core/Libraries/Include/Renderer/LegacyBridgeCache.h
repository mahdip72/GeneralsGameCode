#pragma once

#include <cstddef>
#include <unordered_map>

namespace rts { namespace render {

// The D3D11 legacy bridge keeps its entries in vectors so eviction remains
// deterministic.  This side index makes the per-draw source lookup constant
// time while preserving vector indices as the eviction-order authority.
class LegacyBridgePointerIndex
{
public:
	LegacyBridgePointerIndex() : entries() {}

	bool Insert(const void *key, unsigned int index)
	{
		if (key == 0)
		{
			return false;
		}
		try
		{
			return entries.insert(Map::value_type(key, index)).second;
		}
		catch (...)
		{
			return false;
		}
	}

	bool Find(const void *key, unsigned int *index) const
	{
		if (key == 0 || index == 0)
		{
			return false;
		}
		const Map::const_iterator found = entries.find(key);
		if (found == entries.end())
		{
			return false;
		}
		*index = found->second;
		return true;
	}

	bool Erase(const void *key)
	{
		if (key == 0)
		{
			return false;
		}
		return entries.erase(key) != 0;
	}

	// Remove the key whose vector slot was erased and shift only the indices
	// after that slot.  The caller erases the vector element immediately after
	// this operation, so no entry pointer is retained across reallocation.
	bool EraseAt(const void *key, unsigned int removed_index)
	{
		if (key == 0)
		{
			return false;
		}
		const Map::iterator found = entries.find(key);
		if (found == entries.end() || found->second != removed_index)
		{
			return false;
		}
		entries.erase(found);
		for (Map::iterator entry = entries.begin(); entry != entries.end();
			++entry)
		{
			if (entry->second > removed_index)
			{
				--entry->second;
			}
		}
		return true;
	}

	void Clear()
	{
		entries.clear();
	}

	std::size_t Size() const
	{
		return entries.size();
	}

private:
	typedef std::unordered_map<const void *, unsigned int> Map;
	Map entries;
};

inline bool LegacyBridgeTypedBufferNeedsUpload(
	unsigned int source_generation, unsigned int uploaded_generation)
{
	return source_generation != uploaded_generation;
}

inline bool LegacyBridgeRawBufferNeedsUpload(bool source_dirty)
{
	return source_dirty;
}

struct LegacyBridgeCacheCounters
{
	LegacyBridgeCacheCounters() : bufferLookups(0), bufferHits(0),
		textureLookups(0), textureHits(0), bufferUploads(0),
		textureRefreshes(0) {}

	void RecordBufferLookup(bool hit)
	{
		++bufferLookups;
		if (hit)
		{
			++bufferHits;
		}
	}

	void RecordTextureLookup(bool hit)
	{
		++textureLookups;
		if (hit)
		{
			++textureHits;
		}
	}

	void RecordBufferUpload()
	{
		++bufferUploads;
	}

	void RecordTextureRefresh()
	{
		++textureRefreshes;
	}

	unsigned int bufferLookups;
	unsigned int bufferHits;
	unsigned int textureLookups;
	unsigned int textureHits;
	unsigned int bufferUploads;
	unsigned int textureRefreshes;
};

} }
