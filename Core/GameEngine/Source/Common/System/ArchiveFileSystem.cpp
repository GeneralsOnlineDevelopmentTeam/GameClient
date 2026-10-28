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
#include <cctype>
#include <vector>
#include <set>
#include <string>
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/AsciiString.h"
#include "Common/LocalFileSystem.h"
#include "Common/PerfTimer.h"
#include "Common/Registry.h"
#include "../NGMP_include.h"
#include "../OnlineServices_Init.h"


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

static AsciiString getBaseFilename(const AsciiString& path)
{
	const char* str = path.str();
	const char* p1 = strrchr(str, '\\');
	const char* p2 = strrchr(str, '/');
	const char* sep = (p1 == nullptr) ? p2 : ((p2 == nullptr) ? p1 : ((p1 > p2) ? p1 : p2));
	return sep ? AsciiString(sep + 1) : path;
}



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

void ArchiveFileSystem::loadIntoDirectoryTree(ArchiveFile *archiveFile, Int priority)
{
	archiveFile->setPriority(priority);

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
		tokenizer.nextToken(&token, "\\/");

		while (!tokenizer.isEmpty())
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

			tokenizer.nextToken(&token, "\\/");
		}

		// Insert behind every archive of the same or a lower priority number, so the first one mounted wins on equal priority.
		// A plain insert goes to the end of the range of equal keys, the range is rebuilt when the new archive belongs in front of others.
		std::pair<ArchivedFileLocationMap::iterator, ArchivedFileLocationMap::iterator> range = dirInfo->m_files.equal_range(token);
		std::vector<ArchiveFile*> ordered;
		for (ArchivedFileLocationMap::iterator rangeIt = range.first; rangeIt != range.second; ++rangeIt)
			ordered.push_back(rangeIt->second);

		size_t position = 0;
		while (position < ordered.size() && ordered[position]->getPriority() <= priority)
			++position;

		if (position == ordered.size())
		{
			dirInfo->m_files.insert(std::make_pair(token, archiveFile));
		}
		else
		{
			ordered.insert(ordered.begin() + position, archiveFile);
			dirInfo->m_files.erase(range.first, range.second);
			for (size_t k = 0; k < ordered.size(); ++k)
				dirInfo->m_files.insert(std::make_pair(token, ordered[k]));
		}

		DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, priority %d", it->str(), archiveFile->getName().str(), priority));

		it++;
	}
}

// Retail archive names, without the extension and in lower case. Only the language of the game counts as retail.
static void addRetailNames(std::set<std::string>& names, Bool zeroHour)
{
	auto add = [&names](const std::string& name)
	{
		std::string lower = name;
		for (char& c : lower)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		names.insert(lower);
	};

	const std::string language = GetRegistryLanguage().str();

	if (zeroHour)
	{
		add(language + "ZH");
		add("Audio" + language + "ZH");
		add("Speech" + language + "ZH");
		add("W3D" + language + "ZH");
		for (const char* name : { "AudioZH", "INIZH", "MapsZH", "Music", "MusicZH", "ShadersZH", "SpeechZH", "TerrainZH", "TexturesZH", "W3DZH", "WindowZH", "gensecZH",
			"PatchData", "PatchINI", "PatchWindow", "PatchZH" })
			add(name);
	}
	else
	{
		add(language);
		add("Audio" + language);
		add("Speech" + language);
		for (const char* name : { "Audio", "INI", "Music", "Patch", "PatchData", "PatchINI", "PatchWindow", "Speech", "Terrain", "Textures", "W3D", "Window", "gensec", "maps", "shaders" })
			add(name);
	}
}

Bool ArchiveFileSystem::isGeneralsArchivePath(const AsciiString& path, Bool generalsFolder)
{
	if (generalsFolder)
		return TRUE;

	AsciiString lowerPath = path;
	lowerPath.toLower();
	return strstr(lowerPath.str(), "zh_generals\\") != nullptr || strstr(lowerPath.str(), "zh_generals/") != nullptr;
}

Bool ArchiveFileSystem::isRetailArchive(const AsciiString& path, Bool generalsFolder, Bool zeroHour)
{
	static std::set<std::string> zeroHourNames;
	static std::set<std::string> generalsNames;
	if (zeroHourNames.empty())
	{
		addRetailNames(zeroHourNames, TRUE);
		addRetailNames(generalsNames, FALSE);
	}

	AsciiString name = getBaseFilename(path);
	if (!name.endsWithNoCase(".big"))
		return FALSE;
	name.truncateBy(4);
	std::string lower = name.str();
	for (char& c : lower)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

	// The Generals archives are in the Generals install folder, or in a ZH_Generals folder inside the game folder
	const Bool inGeneralsFolder = isGeneralsArchivePath(path, generalsFolder);

	if (zeroHour && !inGeneralsFolder)
		return zeroHourNames.count(lower) != 0;
	return generalsNames.count(lower) != 0;
}

Int ArchiveFileSystem::getGameFolderArchivePriority(const AsciiString& path, Bool generalsFolder, Bool zeroHour)
{
	// The Zero Hour archives rank above the Generals ones in their tier, whatever the order of the scan
	const Bool generals = zeroHour && isGeneralsArchivePath(path, generalsFolder);
	if (isRetailArchive(path, generalsFolder, zeroHour))
		return generals ? ARCHIVE_PRIORITY_RETAIL_GENERALS : ARCHIVE_PRIORITY_RETAIL;

	if (TheFileSystem != nullptr && TheFileSystem->getExcludedPaths().matchesPath(path.str()))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Archive %s is excluded by user.exclude_paths", path.str());
		return ARCHIVE_PRIORITY_EXCLUDED;
	}

	m_gameFolderArchives.push_back(path);
	return generals ? ARCHIVE_PRIORITY_GAME_FOLDER_GENERALS : ARCHIVE_PRIORITY_GAME_FOLDER;
}

Bool ArchiveFileSystem::parseMountEntry(const AsciiString& text, ArchiveMountEntry* entry, AsciiString* reason)
{
	auto fail = [reason](const char* why)
	{
		if (reason != nullptr)
			*reason = why;
		return FALSE;
	};

	std::string value = text.str();
	// trim
	while (!value.empty() && isspace(static_cast<unsigned char>(value.back())))
		value.pop_back();
	size_t start = 0;
	while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
		++start;
	value = value.substr(start);
	if (value.empty())
		return fail("empty");

	for (char& c : value)
	{
		if (c == '/')
			c = '\\';
		if (static_cast<unsigned char>(c) < 32 || c == '<' || c == '>' || c == '"' || c == '|')
			return fail("invalid character");
	}

	ArchiveMountEntry::Base base;
	std::string path;
	std::string lowerPrefix = value.substr(0, 5);
	for (char& c : lowerPrefix)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	if (lowerPrefix == "game:")
	{
		base = ArchiveMountEntry::BASE_GAME;
		path = value.substr(5);
	}
	else if (lowerPrefix == "user:")
	{
		base = ArchiveMountEntry::BASE_USER;
		path = value.substr(5);
	}
	else if (value.size() > 3 && isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':' && value[2] == '\\')
	{
		base = ArchiveMountEntry::BASE_ABSOLUTE;
		path = value;
	}
	else if (value.size() > 2 && value[0] == '\\' && value[1] == '\\')
	{
		base = ArchiveMountEntry::BASE_ABSOLUTE;
		path = value;
	}
	else
	{
		return fail("needs the prefix game: or user:, or an absolute path");
	}

	if (base != ArchiveMountEntry::BASE_ABSOLUTE)
	{
		if (path.empty() || path[0] == '\\' || path.find(':') != std::string::npos)
			return fail("invalid path");
	}

	// no parent folders
	size_t pos = 0;
	while (pos <= path.size())
	{
		size_t next = path.find('\\', pos);
		if (next == std::string::npos)
			next = path.size();
		if (path.substr(pos, next - pos) == "..")
			return fail("parent folder");
		pos = next + 1;
	}

	const size_t lastSeparator = path.find_last_of('\\');
	const std::string lastComponent = lastSeparator == std::string::npos ? path : path.substr(lastSeparator + 1);
	const std::string directory = lastSeparator == std::string::npos ? std::string() : path.substr(0, lastSeparator + 1);
	if (directory.find_first_of("*?") != std::string::npos)
		return fail("wildcards are only allowed in the file name");

	std::string lowerLast = lastComponent;
	for (char& c : lowerLast)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

	ArchiveMountEntry::Kind kind;
	if (lastComponent.empty())
	{
		// a folder: every BIG in it
		if (directory.empty() || directory == "\\\\")
			return fail("invalid path");
		kind = ArchiveMountEntry::KIND_BIG_GLOB;
		path += "*.big";
	}
	else if (lowerLast.size() < 5 || lowerLast.compare(lowerLast.size() - 4, 4, ".big") != 0)
	{
		return fail("not a BIG file, a BIG folder or a *.big pattern");
	}
	else if (lastComponent.find_first_of("*?") != std::string::npos)
	{
		kind = ArchiveMountEntry::KIND_BIG_GLOB;
	}
	else
	{
		kind = ArchiveMountEntry::KIND_BIG;
	}

	if (entry != nullptr)
	{
		entry->m_kind = kind;
		entry->m_base = base;
		entry->m_path = path.c_str();
	}
	return TRUE;
}

void ArchiveFileSystem::mountListedArchive(const ArchiveMountEntry& entry, Int priority, const PathExcludeList* excludes)
{
	std::string fullPath = entry.m_path.str();
	if (entry.m_base == ArchiveMountEntry::BASE_USER)
		fullPath = std::string(TheGlobalData->getPath_UserData().str()) + fullPath;

	// the files to mount, in name order
	std::vector<std::string> bigPaths;
	if (entry.m_kind == ArchiveMountEntry::KIND_BIG)
	{
		bigPaths.push_back(fullPath);
	}
	else
	{
		const size_t lastSeparator = fullPath.find_last_of('\\');
		const std::string directory = lastSeparator == std::string::npos ? std::string() : fullPath.substr(0, lastSeparator + 1);
		const std::string mask = lastSeparator == std::string::npos ? fullPath : fullPath.substr(lastSeparator + 1);
		FilenameList files;
		TheLocalFileSystem->getFileListInDirectory(AsciiString(directory.c_str()), "", AsciiString(mask.c_str()), files, FALSE);
		for (FilenameListIter it = files.begin(); it != files.end(); ++it)
			bigPaths.push_back(it->str());
	}

	Int mounted = 0;
	for (const std::string& bigPath : bigPaths)
	{
		if (!TheLocalFileSystem->doesFileExist(bigPath.c_str()))
			continue;
		if (excludes != nullptr && !excludes->isEmpty() && excludes->matches(PathExcludeList::makeKey(bigPath.c_str(), TheGlobalData->getPath_UserData().str())))
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Archive %s is excluded", bigPath.c_str());
			continue;
		}
		if (m_archiveFileMap.find(AsciiString(bigPath.c_str())) != m_archiveFileMap.end())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Archive %s is already mounted", bigPath.c_str());
			continue;
		}
		ArchiveFile* archiveFile = openArchiveFile(bigPath.c_str());
		if (archiveFile == nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Archive %s could not be opened", bigPath.c_str());
			continue;
		}
		loadIntoDirectoryTree(archiveFile, priority);
		m_archiveFileMap[AsciiString(bigPath.c_str())] = archiveFile;
		++mounted;
	}
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Archive %s at priority %d: %d archives", fullPath.c_str(), priority, mounted);
}

void ArchiveFileSystem::mountDataPackLayer(const char* layer, Bool enabled, const std::vector<std::string>& includePaths, const PathExcludeList* excludes, Int priority)
{
	if (!enabled)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s is disabled", layer);
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s: %d include_paths", layer, static_cast<int>(includePaths.size()));
	for (size_t i = 0; i < includePaths.size(); ++i)
	{
		ArchiveMountEntry entry;
		AsciiString reason;
		if (!parseMountEntry(AsciiString(includePaths[i].c_str()), &entry, &reason))
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s: path ignored (%s): %s", layer, reason.str(), includePaths[i].c_str());
			continue;
		}
		mountListedArchive(entry, priority + static_cast<Int>(i), excludes);
	}
}

Bool ArchiveFileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename);

	if (!result.valid())
		return false;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	return range.valid();
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
	tokenizer.nextToken(&token, "\\/");

	while (!tokenizer.isEmpty())
	{
		ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
		if (tempiter != dirInfo->m_directories.end())
		{
			dirInfo = &tempiter->second;
			tokenizer.nextToken(&token, "\\/");
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

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	if (!range.valid())
		return nullptr;
	
	return range.get()->second;
}

void ArchiveFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	ArchiveFileMap::const_iterator it = m_archiveFileMap.begin();
	while (it != m_archiveFileMap.end()) {
		it->second->getFileListInDirectory(currentDirectory, originalDirectory, searchName, filenameList, searchSubdirectories);
		it++;
	}
}
