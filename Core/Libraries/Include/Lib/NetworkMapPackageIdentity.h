#pragma once

#include <cstddef>
#include <cstdint>

namespace rts
{
namespace network_epoch
{

constexpr std::size_t kNetworkMapPackageCompanionCount = 6U;

// The map package identity intentionally includes presentation/documentation
// companions as well as simulation inputs. The map file itself has its own CRC.
constexpr std::uint32_t kNetworkMapPackageCompanionMaskBits[] =
	{ 2U, 4U, 8U, 16U, 32U, 64U };

// CompanionReader contract:
//   open(index, &length) -> -1 on error, 0 when absent, 1 when present
//   read(buffer, requested) -> number of bytes read (positive while content remains)
//   close() closes the current entry; it is called after every successful open
// The reader can project staged bytes in place of installed files. Keeping this
// stream/framing logic independent lets tests exercise the same six-file
// producer without initializing the game filesystem.
template <typename CompanionReader, typename Crc>
inline bool ComputeNetworkMapPackageIdentity(CompanionReader &reader, Crc &crc,
	std::uint32_t *maskOut, std::uint32_t *crcOut)
{
	if (crcOut == nullptr)
		return false;
	*crcOut = 0U;
	if (maskOut != nullptr)
		*maskOut = 0U;

	crc.clear();
	const unsigned char domain[] = { 'M', 'A', 'P', 'C', 'O', 'M', 1U };
	crc.computeCRC(domain, static_cast<int>(sizeof(domain)));

	for (std::size_t i = 0; i < kNetworkMapPackageCompanionCount; ++i)
	{
		const unsigned char kind = static_cast<unsigned char>(i + 1U);
		crc.computeCRC(&kind, 1);

		std::uint32_t length = 0U;
		const int opened = reader.open(i, &length);
		if (opened < 0 || opened > 1)
			return false;
		const unsigned char present = opened == 1 ? 1U : 0U;
		crc.computeCRC(&present, 1);
		if (opened == 0)
			continue;

		if (maskOut != nullptr)
			*maskOut |= kNetworkMapPackageCompanionMaskBits[i];
		const unsigned char lengthBytes[] = {
			static_cast<unsigned char>(length),
			static_cast<unsigned char>(length >> 8),
			static_cast<unsigned char>(length >> 16),
			static_cast<unsigned char>(length >> 24)
		};
		crc.computeCRC(lengthBytes, static_cast<int>(sizeof(lengthBytes)));

		unsigned char buffer[4096];
		std::uint32_t remaining = length;
		bool complete = true;
		while (remaining > 0U)
		{
			const int wanted = remaining < sizeof(buffer) ?
				static_cast<int>(remaining) : static_cast<int>(sizeof(buffer));
			const int count = reader.read(buffer, wanted);
			if (count <= 0 || count > wanted)
			{
				complete = false;
				break;
			}
			crc.computeCRC(buffer, count);
			remaining -= static_cast<std::uint32_t>(count);
		}
		reader.close();
		if (!complete)
			return false;
	}

	*crcOut = static_cast<std::uint32_t>(crc.get());
	return true;
}

} // namespace network_epoch
} // namespace rts
