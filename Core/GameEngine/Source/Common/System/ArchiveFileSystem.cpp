/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   Generals
//
// Module:    Game Engine Common
//
// File name: ArchiveFileSystem.cpp
//
// Created:   11/26/01 TR
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/AsciiString.h"
#include "Common/PerfTimer.h"
#if defined(_WIN64) && RTS_ZEROHOUR
#include "Common/GlobalData.h"

namespace
{
	const Int kControlBarPro2160Width = 3840;
	const Int kControlBarPro2160Height = 2160;
}
#endif


//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

ArchiveFileSystem *TheArchiveFileSystem = nullptr;


//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------

//------------------------------------------------------
// ArchivedFileInfo
//------------------------------------------------------
ArchiveFileSystem::ArchiveFileSystem()
{
}

ArchiveFileSystem::~ArchiveFileSystem()
{
	ArchiveFileMap::iterator iter = m_archiveFileMap.begin();
	while (iter != m_archiveFileMap.end()) {
		ArchiveFile *file = iter->second;
		delete file;
		iter++;
	}
}

void ArchiveFileSystem::loadIntoDirectoryTree(ArchiveFile *archiveFile, Bool overwrite)
{
#if defined(_WIN64) && RTS_ZEROHOUR
	const AsciiString archiveName = archiveFile->getName();
	if (!isArchiveEligibleForResolution(archiveName, 0, 0))
		m_2160Archives.insert(archiveFile);
	if (archiveName.find('\\') == nullptr && archiveName.find('/') == nullptr)
		m_zeroHourRootArchives.insert(archiveFile);
#endif

	FilenameList filenameList;

	archiveFile->getFileListInDirectory("", "", "*", filenameList, TRUE);

	FilenameListIter it = filenameList.begin();

	while (it != filenameList.end())
	{
		ArchivedDirectoryInfo *dirInfo = &m_rootDirectory;

		AsciiString path;
		AsciiString token;
		AsciiString tokenizer = *it;
		tokenizer.toLower();
		Bool infoInPath = tokenizer.nextToken(&token, "\\/");

		while (infoInPath && (!token.find('.') || tokenizer.find('.')))
		{
			path.concat(token);
			path.concat('\\');

			ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
			if (tempiter == dirInfo->m_directories.end())
			{
				dirInfo = &(dirInfo->m_directories[token]);
				dirInfo->m_path = path;
				dirInfo->m_directoryName = token;
			}
			else
			{
				dirInfo = &tempiter->second;
			}

			infoInPath = tokenizer.nextToken(&token, "\\/");
		}

		ArchivedFileLocationMap::iterator fileIt;
		if (overwrite)
		{
			// When overwriting, try place the new value at the beginning of the key list.
			fileIt = dirInfo->m_files.find(token);
		}
		else
		{
			// Append to the end of the key list.
			fileIt = dirInfo->m_files.end();
		}

		dirInfo->m_files.insert(fileIt, std::make_pair(token, archiveFile));

#if defined(DEBUG_LOGGING) && ENABLE_FILESYSTEM_LOGGING
		{
			const stl::const_range<ArchivedFileLocationMap> range = stl::get_range(dirInfo->m_files, token, 0);
			if (range.distance() >= 2)
			{
				ArchivedFileLocationMap::const_iterator rangeIt0;
				ArchivedFileLocationMap::const_iterator rangeIt1;

				if (overwrite)
				{
					rangeIt0 = range.begin;
					rangeIt1 = std::next(rangeIt0);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwriting same file in %s",
						it->str(),
						rangeIt0->second->getName().str(),
						rangeIt1->second->getName().str()
					));
				}
				else
				{
					rangeIt1 = std::prev(range.end);
					rangeIt0 = std::prev(rangeIt1);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwritten by same file in %s",
						it->str(),
						rangeIt1->second->getName().str(),
						rangeIt0->second->getName().str()
					));
				}
			}
			else
			{
				DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s", it->str(), archiveFile->getName().str()));
			}
		}
#endif

		it++;
	}
}

void ArchiveFileSystem::loadMods()
{
	if (TheGlobalData->m_modBIG.isNotEmpty())
	{
		ArchiveFile *archiveFile = openArchiveFile(TheGlobalData->m_modBIG.str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("ArchiveFileSystem::loadMods - loading %s into the directory tree.", TheGlobalData->m_modBIG.str()));
			loadIntoDirectoryTree(archiveFile, TRUE);
			m_archiveFileMap[TheGlobalData->m_modBIG] = archiveFile;
			DEBUG_LOG(("ArchiveFileSystem::loadMods - %s inserted into the archive file map.", TheGlobalData->m_modBIG.str()));
		}
		else
		{
			DEBUG_LOG(("ArchiveFileSystem::loadMods - could not openArchiveFile(%s)", TheGlobalData->m_modBIG.str()));
		}
	}

	if (TheGlobalData->m_modDir.isNotEmpty())
	{
		MAYBE_UNUSED Bool ret = loadBigFilesFromDirectory(TheGlobalData->m_modDir, "*.big", TRUE);
		(void)ret;
		DEBUG_ASSERTLOG(ret, ("loadBigFilesFromDirectory(%s) returned FALSE!", TheGlobalData->m_modDir.str()));
	}
}

Bool ArchiveFileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
#if defined(_WIN64) && RTS_ZEROHOUR
	return getArchiveFile(filename, instance) != nullptr;
#else
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename);

	if (!result.valid())
		return false;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	return range.valid();
#endif
}

ArchivedDirectoryInfo* ArchiveFileSystem::friend_getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result = getArchivedDirectoryInfo(directory);

	return result.dirInfo;
}

ArchiveFileSystem::ArchivedDirectoryInfoResult ArchiveFileSystem::getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result;
	ArchivedDirectoryInfo* dirInfo = &m_rootDirectory;

	AsciiString token;
	AsciiString tokenizer = directory;
	tokenizer.toLower();
	Bool infoInPath = tokenizer.nextToken(&token, "\\/");

	while (infoInPath && (!token.find('.') || tokenizer.find('.')))
	{
		ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
		if (tempiter != dirInfo->m_directories.end())
		{
			dirInfo = &tempiter->second;
			infoInPath = tokenizer.nextToken(&token, "\\/");
		}
		else
		{
			// the directory doesn't exist
			result.dirInfo = nullptr;
			result.lastToken = AsciiString::TheEmptyString;
			return result;
		}
	}

	result.dirInfo = dirInfo;
	result.lastToken = token;
	return result;
}

File * ArchiveFileSystem::openFile(const Char *filename, Int access, FileInstance instance)
{
	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return nullptr;

	return archive->openFile(filename, access);
}

Bool ArchiveFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance) const
{
	if (fileInfo == nullptr) {
		return FALSE;
	}

	if (filename.isEmpty()) {
		return FALSE;
	}

	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return FALSE;

	return archive->getFileInfo(filename, fileInfo);
}

ArchiveFile* ArchiveFileSystem::getArchiveFile(const AsciiString& filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename.str());

	if (!result.valid())
		return nullptr;

#if defined(_WIN64) && RTS_ZEROHOUR
	stl::const_range<ArchivedFileLocationMap> range =
		stl::get_range(result.dirInfo->m_files, result.lastToken);
#else
	stl::const_range<ArchivedFileLocationMap> range =
		stl::get_range(result.dirInfo->m_files, result.lastToken, instance);
#endif

	if (!range.valid())
		return nullptr;

#if defined(_WIN64) && RTS_ZEROHOUR
	const Int width = TheGlobalData ? TheGlobalData->m_xResolution : 0;
	const Int height = TheGlobalData ? TheGlobalData->m_yResolution : 0;
	// All archive instances retain their legacy ordering at UHD. At lower
	// resolutions, most files have no 2160p override; keep that hot path O(1).
	if (width >= kControlBarPro2160Width && height >= kControlBarPro2160Height)
	{
		const stl::const_range<ArchivedFileLocationMap> original =
			stl::get_range(result.dirInfo->m_files, result.lastToken, instance);
		return original.valid() ? original.get()->second : nullptr;
	}
	if (instance == 0 && m_2160Archives.find(range.begin->second) == m_2160Archives.end())
		return range.begin->second;

	Bool hasRootAlternative = FALSE;
	for (ArchivedFileLocationMap::const_iterator it = range.begin; it != range.end; ++it)
	{
		if (m_2160Archives.find(it->second) != m_2160Archives.end())
			continue;
		// Root archives belong to this title. The paired Generals install may
		// contain an older localized file with the same path; it is not a
		// replacement for a Zero Hour-specific 2160p localization.
		if (m_zeroHourRootArchives.find(it->second) != m_zeroHourRootArchives.end())
		{
			hasRootAlternative = TRUE;
			break;
		}
	}
	if (!hasRootAlternative)
	{
		const stl::const_range<ArchivedFileLocationMap> original =
			stl::get_range(result.dirInfo->m_files, result.lastToken, instance);
		return original.valid() ? original.get()->second : nullptr;
	}
	for (ArchivedFileLocationMap::const_iterator it = range.begin; it != range.end; ++it)
	{
		if (m_2160Archives.find(it->second) != m_2160Archives.end())
			continue;
		if (instance == 0)
			return it->second;
		--instance;
	}
	return nullptr;
#else
	return range.get()->second;
#endif
}

#if defined(_WIN64) && RTS_ZEROHOUR
Bool ArchiveFileSystem::isArchiveEligibleForResolution(
	const AsciiString& archiveName, Int width, Int height)
{
	const Char *basename = archiveName.str();
	for (const Char *p = basename; *p != 0; ++p)
	{
		if (*p == '\\' || *p == '/')
			basename = p + 1;
	}
	if (_stricmp(basename, "340_ControlBarPro-Fix2160ZH.big") == 0 ||
		_stricmp(basename, "340_ControlBarPro2160ZH.big") == 0)
		return width >= kControlBarPro2160Width && height >= kControlBarPro2160Height;
	return TRUE;
}
#endif

void ArchiveFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	ArchiveFileMap::const_iterator it = m_archiveFileMap.begin();
	while (it != m_archiveFileMap.end()) {
		it->second->getFileListInDirectory(currentDirectory, originalDirectory, searchName, filenameList, searchSubdirectories);
		it++;
	}
}
