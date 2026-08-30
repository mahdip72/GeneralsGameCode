#pragma once

// Small, legacy-compatible predicates shared by the NAT control path and its
// executable contract tests.  Keep these independent of the game object
// graph: the production path performs the slot/node lookup after these bounds
// and provenance checks have passed.
namespace rts
{
namespace network_nat
{

inline bool IsValidNode(int node, unsigned int nodeCount, unsigned int maxNodes)
{
	return node >= 0 && static_cast<unsigned int>(node) < nodeCount &&
		static_cast<unsigned int>(node) < maxNodes;
}

inline bool IsValidConnectionPairState(int pairIndex, unsigned int round,
	unsigned int pairCount)
{
	return pairIndex >= 0 && static_cast<unsigned int>(pairIndex) < pairCount &&
		round < pairCount;
}

inline bool IsValidControlSource(int slot, unsigned int maxSlots)
{
	return slot >= 0 && static_cast<unsigned int>(slot) < maxSlots;
}

inline bool IsValidNatPort(unsigned int port)
{
	return port >= 1024U && port <= 65535U;
}

#if defined(_WIN64)
// A raw probe may learn a new public address only after the expected target
// has announced its port and per-round cookie through the validated player-UTM
// path.  The address is intentionally not compared with the prior PORT or slot
// address because a NAT remap can change it; the cookie binds the datagram to
// that validated announcement for this connection round.
inline bool IsExpectedProbeSource(int expectedNode, int observedNode,
	unsigned int expectedCookie, unsigned int observedCookie,
	unsigned int observedAddress, unsigned int nodeCount,
	unsigned int maxNodes)
{
	return IsValidNode(expectedNode, nodeCount, maxNodes) &&
		IsValidNode(observedNode, nodeCount, maxNodes) &&
		expectedCookie != 0U && expectedCookie == observedCookie &&
		observedAddress != 0U && expectedNode == observedNode;
}

// Probe generations are per-round unsigned serial numbers.  Zero is reserved
// as the uninitialized value, and the half-range rule makes wrap-around
// deterministic while rejecting duplicates and delayed older probes.
inline bool IsNewerProbeGeneration(unsigned int observedGeneration,
	unsigned int lastAcceptedGeneration)
{
	const unsigned int delta = observedGeneration - lastAcceptedGeneration;
	return observedGeneration != 0U && delta != 0U && delta < 0x80000000U;
}

inline bool IsNewProbeEpoch(int previousNode, unsigned int previousCookie,
	int announcedNode, unsigned int announcedCookie)
{
	return previousNode != announcedNode || previousCookie != announcedCookie;
}
#endif

}
}
