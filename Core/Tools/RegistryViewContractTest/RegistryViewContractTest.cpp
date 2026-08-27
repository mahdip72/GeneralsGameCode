#include "Common/RegistryView.h"

#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int s_failures = 0;

#define CHECK(expression) Check((expression), #expression, __LINE__)

static void Check(bool result, const char *expression, int line)
{
	if (!result)
	{
		printf("FAIL line %d: %s\n", line, expression);
		++s_failures;
	}
}

static bool SetStringValue(HKEY key, const char *name, const char *value)
{
	return RegSetValueExA(key, name, 0, REG_SZ,
		reinterpret_cast<const BYTE *>(value),
		static_cast<DWORD>(strlen(value) + 1)) == ERROR_SUCCESS;
}

static bool QueryStringValue(HKEY key, const char *name, char *value, DWORD valueBytes)
{
	DWORD type = 0;
	DWORD bytes = valueBytes;
	return RegQueryValueExA(key, name, nullptr, &type,
		reinterpret_cast<BYTE *>(value), &bytes) == ERROR_SUCCESS &&
		type == REG_SZ && bytes > 0 && bytes <= valueBytes;
}

static bool DeleteTestKey(const char *path, REGSAM view)
{
	const LONG result = RegDeleteKeyExA(HKEY_CURRENT_USER, path, view, 0);
	return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

int main()
{
	const REGSAM retailMask = GetRetailRegistryAccessMask(KEY_READ | KEY_WOW64_64KEY);
	CHECK((retailMask & KEY_WOW64_32KEY) == KEY_WOW64_32KEY);
	CHECK((retailMask & KEY_WOW64_64KEY) == 0);
	const REGSAM nativeMask = GetNativeRegistryAccessMask(KEY_READ | KEY_WOW64_32KEY);
	CHECK((nativeMask & KEY_WOW64_64KEY) == KEY_WOW64_64KEY);
	CHECK((nativeMask & KEY_WOW64_32KEY) == 0);

	GUID id = {};
	const HRESULT guidResult = CoCreateGuid(&id);
	CHECK(guidResult == S_OK);
	if (guidResult != S_OK)
	{
		return 1;
	}

	char readPath[192];
	char writePath[192];
	char nativePath[192];
	char deletePath[192];
	// Some Windows Server WOW64 registry views reject volatile Classes keys.
	const DWORD fixtureOptions = REG_OPTION_NON_VOLATILE;
	_snprintf_s(readPath, sizeof(readPath), _TRUNCATE,
		"Software\\Classes\\CLSID\\GGCRegistryReadView_%08lX_%04X_%04X_%02X%02X%02X%02X%02X%02X%02X%02X",
		static_cast<unsigned long>(id.Data1), static_cast<unsigned int>(id.Data2),
		static_cast<unsigned int>(id.Data3), static_cast<unsigned int>(id.Data4[0]),
		static_cast<unsigned int>(id.Data4[1]), static_cast<unsigned int>(id.Data4[2]),
		static_cast<unsigned int>(id.Data4[3]), static_cast<unsigned int>(id.Data4[4]),
		static_cast<unsigned int>(id.Data4[5]), static_cast<unsigned int>(id.Data4[6]),
		static_cast<unsigned int>(id.Data4[7]));
	_snprintf_s(writePath, sizeof(writePath), _TRUNCATE,
		"Software\\Classes\\CLSID\\GGCRegistryWriteView_%08lX_%04X_%04X_%02X%02X%02X%02X%02X%02X%02X%02X",
		static_cast<unsigned long>(id.Data1), static_cast<unsigned int>(id.Data2),
		static_cast<unsigned int>(id.Data3), static_cast<unsigned int>(id.Data4[0]),
		static_cast<unsigned int>(id.Data4[1]), static_cast<unsigned int>(id.Data4[2]),
		static_cast<unsigned int>(id.Data4[3]), static_cast<unsigned int>(id.Data4[4]),
		static_cast<unsigned int>(id.Data4[5]), static_cast<unsigned int>(id.Data4[6]),
		static_cast<unsigned int>(id.Data4[7]));
	_snprintf_s(nativePath, sizeof(nativePath), _TRUNCATE,
		"Software\\Classes\\CLSID\\GGCRegistryNativeView_%08lX_%04X_%04X_%02X%02X%02X%02X%02X%02X%02X%02X",
		static_cast<unsigned long>(id.Data1), static_cast<unsigned int>(id.Data2),
		static_cast<unsigned int>(id.Data3), static_cast<unsigned int>(id.Data4[0]),
		static_cast<unsigned int>(id.Data4[1]), static_cast<unsigned int>(id.Data4[2]),
		static_cast<unsigned int>(id.Data4[3]), static_cast<unsigned int>(id.Data4[4]),
		static_cast<unsigned int>(id.Data4[5]), static_cast<unsigned int>(id.Data4[6]),
		static_cast<unsigned int>(id.Data4[7]));
	_snprintf_s(deletePath, sizeof(deletePath), _TRUNCATE,
		"Software\\Classes\\CLSID\\GGCRegistryDeleteView_%08lX_%04X_%04X_%02X%02X%02X%02X%02X%02X%02X%02X",
		static_cast<unsigned long>(id.Data1), static_cast<unsigned int>(id.Data2),
		static_cast<unsigned int>(id.Data3), static_cast<unsigned int>(id.Data4[0]),
		static_cast<unsigned int>(id.Data4[1]), static_cast<unsigned int>(id.Data4[2]),
		static_cast<unsigned int>(id.Data4[3]), static_cast<unsigned int>(id.Data4[4]),
		static_cast<unsigned int>(id.Data4[5]), static_cast<unsigned int>(id.Data4[6]),
		static_cast<unsigned int>(id.Data4[7]));

	HKEY key = nullptr;
	DWORD disposition = 0;
	CHECK(RegCreateKeyExA(HKEY_CURRENT_USER, readPath, 0, nullptr,
		fixtureOptions, KEY_WRITE | KEY_WOW64_32KEY,
		nullptr, &key, &disposition) == ERROR_SUCCESS);
	const bool readKeyCreated = key != nullptr && disposition == REG_CREATED_NEW_KEY;
	CHECK(readKeyCreated);
	if (readKeyCreated)
	{
		CHECK(SetStringValue(key, "Probe", "retail32"));
		RegCloseKey(key);
		key = nullptr;
	}
	else if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	CHECK(OpenRetailRegistryKey(HKEY_CURRENT_USER, readPath, 0, KEY_READ, &key) == ERROR_SUCCESS);
	if (key != nullptr)
	{
		char value[32] = {};
		CHECK(QueryStringValue(key, "Probe", value, sizeof(value)));
		CHECK(strcmp(value, "retail32") == 0);
		RegCloseKey(key);
		key = nullptr;
	}
	const LONG retailVisibleFromNative = OpenNativeRegistryKey(
		HKEY_CURRENT_USER, readPath, 0, KEY_READ, &key);
	CHECK(retailVisibleFromNative == ERROR_FILE_NOT_FOUND);
	if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	disposition = 0;
	CHECK(CreateNativeRegistryKey(HKEY_CURRENT_USER, nativePath, 0, nullptr,
		fixtureOptions, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS);
	const bool nativeKeyCreated = key != nullptr && disposition == REG_CREATED_NEW_KEY;
	CHECK(nativeKeyCreated);
	if (nativeKeyCreated)
	{
		CHECK(SetStringValue(key, "Probe", "native64"));
		RegCloseKey(key);
		key = nullptr;
	}
	else if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	CHECK(OpenNativeRegistryKey(HKEY_CURRENT_USER, nativePath, 0, KEY_READ, &key) == ERROR_SUCCESS);
	if (key != nullptr)
	{
		char value[32] = {};
		CHECK(QueryStringValue(key, "Probe", value, sizeof(value)));
		CHECK(strcmp(value, "native64") == 0);
		RegCloseKey(key);
		key = nullptr;
	}
	const LONG nativeVisibleFromRetail = OpenRetailRegistryKey(
		HKEY_CURRENT_USER, nativePath, 0, KEY_READ, &key);
	CHECK(nativeVisibleFromRetail == ERROR_FILE_NOT_FOUND);
	if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	disposition = 0;
	CHECK(CreateRetailRegistryKey(HKEY_CURRENT_USER, writePath, 0, nullptr,
		fixtureOptions, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS);
	const bool writeKeyCreated = key != nullptr && disposition == REG_CREATED_NEW_KEY;
	CHECK(writeKeyCreated);
	if (writeKeyCreated)
	{
		CHECK(SetStringValue(key, "Probe", "created32"));
		RegCloseKey(key);
		key = nullptr;
	}
	else if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, writePath, 0,
		KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS);
	if (key != nullptr)
	{
		char value[32] = {};
		CHECK(QueryStringValue(key, "Probe", value, sizeof(value)));
		CHECK(strcmp(value, "created32") == 0);
		RegCloseKey(key);
		key = nullptr;
	}
	const LONG retailWriteVisibleFromNative = RegOpenKeyExA(HKEY_CURRENT_USER,
		writePath, 0, KEY_READ | KEY_WOW64_64KEY, &key);
	CHECK(retailWriteVisibleFromNative == ERROR_FILE_NOT_FOUND);
	if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	disposition = 0;
	CHECK(CreateRetailRegistryKey(HKEY_CURRENT_USER, deletePath, 0, nullptr,
		fixtureOptions, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS);
	const bool retailDeleteKeyCreated = key != nullptr && disposition == REG_CREATED_NEW_KEY;
	CHECK(retailDeleteKeyCreated);
	if (retailDeleteKeyCreated)
	{
		CHECK(SetStringValue(key, "Probe", "delete32"));
		RegCloseKey(key);
		key = nullptr;
	}
	else if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	disposition = 0;
	CHECK(CreateNativeRegistryKey(HKEY_CURRENT_USER, deletePath, 0, nullptr,
		fixtureOptions, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS);
	const bool nativeDeleteKeyCreated = key != nullptr && disposition == REG_CREATED_NEW_KEY;
	CHECK(nativeDeleteKeyCreated);
	if (nativeDeleteKeyCreated)
	{
		CHECK(SetStringValue(key, "Probe", "keep64"));
		RegCloseKey(key);
		key = nullptr;
	}
	else if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}

	const bool retailDeleteSucceeded = retailDeleteKeyCreated &&
		DeleteRetailRegistryKey(HKEY_CURRENT_USER, deletePath) == ERROR_SUCCESS;
	CHECK(retailDeleteSucceeded);
	const LONG deletedRetailVisible = OpenRetailRegistryKey(
		HKEY_CURRENT_USER, deletePath, 0, KEY_READ, &key);
	CHECK(deletedRetailVisible == ERROR_FILE_NOT_FOUND);
	if (key != nullptr)
	{
		RegCloseKey(key);
		key = nullptr;
	}
	CHECK(OpenNativeRegistryKey(HKEY_CURRENT_USER, deletePath, 0, KEY_READ, &key) == ERROR_SUCCESS);
	if (key != nullptr)
	{
		char value[32] = {};
		CHECK(QueryStringValue(key, "Probe", value, sizeof(value)));
		CHECK(strcmp(value, "keep64") == 0);
		RegCloseKey(key);
		key = nullptr;
	}

	if (readKeyCreated)
	{
		CHECK(DeleteTestKey(readPath, KEY_WOW64_32KEY));
	}
	if (writeKeyCreated)
	{
		CHECK(DeleteTestKey(writePath, KEY_WOW64_32KEY));
	}
	if (nativeKeyCreated)
	{
		CHECK(DeleteTestKey(nativePath, KEY_WOW64_64KEY));
	}
	if (retailDeleteKeyCreated && !retailDeleteSucceeded)
	{
		CHECK(DeleteTestKey(deletePath, KEY_WOW64_32KEY));
	}
	if (nativeDeleteKeyCreated)
	{
		CHECK(DeleteTestKey(deletePath, KEY_WOW64_64KEY));
	}

	return s_failures == 0 ? 0 : 1;
}
