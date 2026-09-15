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

// TheSuperHackers @feature bill-rich 15/09/2026 Lobby "Randomize" button. The placement mirrors
// populateRandomSideAndColor / populateRandomStartPosition in GameLogic.cpp so the
// lobby result looks like what a match start would have produced, but it is drawn
// from an independent, clock-seeded RNG: nothing here consumes or predicts the
// game-logic RNG that match start seeds from the lobby seed.

#include "PreRTS.h"

#include "GameNetwork/RandomAssign.h"
#include "GameNetwork/GameInfo.h"
#include "Common/PlayerTemplate.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/MapUtil.h"
#include "GameClient/ChallengeGenerals.h"

#include <cstdlib>
#include <cmath>
#include <vector>

// Build the list of valid template indices for random assignment.
// Same filtering as populateRandomSideAndColor (GameLogic.cpp).
static void buildValidTemplates(const GameInfo *game, const std::vector<Int> &lockedTemplates, std::vector<Int> &out)
{
	Int count = ThePlayerTemplateStore->getPlayerTemplateCount();
	for (Int c = 0; c < count; ++c)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(c);
		if (!fac)
			continue;

		// Must have a starting building (filters out civilian etc)
		if (fac->getStartingBuilding().isEmpty())
			continue;

		// Respect old factions only mode
		if (game->oldFactionsOnly() && !fac->isOldFaction())
			continue;

		// Skip locked generals (list provided by caller from client code)
		Bool isLocked = FALSE;
		for (size_t k = 0; k < lockedTemplates.size(); ++k)
		{
			if (lockedTemplates[k] == c)
			{
				isLocked = TRUE;
				break;
			}
		}
		if (isLocked)
			continue;

		out.push_back(c);
	}
}

// Phase 1: Assign random factions and colors.
// Mirrors populateRandomSideAndColor (GameLogic.cpp:691-781).
// Only touches slots that have PLAYERTEMPLATE_RANDOM / color == -1.
static void assignRandomFactions(GameInfo *game, const std::vector<Int> &validTemplates)
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		// Assign faction if random
		Int playerTemplateIdx = slot->getPlayerTemplate();
		if (playerTemplateIdx == PLAYERTEMPLATE_RANDOM && !validTemplates.empty())
		{
			playerTemplateIdx = validTemplates[rand() % validTemplates.size()];
			slot->setPlayerTemplate(playerTemplateIdx);
		}

		// Assign color if random (-1)
		Int colorIdx = slot->getColor();
		if (colorIdx < 0 || colorIdx >= TheMultiplayerSettings->getNumColors())
		{
			// Pick uniformly among the colors nobody holds yet, so a full lobby
			// never ends up with a slot left at -1 because a blind retry loop
			// kept landing on taken colors.
			std::vector<Int> freeColors;
			Int numColors = TheMultiplayerSettings->getNumColors();
			for (Int c = 0; c < numColors; ++c)
			{
				if (!game->isColorTaken(c))
					freeColors.push_back(c);
			}
			if (!freeColors.empty())
				slot->setColor(freeColors[rand() % freeColors.size()]);
		}
	}
}

// Phase 2: Assign start positions using distance-based placement.
// Mirrors populateRandomStartPosition (GameLogic.cpp:787-1053).
// Different teams are placed far apart, teammates close together.
// Only touches slots that have startPos == -1.
static void assignRandomPositions(GameInfo *game)
{
	Int i;
	Int numPlayers = MAX_SLOTS;
	const MapMetaData *md = TheMapCache ? TheMapCache->findMap(game->getMap()) : nullptr;
	if (md)
		numPlayers = md->m_numPlayers;

	if (numPlayers <= 0)
		return;
	// The per-spot tables below are MAX_SLOTS wide; never index past them.
	if (numPlayers > MAX_SLOTS)
		numPlayers = MAX_SLOTS;

	// Build distance matrix between all start positions using map waypoints
	static const WaypointMap s_emptyWaypoints = {};
	const WaypointMap &waypoints = md ? md->m_waypoints : s_emptyWaypoints;
	Real startSpotDistance[MAX_SLOTS][MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		for (Int j = 0; j < MAX_SLOTS; ++j)
		{
			if (i != j && i < numPlayers && j < numPlayers)
			{
				AsciiString w1, w2;
				w1.format("Player_%d_Start", i + 1);
				w2.format("Player_%d_Start", j + 1);
				WaypointMap::const_iterator c1 = waypoints.find(w1);
				WaypointMap::const_iterator c2 = waypoints.find(w2);
				if (c1 == waypoints.end() || c2 == waypoints.end())
				{
					startSpotDistance[i][j] = 1000000.0f;
				}
				else
				{
					Coord3D p1 = c1->second;
					Coord3D p2 = c2->second;
					startSpotDistance[i][j] = sqrt(sqr(p1.x - p2.x) + sqr(p1.y - p2.y));
				}
			}
			else
			{
				startSpotDistance[i][j] = 0.0f;
			}
		}
	}

	// Track which positions are already taken (deliberately assigned)
	Bool taken[MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
		taken[i] = (i < numPlayers) ? FALSE : TRUE;

	Bool hasStartSpotBeenPicked = FALSE;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
		{
			hasStartSpotBeenPicked = TRUE;
			taken[posIdx] = TRUE;
		}
	}

	// Track first position per team for teammate clustering
	Int teamPosIdx[MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
		teamPosIdx[i] = -1;

	// Seed teamPosIdx from already-assigned slots
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;
		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
		{
			Int team = slot->getTeamNumber();
			if (team >= 0 && teamPosIdx[team] == -1)
				teamPosIdx[team] = posIdx;
		}
	}

	// Assign positions for non-observer slots that don't have one yet
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
			continue; // already assigned

		Int team = slot->getTeamNumber();

		if (!hasStartSpotBeenPicked)
		{
			// First player: pick randomly
			posIdx = -1;
			for (Int attempt = 0; attempt < numPlayers * 2 && posIdx == -1; ++attempt)
			{
				Int candidate = rand() % numPlayers;
				if (!taken[candidate])
					posIdx = candidate;
			}
			if (posIdx < 0)
				continue;
			hasStartSpotBeenPicked = TRUE;
			slot->setStartPos(posIdx);
			taken[posIdx] = TRUE;
			if (team >= 0)
				teamPosIdx[team] = posIdx;
		}
		else if (team < 0 || teamPosIdx[team] == -1)
		{
			// New team or no team: pick position farthest from all taken positions
			Real farthestDistance = 0.0f;
			Int farthestIndex = -1;
			for (posIdx = 0; posIdx < numPlayers; ++posIdx)
			{
				if (taken[posIdx])
					continue;

				Real dist = 0.0f;
				for (Int n = 0; n < numPlayers; ++n)
				{
					if (taken[n] && n != posIdx)
						dist += startSpotDistance[posIdx][n];
				}
				if (farthestIndex < 0 || dist > farthestDistance)
				{
					farthestDistance = dist;
					farthestIndex = posIdx;
				}
			}

			if (farthestIndex >= 0)
			{
				slot->setStartPos(farthestIndex);
				taken[farthestIndex] = TRUE;
				if (team >= 0)
					teamPosIdx[team] = farthestIndex;
			}
		}
		else
		{
			// Teammate: pick position closest to team's existing position
			Real closestDist = FLT_MAX;
			Int closestIdx = -1;
			for (Int n = 0; n < numPlayers; ++n)
			{
				if (!taken[n] && startSpotDistance[teamPosIdx[team]][n] < closestDist)
				{
					closestDist = startSpotDistance[teamPosIdx[team]][n];
					closestIdx = n;
				}
			}
			if (closestIdx >= 0)
			{
				slot->setStartPos(closestIdx);
				taken[closestIdx] = TRUE;
			}
		}
	}

	// Assign observer slots to an existing player's position
	Int numPlayersInGame = 0;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++numPlayersInGame;
	}
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = -1;
		if (numPlayersInGame == 0)
		{
			posIdx = 0;
		}
		else
		{
			// Pick a random position that IS taken by a real player
			for (Int attempt = 0; attempt < numPlayers * 2 && posIdx == -1; ++attempt)
			{
				Int candidate = rand() % numPlayers;
				if (game->isStartPositionTaken(candidate))
					posIdx = candidate;
			}
		}
		if (posIdx >= 0)
			slot->setStartPos(posIdx);
	}
}

void performRandomAssign(GameInfo *game, const std::vector<Int> &lockedTemplates)
{
	if (!game)
		return;

	// Clock-seeded CRT RNG, independent of the game-logic RNG (see header).
	srand((unsigned int)timeGetTime());

	// Phase 1: Assign factions and colors for random slots
	std::vector<Int> validTemplates;
	buildValidTemplates(game, lockedTemplates, validTemplates);
	assignRandomFactions(game, validTemplates);

	// Phase 2: Assign start positions using distance-based placement
	assignRandomPositions(game);

	// Reset accepted state since we changed settings
	game->resetAccepted();
}

std::vector<Int> buildLockedTemplates()
{
	std::vector<Int> lockedTemplates;
	Int templateCount = ThePlayerTemplateStore->getPlayerTemplateCount();
	for (Int t = 0; t < templateCount; ++t)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(t);
		if (fac)
		{
			const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(fac->getName());
			if (general && !general->isStartingEnabled())
				lockedTemplates.push_back(t);
		}
	}
	return lockedTemplates;
}
