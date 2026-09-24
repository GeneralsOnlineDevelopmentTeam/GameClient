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

// TheSuperHackers @feature bill-rich 24/09/2026 Resume-from-replay arming and validation. See the header.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/ResumeFromReplay.h"

#include "Common/GlobalData.h"
#include "Common/Version.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/GameInfo.h"

namespace ResumeFromReplay
{

AsciiString getSourceFileName()
{
	AsciiString name = RecorderClass::getResumeReplayFileName();
	name.concat(RecorderClass::getReplayExtention());
	return name;
}

AsciiString getSourceFilePath()
{
	AsciiString path = RecorderClass::getReplayDir();
	path.concat(getSourceFileName());
	return path;
}

UnicodeString formatGameTime(UnsignedInt frames)
{
	const UnsignedInt seconds = frames / LOGICFRAMES_PER_SECOND;
	UnicodeString text;
	text.format(L"%u:%02u", seconds / 60, seconds % 60);
	return text;
}

Bool readSource(SourceReplay &out, UnicodeString &why)
{
	out.mapData = nullptr;
	out.lastFrame = 0;
	if (TheRecorder == nullptr || !readReplayMapInfo(getSourceFileName(), out.header, out.game, out.mapData))
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeHeaderUnreadable", L"the resume replay could not be read");
		return FALSE;
	}
	if (out.header.versionNumber != TheVersion->getVersionNumber()
		|| out.header.exeCRC != TheGlobalData->m_exeCRC
		|| out.header.iniCRC != TheGlobalData->m_iniCRC)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeVersionMismatch", L"the resume replay was recorded with a different game version");
		return FALSE;
	}
	// The header's frame count is only patched in when a recording ends normally, so a recording
	// cut short by a crash (the very file a resume is for) reads 0 there, and a torn tail may hold
	// fewer complete records than the count says. Walk the records either way.
	out.lastFrame = TheRecorder->scanReplayLastFrame(getSourceFileName());
	if (out.lastFrame == 0)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeNoFrames", L"the resume replay holds no complete frames (or the recorder is busy)");
		return FALSE;
	}
	return TRUE;
}

static Bool computeHandoffFrame(UnsignedInt lastFrame, UnsignedInt &handoffOut, UnicodeString &why)
{
	const UnsignedInt slackFrames = HANDOFF_SLACK_SECONDS * LOGICFRAMES_PER_SECOND;
	if (lastFrame <= slackFrames)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeTooShort", L"the last replay is too short to resume");
		return FALSE;
	}
	handoffOut = lastFrame - slackFrames;
	return TRUE;
}

Bool prepareHostSource(SourceReplay &out, UnsignedInt &handoffFrame, UnicodeString &why)
{
	if (TheRecorder == nullptr)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeHeaderUnreadable", L"the resume replay could not be read");
		return FALSE;
	}

	AsciiString lastReplay = RecorderClass::getReplayDir();
	lastReplay.concat(RecorderClass::getLastReplayFileName());
	lastReplay.concat(RecorderClass::getReplayExtention());
	const AsciiString source = getSourceFilePath();
	if (!CopyFile(lastReplay.str(), source.str(), FALSE))
	{
		DEBUG_LOG(("ResumeFromReplay::prepareHostSource - could not copy %s to %s", lastReplay.str(), source.str()));
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeCopyFailed", L"the last replay could not be copied to the resume replay");
		return FALSE;
	}

	if (!readSource(out, why))
		return FALSE;
	if (out.mapData == nullptr)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeMapMissing", L"the replay's map is not installed");
		return FALSE;
	}
	return computeHandoffFrame(out.lastFrame, handoffFrame, why);
}

static UnicodeString slotOccupantName(const GameSlot *slot)
{
	if (slot != nullptr && slot->isHuman())
		return slot->getName();
	return TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeAIPlayer", L"the AI player");
}

static UnicodeString slotStateName(const GameSlot *slot)
{
	if (slot == nullptr)
		return UnicodeString::TheEmptyString;
	switch (slot->getState())
	{
		case SLOT_EASY_AI:   return TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeEasyAI", L"an easy AI");
		case SLOT_MED_AI:    return TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeMediumAI", L"a medium AI");
		case SLOT_BRUTAL_AI: return TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeHardAI", L"a hard AI");
		default:             return slot->getName();
	}
}

// The lobby must line up with the recording slot for slot. Recorded commands are indexed by
// slot position and the simulation is seeded by every slot's occupant, faction, start spot
// and team, so all of it has to match: every recorded human in the slot they held under the
// same name, every recorded AI in its slot at its difficulty, nobody in a slot that was empty,
// and the same faction, color, start position and team everywhere. Open and closed slots
// are both empty.
Bool slotsMatch(const ReplayGameInfo &replay, const GameInfo *lobby, UnicodeString &why)
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *rs = replay.getConstSlot(i);
		const GameSlot *ls = lobby->getConstSlot(i);
		const Bool replayOccupied = rs && rs->isOccupied();
		const Bool lobbyOccupied = ls && ls->isOccupied();
		if (!replayOccupied && !lobbyOccupied)
			continue;
		if (replayOccupied && !lobbyOccupied)
		{
			why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeSlotMismatch",
				L"slot %d must be %ls (it was theirs in the recording)", i + 1, slotStateName(rs).str());
			return FALSE;
		}
		if (!replayOccupied && lobbyOccupied)
		{
			why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeSlotExtra",
				L"slot %d was not a player in the recording, close it or move %ls", i + 1, slotOccupantName(ls).str());
			return FALSE;
		}
		// human vs AI, AI difficulty
		if (rs->getState() != ls->getState() || (rs->isHuman() && rs->getName().compare(ls->getName()) != 0))
		{
			why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeSlotMismatch",
				L"slot %d must be %ls (it was theirs in the recording)", i + 1, slotStateName(rs).str());
			return FALSE;
		}
		if (rs->getPlayerTemplate() != ls->getPlayerTemplate()
			|| rs->getColor() != ls->getColor()
			|| rs->getStartPos() != ls->getStartPos()
			|| rs->getTeamNumber() != ls->getTeamNumber())
		{
			why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeSlotSettingsMismatch",
				L"slot %d (%ls) has a different faction, color, start position or team than in the recording", i + 1, slotOccupantName(ls).str());
			return FALSE;
		}
	}
	return TRUE;
}

Bool sourceMatchesGame(const SourceReplay &replay, const GameInfo *game, UnsignedInt handoffFrame, UnicodeString &why)
{
	if (game == nullptr)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeNoLobby", L"there is no lobby to compare the replay with");
		return FALSE;
	}
	const ReplayGameInfo &rg = replay.game;
	if (replay.mapData == nullptr)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeMapMissing", L"the replay's map is not installed");
		return FALSE;
	}
	if (game->getMap().compareNoCase(rg.getMap()) != 0)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeMapMismatch", L"the lobby map is not the replay's map (%hs)", rg.getMap().str());
		return FALSE;
	}
	if (game->getMapCRC() != rg.getMapCRC())
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeMapCRCMismatch", L"the map file differs from the one the replay was recorded on");
		return FALSE;
	}
	if (game->getSeed() != rg.getSeed())
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeSeedMismatch", L"the lobby's random seed is not the replay's");
		return FALSE;
	}
	if (game->getStartingCash().countMoney() != rg.getStartingCash().countMoney())
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeCashMismatch", L"the starting cash differs from the recording (%u)", rg.getStartingCash().countMoney());
		return FALSE;
	}
	if (game->getSuperweaponRestriction() != rg.getSuperweaponRestriction())
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ResumeSuperweaponMismatch", L"the superweapon limit differs from the recording");
		return FALSE;
	}
	if (!slotsMatch(rg, game, why))
		return FALSE;
	if (handoffFrame != 0 && replay.lastFrame < handoffFrame)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeSourceTooShort",
			L"the replay ends at %ls, before the handoff at %ls", formatGameTime(replay.lastFrame).str(), formatGameTime(handoffFrame).str());
		return FALSE;
	}
	return TRUE;
}

Bool validateRoster(const ReplayGameInfo &replay, const GameInfo *lobby, UnicodeString &why)
{
	Int i;
	Int replayCount = 0;
	Int lobbyCount = 0;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *rs = replay.getConstSlot(i);
		if (rs && rs->isHuman())
			++replayCount;
		const GameSlot *ls = lobby->getConstSlot(i);
		if (ls && ls->isHuman())
			++lobbyCount;
	}
	if (replayCount != lobbyCount)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeCountMismatch",
			L"the lobby has %d players but the replay has %d", lobbyCount, replayCount);
		return FALSE;
	}

	const GameSlot *replayHost = replay.getConstSlot(0);
	const GameSlot *lobbyHost = lobby->getConstSlot(0);
	if (!replayHost || !lobbyHost || !replayHost->isHuman() || replayHost->getName().compare(lobbyHost->getName()) != 0)
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeHostMismatch",
			L"only %ls, the host of the recorded game, can resume it", replayHost ? replayHost->getName().str() : L"?");
		return FALSE;
	}

	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *rs = replay.getConstSlot(i);
		if (!rs || !rs->isHuman())
			continue;
		Bool found = FALSE;
		for (Int j = 0; j < MAX_SLOTS && !found; ++j)
		{
			const GameSlot *ls = lobby->getConstSlot(j);
			found = ls && ls->isHuman() && ls->getName().compare(rs->getName()) == 0;
		}
		if (!found)
		{
			why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeMissingPlayer", L"%ls is not in the lobby", rs->getName().str());
			return FALSE;
		}
	}
	return TRUE;
}

Bool guestSourceReport(const GameInfo *lobby, UnicodeString &report)
{
	SourceReplay replay;
	UnicodeString why;
	if (!readSource(replay, why))
	{
		report = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeGuestUnreadable", L"Resume: my copy of the replay could not be used (%ls)", why.str());
		return FALSE;
	}
	if (!sourceMatchesGame(replay, lobby, lobby ? lobby->getResumeHandoffFrame() : 0, why))
	{
		report = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeGuestMismatch", L"Resume: the replay does not match this lobby (%ls)", why.str());
		return FALSE;
	}
	report = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeGuestDownloaded", L"Resume: replay downloaded (ends at %ls)", formatGameTime(replay.lastFrame).str());
	return TRUE;
}

Bool prepareGameStart(GameInfo *game, UnicodeString &why)
{
	if (TheRecorder == nullptr)
		return TRUE;
	TheRecorder->disarmResume();
	if (game == nullptr || game->getResumeHandoffFrame() == 0)
		return TRUE;

	SourceReplay replay;
	UnicodeString reason;
	if (!readSource(replay, reason) || !sourceMatchesGame(replay, game, game->getResumeHandoffFrame(), reason))
	{
		why = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ResumeStartAborted",
			L"Resume aborted: %ls (my replay ends at %ls)", reason.str(), formatGameTime(replay.lastFrame).str());
		return FALSE;
	}
	TheRecorder->armResumeForNextGame(game->getResumeHandoffFrame(), replay.lastFrame);
	// The recorder owns the arm from here. Clearing it on the game info keeps RH= out of the
	// new recording's options string (external replay parsers do not know the token) and
	// leaves the lobby disarmed when the players come back after the match.
	game->setResumeHandoffFrame(0);
	return TRUE;
}

} // namespace ResumeFromReplay
