/*
**	Command & Conquer Generals(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include <windows.h>

inline REGSAM GetRetailRegistryAccessMask(REGSAM access)
{
#if defined(_WIN64)
	return (access & ~KEY_WOW64_64KEY) | KEY_WOW64_32KEY;
#else
	return access;
#endif
}

inline REGSAM GetNativeRegistryAccessMask(REGSAM access)
{
#if defined(_WIN64)
	return (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
#else
	return access;
#endif
}

inline LONG OpenRetailRegistryKey(HKEY root, const char *path, DWORD options,
	REGSAM access, HKEY *handle)
{
	return RegOpenKeyExA(root, path, options,
		GetRetailRegistryAccessMask(access), handle);
}

inline LONG OpenNativeRegistryKey(HKEY root, const char *path, DWORD options,
	REGSAM access, HKEY *handle)
{
	return RegOpenKeyExA(root, path, options,
		GetNativeRegistryAccessMask(access), handle);
}

inline LONG CreateRetailRegistryKey(HKEY root, const char *path, DWORD reserved,
	char *keyClass, DWORD options, REGSAM access,
	LPSECURITY_ATTRIBUTES securityAttributes, HKEY *handle, LPDWORD disposition)
{
	return RegCreateKeyExA(root, path, reserved, keyClass, options,
		GetRetailRegistryAccessMask(access), securityAttributes, handle, disposition);
}

inline LONG CreateNativeRegistryKey(HKEY root, const char *path, DWORD reserved,
	char *keyClass, DWORD options, REGSAM access,
	LPSECURITY_ATTRIBUTES securityAttributes, HKEY *handle, LPDWORD disposition)
{
	return RegCreateKeyExA(root, path, reserved, keyClass, options,
		GetNativeRegistryAccessMask(access), securityAttributes, handle, disposition);
}
