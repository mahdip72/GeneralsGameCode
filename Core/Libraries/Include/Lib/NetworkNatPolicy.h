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

inline bool IsValidNatAddress(unsigned int address)
{
	return address != 0U;
}

struct NativePortMessage
{
	int node;
	unsigned int port;
	unsigned int address;
	unsigned int probeCookie;
};

// The GameSpy UTM option is a null-terminated control string, but it is still
// bounded here before any token parser is allowed to inspect it. Keep this
// native parser separate from the legacy Win32 sscanf path below.
static const unsigned int kNativePortMessageMaxLength = 64U;

inline bool IsNativePortWhitespace(char value)
{
	return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
		value == '\v' || value == '\f';
}

inline void SkipNativePortWhitespace(const char *&cursor, const char *end)
{
	while (cursor < end && IsNativePortWhitespace(*cursor))
		++cursor;
}

inline bool ParseNativePortUnsignedToken(const char *&cursor, const char *end,
	unsigned int base, unsigned int *value)
{
	if (value == nullptr || (base != 10U && base != 16U))
		return false;

	SkipNativePortWhitespace(cursor, end);
	if (base == 16U && cursor < end && (end - cursor) > 1 && cursor[0] == '0' &&
		(cursor[1] == 'x' || cursor[1] == 'X'))
	{
		cursor += 2;
	}

	bool hasDigit = false;
	unsigned int parsed = 0U;
	while (cursor < end)
	{
		const char current = *cursor;
		unsigned int digit = 0U;
		if (current >= '0' && current <= '9')
			digit = static_cast<unsigned int>(current - '0');
		else if (base == 16U && current >= 'a' && current <= 'f')
			digit = static_cast<unsigned int>(current - 'a') + 10U;
		else if (base == 16U && current >= 'A' && current <= 'F')
			digit = static_cast<unsigned int>(current - 'A') + 10U;
		else
			break;

		if (digit >= base || parsed > ((~0U) - digit) / base)
			return false;
		parsed = parsed * base + digit;
		hasDigit = true;
		++cursor;
	}

	if (!hasDigit)
		return false;
	*value = parsed;
	return true;
}

inline bool RequireNativePortDelimiter(const char *&cursor, const char *end)
{
	if (cursor >= end || !IsNativePortWhitespace(*cursor))
		return false;
	SkipNativePortWhitespace(cursor, end);
	return true;
}

inline bool ParseNativePortNodeToken(const char *&cursor, const char *end,
	int *node)
{
	if (node == nullptr)
		return false;

	unsigned int parsed = 0U;
	if (!ParseNativePortUnsignedToken(cursor, end, 10U, &parsed) ||
		parsed > 0x7fffffffU)
	{
		return false;
	}
	*node = static_cast<int>(parsed);
	return true;
}

inline bool TryParseNativePortMessage(const char *input, NativePortMessage *message)
{
	if (input == nullptr || message == nullptr)
		return false;

	unsigned int length = 0U;
	while (length < kNativePortMessageMaxLength && input[length] != '\0')
		++length;
	if (length == kNativePortMessageMaxLength)
		return false;

	const char *cursor = input;
	const char *end = input + length;
	NativePortMessage parsed = {-1, 0U, 0U, 0U};
	if (!ParseNativePortNodeToken(cursor, end, &parsed.node) ||
		!RequireNativePortDelimiter(cursor, end) ||
		!ParseNativePortUnsignedToken(cursor, end, 10U, &parsed.port) ||
		!RequireNativePortDelimiter(cursor, end) ||
		!ParseNativePortUnsignedToken(cursor, end, 16U, &parsed.address) ||
		!RequireNativePortDelimiter(cursor, end) ||
		!ParseNativePortUnsignedToken(cursor, end, 16U, &parsed.probeCookie))
	{
		return false;
	}

	SkipNativePortWhitespace(cursor, end);
	if (cursor != end)
		return false;

	*message = parsed;
	return true;
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
