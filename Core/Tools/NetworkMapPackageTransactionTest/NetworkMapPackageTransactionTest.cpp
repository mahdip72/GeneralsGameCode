#define GGC_NET3_TEST_CRASH_PREPARE 1
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

static bool NoTemps(const std::string &folder)
{
	WIN32_FIND_DATAA data;
	HANDLE found = FindFirstFileA((folder + "\\ggc*.tmp").c_str(), &data);
	if (found == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	FindClose(found);
	return false;
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
static bool CrashAfterThird(std::size_t committed, void *)
{
	if (committed == 3)
		ExitProcess(89);
	return true;
}
static void CrashAfterFirstRecovery(const char *, void *)
{
	ExitProcess(90);
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
	if (argc == 3 && std::string(argv[1]) == "--crash-new")
	{
		const std::string folder = argv[2];
		const std::string preview = folder + "\\fresh.tga";
		const std::string str = folder + "\\map.str";
		const std::string readme = folder + "\\readme.txt";
		const std::string map = folder + "\\fresh.map";
		const std::string bytes = "received";
		NetworkMapPackageTransaction child;
		if (!child.stage(preview.c_str(),
			reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) ||
			!child.stage(str.c_str(),
				reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) ||
			!child.stage(readme.c_str(),
				reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) ||
			!child.stage(map.c_str(),
				reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()))
			return 2;
		child.commit(Valid, nullptr, nullptr, CrashAfterThird);
		return 3;
	}
	if (argc == 3 && std::string(argv[1]) == "--recover-crash")
	{
		const std::string map = std::string(argv[2]) + "\\arena.map";
		NetworkMapPackageTransaction::recover(map.c_str(),
			CrashAfterFirstRecovery);
		return 4;
	}
	if (argc == 3 && std::string(argv[1]) == "--recover-crash-new")
	{
		const std::string map = std::string(argv[2]) + "\\fresh.map";
		NetworkMapPackageTransaction::recover(map.c_str(),
			CrashAfterFirstRecovery);
		return 4;
	}
	if (argc == 3 && std::string(argv[1]) == "--crash-prepare")
	{
		const std::string folder = argv[2];
		const std::string ini = folder + "\\map.ini";
		const std::string map = folder + "\\arena.map";
		const std::string bytes(1024 * 1024, 'x');
		NetworkMapPackageTransaction child;
		if (!child.stage(ini.c_str(),
			reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) ||
			!child.stage(map.c_str(),
				reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()))
			return 2;
		SetEnvironmentVariableA("GGC_NET3_TEST_CRASH_PREPARE", "1");
		child.commit(Valid, nullptr);
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
	const std::string rogue = folder + "\\rogue.dat";
	NetworkMapPackageTransaction invalidPackage;
	ok = Check(invalidPackage.stage(rogue.c_str(),
		reinterpret_cast<const unsigned char *>(newIni.data()), newIni.size()) &&
		invalidPackage.stage(map.c_str(),
			reinterpret_cast<const unsigned char *>(newMap.data()), newMap.size()) &&
		!invalidPackage.commit(Valid, nullptr) &&
		GetFileAttributesA(rogue.c_str()) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA((map + ".ggctxn").c_str()) == INVALID_FILE_ATTRIBUTES,
		"unsupported target fails before preparing or journaling") && ok;
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
	const std::string sentinel = folder + ".sentinel";
	const std::string nested = folder + "\\ggc";
	const std::string sentinelName = sentinel.substr(sentinel.find_last_of("\\/") + 1);
	const std::string traversingBackup = nested + "/../../" + sentinelName;
	const std::string forgedJournal = "GGCNET31\n1\n1\t" + map +
		"\t" + traversingBackup + "\n";
	ok = Check(Write(sentinel, "outside-untouched") &&
		CreateDirectoryA(nested.c_str(), nullptr) &&
		Write(incompleteJournal, forgedJournal) &&
		!NetworkMapPackageTransaction::recover(map.c_str()) &&
		Read(map) == oldMap && Read(sentinel) == "outside-untouched",
		"forged forward-slash backup traversal is rejected before external read") && ok;
	const std::string forgedPreparation = "GGCNET30\n1\n0\t" + map +
		"\t" + traversingBackup + "\t" + folder + "\\ggcA.tmp\n";
	ok = Check(Write(incompleteJournal, forgedPreparation) &&
		!NetworkMapPackageTransaction::recover(map.c_str()) &&
		Read(sentinel) == "outside-untouched",
		"forged preparation path cannot delete outside the map directory") && ok;
	DeleteFileA(incompleteJournal.c_str());
	RemoveDirectoryA(nested.c_str());
	DeleteFileA(sentinel.c_str());
	const std::string shortMap = folder + "\\short.map";
	const std::string shortJournal = shortMap + ".ggctxn";
	const std::string shortBackup = folder + "\\ggcA.tmp";
	const std::string longBackup = folder + "\\ggc12345.tmp";
	const std::string shortRecord = "GGCNET31\n1\n1\t" + shortMap +
		"\t" + shortBackup + "\n";
	ok = Check(Write(shortMap, "partial") && Write(shortBackup, "original") &&
		Write(shortJournal, shortRecord) &&
		NetworkMapPackageTransaction::recover(shortMap.c_str()) &&
		Read(shortMap) == "original" &&
		GetFileAttributesA(shortBackup.c_str()) == INVALID_FILE_ATTRIBUTES,
		"short GetTempFileName backup suffix restores original bytes") && ok;
	const std::string longRecord = "GGCNET31\n1\n1\t" + shortMap +
		"\t" + longBackup + "\n";
	ok = Check(Write(longBackup, "invalid") && Write(shortJournal, longRecord) &&
		!NetworkMapPackageTransaction::recover(shortMap.c_str()) &&
		Read(shortMap) == "original" && Read(longBackup) == "invalid",
		"oversized backup suffix is rejected before file access") && ok;
	DeleteFileA(shortJournal.c_str());
	DeleteFileA(shortMap.c_str());
	DeleteFileA(longBackup.c_str());
	char executable[MAX_PATH];
	STARTUPINFOA startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	const DWORD exeLength = GetModuleFileNameA(nullptr, executable, MAX_PATH);
	std::string prepareCommand = "\"" + std::string(executable) +
		"\" --crash-prepare \"" + folder + "\"";
	std::vector<char> prepareCommandLine(prepareCommand.begin(),
		prepareCommand.end());
	prepareCommandLine.push_back(0);
	PROCESS_INFORMATION prepareProcess = {};
	const bool prepareLaunched = exeLength != 0 && exeLength < MAX_PATH &&
		CreateProcessA(nullptr,
		prepareCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &startup, &prepareProcess) != 0;
	DWORD prepareCode = 0;
	if (prepareLaunched)
	{
		WaitForSingleObject(prepareProcess.hProcess, 30000);
		GetExitCodeProcess(prepareProcess.hProcess, &prepareCode);
		CloseHandle(prepareProcess.hThread);
		CloseHandle(prepareProcess.hProcess);
	}
	ok = Check(prepareLaunched && prepareCode == 91 &&
		Read(incompleteJournal).compare(0, 9, "GGCNET30\n") == 0 &&
		Read(ini) == oldIni && Read(map) == oldMap && !NoTemps(folder),
		"preparation crash leaves only recorded temporary files") && ok;
	ok = Check(NetworkMapPackageTransaction::recover(map.c_str()) &&
		NoTemps(folder) &&
		GetFileAttributesA(incompleteJournal.c_str()) == INVALID_FILE_ATTRIBUTES &&
		Read(ini) == oldIni && Read(map) == oldMap,
		"preparation recovery reclaims staged bytes without replacing package") && ok;
	ok = Check(NetworkMapPackageTransaction::recover(map.c_str()) && NoTemps(folder),
		"preparation recovery remains idempotent") && ok;
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
		WaitForSingleObject(process.hProcess, 30000);
		GetExitCodeProcess(process.hProcess, &crashCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
	ok = Check(launched && crashCode == 88 &&
		Read(ini) == newIni && Read(map) == oldMap,
		"hard exit exposes only a journaled partial package") && ok;
	std::string recoverCommand = "\"" + std::string(executable) +
		"\" --recover-crash \"" + folder + "\"";
	std::vector<char> recoverCommandLine(recoverCommand.begin(),
		recoverCommand.end());
	recoverCommandLine.push_back(0);
	PROCESS_INFORMATION recoveryProcess = {};
	const bool recoveryLaunched = CreateProcessA(nullptr,
		recoverCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &startup, &recoveryProcess) != 0;
	DWORD recoveryCrashCode = 0;
	if (recoveryLaunched)
	{
		WaitForSingleObject(recoveryProcess.hProcess, 30000);
		GetExitCodeProcess(recoveryProcess.hProcess, &recoveryCrashCode);
		CloseHandle(recoveryProcess.hThread);
		CloseHandle(recoveryProcess.hProcess);
	}
	ok = Check(recoveryLaunched && recoveryCrashCode == 90 &&
		GetFileAttributesA(incompleteJournal.c_str()) != INVALID_FILE_ATTRIBUTES &&
		Read(ini) == oldIni,
		"interrupted rollback keeps its journal after a durable file restore") && ok;
	ok = Check(NetworkMapPackageTransaction::recover(map.c_str()) &&
		Read(ini) == oldIni && Read(map) == oldMap &&
		Read(preview) == oldPreview && NoTemps(folder),
		"restart recovery restores the original package byte-for-byte") && ok;
	const std::string freshMap = folder + "\\fresh.map";
	std::string freshMapForward = freshMap;
	for (std::size_t i = 0; i < freshMapForward.size(); ++i)
	{
		if (freshMapForward[i] == '\\')
			freshMapForward[i] = '/';
	}
	const std::string freshPreview = folder + "\\fresh.tga";
	const std::string freshStr = folder + "\\map.str";
	const std::string freshReadme = folder + "\\readme.txt";
	std::string newCommand = "\"" + std::string(executable) +
		"\" --crash-new \"" + folder + "\"";
	std::vector<char> newCommandLine(newCommand.begin(), newCommand.end());
	newCommandLine.push_back(0);
	PROCESS_INFORMATION newProcess = {};
	const bool newLaunched = CreateProcessA(nullptr, newCommandLine.data(),
		nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
		&startup, &newProcess) != 0;
	DWORD newCrashCode = 0;
	if (newLaunched)
	{
		WaitForSingleObject(newProcess.hProcess, 30000);
		GetExitCodeProcess(newProcess.hProcess, &newCrashCode);
		CloseHandle(newProcess.hThread);
		CloseHandle(newProcess.hProcess);
	}
	ok = Check(newLaunched && newCrashCode == 89 &&
		GetFileAttributesA(freshPreview.c_str()) != INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshStr.c_str()) != INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshReadme.c_str()) != INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshMap.c_str()) == INVALID_FILE_ATTRIBUTES,
		"hard exit after new companions leaves a recoverable journal") && ok;
	std::string recoverNewCommand = "\"" + std::string(executable) +
		"\" --recover-crash-new \"" + folder + "\"";
	const std::string freshJournal = freshMap + ".ggctxn";
	std::vector<char> recoverNewCommandLine(recoverNewCommand.begin(),
		recoverNewCommand.end());
	recoverNewCommandLine.push_back(0);
	PROCESS_INFORMATION recoveryNewProcess = {};
	const bool recoveryNewLaunched = CreateProcessA(nullptr,
		recoverNewCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &startup, &recoveryNewProcess) != 0;
	DWORD recoveryNewCrashCode = 0;
	if (recoveryNewLaunched)
	{
		WaitForSingleObject(recoveryNewProcess.hProcess, 30000);
		GetExitCodeProcess(recoveryNewProcess.hProcess, &recoveryNewCrashCode);
		CloseHandle(recoveryNewProcess.hThread);
		CloseHandle(recoveryNewProcess.hProcess);
	}
	ok = Check(recoveryNewLaunched && recoveryNewCrashCode == 90 &&
		GetFileAttributesA(freshJournal.c_str()) != INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshPreview.c_str()) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshStr.c_str()) != INVALID_FILE_ATTRIBUTES,
		"interrupted removal keeps its journal for idempotent recovery") && ok;
	ok = Check(NetworkMapPackageTransaction::recover(freshMapForward.c_str()) &&
		GetFileAttributesA(freshPreview.c_str()) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshStr.c_str()) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshReadme.c_str()) == INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(freshMap.c_str()) == INVALID_FILE_ATTRIBUTES &&
		Read(ini) == oldIni && Read(map) == oldMap && NoTemps(folder),
		"restart recovery with forward-slash map removes absent companions") && ok;
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
