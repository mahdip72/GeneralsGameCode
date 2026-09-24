#pragma once

#include <windows.h>

#include <ctype.h>
#include <string.h>

namespace rts { namespace validation {

enum ProcessLocalProfileRootResult
{
	PROCESS_LOCAL_PROFILE_ROOT_NOT_SET = 0,
	PROCESS_LOCAL_PROFILE_ROOT_VALID = 1,
	PROCESS_LOCAL_PROFILE_ROOT_INVALID = 2
};

inline const volatile char *ProcessLocalProfileRootCapabilityMarker()
{
	static const volatile char marker[] =
		"RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1";
	return marker;
}

inline bool IsProcessLocalProfileRootDriveLetter(char value)
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

inline bool IsProcessLocalProfileRootSeparator(char value)
{
	return value == '\\' || value == '/';
}

inline bool IsProcessLocalProfileRootForbiddenCharacter(char value)
{
	return value < 0x20 || value == '"' || value == '*' || value == '?' ||
		value == '<' || value == '>' || value == '|' || value == ':';
}

inline bool IsProcessLocalProfileRootPathValid(const char *path, DWORD length)
{
	if (path == NULL || length < 4 || length >= MAX_PATH ||
		!IsProcessLocalProfileRootDriveLetter(path[0]) || path[1] != ':' ||
		!IsProcessLocalProfileRootSeparator(path[2]))
		return false;

	// The drive root itself is not a profile. A single trailing separator is
	// accepted; repeated trailing separators remain invalid.
	if (length == 3)
		return false;
	if (IsProcessLocalProfileRootSeparator(path[length - 1]))
	{
		if (length == 3 || IsProcessLocalProfileRootSeparator(path[length - 2]))
			return false;
		--length;
	}

	DWORD segmentStart = 3;
	for (DWORD index = 3; index <= length; ++index)
	{
		const bool atEnd = index == length;
		if (!atEnd && !IsProcessLocalProfileRootSeparator(path[index]))
		{
			if (IsProcessLocalProfileRootForbiddenCharacter(path[index]))
				return false;
			continue;
		}

		const DWORD segmentLength = index - segmentStart;
		if (segmentLength == 0 || segmentLength == 1 && path[segmentStart] == '.' ||
			segmentLength == 2 && path[segmentStart] == '.' &&
			path[segmentStart + 1] == '.')
			return false;
		segmentStart = index + 1;
	}

	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// The value is the complete profile directory, including the title leaf. The
// caller must not append a title-specific leaf to the returned path. An
// existing but malformed value is intentionally distinguished from an absent
// value so callers can fail closed instead of falling back to live Documents.
inline ProcessLocalProfileRootResult ReadProcessLocalProfileRoot(
	char *buffer, DWORD capacity)
{
	const volatile char *capabilityMarker =
		ProcessLocalProfileRootCapabilityMarker();
	if (capabilityMarker[0] == '\0')
		return PROCESS_LOCAL_PROFILE_ROOT_INVALID;
	static const char *const environmentName =
		"RTS_STAGE5_VALIDATION_PROFILE_ROOT";
	if (buffer == NULL || capacity < 5)
		return PROCESS_LOCAL_PROFILE_ROOT_INVALID;
	buffer[0] = '\0';

	SetLastError(ERROR_SUCCESS);
	DWORD length = GetEnvironmentVariableA(environmentName, buffer, capacity);
	if (length == 0)
	{
		return GetLastError() == ERROR_ENVVAR_NOT_FOUND
			? PROCESS_LOCAL_PROFILE_ROOT_NOT_SET
			: PROCESS_LOCAL_PROFILE_ROOT_INVALID;
	}
	if (length >= capacity || !IsProcessLocalProfileRootPathValid(buffer, length))
	{
		buffer[0] = '\0';
		return PROCESS_LOCAL_PROFILE_ROOT_INVALID;
	}

	if (!IsProcessLocalProfileRootSeparator(buffer[length - 1]))
	{
		if (length + 1 >= capacity)
		{
			buffer[0] = '\0';
			return PROCESS_LOCAL_PROFILE_ROOT_INVALID;
		}
		buffer[length++] = '\\';
		buffer[length] = '\0';
	}
	return PROCESS_LOCAL_PROFILE_ROOT_VALID;
}

} }
