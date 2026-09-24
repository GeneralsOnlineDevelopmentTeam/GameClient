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

#include <cmath>
#include <random>
#include <vector>

// Private RNG for the roll: seeded from the clock, never the game-logic RNG, and
// not the process CRT RNG either so the roll has no side effect on anything else.
static Int rollBelow(Int n)
{
	static std::mt19937 s_rng((unsigned int)std::random_device{}() ^ (unsigned int)timeGetTime());
	return (n > 0) ? (Int)(s_rng() % (unsigned int)n) : 0;
}

// Build the list of valid template indices for random assignment.
// Same filtering as populateRandomSideAndColor (GameLogic.cpp).
static void buildValidTemplates(const GameInfo *game, std::vector<Int> &out)
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

		// Skip generals that are not unlocked on this client, the same test the
		// lobby faction combo applies (GUIUtil.cpp) and match start enforces.
		const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(fac->getName());
		if (general && !general->isStartingEnabled())
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
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		// Assign faction if random
		Int playerTemplateIdx = slot->getPlayerTemplate();
		if (playerTemplateIdx == PLAYERTEMPLATE_RANDOM && !validTemplates.empty())
		{
			playerTemplateIdx = validTemplates[rollBelow((Int)validTemplates.size())];
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
				slot->setColor(freeColors[rollBelow((Int)freeColors.size())]);
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
	const MapMetaData *md = TheMapCache ? TheMapCache->findMap(game->getMap()) : nullptr;
	if (!md)
		return; // unknown map: no start spots to hand out; match start will do it
	Int numPlayers = md->m_numPlayers;

	if (numPlayers <= 0)
		return;
	// The per-spot tables below are MAX_SLOTS wide; never index past them.
	if (numPlayers > MAX_SLOTS)
		numPlayers = MAX_SLOTS;

	// Build distance matrix between all start positions using map waypoints
	const WaypointMap &waypoints = md->m_waypoints;
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
				Int candidate = rollBelow(numPlayers);
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

	// Observer slots are left alone: they take a player's spot at match start.
}

void performRandomAssign(GameInfo *game, std::vector<RandomSlotAssignment> *outChanges)
{
	if (!game)
		return;

	Int before[MAX_SLOTS][3];
	Int i;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		before[i][0] = slot ? slot->getPlayerTemplate() : -1;
		before[i][1] = slot ? slot->getColor() : -1;
		before[i][2] = slot ? slot->getStartPos() : -1;
	}

	// Phase 1: Assign factions and colors for random slots
	std::vector<Int> validTemplates;
	buildValidTemplates(game, validTemplates);
	assignRandomFactions(game, validTemplates);

	// Phase 2: Assign start positions using distance-based placement
	assignRandomPositions(game);

	if (outChanges)
	{
		outChanges->clear();
		for (i = 0; i < MAX_SLOTS; ++i)
		{
			const GameSlot *slot = game->getConstSlot(i);
			if (!slot)
				continue;
			RandomSlotAssignment change;
			change.slotIndex = i;
			if (slot->getPlayerTemplate() != before[i][0])
				change.side = slot->getPlayerTemplate();
			if (slot->getColor() != before[i][1])
				change.color = slot->getColor();
			if (slot->getStartPos() != before[i][2])
				change.startPos = slot->getStartPos();
			if (change.side != -1 || change.color != -1 || change.startPos != -1)
				outChanges->push_back(change);
		}
	}

	// Reset accepted state since we changed settings
	game->resetAccepted();
}
