#pragma once

#if defined(_WIN64)

#include <windows.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rts { namespace network_epoch {

// The NET3 receiver keeps every selected-map member away from the installed
// package until the host's map and sidecar identities have been checked.
class NetworkMapPackageTransaction
{
private:
	// Serialize the journal check and every transaction phase among processes
	// in this game session, without requiring writes to the map directory.
	class MapMutex
	{
	public:
		explicit MapMutex(const std::string &map) : m_handle(nullptr), m_owned(false)
		{
			if (map.empty())
				return;
			char absolute[MAX_PATH];
			const DWORD length = GetFullPathNameA(map.c_str(), MAX_PATH,
				absolute, nullptr);
			if (length == 0 || length >= MAX_PATH)
				return;
			unsigned long long hash = 14695981039346656037ULL;
			for (DWORD i = 0; i < length; ++i)
			{
				unsigned char ch = static_cast<unsigned char>(absolute[i]);
				if (ch >= 'a' && ch <= 'z')
					ch -= 'a' - 'A';
				hash ^= ch;
				hash *= 1099511628211ULL;
			}
			std::string name = "Local\\GGCNET3-";
			const char digits[] = "0123456789ABCDEF";
			for (int shift = 60; shift >= 0; shift -= 4)
				name += digits[(hash >> shift) & 15];
			m_handle = CreateMutexA(nullptr, FALSE, name.c_str());
			if (m_handle != nullptr)
			{
				const DWORD result = WaitForSingleObject(m_handle, 0);
				m_owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
			}
		}
		~MapMutex()
		{
			if (m_handle != nullptr)
			{
				if (m_owned)
					ReleaseMutex(m_handle);
				CloseHandle(m_handle);
			}
		}
		bool valid() const { return m_owned; }
	private:
		MapMutex(const MapMutex &);
		MapMutex &operator=(const MapMutex &);
		HANDLE m_handle;
		bool m_owned;
	};

public:
	typedef bool (*ValidateInstalled)(void *context);
	typedef void (*NotifyReplacement)(const char *path, void *context);
	typedef bool (*ContinueCommit)(std::size_t committed, void *context);

	NetworkMapPackageTransaction() : m_rollbackComplete(true) {}

	class ReadGuard
	{
	public:
		ReadGuard(const char *map, NotifyReplacement notify = nullptr,
			void *context = nullptr) :
			m_map(map != nullptr ? normalizePath(map) : std::string()),
			m_mutex(m_map), m_ready(false)
		{
			if (!safePath(map) || !m_mutex.valid())
				return;
			const std::string journal = journalPath(m_map.c_str());
			if (!journal.empty())
				m_ready = isCommitting() ||
					recoverLocked(m_map.c_str(), journal, notify, context);
		}
		bool ready() const { return m_ready; }
	private:
		ReadGuard(const ReadGuard &);
		ReadGuard &operator=(const ReadGuard &);
		std::string m_map;
		MapMutex m_mutex;
		bool m_ready;
	};

	bool stage(const char *path, const unsigned char *bytes, std::size_t length)
	{
		if (!safePath(path) || (length != 0 && bytes == nullptr))
			return false;
		const std::string normalized = normalizePath(path);
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			if (_stricmp(m_files[i].path.c_str(), normalized.c_str()) == 0)
			{
				m_files[i].bytes.clear();
				if (length != 0)
					m_files[i].bytes.assign(bytes, bytes + length);
				return true;
			}
		}
		if (m_files.size() >= 7)
			return false;
		File file;
		file.path = normalized;
		if (length != 0)
			file.bytes.assign(bytes, bytes + length);
		m_files.push_back(file);
		return true;
	}

	bool staged(const char *path, const unsigned char **bytes,
		std::size_t *length) const
	{
		if (path == nullptr || bytes == nullptr || length == nullptr)
			return false;
		const std::string normalized = normalizePath(path);
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			if (_stricmp(m_files[i].path.c_str(), normalized.c_str()) == 0)
			{
				*bytes = m_files[i].bytes.empty() ? nullptr : m_files[i].bytes.data();
				*length = m_files[i].bytes.size();
				return true;
			}
		}
		return false;
	}

	void clear() { m_files.clear(); }
	bool rollbackComplete() const { return m_rollbackComplete; }

	// Recover an interrupted replacement before this map is read again. The
	// journal is bounded to the seven known files in this map's own directory.
	static bool recover(const char *map, NotifyReplacement notify = nullptr,
		void *context = nullptr)
	{
		if (!safePath(map))
			return false;
		const std::string normalizedMap = normalizePath(map);
		map = normalizedMap.c_str();
		const std::string journal = journalPath(map);
		if (journal.empty())
			return false;
		MapMutex lock(map);
		if (!lock.valid())
			return false;
		return recoverLocked(map, journal, notify, context);
	}

private:
	static bool recoverLocked(const char *map, const std::string &journal,
		NotifyReplacement notify, void *context)
	{
		HANDLE handle = CreateFileA(journal.c_str(), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return GetLastError() == ERROR_FILE_NOT_FOUND ||
				GetLastError() == ERROR_PATH_NOT_FOUND;
		const DWORD size = GetFileSize(handle, nullptr);
		std::string text;
		if (size == INVALID_FILE_SIZE || size > 16384U)
		{
			CloseHandle(handle);
			return false;
		}
		text.resize(size);
		DWORD read = 0;
		const bool readOk = size == 0 || ReadFile(handle, &text[0], size,
			&read, nullptr) != 0;
		CloseHandle(handle);
		if (!readOk || read != size ||
			(text.compare(0, 9, "GGCNET34\n") != 0 &&
			text.compare(0, 9, "GGCNET33\n") != 0 &&
			text.compare(0, 9, "GGCNET32\n") != 0 &&
			text.compare(0, 9, "GGCNET31\n") != 0 &&
			text.compare(0, 9, "GGCNET30\n") != 0))
			return false;
		const bool preparing = text[7] == '0';
		const bool cleanupOnly = text[7] == '3' || text[7] == '4';
		const bool recordedTemporaries = text[7] != '1' && text[7] != '4';
		std::size_t at = 9;
		const std::size_t countEnd = text.find('\n', at);
		if (countEnd == std::string::npos || countEnd != at + 1 ||
			text[at] < '1' || text[at] > '7')
			return false;
		const std::size_t count = text[at] - '0';
		at = countEnd + 1;
		std::vector<DiskFile> disk(count);
		std::vector<std::string> targets(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			const std::size_t end = text.find('\n', at);
			if (end == std::string::npos || end <= at + 2 ||
				(text[at] != '0' && text[at] != '1') || text[at + 1] != '\t')
				return false;
			const std::size_t separator = text.find('\t', at + 2);
			if (separator == std::string::npos || separator >= end)
				return false;
			disk[i].hadOriginal = text[at] == '1';
			targets[i] = text.substr(at + 2, separator - at - 2);
			const std::size_t temporaryEnd = recordedTemporaries ?
				text.find('\t', separator + 1) : std::string::npos;
			if (recordedTemporaries && (temporaryEnd == std::string::npos ||
				temporaryEnd >= end))
				return false;
			if (recordedTemporaries)
				disk[i].temporary = normalizePath(text.substr(separator + 1,
					temporaryEnd - separator - 1).c_str());
			const std::string recordedBackup = text.substr(
				recordedTemporaries ? temporaryEnd + 1 : separator + 1,
				end - (recordedTemporaries ? temporaryEnd + 1 : separator + 1));
			disk[i].backup = normalizePath(recordedBackup.c_str());
			if (!allowedTarget(map, targets[i].c_str()) ||
				(recordedTemporaries &&
					!allowedBackup(map, disk[i].temporary.c_str())) ||
				(recordedTemporaries ?
					!allowedBackup(map, disk[i].backup.c_str()) :
					(disk[i].hadOriginal ?
						!allowedBackup(map, disk[i].backup.c_str()) :
						!disk[i].backup.empty())))
				return false;
			at = end + 1;
		}
		if (at != text.size())
			return false;
		if (preparing || cleanupOnly)
		{
			// Preparing has not touched installed files. Cleanup-only means the
			// validated package or complete rollback is already on disk.
			if (!cleanup(disk, false) || !retireJournal(journal))
				return false;
			return true;
		}
		for (std::size_t i = 0; i < count; ++i)
		{
			if (disk[i].hadOriginal)
			{
				if (!restoreBackup(disk[i].backup, targets[i]))
					return false;
			}
			else if (!removeInstalledFile(targets[i]))
				return false;
			if (notify != nullptr)
				notify(targets[i].c_str(), context);
		}
		// Rollback is complete. Record that only cleanup remains before deleting
		// any backup, so a restart cannot roll the package back a second time.
		text[7] = recordedTemporaries ? '3' : '4';
		if (!writeJournalText(journal, text, true))
			return false;
#if defined(GGC_NET3_TEST_CRASH_CLEANUP)
		if (GetEnvironmentVariableA("GGC_NET3_TEST_CRASH_ROLLBACK_CLEANUP", nullptr, 0) != 0)
			ExitProcess(93);
#endif
		return cleanup(disk, false) && retireJournal(journal);
	}

public:
	bool commit(ValidateInstalled validate, void *context,
		NotifyReplacement notify = nullptr,
		ContinueCommit continueCommit = nullptr)
	{
		m_rollbackComplete = true;
		if (m_files.empty() || validate == nullptr)
			return false;
		const std::string map = m_files.back().path;
		const std::string journal = journalPath(map.c_str());
		if (journal.empty())
			return false;
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			if (!allowedTarget(map.c_str(), m_files[i].path.c_str()))
				return false;
		}
		std::string mapDirectory;
		if (!createDirectoryTree(map.c_str(), mapDirectory))
			return false;
		MapMutex lock(map);
		if (!lock.valid())
			return false;
		std::vector<DiskFile> disk(m_files.size());
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			std::string directory;
			if (!createDirectoryTree(m_files[i].path.c_str(), directory) ||
				!tempName(directory, disk[i].temporary) ||
				!tempName(directory, disk[i].backup))
			{
				cleanup(disk, false);
				return false;
			}
		}
		if (!writeJournal(journal, disk, true))
		{
			cleanup(disk, false);
			return false;
		}
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			if (!prepare(m_files[i], disk[i]))
			{
				if (cleanup(disk, false))
					retireJournal(journal);
				return false;
			}
#if defined(GGC_NET3_TEST_CRASH_PREPARE)
			if (GetEnvironmentVariableA("GGC_NET3_TEST_CRASH_PREPARE", nullptr, 0) != 0)
				ExitProcess(91);
#endif
#if defined(GGC_NET3_TEST_HOLD_PREPARE)
			if (i == 0 &&
				GetEnvironmentVariableA("GGC_NET3_TEST_HOLD_PREPARE", nullptr, 0) != 0)
			{
				const std::string ready = m_files[i].path + ".ready";
				const std::string release = m_files[i].path + ".release";
				HANDLE marker = CreateFileA(ready.c_str(), GENERIC_WRITE, 0,
					nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (marker != INVALID_HANDLE_VALUE)
					CloseHandle(marker);
				for (int attempt = 0; attempt < 3000 &&
					GetFileAttributesA(release.c_str()) == INVALID_FILE_ATTRIBUTES;
					++attempt)
					Sleep(10);
			}
#endif
		}
		if (!writeJournal(journal, disk, false))
		{
			if (cleanup(disk, false))
				retireJournal(journal);
			return false;
		}
		++commitDepth();

		std::size_t committed = 0;
		bool allowed = true;
		for (; committed < disk.size(); ++committed)
		{
			DiskFile &file = disk[committed];
			if (!MoveFileExA(file.temporary.c_str(), m_files[committed].path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				break;
			file.installed = true;
			if (notify != nullptr)
				notify(m_files[committed].path.c_str(), context);
			if (continueCommit != nullptr &&
				!continueCommit(committed + 1, context))
			{
				++committed;
				allowed = false;
				break;
			}
		}

		if (allowed && committed == disk.size() && validate(context))
		{
			if (!writeJournal(journal, disk, false, true))
			{
				m_rollbackComplete = recoverLocked(map.c_str(), journal,
					notify, context);
				--commitDepth();
				cleanup(disk, !m_rollbackComplete);
				clear();
				return false;
			}
#if defined(GGC_NET3_TEST_CRASH_CLEANUP)
			char crashMode[16] = {};
			if (GetEnvironmentVariableA("GGC_NET3_TEST_CRASH_COMMIT_CLEANUP",
				crashMode, sizeof(crashMode)) != 0)
			{
				if (strcmp(crashMode, "readonly") == 0)
					SetFileAttributesA(disk[0].backup.c_str(), FILE_ATTRIBUTE_READONLY);
				ExitProcess(92);
			}
#endif
			--commitDepth();
			if (cleanup(disk, false))
				retireJournal(journal);
			clear();
			return true;
		}

		m_rollbackComplete = recoverLocked(map.c_str(), journal, notify, context);
		--commitDepth();
		// Retain backups if recovery failed; the caller must fail closed.
		cleanup(disk, !m_rollbackComplete);
		clear();
		return false;
	}

	static bool isCommitting() { return commitDepth() != 0; }

private:
	static std::string normalizePath(const char *path)
	{
		std::string normalized(path);
		for (std::size_t i = 0; i < normalized.size(); ++i)
		{
			if (normalized[i] == '/')
				normalized[i] = '\\';
		}
		return normalized;
	}

	struct File
	{
		std::string path;
		std::vector<unsigned char> bytes;
	};
	struct DiskFile
	{
		std::string temporary;
		std::string backup;
		bool hadOriginal = false;
		bool installed = false;
	};

	static std::string journalPath(const char *map)
	{
		if (!safePath(map) || strlen(map) < 4 || strlen(map) + 8 >= MAX_PATH ||
			_stricmp(map + strlen(map) - 4, ".map") != 0)
			return std::string();
		return std::string(map) + ".ggctxn";
	}

	static bool allowedTarget(const char *map, const char *target)
	{
		if (!safePath(map) || !safePath(target))
			return false;
		const char *separator = strrchr(map, '\\');
		if (separator == nullptr)
			return false;
		const std::string directory(map, separator - map + 1);
		const char *names[] = { "map.ini", "map.str", "solo.ini",
			"assetusage.txt", "readme.txt" };
		if (_stricmp(target, map) == 0)
			return true;
		for (std::size_t i = 0; i < 5; ++i)
		{
			if (_stricmp(target, (directory + names[i]).c_str()) == 0)
				return true;
		}
		const std::string stem(separator + 1, strlen(separator + 1) - 4);
		return _stricmp(target, (directory + stem + ".tga").c_str()) == 0;
	}

	static bool allowedBackup(const char *map, const char *backup)
	{
		if (!safePath(map) || !safePath(backup))
			return false;
		const char *separator = strrchr(map, '\\');
		if (separator == nullptr)
			return false;
		const std::string directory(map, separator - map + 1);
		const std::string normalized = normalizePath(backup);
		if (normalized.size() < directory.size() + 8U ||
			normalized.size() > directory.size() + 11U ||
			_strnicmp(normalized.c_str(), directory.c_str(), directory.size()) != 0)
			return false;
		// GetTempFileNameA("ggc") uses one to four hex digits in this folder.
		// An arbitrary or traversing journal path must not become a CopyFileA
		// source or a cleanup deletion target during restart recovery.
		const char *name = normalized.c_str() + directory.size();
		const std::size_t nameLength = normalized.size() - directory.size();
		if (_strnicmp(name, "ggc", 3) != 0 ||
			_stricmp(name + nameLength - 4, ".tmp") != 0)
			return false;
		for (std::size_t i = 3; i < nameLength - 4; ++i)
		{
			const char ch = name[i];
			if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
				(ch >= 'a' && ch <= 'f')))
				return false;
		}
		return true;
	}

	static bool safePath(const char *path)
	{
		if (path == nullptr || path[0] == 0 || strlen(path) >= MAX_PATH)
			return false;
		for (const unsigned char *p =
			reinterpret_cast<const unsigned char *>(path); *p; ++p)
		{
			if (*p < 32U)
				return false; // Win32 paths cannot contain tab/newline journal delimiters.
		}
		return true;
	}

	static int &commitDepth()
	{
		static thread_local int depth = 0;
		return depth;
	}

	bool writeJournal(const std::string &path,
		const std::vector<DiskFile> &disk, bool preparing,
		bool cleanupOnly = false) const
	{
		std::string contents = preparing ? "GGCNET30\n" :
			(cleanupOnly ? "GGCNET33\n" : "GGCNET32\n");
		contents += static_cast<char>('0' + disk.size());
		contents += '\n';
		for (std::size_t i = 0; i < disk.size(); ++i)
		{
			contents += disk[i].hadOriginal ? '1' : '0';
			contents += '\t';
			contents += m_files[i].path;
			contents += '\t';
			contents += disk[i].temporary;
			contents += '\t';
			contents += disk[i].backup;
			contents += '\n';
		}
		return writeJournalText(path, contents, !preparing);
	}

	static bool writeJournalText(const std::string &path,
		const std::string &contents, bool replace)
	{
		const std::size_t separator = path.find_last_of('\\');
		if (separator == std::string::npos)
			return false;
		std::string temporary;
		if (!tempName(path.substr(0, separator + 1), temporary))
			return false;
		HANDLE handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0,
			nullptr, TRUNCATE_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			DeleteFileA(temporary.c_str());
			return false;
		}
		DWORD written = 0;
		const bool complete = WriteFile(handle, contents.data(),
			static_cast<DWORD>(contents.size()), &written, nullptr) != 0 &&
			written == contents.size() && FlushFileBuffers(handle) != 0;
		CloseHandle(handle);
		const bool published = complete && publishJournal(temporary, path,
			replace);
		if (!published)
			DeleteFileA(temporary.c_str());
		return published;
	}

	static bool publishJournal(const std::string &temporary,
		const std::string &journal, bool replace)
	{
		// Antivirus/indexing software can briefly hold a just-flushed temporary
		// file without FILE_SHARE_DELETE. Retry only that transient condition;
		// all other errors and exhausted retries remain fail-closed.
		const DWORD delays[] = { 10U, 20U, 40U, 80U, 160U, 320U, 640U };
		for (std::size_t attempt = 0; ; ++attempt)
		{
			if (MoveFileExA(temporary.c_str(), journal.c_str(),
				MOVEFILE_WRITE_THROUGH |
				(replace ? MOVEFILE_REPLACE_EXISTING : 0)))
				return true;
			const DWORD error = GetLastError();
			if (error != ERROR_SHARING_VIOLATION ||
				attempt >= sizeof(delays) / sizeof(delays[0]))
			{
				SetLastError(error);
				return false;
			}
			Sleep(delays[attempt]);
		}
	}

	static bool createDirectoryTree(const char *path, std::string &directory)
	{
		const char *separator = strrchr(path, '\\');
		if (separator == nullptr || separator - path >= MAX_PATH)
			return false;
		directory.assign(path, separator - path + 1);
		std::size_t first = directory.size() > 2 && directory[1] == ':' ? 3 : 0;
		if (directory.size() > 2 && directory[0] == '\\' && directory[1] == '\\')
		{
			const std::size_t server = directory.find('\\', 2);
			const std::size_t share = server == std::string::npos ?
				std::string::npos : directory.find('\\', server + 1);
			if (share == std::string::npos)
				return false;
			first = share + 1;
		}
		for (std::size_t i = first; i < directory.size(); ++i)
		{
			if (directory[i] != '\\' || i == 0)
				continue;
			const std::string component = directory.substr(0, i);
			if (!CreateDirectoryA(component.c_str(), nullptr) &&
				GetLastError() != ERROR_ALREADY_EXISTS)
				return false;
		}
		return true;
	}

	static bool tempName(const std::string &directory, std::string &path)
	{
		char name[MAX_PATH];
		if (GetTempFileNameA(directory.c_str(), "ggc", 0, name) == 0)
			return false;
		path = name;
		return true;
	}

	static bool flushFile(const std::string &path)
	{
		HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return false;
		const bool flushed = FlushFileBuffers(handle) != 0;
		CloseHandle(handle);
		return flushed;
	}

	static bool restoreBackup(const std::string &backup, const std::string &target)
	{
		const std::size_t separator = target.find_last_of('\\');
		if (separator == std::string::npos)
			return false;
		std::string temporary;
		if (!tempName(target.substr(0, separator + 1), temporary))
			return false;
		const bool copied = CopyFileA(backup.c_str(), temporary.c_str(), FALSE) != 0;
		const bool flushed = copied && flushFile(temporary);
		const bool restored = flushed &&
			MoveFileExA(temporary.c_str(), target.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		if (!restored)
			DeleteFileA(temporary.c_str());
		return restored;
	}

	static bool removeInstalledFile(const std::string &target)
	{
		const std::size_t separator = target.find_last_of('\\');
		if (separator == std::string::npos)
			return false;
		std::string temporary;
		if (!tempName(target.substr(0, separator + 1), temporary))
			return false;
		const bool removed = MoveFileExA(target.c_str(), temporary.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
		DeleteFileA(temporary.c_str());
		return removed || error == ERROR_FILE_NOT_FOUND ||
			error == ERROR_PATH_NOT_FOUND;
	}

	static bool retireJournal(const std::string &journal)
	{
		const std::size_t separator = journal.find_last_of('\\');
		if (separator == std::string::npos)
			return false;
		std::string retired;
		if (!tempName(journal.substr(0, separator + 1), retired))
			return false;
		if (!MoveFileExA(journal.c_str(), retired.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileA(retired.c_str());
			return false;
		}
		// The active journal is durably retired now; this small tombstone is
		// harmless if cleanup is interrupted.
		DeleteFileA(retired.c_str());
		return true;
	}

	static bool prepare(const File &source, DiskFile &disk)
	{
		HANDLE handle = CreateFileA(disk.temporary.c_str(), GENERIC_WRITE, 0,
			nullptr, TRUNCATE_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const bool complete =
			(source.bytes.empty() ||
				(WriteFile(handle, source.bytes.data(),
					static_cast<DWORD>(source.bytes.size()), &written, nullptr) != 0 &&
					written == source.bytes.size())) &&
			FlushFileBuffers(handle) != 0;
		CloseHandle(handle);
		if (!complete)
			return false;
		const DWORD attributes = GetFileAttributesA(source.path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
			return GetLastError() == ERROR_FILE_NOT_FOUND;
		if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) != 0)
			return false;
		DeleteFileA(disk.backup.c_str());
		if (!CopyFileA(source.path.c_str(), disk.backup.c_str(), TRUE))
			return false;
		HANDLE backup = CreateFileA(disk.backup.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (backup == INVALID_HANDLE_VALUE)
			return false;
		const bool flushed = FlushFileBuffers(backup) != 0;
		CloseHandle(backup);
		if (!flushed)
			return false;
		disk.hadOriginal = true;
		return true;
	}

	static bool cleanup(const std::vector<DiskFile> &disk, bool retainBackups)
	{
		bool complete = true;
		for (std::size_t i = 0; i < disk.size(); ++i)
		{
			if (!disk[i].temporary.empty() &&
				!DeleteFileA(disk[i].temporary.c_str()) &&
				GetLastError() != ERROR_FILE_NOT_FOUND)
				complete = false;
			if (!retainBackups && !disk[i].backup.empty() &&
				!DeleteFileA(disk[i].backup.c_str()) &&
				GetLastError() != ERROR_FILE_NOT_FOUND)
				complete = false;
		}
		return complete;
	}

	std::vector<File> m_files;
	bool m_rollbackComplete;
};

} }

#endif
