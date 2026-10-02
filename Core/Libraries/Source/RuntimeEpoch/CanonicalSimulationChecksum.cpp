#include "Lib/CanonicalSimulationChecksum.h"

#include <cstring>
#include <limits>

namespace rts
{
namespace canonical_simulation
{
namespace
{

constexpr std::uint32_t kCrc32IsoHdlcReversedPolynomial = 0xedb88320U;

std::uint32_t Gf2MatrixTimes(const std::uint32_t *matrix, std::uint32_t vector)
{
	std::uint32_t sum = 0U;
	while (vector != 0U)
	{
		if ((vector & 1U) != 0U)
		{
			sum ^= *matrix;
		}
		vector >>= 1U;
		++matrix;
	}
	return sum;
}

void Gf2MatrixSquare(std::uint32_t *square, const std::uint32_t *matrix)
{
	for (unsigned index = 0U; index < 32U; ++index)
	{
		square[index] = Gf2MatrixTimes(matrix, matrix[index]);
	}
}

} // namespace

template <typename Unsigned>
void ChecksumEncoder::writeLittleEndian(Unsigned value)
{
	runtime_epoch::Byte bytes[sizeof(Unsigned)] = {};
	for (std::size_t index = 0U; index < sizeof(Unsigned); ++index)
	{
		bytes[index] = static_cast<runtime_epoch::Byte>(
			value & static_cast<Unsigned>(0xffU));
		value >>= 8U;
	}
	m_checksum.update(bytes, sizeof(bytes));
}

void ChecksumEncoder::writePartitionKey(const PartitionKey &key)
{
	writeUInt32(key.sectionId);
	writeUInt32(key.partitionId);
}

void ChecksumEncoder::writeBool(bool value)
{
	writeUInt8(value ? 1U : 0U);
}

void ChecksumEncoder::writeUInt8(std::uint8_t value)
{
	writeLittleEndian(value);
}

void ChecksumEncoder::writeUInt16(std::uint16_t value)
{
	writeLittleEndian(value);
}

void ChecksumEncoder::writeUInt32(std::uint32_t value)
{
	writeLittleEndian(value);
}

void ChecksumEncoder::writeUInt64(std::uint64_t value)
{
	writeLittleEndian(value);
}

void ChecksumEncoder::writeInt32(std::int32_t value)
{
	writeUInt32(static_cast<std::uint32_t>(value));
}

void ChecksumEncoder::writeInt64(std::int64_t value)
{
	writeUInt64(static_cast<std::uint64_t>(value));
}

void ChecksumEncoder::writeFloat32Bits(float value)
{
	static_assert(sizeof(value) == sizeof(std::uint32_t));
	static_assert(std::numeric_limits<float>::is_iec559);
	std::uint32_t bits = 0U;
	std::memcpy(&bits, &value, sizeof(bits));
	writeUInt32(bits);
}

void ChecksumEncoder::writeFloat64Bits(double value)
{
	static_assert(sizeof(value) == sizeof(std::uint64_t));
	static_assert(std::numeric_limits<double>::is_iec559);
	std::uint64_t bits = 0U;
	std::memcpy(&bits, &value, sizeof(bits));
	writeUInt64(bits);
}

bool ChecksumEncoder::writeBytes(const runtime_epoch::Byte *bytes,
	std::size_t byteCount)
{
	if (bytes == nullptr && byteCount != 0U)
	{
		return false;
	}
	m_checksum.update(bytes, byteCount);
	return true;
}

std::uint32_t CombineChecksums(std::uint32_t prefixChecksum,
	std::uint32_t suffixChecksum,
	std::uint64_t suffixByteCount)
{
	if (suffixByteCount == 0U)
	{
		return prefixChecksum;
	}

	std::uint32_t odd[32] = {};
	std::uint32_t even[32] = {};
	odd[0] = kCrc32IsoHdlcReversedPolynomial;
	std::uint32_t row = 1U;
	for (unsigned index = 1U; index < 32U; ++index)
	{
		odd[index] = row;
		row <<= 1U;
	}

	Gf2MatrixSquare(even, odd);
	Gf2MatrixSquare(odd, even);
	do
	{
		Gf2MatrixSquare(even, odd);
		if ((suffixByteCount & 1U) != 0U)
		{
			prefixChecksum = Gf2MatrixTimes(even, prefixChecksum);
		}
		suffixByteCount >>= 1U;
		if (suffixByteCount == 0U)
		{
			break;
		}

		Gf2MatrixSquare(odd, even);
		if ((suffixByteCount & 1U) != 0U)
		{
			prefixChecksum = Gf2MatrixTimes(odd, prefixChecksum);
		}
		suffixByteCount >>= 1U;
	}
	while (suffixByteCount != 0U);

	return prefixChecksum ^ suffixChecksum;
}

bool FoldPartitionChecksums(const PartitionChecksum *partitions,
	std::size_t partitionCount,
	FoldedChecksum *output)
{
	if (output == nullptr || (partitions == nullptr && partitionCount != 0U))
	{
		return false;
	}

	FoldedChecksum folded;
	for (std::size_t index = 0U; index < partitionCount; ++index)
	{
		const PartitionChecksum &partition = partitions[index];
		if (index != 0U && !(partitions[index - 1U].key < partition.key))
		{
			return false;
		}
		if (partition.byteCount == 0U && partition.checksum != 0U)
		{
			return false;
		}
		if (folded.byteCount >
			std::numeric_limits<std::uint64_t>::max() - kEncodedPartitionKeyBytes ||
			partition.byteCount > std::numeric_limits<std::uint64_t>::max() -
				(folded.byteCount + kEncodedPartitionKeyBytes))
		{
			return false;
		}

		ChecksumEncoder keyEncoder;
		keyEncoder.writePartitionKey(partition.key);
		folded.checksum = CombineChecksums(folded.checksum,
			keyEncoder.finish(), keyEncoder.byteCount());
		folded.byteCount += keyEncoder.byteCount();
		folded.checksum = CombineChecksums(folded.checksum,
			partition.checksum, partition.byteCount);
		folded.byteCount += partition.byteCount;
	}

	*output = folded;
	return true;
}

} // namespace canonical_simulation
} // namespace rts
