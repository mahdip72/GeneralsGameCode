#include "Common/BezierSegment.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

namespace
{

struct PointBits
{
	std::uint32_t x;
	std::uint32_t y;
	std::uint32_t z;
};

constexpr std::array<PointBits, 10> LEGACY_SCORPION_SHELL_PATH = {{
	{0x44D63D79U, 0x44D0C5F2U, 0x41CFAA2EU},
	{0x44D86F43U, 0x44CFE669U, 0x41E73DCAU},
	{0x44DA81C9U, 0x44CF1351U, 0x41F8211EU},
	{0x44DC6F5DU, 0x44CE4EEDU, 0x4200C875U},
	{0x44DE324FU, 0x44CD9B7FU, 0x420164F9U},
	{0x44DFC4F0U, 0x44CCFB4BU, 0x41FB08F6U},
	{0x44E12192U, 0x44CC7093U, 0x41E98AB8U},
	{0x44E24285U, 0x44CBFD9BU, 0x41CD8BF9U},
	{0x44E3221AU, 0x44CBA4A5U, 0x41A6497AU},
	{0x44E3BAA2U, 0x44CB67F5U, 0x4165FFFAU},
}};

int CheckLegacyPath()
{
	const Coord3D controlPoints[4] = {
		{std::bit_cast<float>(0x44D63D79U), std::bit_cast<float>(0x44D0C5F2U), std::bit_cast<float>(0x41CFAA2EU)},
		{std::bit_cast<float>(0x44DCFC0CU), std::bit_cast<float>(0x44CE16F3U), std::bit_cast<float>(0x420FD517U)},
		{std::bit_cast<float>(0x44E2614EU), std::bit_cast<float>(0x44CBF15AU), std::bit_cast<float>(0x420FD517U)},
		{std::bit_cast<float>(0x44E3BA9FU), std::bit_cast<float>(0x44CB67F4U), std::bit_cast<float>(0x41660000U)},
	};
	BezierSegment segment(controlPoints[0], controlPoints[1], controlPoints[2], controlPoints[3]);
	VecCoord3D path;
	segment.getSegmentPoints(static_cast<Int>(LEGACY_SCORPION_SHELL_PATH.size()), &path);
	if (path.size() != LEGACY_SCORPION_SHELL_PATH.size())
	{
		std::fprintf(stderr, "FAIL: expected %zu path points, got %zu\n",
			LEGACY_SCORPION_SHELL_PATH.size(), path.size());
		return 1;
	}

	for (std::size_t index = 0; index < path.size(); ++index)
	{
		const PointBits actual = {
			std::bit_cast<std::uint32_t>(path[index].x),
			std::bit_cast<std::uint32_t>(path[index].y),
			std::bit_cast<std::uint32_t>(path[index].z),
		};
		const PointBits expected = LEGACY_SCORPION_SHELL_PATH[index];
		if (actual.x != expected.x || actual.y != expected.y || actual.z != expected.z)
		{
			std::fprintf(stderr,
				"FAIL: path point %zu expected %08X,%08X,%08X, got %08X,%08X,%08X\n",
				index, expected.x, expected.y, expected.z, actual.x, actual.y, actual.z);
			return 1;
		}
	}
	return 0;
}

} // namespace

int main()
{
	return CheckLegacyPath();
}
