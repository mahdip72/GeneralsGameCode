#include "Lib/NetworkMapPackageTransaction.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using rts::network_epoch::NetworkMapPackageTransaction;

static bool Check(bool condition, const char *message)
{
	if (!condition)
		std::fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static bool Write(const std::string &path, const std::string &bytes)
{
	std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
	file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return file.good();
}

static std::string Read(const std::string &path)
{
	std::ifstream file(path.c_str(), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file),
		std::istreambuf_iterator<char>());
}

static bool Valid(void *) { return true; }
static bool Invalid(void *) { return false; }
static bool InterruptAfterFirst(std::size_t committed, void *)
{
	return committed != 1;
}
static bool CrashAfterFirst(std::size_t committed, void *)
{
	if (committed == 1)
		ExitProcess(88);
	return true;
}

int main(int argc, char **argv)
{
	if (argc == 3 && std::string(argv[1]) == "--crash")
	{
		const std::string folder = argv[2];
		const std::string ini = folder + "\\map.ini";
		const std::string map = folder + "\\arena.map";
		const std::string newIni = "new=2\r\n";
		const std::string newMap("new-map\0bytes", 13);
		NetworkMapPackageTransaction child;
		if (!child.stage(ini.c_str(),
			reinterpret_cast<const unsigned char *>(newIni.data()), newIni.size()) ||
			!child.stage(map.c_str(),
				reinterpret_cast<const unsigned char *>(newMap.data()), newMap.size()))
			return 2;
		child.commit(Valid, nullptr, nullptr, CrashAfterFirst);
		return 3;
	}
	char current[MAX_PATH];
	if (GetCurrentDirectoryA(MAX_PATH, current) == 0)
		return 1;
	char suffix[80];
	std::snprintf(suffix, sizeof(suffix), "\\net3-package-%lu-%lu",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long>(GetTickCount()));
	const std::string folder = std::string(current) + suffix;
	if (!CreateDirectoryA(folder.c_str(), nullptr))
		return 1;
	const std::string ini = folder + "\\map.ini";
	const std::string map = folder + "\\arena.map";
	const std::string preview = folder + "\\arena.tga";
	bool ok = Write(ini, "old=1\r\n") &&
		Write(map, std::string("old-map\0bytes", 13)) &&
		Write(preview, "old-preview");
	const std::string oldIni = Read(ini);
	const std::string oldMap = Read(map);
	const std::string oldPreview = Read(preview);
	const std::string newIni = "new=2\r\n";
	const std::string newMap("new-map\0bytes", 13);
	NetworkMapPackageTransaction transaction;
	const auto stage = [&]() {
		return transaction.stage(ini.c_str(),
			reinterpret_cast<const unsigned char *>(newIni.data()), newIni.size()) &&
			transaction.stage(map.c_str(),
				reinterpret_cast<const unsigned char *>(newMap.data()), newMap.size());
	};

	ok = Check(ok && stage(), "stage sidecar and final map") && ok;
	ok = Check(Read(ini) == oldIni && Read(map) == oldMap &&
		Read(preview) == oldPreview,
		"receipt before map validation preserves installed bytes") && ok;
	ok = Check(!transaction.commit(Invalid, nullptr) &&
		transaction.rollbackComplete() && Read(ini) == oldIni &&
		Read(map) == oldMap && Read(preview) == oldPreview,
		"rejected package restores original map and sidecar byte-for-byte") && ok;
	ok = Check(stage() && !transaction.commit(Valid, nullptr, nullptr,
		InterruptAfterFirst) && transaction.rollbackComplete() &&
		Read(ini) == oldIni && Read(map) == oldMap,
		"partial commit failure restores original installed package") && ok;
	const std::string incompleteJournal = map + ".ggctxn";
	ok = Check(!transaction.stage((map + "\n").c_str(), nullptr, 0),
		"journal delimiters are rejected in destination paths") && ok;
	ok = Check(stage() && Write(incompleteJournal, "GGCNET31\n2\n1\tpartial") &&
		!NetworkMapPackageTransaction::recover(map.c_str()) &&
		!transaction.commit(Valid, nullptr) &&
		Read(ini) == oldIni && Read(map) == oldMap,
		"incomplete journal blocks replacement without touching installed bytes") && ok;
	DeleteFileA(incompleteJournal.c_str());
	char executable[MAX_PATH];
	STARTUPINFOA startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	const DWORD exeLength = GetModuleFileNameA(nullptr, executable, MAX_PATH);
	std::string command = "\"" + std::string(executable) +
		"\" --crash \"" + folder + "\"";
	std::vector<char> commandLine(command.begin(), command.end());
	commandLine.push_back(0);
	const bool launched = exeLength != 0 && exeLength < MAX_PATH &&
		CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0;
	DWORD crashCode = 0;
	if (launched)
	{
		WaitForSingleObject(process.hProcess, 10000);
		GetExitCodeProcess(process.hProcess, &crashCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
	ok = Check(launched && crashCode == 88 &&
		Read(ini) == newIni && Read(map) == oldMap,
		"hard exit exposes only a journaled partial package") && ok;
	ok = Check(NetworkMapPackageTransaction::recover(map.c_str()) &&
		Read(ini) == oldIni && Read(map) == oldMap &&
		Read(preview) == oldPreview,
		"restart recovery restores the original package byte-for-byte") && ok;
	ok = Check(stage() && transaction.commit(Valid, nullptr) &&
		Read(ini) == newIni && Read(map) == newMap &&
		Read(preview) == oldPreview,
		"successful package commits expected files and preserves untransferred files") && ok;
	const std::string newMapPath = folder + "\\newarena.map";
	const std::string mapOnly = "map-only";
	NetworkMapPackageTransaction newPackage;
	ok = Check(newPackage.stage(newMapPath.c_str(),
		reinterpret_cast<const unsigned char *>(mapOnly.data()), mapOnly.size()) &&
		newPackage.commit(Valid, nullptr) && Read(newMapPath) == mapOnly &&
		Read(ini) == newIni,
		"new map-only package commits without changing existing sidecars") && ok;
	DeleteFileA(newMapPath.c_str());
	NetworkMapPackageTransaction rejectedNewPackage;
	ok = Check(rejectedNewPackage.stage(ini.c_str(),
		reinterpret_cast<const unsigned char *>(oldIni.data()), oldIni.size()) &&
		rejectedNewPackage.stage(newMapPath.c_str(),
			reinterpret_cast<const unsigned char *>(mapOnly.data()), mapOnly.size()) &&
		!rejectedNewPackage.commit(Invalid, nullptr) &&
		GetFileAttributesA(newMapPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
		Read(ini) == newIni,
		"rejected package removes a newly created map and restores prior sidecar") && ok;

	DeleteFileA(ini.c_str());
	DeleteFileA(map.c_str());
	DeleteFileA(preview.c_str());
	RemoveDirectoryA(folder.c_str());
	return ok ? 0 : 1;
}
