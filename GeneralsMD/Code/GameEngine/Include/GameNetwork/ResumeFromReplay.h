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

// TheSuperHackers @feature bill-rich 24/09/2026 Resume-from-replay: the arming and validation
// shared by the LAN lobby, the online lobby and the game start path.
//
// After a match dies, the same players resume it from the host's recording. The host arms it
// in the lobby (/resume): its own last recording (00000000.rep) is copied to Resume.rep, the
// lobby is aligned with the recording (slots, map, seed) and the handoff frame is broadcast in
// the game options. That one file is then pushed to every guest (the map transfer path on LAN,
// an upload to the service online), each guest validates its copy against the lobby, and on
// game start RecorderClass replays Resume.rep in lockstep up to the handoff frame while
// recording a brand new 00000000.rep of the resumed match.

#pragma once

#include "Common/Recorder.h"

class GameInfo;
class MapMetaData;

/// Reads a replay's header and game info without opening it for playback. Defined in ReplayMenu.cpp.
Bool readReplayMapInfo(const AsciiString& filename, RecorderClass::ReplayHeader &header, ReplayGameInfo &info, const MapMetaData *&mapData);

namespace ResumeFromReplay
{
	/// The handoff sits this far before the end of the recording, so the last seconds before
	/// the match died (which every player has seen play out) are not replayed as if new.
	constexpr Int HANDOFF_SLACK_SECONDS = 10;

	/// What a client knows about its copy of the resume source (Resume.rep).
	struct SourceReplay
	{
		RecorderClass::ReplayHeader header;
		ReplayGameInfo game;
		const MapMetaData *mapData = nullptr;
		UnsignedInt lastFrame = 0;   ///< last frame with a complete record; the header's count is 0 for a recording cut short by a crash
	};

	AsciiString getSourceFileName();                   ///< "Resume.rep"
	AsciiString getSourceFilePath();                   ///< full path of the resume source in the replay directory
	UnicodeString formatGameTime(UnsignedInt frames);  ///< m:ss

	/// Host: copies the last recording to Resume.rep, reads it back, checks the version and computes the
	/// handoff frame. The reason for a refusal is returned in why (without a "Resume:" prefix).
	Bool prepareHostSource(SourceReplay &out, UnsignedInt &handoffFrame, UnicodeString &why);

	/// Reads this client's Resume.rep: header, version check and the last complete frame.
	Bool readSource(SourceReplay &out, UnicodeString &why);

	/// Compares the source recording against the lobby: map, map CRC, seed, starting cash, superweapon
	/// restriction, every slot's occupant, and (when handoffFrame != 0) that the recording reaches it.
	Bool sourceMatchesGame(const SourceReplay &replay, const GameInfo *game, UnsignedInt handoffFrame, UnicodeString &why);

	/// Host arming: every human in the recording is in the lobby under the same name, the host is
	/// the recording's slot-0 player and nobody extra is present (slot order is checked separately).
	Bool validateRoster(const ReplayGameInfo &replay, const GameInfo *lobby, UnicodeString &why);

	/// Every slot as recorded: occupant (human by name, AI by difficulty, or empty), faction, color, start position and team.
	Bool slotsMatch(const ReplayGameInfo &replay, const GameInfo *lobby, UnicodeString &why);

	/// Guest: after its copy of Resume.rep arrived, whether it matches the lobby, with one chat line
	/// for everyone either way.
	Bool guestSourceReport(const GameInfo *lobby, UnicodeString &report);

	/// Game start (LAN OnGameStart / NGMPGame::startGame), before MSG_NEW_GAME: when the lobby is armed,
	/// validates this client's Resume.rep against it, arms the recorder and clears the arm on the game
	/// info (the recorder owns it from here). Returns FALSE with the reason when the start must be
	/// abandoned; the recorder is never left armed in that case.
	Bool prepareGameStart(GameInfo *game, UnicodeString &why);
}
