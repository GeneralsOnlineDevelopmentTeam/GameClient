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
//                Copyright(C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   WSYS Library
//
// Module:    IO
//
// File name: IO_FileSystem.cpp
//
// Created:   4/23/01
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/file.h"
#include "Common/FileSystem.h"

#include "Common/ArchiveFileSystem.h"
#include "Common/GameAudio.h"
#include "Common/LocalFileSystem.h"
#include "Common/PerfTimer.h"

#include "Lib/PathUtil.h"

#include <cctype>


DECLARE_PERF_TIMER(FileSystem)

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

//===============================
// TheFileSystem
//===============================
/**
  *	This is the FileSystem's singleton class. All file access
	* should be through TheFileSystem, unless code needs to use an explicit
	* File or FileSystem derivative.
	*
	* Using TheFileSystem->open and File exclusively for file access, particularly
	* in library or modular code, allows applications to transparently implement
	* file access as they see fit. This is particularly important for code that
	* needs to be shared between applications, such as games and tools.
	*/
//===============================

FileSystem	*TheFileSystem = nullptr;

//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//============================================================================
// PathExcludeList
//============================================================================

static std::string normalizeExcludeText(const std::string& text)
{
	std::string value = text;
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] == '/')
			value[i] = '\\';
		value[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
	}
	return value;
}

static Bool isAbsoluteExcludePath(const std::string& value)
{
	return (value.size() > 2 && isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':' && value[2] == '\\')
		|| (value.size() > 2 && value[0] == '\\' && value[1] == '\\');
}

static std::string stripCurrentFolder(const std::string& value)
{
	size_t start = 0;
	while (start + 2 <= value.size() && value.compare(start, 2, ".\\") == 0)
		start += 2;
	return value.substr(start);
}

Bool PathExcludeList::parse(const std::string& text, std::string* pattern, AsciiString* reason)
{
	std::string value = text;
	while (!value.empty() && isspace(static_cast<unsigned char>(value[value.size() - 1])))
		value.erase(value.size() - 1);
	size_t start = 0;
	while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
		++start;
	value = normalizeExcludeText(value.substr(start));

	if (value.empty())
	{
		if (reason != nullptr)
			*reason = "empty";
		return FALSE;
	}
	for (size_t i = 0; i < value.size(); ++i)
	{
		const unsigned char c = static_cast<unsigned char>(value[i]);
		if (c < 32 || c == '<' || c == '>' || c == '"' || c == '|')
		{
			if (reason != nullptr)
				*reason = "invalid character";
			return FALSE;
		}
	}

	std::string rest;
	std::string prefix;
	if (value.compare(0, 5, "game:") == 0 || value.compare(0, 5, "user:") == 0)
	{
		prefix = value.substr(0, 5);
		rest = stripCurrentFolder(value.substr(5));
		if (rest.empty() || rest[0] == '\\' || rest.find(':') != std::string::npos)
		{
			if (reason != nullptr)
				*reason = "invalid path";
			return FALSE;
		}
	}
	else if (isAbsoluteExcludePath(value))
	{
		rest = value;
	}
	else
	{
		if (reason != nullptr)
			*reason = "needs the prefix game: or user:, or an absolute path";
		return FALSE;
	}

	// no parent folders
	size_t pos = 0;
	while (pos <= rest.size())
	{
		size_t next = rest.find('\\', pos);
		if (next == std::string::npos)
			next = rest.size();
		if (next - pos == 2 && rest.compare(pos, 2, "..") == 0)
		{
			if (reason != nullptr)
				*reason = "parent folder";
			return FALSE;
		}
		pos = next + 1;
	}

	// a folder: everything in it
	if (rest[rest.size() - 1] == '\\')
		rest += "*";

	if (pattern != nullptr)
		*pattern = prefix + rest;
	return TRUE;
}

std::string PathExcludeList::makeKey(const Char* path)
{
	std::string value = stripCurrentFolder(normalizeExcludeText(path));
	if (isAbsoluteExcludePath(value))
		return value;
	return "game:" + value;
}

std::string PathExcludeList::makeKey(const Char* path, const Char* userDataPath)
{
	std::string value = normalizeExcludeText(path);
	std::string userData = normalizeExcludeText(userDataPath);
	if (!userData.empty() && userData[userData.size() - 1] != '\\')
		userData += "\\";
	if (!userData.empty() && value.size() > userData.size() && value.compare(0, userData.size(), userData) == 0)
		return "user:" + value.substr(userData.size());
	return makeKey(path);
}

void PathExcludeList::set(const std::vector<std::string>& entries)
{
	m_patterns.clear();
	for (size_t i = 0; i < entries.size(); ++i)
	{
		std::string pattern;
		if (parse(entries[i], &pattern, nullptr))
			m_patterns.push_back(pattern);
	}
}

// * matches any text, ? one character
static Bool matchesExcludePattern(const char* pattern, const char* text)
{
	const char* starPattern = nullptr;
	const char* starText = nullptr;
	while (*text != '\0')
	{
		if (*pattern == '*')
		{
			starPattern = ++pattern;
			starText = text;
		}
		else if (*pattern == '?' || *pattern == *text)
		{
			++pattern;
			++text;
		}
		else if (starPattern != nullptr)
		{
			pattern = starPattern;
			text = ++starText;
		}
		else
		{
			return FALSE;
		}
	}
	while (*pattern == '*')
		++pattern;
	return *pattern == '\0';
}

Bool PathExcludeList::matches(const std::string& key) const
{
	for (size_t i = 0; i < m_patterns.size(); ++i)
	{
		if (matchesExcludePattern(m_patterns[i].c_str(), key.c_str()))
			return TRUE;
	}
	return FALSE;
}

//============================================================================
// FileSystem::FileSystem
//============================================================================

FileSystem::FileSystem()
{

}

//============================================================================
// FileSystem::~FileSystem
//============================================================================

FileSystem::~FileSystem()
{

}

//============================================================================
// FileSystem::init
//============================================================================

void		FileSystem::init()
{
	TheLocalFileSystem->init();
	TheArchiveFileSystem->init();
}

//============================================================================
// FileSystem::update
//============================================================================

void		FileSystem::update()
{
	USE_PERF_TIMER(FileSystem)
	TheLocalFileSystem->update();
	TheArchiveFileSystem->update();
}

//============================================================================
// FileSystem::reset
//============================================================================

void		FileSystem::reset()
{
	USE_PERF_TIMER(FileSystem)
	TheLocalFileSystem->reset();
	TheArchiveFileSystem->reset();
}

//============================================================================
// FileSystem::open
//============================================================================

File*		FileSystem::openFile( const Char *filename, Int access, size_t bufferSize, FileInstance instance )
{
	USE_PERF_TIMER(FileSystem)
	File *file = nullptr;

	if ( TheLocalFileSystem != nullptr )
	{
		// an excluded loose file does not exist, unless the file is written
		const Bool excluded = (access & (File::WRITE | File::APPEND | File::CREATE | File::TRUNCATE)) == 0 && isLooseFileExcluded(filename);

		if (instance != 0)
		{
			if (!excluded && TheLocalFileSystem->doesFileExist(filename))
			{
				--instance;
			}
		}
		else if (!excluded)
		{
			file = TheLocalFileSystem->openFile( filename, access, bufferSize );

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
			if (file != nullptr && (file->getAccess() & File::CREATE))
			{
				FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
				FileExistMap::iterator it = m_fileExist.find(FileExistMap::key_type::temporary(filename));
				if (it != m_fileExist.end())
				{
					++it->second.instanceExists;
					if (it->second.instanceDoesNotExist != ~FileInstance(0))
						++it->second.instanceDoesNotExist;
				}
				else
				{
					m_fileExist[filename];
				}
			}
#endif
		}
	}

	if ( (TheArchiveFileSystem != nullptr) && (file == nullptr) )
	{
		// TheSuperHackers @todo Pass 'access' here?
		file = TheArchiveFileSystem->openFile( filename, 0, instance );
	}

	return file;
}

//============================================================================
// FileSystem::doesFileExist
//============================================================================

Bool FileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
	USE_PERF_TIMER(FileSystem)

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
	{
		FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
		FileExistMap::const_iterator it = m_fileExist.find(FileExistMap::key_type::temporary(filename));
		if (it != m_fileExist.end())
		{
			// Must test instanceDoesNotExist first!
			if (instance >= it->second.instanceDoesNotExist)
				return FALSE;
			if (instance <= it->second.instanceExists)
				return TRUE;
		}
	}
#endif

	if (!isLooseFileExcluded(filename) && TheLocalFileSystem->doesFileExist(filename))
	{
		if (instance == 0)
		{
#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
			{
				FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
				m_fileExist[filename];
			}
#endif
			return TRUE;
		}

		--instance;
	}

	if (TheArchiveFileSystem->doesFileExist(filename, instance))
	{
#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
		{
			FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
			FileExistMap::mapped_type& value = m_fileExist[filename];
			value.instanceExists = max(value.instanceExists, instance);
		}
#endif
		return TRUE;
	}

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
	{
		FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
		FileExistMap::mapped_type& value = m_fileExist[filename];
		value.instanceDoesNotExist = min(value.instanceDoesNotExist, instance);
	}
#endif
	return FALSE;
}

//============================================================================
// FileSystem::getFileListInDirectory
//============================================================================
void FileSystem::getFileListInDirectory(const AsciiString& directory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	USE_PERF_TIMER(FileSystem)
	if (m_excludedPaths.isEmpty())
	{
		TheLocalFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, directory, searchName, filenameList, searchSubdirectories);
	}
	else
	{
		FilenameList localFiles;
		TheLocalFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, directory, searchName, localFiles, searchSubdirectories);
		for (FilenameListIter it = localFiles.begin(); it != localFiles.end(); ++it)
		{
			if (!isLooseFileExcluded(it->str()))
				filenameList.insert(*it);
		}
	}
	TheArchiveFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, directory, searchName, filenameList, searchSubdirectories);
}

//============================================================================
// FileSystem::getFileInfo
//============================================================================
Bool FileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance) const
{
	USE_PERF_TIMER(FileSystem)

	// TheSuperHackers @todo Add file info cache?

	if (fileInfo == nullptr) {
		return FALSE;
	}
	memset(fileInfo, 0, sizeof(*fileInfo));

	if (!isLooseFileExcluded(filename.str()) && TheLocalFileSystem->getFileInfo(filename, fileInfo)) {
		if (instance == 0) {
			return TRUE;
		}

		--instance;
	}

	if (TheArchiveFileSystem->getFileInfo(filename, fileInfo, instance)) {
		return TRUE;
	}

	return FALSE;
}

//============================================================================
// FileSystem::createDirectory
//============================================================================
Bool FileSystem::createDirectory(AsciiString directory)
{
	USE_PERF_TIMER(FileSystem)
	if (TheLocalFileSystem != nullptr) {
		return TheLocalFileSystem->createDirectory(directory);
	}
	return FALSE;
}

//============================================================================
// FileSystem::normalizePath
//============================================================================
AsciiString FileSystem::normalizePath(const AsciiString& path)
{
	return TheLocalFileSystem->normalizePath(path);
}

//============================================================================
// FileSystem::isPathInDirectory
//============================================================================
Bool FileSystem::isPathInDirectory(const AsciiString& testPath, const AsciiString& basePath)
{
	AsciiString testPathNormalized = TheFileSystem->normalizePath(testPath);
	AsciiString basePathNormalized = TheFileSystem->normalizePath(basePath);

	if (basePathNormalized.isEmpty())
	{
		DEBUG_CRASH(("Unable to normalize base directory path '%s'.", basePath.str()));
		return false;
	}
	else if (testPathNormalized.isEmpty())
	{
		DEBUG_CRASH(("Unable to normalize file path '%s'.", testPath.str()));
		return false;
	}

#ifdef _WIN32
	const char* pathSep = "\\";
#else
	const char* pathSep = "/";
#endif

	if (!basePathNormalized.endsWith(pathSep))
	{
		basePathNormalized.concat(pathSep);
	}

#ifdef _WIN32
	if (!testPathNormalized.startsWithNoCase(basePathNormalized))
#else
	if (!testPathNormalized.startsWith(basePathNormalized))
#endif
	{
		return false;
	}

	return true;
}

//============================================================================
// FileSystem::removeExtension - Ascii handling variant
//============================================================================
bool FileSystem::removeExtension(AsciiString& path)
{
	if (const Char* ext = getExtension(path.str()))
	{
		path.truncateTo(ext - path.str());
		return true;
	}

	return false;
}

//============================================================================
// FileSystem::removeExtension - Unicode handling variant
//============================================================================
bool FileSystem::removeExtension(UnicodeString& path)
{
	if (const WideChar* ext = getExtension(path.str()))
	{
		path.truncateTo(ext - path.str());
		return true;
	}

	return false;
}
