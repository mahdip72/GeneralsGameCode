#ifndef RTS_REPLAY_PATH_CONTRACT_H
#define RTS_REPLAY_PATH_CONTRACT_H

#include <stddef.h>

namespace rts
{
namespace replay
{

inline bool IsReplayPathSeparator(char value)
{
	return value == '\\' || value == '/';
}

inline bool IsReplayPathConcreteComponent(const char *value, size_t length)
{
	return length != 0 && !(length == 1 && value[0] == '.') &&
		!(length == 2 && value[0] == '.' && value[1] == '.');
}

// Callers provide separate input/output storage. Failure always clears a
// writable output buffer; no partial path may reach a file open or hasher.
// Native callers use MAX_PATH storage. This does not enable extended/device
// namespaces, current-drive roots, drive-relative names or absolute globs.
inline bool ResolveReplayPlaybackPath(const char *replayDirectory,
	const char *filename, bool nativeHeadlessPlayback,
	char *resolved, size_t capacity)
{
	if (resolved == 0 || capacity == 0) return false;
	resolved[0] = '\0';
	if (replayDirectory == 0 || filename == 0) return false;

	size_t filenameLength = 0;
	while (filename[filenameLength] != '\0')
	{
		if (filenameLength >= capacity - 1) return false;
		++filenameLength;
	}
	bool absolute = false;
	if (nativeHeadlessPlayback)
	{
		if (filenameLength == 0) return false;
		size_t firstComponent = 0;
		if (filenameLength >= 2 && filename[1] == ':')
		{
			const char drive = filename[0];
			if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) ||
				filenameLength <= 3 || !IsReplayPathSeparator(filename[2])) return false;
			absolute = true;
			firstComponent = 3;
		}
		else if (IsReplayPathSeparator(filename[0]))
		{
			if (filenameLength < 2 || filename[1] != filename[0]) return false;
			absolute = true;
			firstComponent = 2;
			size_t serverEnd = firstComponent;
			while (serverEnd < filenameLength && !IsReplayPathSeparator(filename[serverEnd])) ++serverEnd;
			if (!IsReplayPathConcreteComponent(filename + firstComponent, serverEnd - firstComponent) ||
				serverEnd == filenameLength) return false;
			const size_t shareStart = serverEnd + 1;
			size_t shareEnd = shareStart;
			while (shareEnd < filenameLength && !IsReplayPathSeparator(filename[shareEnd])) ++shareEnd;
			if (!IsReplayPathConcreteComponent(filename + shareStart, shareEnd - shareStart) ||
				shareEnd == filenameLength) return false;
		}
		if (absolute)
		{
			size_t component = firstComponent;
			for (size_t index = firstComponent; index < filenameLength; ++index)
			{
				const unsigned char value = static_cast<unsigned char>(filename[index]);
				if (value < 32 || value == ':' || value == '"' || value == '<' || value == '>' ||
					value == '|' || value == '*' || value == '?') return false;
				if (IsReplayPathSeparator(filename[index]))
				{
					if (index == component) return false;
					component = index + 1;
				}
			}
			if (!IsReplayPathConcreteComponent(filename + component, filenameLength - component)) return false;
		}
		else
		{
			// A colon cannot name a relative replay file; in particular, never
			// interpret a drive-relative or alternate-stream name as a root.
			for (size_t relativeIndex = 0; relativeIndex < filenameLength; ++relativeIndex)
				if (filename[relativeIndex] == ':') return false;
		}
	}
	size_t directoryLength = 0;
	while (!absolute && replayDirectory[directoryLength] != '\0')
	{
		if (directoryLength >= capacity - 1 - filenameLength) return false;
		++directoryLength;
	}
	for (size_t i = 0; i < directoryLength; ++i)
		resolved[i] = replayDirectory[i];
	for (size_t j = 0; j <= filenameLength; ++j)
		resolved[directoryLength + j] = filename[j];
	return true;
}

} // namespace replay
} // namespace rts

#endif
