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
// Project:    Generals
//
// Module:     Archive files
//
// File name:  Common/ArchiveFileSystem.h
//
// Created:    11/26/01 TR
//
//----------------------------------------------------------------------------

#pragma once

#define MUSIC_BIG "Music.big"

//----------------------------------------------------------------------------
//           Includes
//----------------------------------------------------------------------------

#include "Common/SubsystemInterface.h"
#include "Common/AsciiString.h"
#include "Common/FileSystem.h" // for typedefs, etc.
#include "Common/STLTypedefs.h"
#include <string>
#include <vector>

//----------------------------------------------------------------------------
//           Forward References
//----------------------------------------------------------------------------

class File;
class ArchiveFile;

//----------------------------------------------------------------------------
//           Type Defines
//----------------------------------------------------------------------------


//===============================
// ArchiveFileSystem
//===============================
/**
  *	Creates and manages ArchiveFile interfaces. ArchiveFiles can be accessed
	* by calling the openArchiveFile() member. ArchiveFiles can be accessed by
	* name or by File interface.
	*
	* openFile() member searches all Archive files for the specified sub file.
	*/
//===============================
class ArchivedDirectoryInfo;
class DetailedArchivedDirectoryInfo;
class ArchivedFileInfo;

typedef std::map<AsciiString, DetailedArchivedDirectoryInfo> DetailedArchivedDirectoryInfoMap; // Archived directory name to detailed archived directory info
typedef std::map<AsciiString, ArchivedDirectoryInfo> ArchivedDirectoryInfoMap; // Archived directory name to archived directory info
typedef std::map<AsciiString, ArchivedFileInfo> ArchivedFileInfoMap; // Archived file name to archived file info
typedef std::map<AsciiString, ArchiveFile *> ArchiveFileMap; // Archive file name to archive data
typedef std::multimap<AsciiString, ArchiveFile *> ArchivedFileLocationMap; // Archived file name to archive data

class ArchivedDirectoryInfo
{
public:
	AsciiString								m_path; // The full path to this directory
	AsciiString								m_directoryName; // The current directory
	ArchivedDirectoryInfoMap	m_directories; // Contained leaf directories
	ArchivedFileLocationMap		m_files; // Contained files
};

class DetailedArchivedDirectoryInfo
{
public:
	AsciiString												m_directoryName;
	DetailedArchivedDirectoryInfoMap	m_directories;
	ArchivedFileInfoMap								m_files;
};

class ArchivedFileInfo
{
public:
	AsciiString m_filename;
	AsciiString m_archiveFilename;
	UnsignedInt m_offset;
	UnsignedInt m_size;

	ArchivedFileInfo()
		: m_offset(0)
		, m_size(0)
	{
	}
};


// Lookup priority of an archive, lower wins. The archive mounted first wins on equal priority.
enum ArchivePriority
{
	ARCHIVE_PRIORITY_EXCLUDED = -1,			///< not mounted: a BIG of a game folder that matches user.exclude_paths
	ARCHIVE_PRIORITY_MOD = 0,						///< -mod and -modDir, 0 to 999, a later one is lower and wins
	ARCHIVE_PRIORITY_GAME_FOLDER = 1000,	///< BIGs of the Zero Hour folder that are not retail, in name order
	ARCHIVE_PRIORITY_GAME_FOLDER_GENERALS = 1100,	///< BIGs of the Generals data (ZH_Generals or the Generals install) that are not retail
	ARCHIVE_PRIORITY_USER = 100000,				///< data_packs.user of settings.json, 100000 plus the position in its include_paths
	ARCHIVE_PRIORITY_GENERALS_ONLINE = 1000000,	///< data_packs.generals_online, 1000000 plus the position in its include_paths
	ARCHIVE_PRIORITY_COMMUNITY_PATCH = 2000000,	///< data_packs.community_patch, 2000000 plus the position in its include_paths
	ARCHIVE_PRIORITY_RETAIL = 3000000,		///< Zero Hour retail and official patch BIGs
	ARCHIVE_PRIORITY_RETAIL_GENERALS = 3000100	///< Generals retail and official patch BIGs
};

// One entry of the include_paths of a data pack layer in settings.json, see ArchiveFileSystem::parseMountEntry
struct ArchiveMountEntry
{
	enum Kind { KIND_BIG, KIND_BIG_GLOB };
	enum Base { BASE_GAME, BASE_USER, BASE_ABSOLUTE };

	Kind m_kind;
	Base m_base;
	AsciiString m_path;	///< with backslashes, relative to the base, the BASE_ABSOLUTE path is complete
};

class ArchiveFileSystem : public SubsystemInterface
{
public:
	ArchiveFileSystem();
	virtual ~ArchiveFileSystem() override;

	virtual void postProcessLoad() override = 0;

	// ArchiveFile operations
	virtual ArchiveFile*	openArchiveFile( const Char *filename ) = 0;		///< Create new or return existing Archive file from file name
	virtual void					closeArchiveFile( const Char *filename ) = 0;		///< Close the one specified big file.
	virtual void					closeAllArchiveFiles() = 0;								///< Close all Archive files currently open

	// File operations
	virtual File*					openFile( const Char *filename, Int access = 0, FileInstance instance = 0);	///< Search Archive files for specified file name and open it if found
	virtual void					closeAllFiles() = 0;									///< Close all files associated with Archive files
	virtual Bool					doesFileExist(const Char *filename, FileInstance instance = 0) const;	///< return true if that file exists in an archive file somewhere.

	void					getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const; ///< search the given directory for files matching the searchName (egs. *.ini, *.rep).  Possibly search subdirectories.  Scans each Archive file.
	Bool					getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance = 0) const; ///< see FileSystem.h

	virtual Bool	loadArchiveFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite = FALSE) = 0;

	virtual void loadMods() = 0;

	/// Parses an entry of the include_paths of a data pack layer: "game:<path>", "user:<path>" or an absolute path. A path ending in .big is one BIG,
	/// a last component with * or ? and ending in .big is a set of BIGs, a path ending in a slash is every BIG in that folder.
	/// Returns FALSE and a reason for an entry that is not valid.
	static Bool parseMountEntry(const AsciiString& text, ArchiveMountEntry* entry, AsciiString* reason);

	/// TRUE for the retail archives and the official patches of Zero Hour and Generals, in every language.
	static Bool isGeneralsArchivePath(const AsciiString& path, Bool generalsFolder);	///< TRUE for the Generals archives: the Generals install folder, or a ZH_Generals folder

	static Bool isRetailArchive(const AsciiString& path, Bool generalsFolder, Bool zeroHour);

	/// BIGs of the game folders that are not retail and not excluded by user.exclude_paths, which are mounted above the data pack layers
	const std::vector<AsciiString>& getGameFolderArchives() const { return m_gameFolderArchives; }

	ArchivedDirectoryInfo* friend_getArchivedDirectoryInfo(const Char* directory);

protected:
	struct ArchivedDirectoryInfoResult
	{
		ArchivedDirectoryInfoResult() : dirInfo(nullptr) {}
		Bool valid() const { return dirInfo != nullptr; }

		ArchivedDirectoryInfo* dirInfo;
		AsciiString lastToken; ///< Synonymous for file name if the search directory was a file path
	};

	ArchiveFile* getArchiveFile(const AsciiString& filename, FileInstance instance = 0) const;
	ArchivedDirectoryInfoResult getArchivedDirectoryInfo(const Char* directory);

	virtual void loadIntoDirectoryTree(ArchiveFile *archiveFile, Int priority = ARCHIVE_PRIORITY_RETAIL);	///< load the archive file's header information and apply it to the global archive directory tree. The lowest priority number wins, the first mounted wins on equal priority.

	Int getNextModPriority() { return m_nextModPriority > ARCHIVE_PRIORITY_MOD ? m_nextModPriority-- : ARCHIVE_PRIORITY_MOD; }	///< -mod and -modDir: a later one wins
	Int getGameFolderArchivePriority(const AsciiString& path, Bool generalsFolder, Bool zeroHour);	///< retail archives are the bottom, any other BIG of the game folders ranks above the data pack layers unless user.exclude_paths matches it

	void mountListedArchive(const ArchiveMountEntry& entry, Int priority, const PathExcludeList* excludes);
	void mountDataPackLayer(const char* layer, Bool enabled, const std::vector<std::string>& includePaths, const PathExcludeList* excludes, Int priority);	///< mounts the include_paths of one layer in list order, the first one wins, minus what excludes matches

	ArchiveFileMap m_archiveFileMap;
	std::vector<AsciiString> m_gameFolderArchives;
	AsciiString m_generalsBaseSource;	///< where the Generals data came from, for the log
	Int m_nextModPriority = ARCHIVE_PRIORITY_MOD + 999;
	ArchivedDirectoryInfo m_rootDirectory;
};


extern ArchiveFileSystem *TheArchiveFileSystem;

//----------------------------------------------------------------------------
//           Inlining
//----------------------------------------------------------------------------
