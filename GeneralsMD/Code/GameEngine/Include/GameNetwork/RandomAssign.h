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

// TheSuperHackers @feature bill-rich 15/09/2026 Lobby "Randomize" button: resolves every slot still set
// to Random (faction, color, start position) in the lobby, so a group no longer has
// to start and abandon a match just to learn the engine's random draw.

#pragma once

#include "Lib/BaseType.h"

#include <vector>

class GameInfo;

// One slot the roll changed. A field left at -1 was not touched by the roll.
struct RandomSlotAssignment
{
	Int slotIndex = -1;
	Int side = -1;
	Int color = -1;
	Int startPos = -1;
};

// Resolve random factions, colors and start positions on every occupied player
// slot of the lobby that still has them unset. Slots with explicit choices and
// observer slots are left alone (observers are resolved at match start, as in
// retail). Draws from a private RNG seeded from the clock, deliberately NOT from
// the game-logic RNG, so the result says nothing about the draw the engine will
// make from the lobby seed at match start. outChanges, when given, receives the
// slots that changed and what changed, for callers that must push the roll
// through an authoritative service instead of applying it locally.
void performRandomAssign(GameInfo *game, std::vector<RandomSlotAssignment> *outChanges = nullptr);
