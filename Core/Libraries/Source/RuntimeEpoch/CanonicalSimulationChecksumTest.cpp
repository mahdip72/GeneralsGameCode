#include "Lib/CanonicalSimulationChecksum.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace
{

using rts::runtime_epoch::Byte;
using namespace rts::canonical_simulation;

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

std::uint32_t Checksum(const Byte *bytes, std::size_t byteCount)
{
	return rts::runtime_epoch::CalculatePayloadChecksum(bytes, byteCount);
}

PartitionChecksum MakePartition(std::uint32_t sectionId,
	std::uint32_t partitionId,
	const Byte *bytes,
	std::size_t byteCount)
{
	const PartitionKey key = {sectionId, partitionId};
	ChecksumEncoder encoder;
	encoder.writeBytes(bytes, byteCount);
	return {key, encoder.finish(), encoder.byteCount()};
}

int TestStandardVectorAndEncoding()
{
	const std::array<Byte, 9> standard = {{'1', '2', '3', '4', '5', '6', '7', '8', '9'}};
	ChecksumEncoder standardEncoder;
	int result = 0;
	result |= Check(standardEncoder.writeBytes(standard.data(), standard.size()),
		"standard vector is accepted");
	result |= Check(standardEncoder.finish() == 0xcbf43926U,
		"CRC-32/ISO-HDLC standard vector matches");
	result |= Check(standardEncoder.byteCount() == standard.size(),
		"encoder counts standard vector bytes");

	ChecksumEncoder fields;
	fields.writePartitionKey({0x10203040U, 0x50607080U});
	fields.writeUInt8(0xabU);
	fields.writeBool(true);
	fields.writeUInt16(0x1234U);
	fields.writeUInt32(0x89abcdefU);
	fields.writeUInt64(UINT64_C(0x0123456789abcdef));
	fields.writeInt32(-2);
	fields.writeInt64(-3);
	const std::array<Byte, 36> expected = {{
		0x40U, 0x30U, 0x20U, 0x10U,
		0x80U, 0x70U, 0x60U, 0x50U,
		0xabU, 0x01U,
		0x34U, 0x12U,
		0xefU, 0xcdU, 0xabU, 0x89U,
		0xefU, 0xcdU, 0xabU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U,
		0xfeU, 0xffU, 0xffU, 0xffU,
		0xfdU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	}};
	result |= Check(fields.finish() == Checksum(expected.data(), expected.size()),
		"fixed-width fields use explicit little-endian bytes");
	result |= Check(fields.byteCount() == expected.size(),
		"fixed-width fields have contractual byte counts");
	result |= Check(!fields.writeBytes(nullptr, 1U),
		"non-empty null byte input is rejected");
	return result;
}

int TestFloatBitEncoding()
{
	const std::array<std::uint32_t, 3> floatBits = {{
		0x3f800000U, 0x80000000U, 0x7fc01234U
	}};
	const std::uint64_t doubleBits = UINT64_C(0x8000000000000000);
	ChecksumEncoder encoder;
	for (std::uint32_t bits : floatBits)
	{
		float value = 0.0F;
		std::memcpy(&value, &bits, sizeof(value));
		encoder.writeFloat32Bits(value);
	}
	double doubleValue = 0.0;
	std::memcpy(&doubleValue, &doubleBits, sizeof(doubleValue));
	encoder.writeFloat64Bits(doubleValue);

	const std::array<Byte, 20> expected = {{
		0x00U, 0x00U, 0x80U, 0x3fU,
		0x00U, 0x00U, 0x00U, 0x80U,
		0x34U, 0x12U, 0xc0U, 0x7fU,
		0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U
	}};
	int result = 0;
	result |= Check(encoder.finish() == Checksum(expected.data(), expected.size()),
		"float encoders preserve exact bit patterns in little-endian order");
	result |= Check(encoder.byteCount() == expected.size(),
		"float encoders use fixed byte widths");
	return result;
}

int TestSplitCombineAndAssociativity()
{
	std::vector<Byte> payload(4096U);
	std::uint32_t state = 0x4d595df4U;
	for (Byte &byte : payload)
	{
		state = state * 1664525U + 1013904223U;
		byte = static_cast<Byte>(state >> 24U);
	}

	const std::uint32_t whole = Checksum(payload.data(), payload.size());
	int result = 0;
	state = 0xa341316cU;
	for (unsigned iteration = 0U; iteration < 512U; ++iteration)
	{
		state = state * 1664525U + 1013904223U;
		const std::size_t split = state % (payload.size() + 1U);
		const std::uint32_t prefix = Checksum(payload.data(), split);
		const std::uint32_t suffix = Checksum(payload.data() + split,
			payload.size() - split);
		result |= Check(CombineChecksums(prefix, suffix, payload.size() - split) == whole,
			"random split combines to the one-shot checksum");
	}

	const std::size_t firstEnd = 777U;
	const std::size_t secondEnd = 3001U;
	const std::uint32_t first = Checksum(payload.data(), firstEnd);
	const std::uint32_t second = Checksum(payload.data() + firstEnd,
		secondEnd - firstEnd);
	const std::uint32_t third = Checksum(payload.data() + secondEnd,
		payload.size() - secondEnd);
	const std::uint32_t leftAssociated = CombineChecksums(
		CombineChecksums(first, second, secondEnd - firstEnd),
		third, payload.size() - secondEnd);
	const std::uint32_t secondAndThird = CombineChecksums(second, third,
		payload.size() - secondEnd);
	const std::uint32_t rightAssociated = CombineChecksums(first,
		secondAndThird, payload.size() - firstEnd);
	result |= Check(leftAssociated == whole && rightAssociated == whole,
		"checksum combine is associative over ordered byte ranges");
	result |= Check(CombineChecksums(whole, 0U, 0U) == whole,
		"empty suffix preserves the prefix checksum");
	return result;
}

int TestUint64SuffixLength()
{
	const Byte zero = 0U;
	std::uint32_t halfChecksum = Checksum(&zero, 1U);
	std::uint64_t halfLength = 1U;
	while (halfLength < (UINT64_C(1) << 31U))
	{
		halfChecksum = CombineChecksums(halfChecksum, halfChecksum, halfLength);
		halfLength *= 2U;
	}
	const std::uint32_t suffixChecksum = CombineChecksums(
		halfChecksum, halfChecksum, halfLength);
	const std::uint64_t suffixLength = halfLength * 2U;

	const std::array<Byte, 6> prefixBytes = {{'p', 'r', 'e', 'f', 'i', 'x'}};
	const std::uint32_t prefix = Checksum(prefixBytes.data(), prefixBytes.size());
	const std::uint32_t direct = CombineChecksums(prefix, suffixChecksum, suffixLength);
	const std::uint32_t viaHalves = CombineChecksums(
		CombineChecksums(prefix, halfChecksum, halfLength),
		halfChecksum, halfLength);
	return Check(suffixLength == (UINT64_C(1) << 32U) && direct == viaHalves,
		"combine preserves suffix lengths at and above 4 GiB");
}

int TestFixedOrderFold()
{
	const std::array<Byte, 3> firstBytes = {{0x10U, 0x20U, 0x30U}};
	const std::array<Byte, 2> secondBytes = {{0x40U, 0x50U}};
	const std::array<Byte, 4> thirdBytes = {{0x60U, 0x70U, 0x80U, 0x90U}};

	PartitionChecksum fixedSlots[3];
	// Model reverse completion: workers write their predetermined logical slots.
	fixedSlots[2] = MakePartition(2U, 0U, thirdBytes.data(), thirdBytes.size());
	fixedSlots[0] = MakePartition(1U, 0U, firstBytes.data(), firstBytes.size());
	fixedSlots[1] = MakePartition(1U, 4U, secondBytes.data(), secondBytes.size());

	FoldedChecksum folded;
	int result = 0;
	result |= Check(FoldPartitionChecksums(fixedSlots, 3U, &folded),
		"fixed logical slots fold after arbitrary completion order");
	const std::array<Byte, 33> concatenated = {{
		0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
		0x10U, 0x20U, 0x30U,
		0x01U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U,
		0x40U, 0x50U,
		0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
		0x60U, 0x70U, 0x80U, 0x90U
	}};
	result |= Check(folded.checksum == Checksum(concatenated.data(), concatenated.size()) &&
		folded.byteCount == concatenated.size(),
		"fixed-order fold matches canonical concatenation");
	PartitionChecksum changedKey[3] = {fixedSlots[0], fixedSlots[1], fixedSlots[2]};
	changedKey[2].key.partitionId = 9U;
	FoldedChecksum changedKeyFold = {};
	result |= Check(FoldPartitionChecksums(changedKey, 3U, &changedKeyFold) &&
		changedKeyFold.checksum != folded.checksum,
		"partition identity participates in the canonical checksum");

	const PartitionChecksum completionOrder[3] = {
		fixedSlots[2], fixedSlots[0], fixedSlots[1]
	};
	FoldedChecksum rejected = {0x11223344U, 55U};
	result |= Check(!FoldPartitionChecksums(completionOrder, 3U, &rejected) &&
		rejected.checksum == 0x11223344U && rejected.byteCount == 55U,
		"completion-order input is rejected without partial publication");
	const PartitionChecksum duplicate[2] = {fixedSlots[0], fixedSlots[0]};
	result |= Check(!FoldPartitionChecksums(duplicate, 2U, &rejected),
		"duplicate logical partition keys are rejected");

	FoldedChecksum empty = {1U, 1U};
	result |= Check(FoldPartitionChecksums(nullptr, 0U, &empty) &&
		empty.checksum == 0U && empty.byteCount == 0U,
		"empty partition arrays fold to the empty checksum");
	const PartitionChecksum malformedEmpty = {{3U, 0U}, 1U, 0U};
	result |= Check(!FoldPartitionChecksums(&malformedEmpty, 1U, &rejected),
		"malformed empty partition checksums are rejected");
	const PartitionChecksum overflowing[2] = {
		{{4U, 0U}, 0U, std::numeric_limits<std::uint64_t>::max()},
		{{4U, 1U}, Checksum(firstBytes.data(), 1U), 1U}
	};
	result |= Check(!FoldPartitionChecksums(overflowing, 2U, &rejected),
		"folded byte-count overflow is rejected");
	return result;
}

} // namespace

int main()
{
	static_assert(kEncodedPartitionKeyBytes == 8U);
	return TestStandardVectorAndEncoding() |
		TestFloatBitEncoding() |
		TestSplitCombineAndAssociativity() |
		TestUint64SuffixLength() |
		TestFixedOrderFold();
}
