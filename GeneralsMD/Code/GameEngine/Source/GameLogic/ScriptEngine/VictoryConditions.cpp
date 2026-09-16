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

// FILE: VictoryConditions.cpp //////////////////////////////////////////////////////
// Generals multiplayer victory condition specifications
// Author: Matthew D. Campbell, February 2002

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AudioEventRTS.h"
#include "Common/GameAudio.h"
#include "Common/GameCommon.h"
#include "Common/GameEngine.h"
#include "Common/GameUtility.h"
#include "Common/KindOf.h"
#include "Common/OptionPreferences.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/Radar.h"
#include "Common/Recorder.h"

#include "GameClient/InGameUI.h"
#include "GameClient/Diplomacy.h"
#include "GameClient/GameText.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GameClient.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkDefs.h"


//-------------------------------------------------------------------------------------------------
#define ISSET(x) (m_victoryConditions & VICTORY_##x)

//-------------------------------------------------------------------------------------------------
VictoryConditionsInterface *TheVictoryConditions = nullptr;

//-------------------------------------------------------------------------------------------------
inline static Bool areAllies(const Player *p1, const Player *p2)
{
	if (p1 != p2 &&
		p1->getRelationship(p2->getDefaultTeam()) == ALLIES &&
		p2->getRelationship(p1->getDefaultTeam()) == ALLIES)
		return true;

	return false;
}

//-------------------------------------------------------------------------------------------------
class VictoryConditions : public VictoryConditionsInterface
{
public:
	VictoryConditions();

	virtual void init() override;
	virtual void reset() override;
	virtual void update() override;

	virtual Bool hasAchievedVictory(Player *player) override;					///< has a specific player and his allies won?
	virtual Bool hasBeenDefeated(Player *player) override;							///< has a specific player and his allies lost?
	virtual Bool hasSinglePlayerBeenDefeated(Player *player) override;	///< has a specific player lost?

	virtual void cachePlayerPtrs() override;											///< players have been created - cache the ones of interest

	virtual Bool isLocalAlliedVictory() override;								///< convenience function
	virtual Bool isLocalAlliedDefeat() override;									///< convenience function
	virtual Bool isLocalDefeat() override;												///< convenience function
	virtual Bool amIObserver() override { return m_isObserver;} 	///< Am I an observer?( need this for scripts )
	virtual UnsignedInt getEndFrame() override { return m_endFrame; }	///< on which frame was the game effectively over?
private:
	Player* findFirstUndefeatedPlayer(); ///< Find the first player that has not been defeated.
	void markAllianceVictorious(Player* victoriousPlayer); ///< Mark the victorious player and his allies as victorious.
	Bool multipleAlliancesExist(); ///< Are there multiple alliances still alive?

	// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
	Bool hasUndefeatedAlly(Player* player); ///< Does this player still have a living ally?
	void updateAutoLeaveOnDefeat();					///< Arm, announce and fire the auto-leave countdown.

	Player*				m_players[MAX_PLAYER_COUNT];
	Int						m_localSlotNum;
	UnsignedInt		m_endFrame;
	Bool					m_isDefeated[MAX_PLAYER_COUNT];
	Bool					m_isVictorious[MAX_PLAYER_COUNT];
	Bool					m_localPlayerDefeated;												///< prevents condition from being signaled each frame
	Bool					m_singleAllianceRemaining;										///< prevents condition from being signaled each frame
	Bool					m_isObserver;

	// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
	UnsignedInt		m_autoLeaveSeconds;					///< configured duration, 0 when the feature is off
	UnsignedInt		m_autoLeaveFrame;						///< frame the client leaves on, 0 when not counting down
	UnsignedInt		m_autoLeaveAnnounced;				///< last announced seconds remaining, so we only say it once
	Bool					m_autoLeaveFired;						///< prevents re-arming while the deferred exit is pending
};

//-------------------------------------------------------------------------------------------------
VictoryConditionsInterface * createVictoryConditions()
{
	// only one created, so no MemoryPool usage
	return NEW VictoryConditions;
}

//-------------------------------------------------------------------------------------------------
VictoryConditions::VictoryConditions()
{
	reset();
}

//-------------------------------------------------------------------------------------------------
void VictoryConditions::init()
{
	reset();
}

//-------------------------------------------------------------------------------------------------
void VictoryConditions::reset()
{
	for (Int i=0; i<MAX_PLAYER_COUNT; ++i)
	{
		m_players[i] = nullptr;
		m_isDefeated[i] = false;
		m_isVictorious[i] = false;
	}
	m_localSlotNum = -1;

	m_localPlayerDefeated = false;
	m_singleAllianceRemaining = false;
	m_isObserver = false;
	m_endFrame = 0;

	// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
	m_autoLeaveSeconds = 0;
	m_autoLeaveFrame = 0;
	m_autoLeaveAnnounced = 0;
	m_autoLeaveFired = false;

	m_victoryConditions = VICTORY_NOBUILDINGS | VICTORY_NOUNITS;
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::multipleAlliancesExist()
{
	Player* alive = nullptr;

	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		Player* player = m_players[i];

		if (player && !hasSinglePlayerBeenDefeated(player))
		{
			if (alive)
			{
				// check to verify they are on the same team
				if (!areAllies(alive, player))
				{
					return true;
				}
			}
			else
			{
				alive = player; // save this pointer to check against
			}
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
void VictoryConditions::update()
{
	if (!TheRecorder->isMultiplayer() || (m_localSlotNum < 0 && !m_isObserver))
		return;

	// Check for a single winning alliance
	if (!m_singleAllianceRemaining)
	{
		if (!multipleAlliancesExist())
		{
			m_singleAllianceRemaining = true; // don't check again
			m_endFrame = TheGameLogic->getFrame();

			Player* victoriousPlayer = findFirstUndefeatedPlayer();

			if (victoriousPlayer)
				markAllianceVictorious(victoriousPlayer);
		}
	}

	// check for player eliminations
	for (Int i=0; i<MAX_PLAYER_COUNT; ++i)
	{
		Player *p = m_players[i];
		if (p && !m_isDefeated[i] && hasSinglePlayerBeenDefeated(p))
		{
			m_isDefeated[i] = true;
			if (TheGameLogic->getFrame() > 1)
			{
				ThePartitionManager->revealMapForPlayerPermanently( p->getPlayerIndex() );
				TheGameClient->updateFakeDrawables();

				TheInGameUI->message("GUI:PlayerHasBeenDefeated", p->getPlayerDisplayName().str() );
				// People are boneheads. Also play a sound
				static AudioEventRTS leftGameSound("GUIMessageReceived");
				TheAudio->addAudioEvent(&leftGameSound);
			}

			for (Int idx = 0; idx < MAX_SLOTS; ++idx)
			{
				AsciiString pName;
				pName.format("player%d", idx);
				if (p->getPlayerNameKey() == NAMEKEY(pName))
				{
					GameSlot *slot = (TheGameInfo)?TheGameInfo->getSlot(idx):nullptr;
					if (slot && slot->isAI())
					{
						DEBUG_LOG(("Marking AI player %s as defeated", pName.str()));
						slot->setLastFrameInGame(TheGameLogic->getFrame());
					}
				}
			}

			// destroy any remaining units (infantry if its a short game, for example)
			p->killPlayer();
			PopulateInGameDiplomacyPopup();
		}
	}

	// Check if the local player has been eliminated
	if (!m_localPlayerDefeated && !m_isObserver)
	{
		Player *localPlayer = m_players[m_localSlotNum];
		if (hasSinglePlayerBeenDefeated(localPlayer))
		{
			if (!m_singleAllianceRemaining)
			{
				//MessageBoxOk(TheGameText->fetch("GUI:Defeat"), TheGameText->fetch("GUI:LocalDefeat"), nullptr);
			}
			m_localPlayerDefeated = true;	// don't check again
			TheRadar->forceOn(localPlayer->getPlayerIndex(), TRUE);
			SetInGameChatType( INGAME_CHAT_EVERYONE ); // can't chat to allies after death.  Only to other observers.
		}
	}

	// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
	updateAutoLeaveOnDefeat();
}

// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
// Announce sparsely while there is plenty of time left, then every second at the end, so a long
// countdown does not flood the message area.
//-------------------------------------------------------------------------------------------------
inline static Bool shouldAnnounceAutoLeave(UnsignedInt secondsLeft)
{
	if (secondsLeft > 60)
		return (secondsLeft % 60) == 0;

	if (secondsLeft > 10)
		return secondsLeft == 60 || secondsLeft == 30;

	return secondsLeft == 10 || secondsLeft <= 5;
}

// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
/** Does this player still have an ally that has not been defeated?  areAllies() is false when a
	* player is compared against itself, so only other players are considered. */
//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::hasUndefeatedAlly(Player *player)
{
	if (!player)
		return false;

	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		Player *other = m_players[i];
		if (other && areAllies(other, player) && !hasSinglePlayerBeenDefeated(other))
			return true;
	}

	return false;
}

// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
/** Returns a defeated local player to the score screen after a configurable delay, so losing does
	* not leave the client sitting in a spectating state.  This is purely local: it automates the
	* quit the player can already perform by hand, and changes no simulation state, so it is safe to
	* drive from a local preference and cannot desync.  Disabled unless the preference is set. */
//-------------------------------------------------------------------------------------------------
void VictoryConditions::updateAutoLeaveOnDefeat()
{
	// exitGame() only posts a deferred MSG_CLEAR_GAME_DATA, so update() keeps running for a few
	// frames afterwards.  Without this the countdown would re-arm and re-announce on the way out.
	if (m_autoLeaveSeconds == 0 || m_autoLeaveFired)
		return;

	// Observers must never be counted down.  cachePlayerPtrs() sets m_localPlayerDefeated for an
	// observer, so testing m_isObserver here is load bearing and must not be removed.
	if (m_isObserver || m_localSlotNum < 0)
		return;

	const UnsignedInt now = TheGameLogic->getFrame();

	// Defeat handling is suppressed on the opening frames of a match; match that.
	if (now <= 1)
		return;

	// Once a single alliance remains the match is already resolving and the score screen is coming
	// on its own.  Firing exitGame() into that transition would race the normal end of match path.
	if (m_singleAllianceRemaining)
	{
		m_autoLeaveFrame = 0;
		return;
	}

	if (m_autoLeaveFrame == 0)
	{
		if (!m_localPlayerDefeated)
			return;

		// A defeated player whose ally is still alive is still marked victorious if that ally wins,
		// so leaving now would throw away a win they are still entitled to.  This is re-tested every
		// frame rather than once at defeat, because with three or more alliances the local alliance
		// can be wiped out while the match carries on between the others.
		if (hasUndefeatedAlly(m_players[m_localSlotNum]))
			return;

		m_autoLeaveFrame = now + m_autoLeaveSeconds * LOGICFRAMES_PER_SECOND;
		m_autoLeaveAnnounced = 0;
	}

	if (now >= m_autoLeaveFrame)
	{
		m_autoLeaveFrame = 0;
		m_autoLeaveFired = true;
		// exitGame() returns to the score screen.  Deliberately not quit(), which would either open
		// the quit menu or self destruct the player in a multiplayer game.
		TheGameLogic->exitGame();
		return;
	}

	// Frame based, so the countdown follows game time: it freezes while the game is paused and
	// tracks the game speed setting rather than the wall clock.
	const UnsignedInt framesLeft = m_autoLeaveFrame - now;
	const UnsignedInt secondsLeft = (framesLeft + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND;

	const Bool firstAnnouncement = (m_autoLeaveAnnounced == 0);
	if (secondsLeft != m_autoLeaveAnnounced && (firstAnnouncement || shouldAnnounceAutoLeave(secondsLeft)))
	{
		m_autoLeaveAnnounced = secondsLeft;
		TheInGameUI->message("GUI:AutoLeaveOnDefeatCountdown", (Int)secondsLeft);
	}
}

//-------------------------------------------------------------------------------------------------
Player* VictoryConditions::findFirstUndefeatedPlayer()
{
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		Player* player = m_players[i];
		if (player && !hasSinglePlayerBeenDefeated(player))
			return player;
	}

	return nullptr;
}

// TheSuperHackers @bugfix Stubbjax 11/02/2026 This marks the player and any allies as victorious, including
// defeated allies. This also ensures players retain their victorious status if their assets are destroyed
// after the victory conditions are met (e.g. when quitting the game prior to the victory screen).
//-------------------------------------------------------------------------------------------------
void VictoryConditions::markAllianceVictorious(Player* victoriousPlayer)
{
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		Player* player = m_players[i];
		if (player == victoriousPlayer || (player && areAllies(player, victoriousPlayer)))
			m_isVictorious[i] = true;
	}
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::hasAchievedVictory(Player *player)
{
	if (!player)
		return false;

	if (!m_singleAllianceRemaining)
		return false;

	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		if (player == m_players[i])
		{
			if (m_isVictorious[i])
				return true;

			break;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::hasBeenDefeated(Player *player)
{
	if (!player)
		return false;

	if (!m_singleAllianceRemaining)
		return false;

	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		if (player == m_players[i])
		{
			if (m_isDefeated[i])
				return true;

			break;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::hasSinglePlayerBeenDefeated(Player *player)
{
	if (!player)
		return false;

	KindOfMaskType mask;
	mask.set(KINDOF_MP_COUNT_FOR_VICTORY);

	if ( ISSET(NOUNITS) && ISSET(NOBUILDINGS) )
	{
		if ( !player->hasAnyObjects() )
		{
			return true;
		}
	}
	else if ( ISSET(NOUNITS) )
	{
		if ( !player->hasAnyUnits() )
		{
			return true;
		}
	}
	else if ( ISSET(NOBUILDINGS) )
	{
		if ( !player->hasAnyBuildings(mask) )
		{
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
void VictoryConditions::cachePlayerPtrs()
{
	if (!TheRecorder->isMultiplayer())
		return;

	Int playerCount = 0;
	const PlayerTemplate *civTemplate = ThePlayerTemplateStore->findPlayerTemplate( NAMEKEY("FactionCivilian") );
	for (Int i=0; i<MAX_PLAYER_COUNT; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		DEBUG_LOG(("Checking whether to cache player %d - [%ls], house [%ls]", i, player?player->getPlayerDisplayName().str():L"<NOBODY>", (player&&player->getPlayerTemplate())?player->getPlayerTemplate()->getDisplayName().str():L"<NONE>"));
		if (player && player != ThePlayerList->getNeutralPlayer() && player->getPlayerTemplate() && player->getPlayerTemplate() != civTemplate && !player->isPlayerObserver())
		{
			DEBUG_LOG(("Caching player"));
			m_players[playerCount] = player;
			if (m_players[playerCount]->isLocalPlayer())
				m_localSlotNum = playerCount;
			++playerCount;
		}
	}
	while (playerCount < MAX_PLAYER_COUNT)
	{
		m_players[playerCount++] = nullptr;
	}

	if (m_localSlotNum < 0)
	{
		m_localPlayerDefeated = true;	// if we have no local player, don't check for defeat
		m_isObserver = true;
	}

	// TheSuperHackers @feature JawadYzbk 16/09/2026 Add optional auto-leave on defeat countdown.
	// Resolve the preference once per match, here rather than at defeat time, so the file read
	// happens during map load instead of mid game. Never arm during replay playback: TheGameLogic
	// points TheGameInfo at the recorded game, and a replay of a match the watched player lost
	// would otherwise exit itself partway through.
	// Only LAN and online games. Skirmish is excluded deliberately: a defeated skirmish player
	// restarts rather than sitting out a match, so there is nothing to leave. isInMultiplayerGame()
	// is exactly GAME_LAN or GAME_INTERNET, which also keeps campaign and replay out.
	m_autoLeaveSeconds = 0;
	if (TheGameLogic && TheGameLogic->isInMultiplayerGame()
		&& !m_isObserver && !(TheRecorder && TheRecorder->isPlaybackMode()))
	{
		// A duration enforced by the host wins over the local preference. Zero means the host does
		// not enforce one, in which case the player's own setting applies. TheGameInfo is null in
		// single player, so it is checked rather than assumed.
		const UnsignedInt hostSeconds = TheGameInfo ? TheGameInfo->getAutoLeaveSeconds() : 0;
		if (hostSeconds > 0)
		{
			m_autoLeaveSeconds = hostSeconds;
		}
		else
		{
			OptionPreferences optionPref;
			m_autoLeaveSeconds = optionPref.getAutoLeaveOnDefeatSeconds();
		}
	}
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::isLocalAlliedVictory()
{
	if (m_isObserver)
		return false;

	return (hasAchievedVictory(m_players[m_localSlotNum]));
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::isLocalAlliedDefeat()
{
	if (m_isObserver)
		return m_singleAllianceRemaining;

	return (hasBeenDefeated(m_players[m_localSlotNum]));
}

//-------------------------------------------------------------------------------------------------
Bool VictoryConditions::isLocalDefeat()
{
	if (m_isObserver)
		return FALSE;

	return (m_localPlayerDefeated);
}



