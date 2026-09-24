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

#pragma once

#include "Common/MessageStream.h"
#include "GameNetwork/GameInfo.h"

class File;

/**
  * The ReplayGameInfo class holds information about the replay game and
	* the contents of its slot list for reconstructing multiplayer games.
	*/
class ReplayGameInfo : public GameInfo
{
private:
	GameSlot m_ReplaySlot[MAX_SLOTS];

public:
	ReplayGameInfo()
	{
		for (Int i = 0; i< MAX_SLOTS; ++i)
			setSlotPointer(i, &m_ReplaySlot[i]);
	}
};

enum RecorderModeType CPP_11(: Int) {
	RECORDERMODETYPE_RECORD,
	RECORDERMODETYPE_PLAYBACK,
	RECORDERMODETYPE_SIMULATION_PLAYBACK, // Play back replay without any graphics
	RECORDERMODETYPE_NONE, // this is a valid state to be in on the shell map, or in saved games
	RECORDERMODETYPE_RESUME_CATCHUP // Resume-from-replay: feed replay commands into a live network game until the handoff frame.
};

class RecorderClass : public SubsystemInterface
{
protected:
	// TheSuperHackers @info helmutbuhler 03/04/2025 CRC overview:
	// Each peer periodically computes a CRC from its local game state and broadcasts it to all peers, including itself,
	// to verify synchronization. CRC messages are received a few frames later in network games to avoid stalling every
	// frame while waiting for all peers. This works because all peers compare the same received CRCs on the same frame.
	//
	// Replays are different: recorded CRC messages appear on the frame they were originally received, so directly
	// comparing them against the current local state would mismatch. To handle this, local CRCs must be queued until the
	// corresponding replay CRC messages arrive. This class implements that queue.
	class CRCInfo
	{
	public:
		CRCInfo();
		CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer);
		void addCRC(UnsignedInt val);
		UnsignedInt readCRC();
		int GetQueueSize() const { return m_data.size(); }
		UnsignedInt getLocalPlayer() const { return m_localPlayer; }
		void setSawCRCMismatch() { m_sawCRCMismatch = TRUE; }
		Bool sawCRCMismatch() const { return m_sawCRCMismatch; }

	protected:
		Bool m_sawCRCMismatch;
		Bool m_skippedOne;
		UnsignedInt m_localPlayer;
		std::list<UnsignedInt> m_data;
	};

public:
	struct ReplayHeader;

	RecorderClass();																	///< Constructor.
	virtual ~RecorderClass() override;													///< Destructor.

	virtual void init() override;																			///< Initialize TheRecorder.
	virtual void reset() override;																			///< Reset the state of TheRecorder.
	virtual void update() override;																		///< General purpose update function.

	// Methods dealing with recording.
	void updateRecord();															///< The update function for recording.

	// Methods dealing with playback.
	void updatePlayback();														///< The update function for playing back a file.
	Bool playbackFile(AsciiString filename);					///< Starts playback of the specified file.
	Bool replayMatchesGameVersion(AsciiString filename); ///< Returns true if the playback is a valid playback file for this version.
	static Bool replayMatchesGameVersion(const ReplayHeader& header); ///< Returns true if the playback is a valid playback file for this version.
	AsciiString getCurrentReplayFilename();			///< valid during playback only
	UnsignedInt getPlaybackFrameCount() const { return m_playbackFrameCount; }			///< valid during playback only
	void stopPlayback();															///< Stops playback.  Its fine to call this even if not playing back a file.
	Bool simulateReplay(AsciiString filename);
#if defined(RTS_DEBUG)
	Bool analyzeReplay( AsciiString filename );
#endif
	Bool isPlaybackInProgress() const;

public:
	void handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback);

	// read in info relating to a replay, conditionally setting up m_file for playback
	struct ReplayHeader
	{
		AsciiString filename;
		Bool forPlayback;
		UnicodeString replayName;
		SYSTEMTIME timeVal;
		UnicodeString versionString;
		UnicodeString versionTimeString;
		UnsignedInt versionNumber;
		UnsignedInt exeCRC;
		UnsignedInt iniCRC;
		time_t startTime;
		time_t endTime;
		UnsignedInt frameCount;
		Bool quitEarly;
		Bool desyncGame;
		Bool playerDiscons[MAX_SLOTS];
		AsciiString gameOptions;
		Int localPlayerIndex;
	};
	Bool readReplayHeader( ReplayHeader& header );

	RecorderModeType getMode();												///< Returns the current operating mode.
	Bool isPlaybackMode() const { return m_mode == RECORDERMODETYPE_PLAYBACK || m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK; }

	// TheSuperHackers @feature bill-rich 15/09/2026 Resume-from-replay.
	//
	// The lobby's start path validates this client's copy of the resume source (Resume.rep,
	// see ResumeFromReplay) and arms the recorder. On the next MSG_NEW_GAME a brand new
	// recording of the resumed match is started as for any other game, and the source's
	// commands are injected into TheCommandList frame by frame while the live network game
	// runs in lockstep at an uncapped rate (catchup); every injected command is written to the
	// new recording as it goes, so the new replay holds the whole match. The last
	// RESUME_LEADIN_SECONDS before the handoff frame run at normal speed (lead-in), then the
	// simulation is held for RESUME_FREEZE_SECONDS of wall-clock time with a countdown (freeze),
	// and finally control is handed back to the players. Camera scrolling works throughout;
	// every other interaction is blocked until the freeze ends (see isResumeInputBlocked).
	static const Int RESUME_LEADIN_SECONDS = 10;
	static const Int RESUME_FREEZE_SECONDS = 10;
	static AsciiString getResumeReplayFileName();    ///< name (without extension) of the resume source every client replays
	void armResumeForNextGame(UnsignedInt handoffFrame, UnsignedInt sourceLastFrame); ///< validated by ResumeFromReplay::prepareGameStart
	void disarmResume();
	Bool isResumeArmed() const { return m_resumeArmedHandoffFrame != 0; }
	Bool isResumeCatchupMode() const { return m_mode == RECORDERMODETYPE_RESUME_CATCHUP; }
	Bool isResumeCatchupLeadIn() const;              ///< catchup, within the last RESUME_LEADIN_SECONDS before handoff
	Bool isResumeFreezeActive() const { return m_resumeFreezeStartMs != 0; } ///< handoff reached, holding the sim with a countdown
	Bool isResumeInputBlocked() const { return isResumeCatchupMode() || isResumeFreezeActive(); }
	Bool isResumeCRCValidationSuppressed() const;    ///< catchup, and the run-ahead window after the handoff
	Bool updateResumeFreeze();                       ///< pumped every engine frame by Network::update; TRUE while frames must not advance
	UnsignedInt scanReplayLastFrame(AsciiString filename); ///< last frame with a complete record; walks the records, for recordings whose header was never finalized (crash) or whose tail is torn
	void initControls();															///< Show or Hide the Replay controls

	static AsciiString getReplayDir();								///< Returns the directory that holds the replay files.
	static AsciiString getReplayArchiveDir();					///< Returns the directory that holds the archived replay files.
	static AsciiString getReplayExtention();					///< Returns the file extention for replay files.
	static AsciiString getLastReplayFileName();				///< Returns the filename used for the default replay.

	GameInfo *getGameInfo() { return &m_gameInfo; }	///< Returns the slot list for playback game start

	Bool isMultiplayer();												///< is this a multiplayer game (record OR playback)?

	Int getGameMode() { return m_originalGameMode; }

	void logPlayerDisconnect(UnicodeString player, Int slot);
	void logCRCMismatch();
	Bool sawCRCMismatch() const;
	void cleanUpReplayFile();										///< after a crash, send replay/debug info to a central repository

	void setArchiveEnabled(Bool enable) { m_archiveReplays = enable; } ///< Enable or disable replay archiving.
	void stopRecording();															///< Stop recording and close m_file.
protected:
	void startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS);					///< Start recording to m_file.
	void writeToFile(GameMessage *msg);								///< Write this GameMessage to m_file.
	void archiveReplay(AsciiString fileName);					///< Move the specified replay file to the archive directory.

	void logGameStart(AsciiString options);
	void logGameEnd();

	AsciiString readAsciiString();										///< Read the next string from m_file using ascii characters.
	UnicodeString readUnicodeString();								///< Read the next string from m_file using unicode characters.
	void readNextFrame();															///< Read the next frame number to execute a command on.
	Bool appendNextCommand();													///< Read the next GameMessage and append it to TheCommandList. FALSE on a torn record.
	void writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg);
	Bool readArgument(GameMessageArgumentDataType type, GameMessage *msg);	///< FALSE on a short read.

	struct CullBadCommandsResult
	{
		CullBadCommandsResult() : hasClearGameDataMessage(false) {}
		Bool hasClearGameDataMessage;
	};

	CullBadCommandsResult cullBadCommands(); ///< prevent the user from giving mouse commands that he shouldn't be able to do during playback.

	CRCInfo m_crcInfo;
	File* m_file;
	AsciiString m_fileName;
	Int m_currentFilePosition;
	RecorderModeType m_mode;
	AsciiString m_currentReplayFilename;							///< valid during playback only
	UnsignedInt m_playbackFrameCount;

	ReplayGameInfo m_gameInfo;
	Bool m_wasDesync;

	Bool m_doingAnalysis;
	Bool m_archiveReplays;														///< if true, each replay is archived to the replay archive folder after recording

	Int m_originalGameMode; // valid in replays

	UnsignedInt m_nextFrame;												///< The Frame that the next message is to be executed on.  This can be -1.

	// resume-from-replay state (see the accessors above)
	File* m_resumeSourceFile;												///< the resume source (Resume.rep) being read during catchup; m_file is the new recording
	UnsignedInt m_resumeArmedHandoffFrame;					///< handoff frame for the next MSG_NEW_GAME, 0 when not armed
	UnsignedInt m_resumeArmedSourceLastFrame;				///< last complete frame of the resume source, from scanReplayLastFrame
	UnsignedInt m_resumeHandoffFrame;								///< frame at which injection stops and the freeze begins; stays set after the handoff for the CRC window
	UnsignedInt m_resumeSourceLastFrame;						///< injection never reads past this frame, so a torn tail cannot be replayed
	Bool m_resumeLeadInAnnounced;										///< the lead-in message has been shown
	UnsignedInt m_resumeFreezeStartMs;							///< wall-clock start of the freeze, 0 when not frozen
	Int m_resumeFreezeLastAnnounced;								///< last countdown second shown

	Bool startResumeCatchup();											///< open the resume source and switch to catchup; the recording is already started
	void updateResumeCatchup();											///< inject the source's commands for this frame, hand off at the handoff frame
	void injectResumeCommands(UnsignedInt curFrame);	///< read the source's records for curFrame into TheCommandList
	void endResumeCatchup();												///< close the source and leave catchup without handing off
	void abortResume(const UnicodeString &why);			///< leave the game: the source cannot be replayed to the handoff
	void clearResumeState();
};

extern RecorderClass *TheRecorder;
RecorderClass *createRecorder();
