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
public:
	typedef bool (*ValidateInstalled)(void *context);
	typedef void (*NotifyReplacement)(const char *path, void *context);
	typedef bool (*ContinueCommit)(std::size_t committed, void *context);

	NetworkMapPackageTransaction() : m_rollbackComplete(true) {}

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
		if (!readOk || read != size || text.compare(0, 9, "GGCNET31\n") != 0)
			return false;
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
			disk[i].backup = text.substr(separator + 1, end - separator - 1);
			if (!allowedTarget(map, targets[i].c_str()) ||
				(disk[i].hadOriginal ?
					!allowedBackup(map, disk[i].backup.c_str()) :
					!disk[i].backup.empty()))
				return false;
			at = end + 1;
		}
		if (at != text.size())
			return false;
		for (std::size_t i = 0; i < count; ++i)
		{
			if (disk[i].hadOriginal)
			{
				if (!CopyFileA(disk[i].backup.c_str(), targets[i].c_str(), FALSE))
					return false;
			}
			else if (!DeleteFileA(targets[i].c_str()) &&
				GetLastError() != ERROR_FILE_NOT_FOUND)
				return false;
			if (notify != nullptr)
				notify(targets[i].c_str(), context);
		}
		if (!DeleteFileA(journal.c_str()))
			return false;
		cleanup(disk, false);
		return true;
	}

	bool commit(ValidateInstalled validate, void *context,
		NotifyReplacement notify = nullptr,
		ContinueCommit continueCommit = nullptr)
	{
		m_rollbackComplete = true;
		if (m_files.empty() || validate == nullptr)
			return false;
		std::vector<DiskFile> disk(m_files.size());
		for (std::size_t i = 0; i < m_files.size(); ++i)
		{
			if (!prepare(m_files[i], disk[i]))
			{
				cleanup(disk, false);
				return false;
			}
		}
		const std::string map = m_files.back().path;
		const std::string journal = journalPath(map.c_str());
		if (journal.empty() || !writeJournal(journal, disk))
		{
			cleanup(disk, false);
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
			if (!DeleteFileA(journal.c_str()))
			{
				m_rollbackComplete = recover(map.c_str(), notify, context);
				--commitDepth();
				cleanup(disk, !m_rollbackComplete);
				clear();
				return false;
			}
			--commitDepth();
			cleanup(disk, false);
			clear();
			return true;
		}

		m_rollbackComplete = recover(map.c_str(), notify, context);
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
		if (strlen(backup) <= directory.size())
			return false;
		const char *name = backup + directory.size();
		return _strnicmp(backup, directory.c_str(), directory.size()) == 0 &&
			_strnicmp(name, "ggc", 3) == 0 &&
			strchr(name, '\\') == nullptr && strlen(name) < 32;
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
		static int depth = 0;
		return depth;
	}

	bool writeJournal(const std::string &path,
		const std::vector<DiskFile> &disk) const
	{
		std::string contents = "GGCNET31\n";
		contents += static_cast<char>('0' + disk.size());
		contents += '\n';
		for (std::size_t i = 0; i < disk.size(); ++i)
		{
			contents += disk[i].hadOriginal ? '1' : '0';
			contents += '\t';
			contents += m_files[i].path;
			contents += '\t';
			contents += disk[i].backup;
			contents += '\n';
		}
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
		const bool published = complete &&
			MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
		if (!published)
			DeleteFileA(temporary.c_str());
		return published;
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

	static bool prepare(const File &source, DiskFile &disk)
	{
		std::string directory;
		if (!createDirectoryTree(source.path.c_str(), directory) ||
			!tempName(directory, disk.temporary))
			return false;
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
		if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) != 0 ||
			!tempName(directory, disk.backup))
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

	static void cleanup(const std::vector<DiskFile> &disk, bool retainBackups)
	{
		for (std::size_t i = 0; i < disk.size(); ++i)
		{
			if (!disk[i].temporary.empty())
				DeleteFileA(disk[i].temporary.c_str());
			if (!retainBackups && !disk[i].backup.empty())
				DeleteFileA(disk[i].backup.c_str());
		}
	}

	std::vector<File> m_files;
	bool m_rollbackComplete;
};

} }

#endif
