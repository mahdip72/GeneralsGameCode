#pragma once

#include "Lib/RuntimeEpochContract.h"

#include <cstddef>
#include <cstdint>

namespace rts
{
namespace canonical_simulation
{

constexpr std::size_t kEncodedPartitionKeyBytes = 8U;

// Stable logical identity for one canonical state partition. Execution jobs
// may process these keys in any order, but results are folded by this key.
struct PartitionKey
{
	std::uint32_t sectionId = 0U;
	std::uint32_t partitionId = 0U;

	constexpr bool operator==(const PartitionKey &other) const
	{
		return sectionId == other.sectionId && partitionId == other.partitionId;
	}

	constexpr bool operator<(const PartitionKey &other) const
	{
		return sectionId < other.sectionId ||
			(sectionId == other.sectionId && partitionId < other.partitionId);
	}
};

struct PartitionChecksum
{
	PartitionKey key;
	// Checksum and byteCount cover only this partition's canonical payload.
	// FoldPartitionChecksums encodes the logical key exactly once.
	std::uint32_t checksum = 0U;
	std::uint64_t byteCount = 0U;
};

struct FoldedChecksum
{
	std::uint32_t checksum = 0U;
	std::uint64_t byteCount = 0U;
};

// Streams fixed-width canonical fields directly into CRC-32/ISO-HDLC. Signed
// integers use their two's-complement unsigned representation. Floating-point
// values preserve their exact IEEE bit pattern and perform no numeric work.
class ChecksumEncoder
{
public:
	void writePartitionKey(const PartitionKey &key);
	void writeBool(bool value);
	void writeUInt8(std::uint8_t value);
	void writeUInt16(std::uint16_t value);
	void writeUInt32(std::uint32_t value);
	void writeUInt64(std::uint64_t value);
	void writeInt32(std::int32_t value);
	void writeInt64(std::int64_t value);
	void writeFloat32Bits(float value);
	void writeFloat64Bits(double value);
	bool writeBytes(const runtime_epoch::Byte *bytes, std::size_t byteCount);

	std::uint32_t finish() const { return m_checksum.finish(); }
	std::uint64_t byteCount() const { return m_checksum.byteCount(); }

private:
	template <typename Unsigned>
	void writeLittleEndian(Unsigned value);

	runtime_epoch::PayloadChecksumAccumulator m_checksum;
};

// Returns CRC(prefix || suffix) from finalized CRC-32/ISO-HDLC values. The
// suffix length is 64-bit so large canonical streams cannot truncate at 4 GiB.
std::uint32_t CombineChecksums(std::uint32_t prefixChecksum,
	std::uint32_t suffixChecksum,
	std::uint64_t suffixByteCount);

// Folds a fixed logical result array in ascending key order. Completion-order
// arrays, duplicate keys, malformed empty partitions, and length overflow are
// rejected without publishing a partial result. An empty array folds to the
// CRC-32/ISO-HDLC checksum of an empty stream (zero).
bool FoldPartitionChecksums(const PartitionChecksum *partitions,
	std::size_t partitionCount,
	FoldedChecksum *output);

} // namespace canonical_simulation
} // namespace rts
