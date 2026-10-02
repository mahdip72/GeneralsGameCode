#include "Lib/ValidationProfileRoot.h"

#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

namespace
{
	const char *const kProfileRootEnvironment =
		"RTS_STAGE5_VALIDATION_PROFILE_ROOT";
	const char *const kUserShellFoldersKey =
		"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
	const char *const kShellFoldersKey =
		"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";

	struct EnvironmentSnapshot
	{
		bool present;
		std::string value;
	};

	EnvironmentSnapshot CaptureEnvironment()
	{
		EnvironmentSnapshot snapshot;
		char buffer[MAX_PATH];
		const DWORD length = GetEnvironmentVariableA(
			kProfileRootEnvironment, buffer, sizeof(buffer));
		snapshot.present = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
		if (snapshot.present && length < sizeof(buffer))
			snapshot.value.assign(buffer, length);
		return snapshot;
	}

	class EnvironmentScope
	{
	public:
		EnvironmentScope() : m_snapshot(CaptureEnvironment()) {}
		~EnvironmentScope()
		{
			SetEnvironmentVariableA(kProfileRootEnvironment,
				m_snapshot.present ? m_snapshot.value.c_str() : NULL);
		}

		bool set(const char *value)
		{
			return SetEnvironmentVariableA(kProfileRootEnvironment, value) != 0;
		}

	private:
		EnvironmentSnapshot m_snapshot;
	};

	struct RegistryValueSnapshot
	{
		RegistryValueSnapshot() : present(false), type(0) {}
		bool present;
		DWORD type;
		std::vector<BYTE> data;
	};

	RegistryValueSnapshot ReadPersonalValue(const char *subKey, REGSAM view)
	{
		RegistryValueSnapshot snapshot;
		HKEY key = NULL;
		const LONG openResult = RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0,
			KEY_QUERY_VALUE | view, &key);
		if (openResult == ERROR_FILE_NOT_FOUND)
			return snapshot;
		if (openResult != ERROR_SUCCESS)
			throw std::string("RegOpenKeyExA failed: ") + subKey;

		DWORD type = 0;
		DWORD size = 0;
		LONG queryResult = RegQueryValueExA(key, "Personal", NULL, &type,
			NULL, &size);
		if (queryResult == ERROR_FILE_NOT_FOUND)
		{
			RegCloseKey(key);
			return snapshot;
		}
		if (queryResult != ERROR_SUCCESS)
		{
			RegCloseKey(key);
			throw std::string("RegQueryValueExA size failed: ") + subKey;
		}

		snapshot.present = true;
		snapshot.type = type;
		snapshot.data.resize(size);
		queryResult = RegQueryValueExA(key, "Personal", NULL, &type,
			snapshot.data.empty() ? NULL : &snapshot.data[0], &size);
		RegCloseKey(key);
		if (queryResult != ERROR_SUCCESS)
			throw std::string("RegQueryValueExA data failed: ") + subKey;
		snapshot.data.resize(size);
		return snapshot;
	}

	bool SameRegistryValue(const RegistryValueSnapshot &left,
		const RegistryValueSnapshot &right)
	{
		return left.present == right.present && left.type == right.type &&
			left.data == right.data;
	}

	bool Check(bool condition, const char *message)
	{
		if (!condition)
			std::cerr << message << std::endl;
		return condition;
	}

	bool CheckProfileRootRead(const char *value,
		rts::validation::ProcessLocalProfileRootResult expectedResult,
		const char *expectedPath)
	{
		char buffer[MAX_PATH];
		const rts::validation::ProcessLocalProfileRootResult result =
			rts::validation::ReadProcessLocalProfileRoot(buffer, sizeof(buffer));
		if (result != expectedResult)
		{
			std::cerr << "unexpected profile-root result for '" << value << "'"
				<< ": expected " << static_cast<int>(expectedResult)
				<< ", got " << static_cast<int>(result) << std::endl;
			return false;
		}
		return
			Check(expectedPath == NULL ? buffer[0] == '\0' :
				std::string(buffer) == expectedPath,
				expectedPath == NULL ? "invalid profile root was not cleared" :
				"profile root was not preserved as the complete path");
	}
}

int main()
{
	try
	{
		const RegistryValueSnapshot userShellFolders32 =
			ReadPersonalValue(kUserShellFoldersKey, KEY_WOW64_32KEY);
		const RegistryValueSnapshot shellFolders32 =
			ReadPersonalValue(kShellFoldersKey, KEY_WOW64_32KEY);
		const RegistryValueSnapshot userShellFolders64 =
			ReadPersonalValue(kUserShellFoldersKey, KEY_WOW64_64KEY);
		const RegistryValueSnapshot shellFolders64 =
			ReadPersonalValue(kShellFoldersKey, KEY_WOW64_64KEY);

		char currentDirectory[MAX_PATH];
		const DWORD currentLength = GetCurrentDirectoryA(
			sizeof(currentDirectory), currentDirectory);
		if (!Check(currentLength > 0 && currentLength < sizeof(currentDirectory),
			"could not resolve the test working directory"))
			return 1;

		std::string testRoot(currentDirectory);
		testRoot += "\\validation-profile-root-test-";
		char processId[32];
		_snprintf(processId, sizeof(processId), "%lu",
			static_cast<unsigned long>(GetCurrentProcessId()));
		testRoot += processId;
		std::string profileRoot = testRoot +
			"\\Command and Conquer Generals Data";
		if (!CreateDirectoryA(testRoot.c_str(), NULL) &&
			GetLastError() != ERROR_ALREADY_EXISTS)
		{
			std::cerr << "could not create test root" << std::endl;
			return 1;
		}
		if (!CreateDirectoryA(profileRoot.c_str(), NULL) &&
			GetLastError() != ERROR_ALREADY_EXISTS)
		{
			std::cerr << "could not create test profile root" << std::endl;
			RemoveDirectoryA(testRoot.c_str());
			return 1;
		}

		int result = 0;
		{
			EnvironmentScope environment;
			std::string expected = profileRoot + "\\";
			if (!environment.set(profileRoot.c_str()) ||
				!CheckProfileRootRead(profileRoot.c_str(),
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_VALID,
					expected.c_str()))
				result = 1;
			std::string profileWithTrailingSeparator = profileRoot + "\\";
			if (!environment.set(profileWithTrailingSeparator.c_str()) ||
				!CheckProfileRootRead(profileWithTrailingSeparator.c_str(),
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_VALID,
					profileWithTrailingSeparator.c_str()))
				result = 1;
			const std::string driveRoot = testRoot.substr(0, 3);
		if (!environment.set(driveRoot.c_str()) ||
			!CheckProfileRootRead(driveRoot.c_str(),
				rts::validation::PROCESS_LOCAL_PROFILE_ROOT_INVALID, NULL))
			result = 1;
		std::string oversized = driveRoot + std::string(MAX_PATH, 'x');
		if (!environment.set(oversized.c_str()) ||
			!CheckProfileRootRead(oversized.c_str(),
				rts::validation::PROCESS_LOCAL_PROFILE_ROOT_INVALID, NULL))
			result = 1;
			if (!environment.set("relative-profile-root") ||
				!CheckProfileRootRead("relative-profile-root",
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_INVALID, NULL))
				result = 1;
			if (!environment.set((testRoot + "\\..\\escape").c_str()) ||
				!CheckProfileRootRead("traversal-profile-root",
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_INVALID, NULL))
				result = 1;
			if (!environment.set("") ||
				!CheckProfileRootRead("empty-profile-root",
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_INVALID, NULL))
				result = 1;
			if (!SetEnvironmentVariableA(kProfileRootEnvironment, NULL) ||
				!CheckProfileRootRead("unset-profile-root",
					rts::validation::PROCESS_LOCAL_PROFILE_ROOT_NOT_SET, NULL))
				result = 1;
		}

		result |= !Check(SameRegistryValue(userShellFolders32,
			ReadPersonalValue(kUserShellFoldersKey, KEY_WOW64_32KEY)),
			"32-bit User Shell Folders Personal changed");
		result |= !Check(SameRegistryValue(shellFolders32,
			ReadPersonalValue(kShellFoldersKey, KEY_WOW64_32KEY)),
			"32-bit Shell Folders Personal changed");
		result |= !Check(SameRegistryValue(userShellFolders64,
			ReadPersonalValue(kUserShellFoldersKey, KEY_WOW64_64KEY)),
			"64-bit User Shell Folders Personal changed");
		result |= !Check(SameRegistryValue(shellFolders64,
			ReadPersonalValue(kShellFoldersKey, KEY_WOW64_64KEY)),
			"64-bit Shell Folders Personal changed");

		RemoveDirectoryA(profileRoot.c_str());
		RemoveDirectoryA(testRoot.c_str());
		return result;
	}
	catch (const std::string &message)
	{
		std::cerr << message << std::endl;
		return 1;
	}
}
