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

//////// BIGFileSystem.cpp ///////////////////////////
// Bryan Cleveland, August 2002
/////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/BIGFileSystem.h"

#include "Common/AudioAffect.h"
#include "Common/ArchiveFile.h"
#include "Common/file.h"
#include "Common/GameAudio.h"
#include "Common/GameMemory.h"
#include "Common/LocalFileSystem.h"

#if RTS_ZEROHOUR
#include "Common/Registry.h"
#endif

#include "Utility/endian_compat.h"
#include "../NGMP_include.h"
#include "../OnlineServices_Init.h"


static const char BIGFileIdentifier[] = { 'B', 'I', 'G', 'F' };

struct BIGFileHeader {
	char identifier[sizeof(BIGFileIdentifier)];
	UnsignedInt archiveFileSize; // little endian, retail reads it as Int so 2 GB max
	UnsignedInt numFiles; // big endian
	UnsignedInt firstEntryOffset; // big endian
};

struct BIGFileEntryHeader {
	UnsignedInt fileOffset; // big endian
	UnsignedInt fileSize; // big endian
};

static_assert(sizeof(BIGFileHeader) == 16, "Incorrect BIG header size");
static_assert(sizeof(BIGFileEntryHeader) == 8, "Incorrect BIG entry header size");

// Returns the length of the string, or -1 if it is truncated or does not fit the buffer.
static Int readNullTerminatedString(File *fp, char *buffer, Int bufferSize) {
	for (Int i = 0; i < bufferSize; ++i) {
		if (fp->read(buffer + i, 1) != 1) {
			return -1;
		}
		if (buffer[i] == 0) {
			return i;
		}
	}
	return -1;
}

BIGFileSystem::BIGFileSystem() : ArchiveFileSystem() {
}

BIGFileSystem::~BIGFileSystem() {
}

void BIGFileSystem::init() {
	DEBUG_ASSERTCRASH(TheLocalFileSystem != nullptr, ("TheLocalFileSystem must be initialized before TheArchiveFileSystem."));
	if (TheLocalFileSystem == nullptr) {
		return;
	}

	loadArchiveFilesFromDirectory("", "*.big");

#if RTS_ZEROHOUR
    // load original Generals assets
    // The Steam install ships them in ZH_Generals, which the scan above already mounted. Mounting the Generals install as well would only add its mods.
    if (TheLocalFileSystem->doesFileExist("ZH_Generals\\INI.big")) {
      m_generalsBaseSource = "ZH_Generals folder";
      return;
    }

    AsciiString installPath;
    GetStringFromGeneralsRegistry("", "InstallPath", installPath );
    //@todo this will need to be ramped up to a crash for release
    DEBUG_ASSERTCRASH(!installPath.isEmpty(), ("Be 1337! Go install Generals!"));
    if (!installPath.isEmpty()) {
      m_generalsBaseSource.format("Generals install folder %s", installPath.str());
      loadArchiveFilesFromDirectory(installPath, "*.big");
    }
#endif
}

void BIGFileSystem::reset() {
}

void BIGFileSystem::update() {
}

void BIGFileSystem::postProcessLoad() {
}

ArchiveFile * BIGFileSystem::openArchiveFile(const Char *filename) {
	File *fp = TheLocalFileSystem->openFile(filename, File::READ | File::BINARY);
	AsciiString archiveFileName;
	archiveFileName = filename;
	archiveFileName.toLower();
	UnsignedInt archiveFileSize = 0;
	UnsignedInt numLittleFiles = 0;
	BIGFileHeader header;

	DEBUG_LOG(("BIGFileSystem::openArchiveFile - opening BIG file %s", filename));

	if (fp == nullptr) {
		DEBUG_CRASH(("Could not open archive file %s for parsing", filename));
		return nullptr;
	}

	AsciiString asciibuf;
	char buffer[_MAX_PATH];
	if (fp->read(&header, sizeof(header)) != sizeof(header) || memcmp(header.identifier, BIGFileIdentifier, sizeof(BIGFileIdentifier)) != 0) {
		DEBUG_CRASH(("Error reading BIG file header in file %s", filename));
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	archiveFileSize = letoh(header.archiveFileSize);
	numLittleFiles = betoh(header.numFiles);

	DEBUG_LOG(("BIGFileSystem::openArchiveFile - size of archive file is %u bytes", archiveFileSize));

//	char t;

	DEBUG_LOG(("BIGFileSystem::openArchiveFile - %u are contained in archive", numLittleFiles));
//	for (Int i = 0; i < 2; ++i) {
//		t = buffer[i];
//		buffer[i] = buffer[(4-i)-1];
//		buffer[(4-i)-1] = t;
//	}

	// read in each directory listing.
	ArchivedFileInfo fileInfo;
	ArchiveFile *archiveFile = NEW ArchiveFile(filename, AsciiString::TheEmptyString);

	Bool corrupt = FALSE;
	for (UnsignedInt i = 0; i < numLittleFiles; ++i) {
		BIGFileEntryHeader entry;
		if (fp->read(&entry, sizeof(entry)) != sizeof(entry)) {
			corrupt = TRUE;
			break;
		}

		fileInfo.m_archiveFilename = archiveFileName;
		fileInfo.m_offset = betoh(entry.fileOffset);
		fileInfo.m_size = betoh(entry.fileSize);

		// read in the path name of the file.
		const Int pathIndex = readNullTerminatedString(fp, buffer, sizeof(buffer));
		if (pathIndex < 0) {
			corrupt = TRUE;
			break;
		}

		Int filenameIndex = pathIndex;
		while ((filenameIndex >= 0) && (buffer[filenameIndex] != '\\') && (buffer[filenameIndex] != '/')) {
			--filenameIndex;
		}

		fileInfo.m_filename = (char *)(buffer + filenameIndex + 1);
		fileInfo.m_filename.toLower();
		buffer[filenameIndex + 1] = 0;

		AsciiString path;
		path = buffer;

		AsciiString debugpath;
		debugpath = path;
		debugpath.concat(fileInfo.m_filename);
//		DEBUG_LOG(("BIGFileSystem::openArchiveFile - adding file %s to archive file %s, file number %d", debugpath.str(), fileInfo->m_archiveFilename.str(), i));

		archiveFile->addFile(path, &fileInfo);
	}

	if (corrupt) {
		DEBUG_CRASH(("Truncated or corrupt directory in BIG file %s", filename));
		delete archiveFile;
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	archiveFile->attachFile(fp);

	// leave fp open as the archive file will be using it.

	return archiveFile;
}

void BIGFileSystem::closeArchiveFile(const Char *filename) {
	// Need to close the specified big file
	ArchiveFileMap::iterator it =  m_archiveFileMap.find(filename);
	if (it == m_archiveFileMap.end()) {
		return;
	}

	if (stricmp(filename, MUSIC_BIG) == 0) {
		// Stop the current audio
		TheAudio->stopAudio(AudioAffect_Music);

		// No need to turn off other audio, as the lookups will just fail.
	}
	DEBUG_ASSERTCRASH(stricmp(filename, MUSIC_BIG) == 0, ("Attempting to close Archive file '%s', need to add code to handle its shutdown correctly.", filename));

	// may need to do some other processing here first.

	delete (it->second);
	m_archiveFileMap.erase(it);
}

void BIGFileSystem::closeAllArchiveFiles() {
}

void BIGFileSystem::closeAllFiles() {
}

Bool BIGFileSystem::loadArchiveFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite) {

	FilenameList filenameList;
	TheLocalFileSystem->getFileListInDirectory(dir, "", fileMask, filenameList, TRUE);

	Bool actuallyAdded = FALSE;
	FilenameListIter it = filenameList.begin();
	while (it != filenameList.end()) {
#if RTS_ZEROHOUR
		// TheSuperHackers @bugfix bobtista 18/11/2025 Skip duplicate INIZH.big in Data\INI to prevent CRC mismatches.
		// English, Chinese, and Korean SKUs shipped with two INIZH.big files (one in Run directory, one in Run\Data\INI).
		// The DeleteFile cleanup doesn't work on EA App/Origin installs because the folder is not writable, so we skip loading it instead.
		if (it->endsWithNoCase("Data\\INI\\INIZH.big") || it->endsWithNoCase("Data/INI/INIZH.big")) {
			it++;
			continue;
		}
#endif

#if RTS_ZEROHOUR
		const Bool zeroHour = TRUE;
#else
		const Bool zeroHour = FALSE;
#endif
		// The first scan is the game folder, the second one the Generals install: the retail archives are the bottom of the lookup
		const Int priority = overwrite ? getNextModPriority() : getGameFolderArchivePriority((*it), dir.isNotEmpty(), zeroHour);
		if (priority == ARCHIVE_PRIORITY_EXCLUDED) {
			it++;
			continue;
		}

		ArchiveFile *archiveFile = openArchiveFile((*it).str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("BIGFileSystem::loadArchiveFilesFromDirectory - loading %s into the directory tree.", (*it).str()));
			loadIntoDirectoryTree(archiveFile, priority);
			m_archiveFileMap[(*it)] = archiveFile;
			DEBUG_LOG(("BIGFileSystem::loadArchiveFilesFromDirectory - %s inserted into the archive file map.", (*it).str()));
			actuallyAdded = TRUE;
		}

		it++;
	}

	return actuallyAdded;
}

void BIGFileSystem::loadMods()
{
#if defined(GENERALS_ONLINE)
	// Order, highest first: loose files of the game folder, -mod and -modDir, the BIGs of the game folders that are not retail (user.exclude_paths removes some),
	// the data pack layers of settings.json (user, generals_online, community_patch), the retail archives. The BIGs of the game folders were mounted by init().
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Generals data: %s", m_generalsBaseSource.isEmpty() ? "none" : m_generalsBaseSource.str());
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Game folder archives above the data pack layers: %d", static_cast<int>(m_gameFolderArchives.size()));
	for (const AsciiString& gameFolderArchive : m_gameFolderArchives)
		NetworkLog(ELogVerbosity::LOG_RELEASE, "  %s", gameFolderArchive.str());

	const GenOnlineSettings& settings = NGMP_OnlineServicesManager::Settings;
	mountDataPackLayer("user", settings.DataPacks_User().m_enabled, settings.DataPacks_User().m_includePaths, &TheFileSystem->getExcludedPaths(), ARCHIVE_PRIORITY_USER);
	mountDataPackLayer("generals_online", settings.DataPacks_GeneralsOnline().m_enabled, settings.DataPacks_GeneralsOnline().m_includePaths, nullptr, ARCHIVE_PRIORITY_GENERALS_ONLINE);

	// -disableCommunityDataPatch skips the community_patch layer for this launch only, the setting is not changed
	const Bool communityPatchEnabled = settings.DataPacks_CommunityPatch().m_enabled && !TheGlobalData->m_commandLineData.isCommunityDataPatchDisabled();
	mountDataPackLayer("community_patch", communityPatchEnabled, settings.DataPacks_CommunityPatch().m_includePaths, nullptr, ARCHIVE_PRIORITY_COMMUNITY_PATCH);
#endif

	if (TheGlobalData->m_modBIG.isNotEmpty())
	{
		ArchiveFile *archiveFile = openArchiveFile(TheGlobalData->m_modBIG.str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("BIGFileSystem::loadMods - loading %s into the directory tree.", TheGlobalData->m_modBIG.str()));
			loadIntoDirectoryTree(archiveFile, getNextModPriority());
			m_archiveFileMap[TheGlobalData->m_modBIG] = archiveFile;
			DEBUG_LOG(("BIGFileSystem::loadMods - %s inserted into the archive file map.", TheGlobalData->m_modBIG.str()));
		}
		else
		{
			DEBUG_LOG(("BIGFileSystem::loadMods - could not openArchiveFile(%s)", TheGlobalData->m_modBIG.str()));
		}
	}

	if (TheGlobalData->m_modDir.isNotEmpty())
	{
		MAYBE_UNUSED Bool ret = loadArchiveFilesFromDirectory(TheGlobalData->m_modDir, "*.big", TRUE);
		(void)ret;
		DEBUG_ASSERTLOG(ret, ("loadArchiveFilesFromDirectory(%s) returned FALSE!", TheGlobalData->m_modDir.str()));
	}
}
