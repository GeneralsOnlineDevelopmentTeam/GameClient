/*
**	Command & Conquer Generals Zero Hour(tm)
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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/StatsExporter.h"
#include "Common/StatsUploader.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/GlobalData.h"
#include "Common/Energy.h"
#include "Common/ThingTemplate.h"
#include "Common/RandomValue.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/BattlePlanUpdate.h"

#include <stdio.h>
#include <zlib.h>

#include "GameNetwork/GeneralsOnline/json.hpp"

using ordered_json = nlohmann::ordered_json;

//-----------------------------------------------------------------------------

static std::string wideToString(const WideChar *s)
{
	std::string result;
	if (s == nullptr) return result;
	for (; *s != L'\0'; ++s)
	{
		unsigned int c = static_cast<unsigned int>(*s);
		if (c < 0x80)
		{
			result += static_cast<char>(c);
		}
		else if (c < 0x800)
		{
			result += static_cast<char>(0xC0 | (c >> 6));
			result += static_cast<char>(0x80 | (c & 0x3F));
		}
		else
		{
			result += static_cast<char>(0xE0 | (c >> 12));
			result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
			result += static_cast<char>(0x80 | (c & 0x3F));
		}
	}
	return result;
}

//-----------------------------------------------------------------------------

static const char* gameModeToString(GameMode mode)
{
	switch (mode)
	{
		case GAME_SINGLE_PLAYER: return "SinglePlayer";
		case GAME_LAN:           return "LAN";
		case GAME_SKIRMISH:      return "Skirmish";
		case GAME_REPLAY:        return "Replay";
		case GAME_SHELL:         return "Shell";
		case GAME_INTERNET:      return "Internet";
		case GAME_NONE:          return "None";
		default:                 return "Unknown";
	}
}

//-----------------------------------------------------------------------------

static Bool isGamePlayer(Player *player)
{
	if (player == nullptr) return FALSE;
	const PlayerTemplate *pt = player->getPlayerTemplate();
	if (pt == nullptr) return FALSE;
	const char *name = pt->getName().str();
	if (name == nullptr || name[0] == '\0') return FALSE;
	if (strcmp(name, "FactionObserver") == 0) return FALSE;
	if (strcmp(name, "FactionCivilian") == 0) return FALSE;
	return TRUE;
}

//-----------------------------------------------------------------------------

struct PlayerSnapshotData
{
	Int playerIndex;
	UnsignedInt money;
	Int moneyEarned;
	Int moneySpent;
};

struct PlayerStateData
{
	Int energyProduction;
	Int energyConsumption;
	Int rankLevel;
	Int skillPoints;
	Int sciencePurchasePoints;
	Bool hasRadar;
	Bool isDead;
	Int bombardment;
	Int holdTheLine;
	Int searchAndDestroy;
};

struct StateChangeEvent
{
	UnsignedInt frame;
	Int playerIndex;
};

struct EnergyEvent : StateChangeEvent { Int production; Int consumption; };
struct RankEvent : StateChangeEvent { Int rankLevel; };
struct SkillPointsEvent : StateChangeEvent { Int skillPoints; };
struct SciencePointsEvent : StateChangeEvent { Int sciencePurchasePoints; };
struct RadarEvent : StateChangeEvent { Bool hasRadar; };
struct DeathEvent : StateChangeEvent {};
struct BattlePlanEvent : StateChangeEvent { Int bombardment; Int holdTheLine; Int searchAndDestroy; };

struct FrameSnapshotData
{
	UnsignedInt frame;
	Int playerCount;
	PlayerSnapshotData players[MAX_PLAYER_COUNT];
};

struct KillEventData
{
	UnsignedInt frame;
	Int killerPlayerIndex;
	Int victimPlayerIndex;
	Real x;
	Real y;
	char killerTemplateName[64];
	char victimTemplateName[64];
	char damageType[32];
};

struct BuildEventData
{
	UnsignedInt frame;
	Int playerIndex;
	Real x;
	Real y;
	Int cost;
	Int buildTime;
	char templateName[64];
	char producerTemplateName[64];
};

struct CaptureEventData
{
	UnsignedInt frame;
	Int newOwnerPlayerIndex;
	Int oldOwnerPlayerIndex;
	Real x;
	Real y;
	char templateName[64];
};

struct StatsExporterState
{
	Bool exportingActive;
	Bool mappingInitialized;
	Int gamePlayerCount;
	Int originalToNewIndex[MAX_PLAYER_COUNT];
	UnsignedInt lastSnapshotFrame;
	PlayerStateData lastPlayerState[MAX_PLAYER_COUNT];

	std::vector<FrameSnapshotData> snapshots;
	std::vector<KillEventData> killEvents;
	std::vector<BuildEventData> buildEvents;
	std::vector<CaptureEventData> captureEvents;
	std::vector<EnergyEvent> energyEvents;
	std::vector<RankEvent> rankEvents;
	std::vector<SkillPointsEvent> skillPointsEvents;
	std::vector<SciencePointsEvent> sciencePointsEvents;
	std::vector<RadarEvent> radarEvents;
	std::vector<DeathEvent> deathEvents;
	std::vector<BattlePlanEvent> battlePlanEvents;

	void resetData()
	{
		mappingInitialized = FALSE;
		gamePlayerCount = 0;
		lastSnapshotFrame = 0;
		memset(originalToNewIndex, 0, sizeof(originalToNewIndex));
		memset(lastPlayerState, 0, sizeof(lastPlayerState));
		snapshots.clear();
		killEvents.clear();
		buildEvents.clear();
		captureEvents.clear();
		energyEvents.clear();
		rankEvents.clear();
		skillPointsEvents.clear();
		sciencePointsEvents.clear();
		radarEvents.clear();
		deathEvents.clear();
		battlePlanEvents.clear();
	}
};

static StatsExporterState s_state;

//-----------------------------------------------------------------------------

static Int remapPlayerIndex(Int rawIndex)
{
	if (rawIndex >= 0 && rawIndex < MAX_PLAYER_COUNT)
		return s_state.originalToNewIndex[rawIndex];
	return 0;
}

//-----------------------------------------------------------------------------

static void initPlayerMapping()
{
	if (s_state.mappingInitialized)
		return;

	s_state.gamePlayerCount = 0;
	memset(s_state.originalToNewIndex, 0, sizeof(s_state.originalToNewIndex));

	const Int totalPlayers = ThePlayerList->getPlayerCount();
	Int i;
	for (i = 0; i < totalPlayers && i < MAX_PLAYER_COUNT; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (isGamePlayer(player))
		{
			++s_state.gamePlayerCount;
			s_state.originalToNewIndex[i] = s_state.gamePlayerCount;
		}
	}

	// Only lock in the mapping once we find actual game players.
	// Early calls (before players are fully initialized) will retry.
	if (s_state.gamePlayerCount > 0)
		s_state.mappingInitialized = TRUE;
}

//-----------------------------------------------------------------------------

void StatsExporterCollectSnapshot()
{
	if (ThePlayerList == nullptr || TheGameLogic == nullptr)
		return;

	UnsignedInt currentFrame = TheGameLogic->getFrame();
	if (!s_state.snapshots.empty() && (currentFrame - s_state.lastSnapshotFrame) < 30)
		return;

	s_state.lastSnapshotFrame = currentFrame;

	initPlayerMapping();

	const Int totalPlayers = ThePlayerList->getPlayerCount();

	FrameSnapshotData snap;
	memset(&snap, 0, sizeof(snap));
	snap.frame = currentFrame;
	snap.playerCount = s_state.gamePlayerCount;

	Int gameIdx = 0;
	Int i;
	for (i = 0; i < totalPlayers && i < MAX_PLAYER_COUNT; ++i)
	{
		if (s_state.originalToNewIndex[i] == 0)
			continue;

		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr)
			continue;

		PlayerSnapshotData &pd = snap.players[gameIdx];
		ScoreKeeper *sk = player->getScoreKeeper();
		const Energy *energy = player->getEnergy();

		pd.playerIndex = s_state.originalToNewIndex[i];
		pd.money = player->getMoney()->countMoney();
		pd.moneyEarned = sk->getTotalMoneyEarned();
		pd.moneySpent = sk->getTotalMoneySpent();

		// Detect state changes and emit events
		{
			PlayerStateData &last = s_state.lastPlayerState[i];
			Int curVal, curVal2, curVal3;
			Bool curBool;

			curVal = energy->getProduction();
			curVal2 = energy->getConsumption();
			if (curVal != last.energyProduction || curVal2 != last.energyConsumption)
			{
				EnergyEvent eev;
				memset(&eev, 0, sizeof(eev));
				eev.frame = currentFrame;
				eev.playerIndex = i;
				eev.production = curVal;
				eev.consumption = curVal2;
				s_state.energyEvents.push_back(eev);
				last.energyProduction = curVal;
				last.energyConsumption = curVal2;
			}

			curVal = player->getRankLevel();
			if (curVal != last.rankLevel)
			{
				RankEvent rev;
				memset(&rev, 0, sizeof(rev));
				rev.frame = currentFrame;
				rev.playerIndex = i;
				rev.rankLevel = curVal;
				s_state.rankEvents.push_back(rev);
				last.rankLevel = curVal;
			}

			curVal = player->getSkillPoints();
			if (curVal != last.skillPoints)
			{
				SkillPointsEvent sev;
				memset(&sev, 0, sizeof(sev));
				sev.frame = currentFrame;
				sev.playerIndex = i;
				sev.skillPoints = curVal;
				s_state.skillPointsEvents.push_back(sev);
				last.skillPoints = curVal;
			}

			curVal = player->getSciencePurchasePoints();
			if (curVal != last.sciencePurchasePoints)
			{
				SciencePointsEvent spev;
				memset(&spev, 0, sizeof(spev));
				spev.frame = currentFrame;
				spev.playerIndex = i;
				spev.sciencePurchasePoints = curVal;
				s_state.sciencePointsEvents.push_back(spev);
				last.sciencePurchasePoints = curVal;
			}

			curBool = player->hasRadar();
			if (curBool != last.hasRadar)
			{
				RadarEvent raev;
				memset(&raev, 0, sizeof(raev));
				raev.frame = currentFrame;
				raev.playerIndex = i;
				raev.hasRadar = curBool;
				s_state.radarEvents.push_back(raev);
				last.hasRadar = curBool;
			}

			curBool = player->isPlayerDead();
			if (curBool && !last.isDead)
			{
				DeathEvent dev;
				memset(&dev, 0, sizeof(dev));
				dev.frame = currentFrame;
				dev.playerIndex = i;
				s_state.deathEvents.push_back(dev);
				last.isDead = curBool;
			}

			curVal = player->getBattlePlansActiveSpecific(PLANSTATUS_BOMBARDMENT);
			curVal2 = player->getBattlePlansActiveSpecific(PLANSTATUS_HOLDTHELINE);
			curVal3 = player->getBattlePlansActiveSpecific(PLANSTATUS_SEARCHANDDESTROY);
			if (curVal != last.bombardment || curVal2 != last.holdTheLine || curVal3 != last.searchAndDestroy)
			{
				BattlePlanEvent bev;
				memset(&bev, 0, sizeof(bev));
				bev.frame = currentFrame;
				bev.playerIndex = i;
				bev.bombardment = curVal;
				bev.holdTheLine = curVal2;
				bev.searchAndDestroy = curVal3;
				s_state.battlePlanEvents.push_back(bev);
				last.bombardment = curVal;
				last.holdTheLine = curVal2;
				last.searchAndDestroy = curVal3;
			}
		}

		++gameIdx;
	}

	s_state.snapshots.push_back(snap);
}

//-----------------------------------------------------------------------------

void StatsExporterBeginRecording()
{
	s_state.exportingActive = TRUE;
	s_state.resetData();
}

//-----------------------------------------------------------------------------

void StatsExporterRecordKill(const Object *killer, const Object *victim, const DamageInfo *damageInfo)
{
	if (!s_state.exportingActive)
		return;
	if (killer == nullptr || victim == nullptr || TheGameLogic == nullptr)
		return;

	const Player *killerPlayer = killer->getControllingPlayer();
	const Player *victimPlayer = victim->getControllingPlayer();
	if (killerPlayer == nullptr || victimPlayer == nullptr)
		return;

	KillEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player indices; remapped to game-player indices at export time.
	ev.killerPlayerIndex = killerPlayer->getPlayerIndex();
	ev.victimPlayerIndex = victimPlayer->getPlayerIndex();

	const Coord3D *pos = victim->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}

	strlcpy(ev.killerTemplateName, killer->getTemplate()->getName().str(), ARRAY_SIZE(ev.killerTemplateName));
	strlcpy(ev.victimTemplateName, victim->getTemplate()->getName().str(), ARRAY_SIZE(ev.victimTemplateName));

	if (damageInfo != nullptr && damageInfo->in.m_damageType >= 0 && damageInfo->in.m_damageType < DAMAGE_NUM_TYPES)
	{
		const char *name = DamageTypeFlags::s_bitNameList[damageInfo->in.m_damageType];
		if (name != nullptr)
			strlcpy(ev.damageType, name, ARRAY_SIZE(ev.damageType));
	}

	s_state.killEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

void StatsExporterRecordBuild(const Object *producer, const Object *built)
{
	if (!s_state.exportingActive)
		return;
	if (built == nullptr || TheGameLogic == nullptr)
		return;

	const Player *player = built->getControllingPlayer();
	if (player == nullptr)
		return;

	BuildEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player index; remapped at export time.
	ev.playerIndex = player->getPlayerIndex();

	const Coord3D *pos = built->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}
	ev.cost = built->getTemplate()->calcCostToBuild(player);
	ev.buildTime = built->getTemplate()->calcTimeToBuild(player);

	strlcpy(ev.templateName, built->getTemplate()->getName().str(), ARRAY_SIZE(ev.templateName));

	if (producer != nullptr)
		strlcpy(ev.producerTemplateName, producer->getTemplate()->getName().str(), ARRAY_SIZE(ev.producerTemplateName));

	s_state.buildEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

void StatsExporterRecordCapture(const Object *captured, const Player *oldOwner, const Player *newOwner)
{
	if (!s_state.exportingActive)
		return;
	if (captured == nullptr || oldOwner == nullptr || newOwner == nullptr || TheGameLogic == nullptr)
		return;
	if (oldOwner == newOwner)
		return;

	CaptureEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player indices; remapped at export time.
	ev.newOwnerPlayerIndex = newOwner->getPlayerIndex();
	ev.oldOwnerPlayerIndex = oldOwner->getPlayerIndex();

	const Coord3D *pos = captured->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}

	strlcpy(ev.templateName, captured->getTemplate()->getName().str(), ARRAY_SIZE(ev.templateName));

	s_state.captureEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

static ordered_json buildCaptureEventsJson()
{
	ordered_json arr = ordered_json::array();
	for (size_t i = 0; i < s_state.captureEvents.size(); ++i)
	{
		const CaptureEventData &ev = s_state.captureEvents[i];
		arr.push_back(ordered_json{
			{"frame", ev.frame},
			{"newOwner", remapPlayerIndex(ev.newOwnerPlayerIndex)},
			{"oldOwner", remapPlayerIndex(ev.oldOwnerPlayerIndex)},
			{"x", ev.x},
			{"y", ev.y},
			{"object", ev.templateName}
		});
	}
	return arr;
}

//-----------------------------------------------------------------------------

static ordered_json buildBuildEventsJson()
{
	ordered_json arr = ordered_json::array();
	for (size_t i = 0; i < s_state.buildEvents.size(); ++i)
	{
		const BuildEventData &ev = s_state.buildEvents[i];
		arr.push_back(ordered_json{
			{"frame", ev.frame},
			{"player", remapPlayerIndex(ev.playerIndex)},
			{"x", ev.x},
			{"y", ev.y},
			{"cost", ev.cost},
			{"buildTime", ev.buildTime},
			{"object", ev.templateName},
			{"producer", ev.producerTemplateName}
		});
	}
	return arr;
}

//-----------------------------------------------------------------------------

static ordered_json buildKillEventsJson()
{
	ordered_json arr = ordered_json::array();
	for (size_t i = 0; i < s_state.killEvents.size(); ++i)
	{
		const KillEventData &ev = s_state.killEvents[i];
		arr.push_back(ordered_json{
			{"frame", ev.frame},
			{"killerPlayer", remapPlayerIndex(ev.killerPlayerIndex)},
			{"victimPlayer", remapPlayerIndex(ev.victimPlayerIndex)},
			{"x", ev.x},
			{"y", ev.y},
			{"killer", ev.killerTemplateName},
			{"victim", ev.victimTemplateName},
			{"damageType", ev.damageType}
		});
	}
	return arr;
}

//-----------------------------------------------------------------------------

static void buildStateChangeEventsJson(ordered_json &root)
{
	size_t i;

	ordered_json energyArr = ordered_json::array();
	for (i = 0; i < s_state.energyEvents.size(); ++i)
	{
		const EnergyEvent &ev = s_state.energyEvents[i];
		energyArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"production", ev.production}, {"consumption", ev.consumption}
		});
	}
	root["energyEvents"] = energyArr;

	ordered_json rankArr = ordered_json::array();
	for (i = 0; i < s_state.rankEvents.size(); ++i)
	{
		const RankEvent &ev = s_state.rankEvents[i];
		rankArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"rankLevel", ev.rankLevel}
		});
	}
	root["rankEvents"] = rankArr;

	ordered_json skillArr = ordered_json::array();
	for (i = 0; i < s_state.skillPointsEvents.size(); ++i)
	{
		const SkillPointsEvent &ev = s_state.skillPointsEvents[i];
		skillArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"skillPoints", ev.skillPoints}
		});
	}
	root["skillPointsEvents"] = skillArr;

	ordered_json scienceArr = ordered_json::array();
	for (i = 0; i < s_state.sciencePointsEvents.size(); ++i)
	{
		const SciencePointsEvent &ev = s_state.sciencePointsEvents[i];
		scienceArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"sciencePurchasePoints", ev.sciencePurchasePoints}
		});
	}
	root["sciencePointsEvents"] = scienceArr;

	ordered_json radarArr = ordered_json::array();
	for (i = 0; i < s_state.radarEvents.size(); ++i)
	{
		const RadarEvent &ev = s_state.radarEvents[i];
		radarArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"hasRadar", static_cast<bool>(ev.hasRadar)}
		});
	}
	root["radarEvents"] = radarArr;

	ordered_json deathArr = ordered_json::array();
	for (i = 0; i < s_state.deathEvents.size(); ++i)
	{
		const DeathEvent &ev = s_state.deathEvents[i];
		deathArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)}
		});
	}
	root["deathEvents"] = deathArr;

	ordered_json bpArr = ordered_json::array();
	for (i = 0; i < s_state.battlePlanEvents.size(); ++i)
	{
		const BattlePlanEvent &ev = s_state.battlePlanEvents[i];
		bpArr.push_back(ordered_json{
			{"frame", ev.frame}, {"player", remapPlayerIndex(ev.playerIndex)},
			{"bombardment", ev.bombardment}, {"holdTheLine", ev.holdTheLine},
			{"searchAndDestroy", ev.searchAndDestroy}
		});
	}
	root["battlePlanEvents"] = bpArr;
}

//-----------------------------------------------------------------------------

static ordered_json buildTimeSeriesJson()
{
	ordered_json ts;
	ordered_json playersArr = ordered_json::array();

	for (Int pi = 0; pi < s_state.gamePlayerCount; ++pi)
	{
		ordered_json p;
		p["index"] = pi + 1;

		ordered_json money = ordered_json::array();
		ordered_json moneyEarned = ordered_json::array();
		ordered_json moneySpent = ordered_json::array();

		for (size_t s = 0; s < s_state.snapshots.size(); ++s)
		{
			money.push_back(s_state.snapshots[s].players[pi].money);
			moneyEarned.push_back(s_state.snapshots[s].players[pi].moneyEarned);
			moneySpent.push_back(s_state.snapshots[s].players[pi].moneySpent);
		}

		p["money"] = money;
		p["moneyEarned"] = moneyEarned;
		p["moneySpent"] = moneySpent;
		playersArr.push_back(p);
	}

	ts["players"] = playersArr;
	return ts;
}

//-----------------------------------------------------------------------------

void ExportGameStatsJSON(const AsciiString& replayDir, const AsciiString& replayFileName)
{
	if (ThePlayerList == nullptr || TheGameLogic == nullptr || TheGlobalData == nullptr)
		return;

	// Strip any directory components from the replay filename
	const char *replayBase = replayFileName.str();
	const char *lastSlash = strrchr(replayBase, '/');
	const char *lastBackslash = strrchr(replayBase, '\\');
	if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
		lastSlash = lastBackslash;
	if (lastSlash != nullptr)
		replayBase = lastSlash + 1;

	// Build stats file path: replace .rep extension with .gamestats.json.gz
	char baseName[_MAX_PATH + 1];
	strlcpy(baseName, replayBase, ARRAY_SIZE(baseName));
	char *dot = strrchr(baseName, '.');
	if (dot != nullptr) *dot = '\0';

	AsciiString statsPath;
	statsPath.format("%s%s.gamestats.json.gz", replayDir.str(), baseName);

	initPlayerMapping();

	const Int playerCount = ThePlayerList->getPlayerCount();

	// Build JSON document
	ordered_json root;
	root["version"] = 1;

	// Game info
	root["game"] = ordered_json{
		{"map", TheGlobalData->m_mapName.str()},
		{"mode", gameModeToString(TheGameLogic->getGameMode())},
		{"frameCount", TheGameLogic->getFrame()},
		{"seed", GetGameLogicRandomSeed()},
		{"replayFile", replayFileName.str()},
		{"playerCount", s_state.gamePlayerCount},
		{"snapshotInterval", 30}
	};

	// Players array
	ordered_json playersArr = ordered_json::array();
	Int i;
	for (i = 0; i < playerCount; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr || !isGamePlayer(player))
			continue;

		ScoreKeeper *sk = player->getScoreKeeper();
		const PlayerTemplate *pt = player->getPlayerTemplate();
		const AcademyStats *academy = player->getAcademyStats();

		ordered_json p;
		p["index"] = s_state.originalToNewIndex[i];
		p["displayName"] = wideToString(player->getPlayerDisplayName().str());
		if (pt != nullptr)
			p["faction"] = pt->getName().str();
		p["side"] = player->getSide().str();
		p["baseSide"] = player->getBaseSide().str();
		p["type"] = player->getPlayerType() == PLAYER_HUMAN ? "Human" : "Computer";

		char colorBuf[8];
		snprintf(colorBuf, sizeof(colorBuf), "#%06X", static_cast<unsigned int>(player->getPlayerColor()) & 0x00FFFFFFu);
		p["color"] = colorBuf;

		p["money"] = player->getMoney()->countMoney();
		p["moneyEarned"] = sk->getTotalMoneyEarned();
		p["moneySpent"] = sk->getTotalMoneySpent();
		p["score"] = sk->calculateScore();

		p["academy"] = ordered_json{
			{"supplyCentersBuilt", academy->getSupplyCentersBuilt()},
			{"peonsBuilt", academy->getPeonsBuilt()},
			{"structuresCaptured", academy->getStructuresCaptured()},
			{"generalsPointsSpent", academy->getGeneralsPointsSpent()},
			{"specialPowersUsed", academy->getSpecialPowersUsed()},
			{"structuresGarrisoned", academy->getStructuresGarrisoned()},
			{"upgradesPurchased", academy->getUpgradesPurchased()},
			{"gatherersBuilt", academy->getGatherersBuilt()},
			{"heroesBuilt", academy->getHeroesBuilt()},
			{"controlGroupsUsed", academy->getControlGroupsUsed()},
			{"secondaryIncomeUnitsBuilt", academy->getSecondaryIncomeUnitsBuilt()},
			{"clearedGarrisonedBuildings", academy->getClearedGarrisonedBuildings()},
			{"salvageCollected", academy->getSalvageCollected()},
			{"guardAbilityUsedCount", academy->getGuardAbilityUsedCount()},
			{"doubleClickAttackMoveOrdersGiven", academy->getDoubleClickAttackMoveOrdersGiven()},
			{"minesCleared", academy->getMinesCleared()},
			{"vehiclesDisguised", academy->getVehiclesDisguised()},
			{"firestormsCreated", academy->getFirestormsCreated()}
		};

		playersArr.push_back(p);
	}
	root["players"] = playersArr;

	root["buildEvents"] = buildBuildEventsJson();
	root["killEvents"] = buildKillEventsJson();
	root["captureEvents"] = buildCaptureEventsJson();
	buildStateChangeEventsJson(root);
	root["timeSeries"] = buildTimeSeriesJson();

	std::string jsonStr = root.dump(2);

	// Write gzip-compressed output to file
	printf("[stats] Writing %u bytes JSON to %s\n", static_cast<unsigned int>(jsonStr.size()), statsPath.str());
	fflush(stdout);
	gzFile gz = gzopen(statsPath.str(), "wb9");
	if (gz != nullptr)
	{
		gzwrite(gz, jsonStr.data(), static_cast<unsigned int>(jsonStr.size()));
		gzclose(gz);
	}
	else
	{
		printf("[stats] ERROR: Failed to open %s for writing\n", statsPath.str());
		fflush(stdout);
	}

	// Upload gzip file to server if URL configured
	if (!TheGlobalData->m_statsUrl.isEmpty())
	{
		FILE *f = fopen(statsPath.str(), "rb");
		if (f != nullptr)
		{
			fseek(f, 0, SEEK_END);
			long fileSize = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (fileSize > 0)
			{
				void *fileData = malloc(static_cast<size_t>(fileSize));
				if (fileData != nullptr)
				{
					if (fread(fileData, 1, static_cast<size_t>(fileSize), f) == static_cast<size_t>(fileSize))
					{
						printf("[stats] Uploading %ld bytes to %s\n", fileSize, TheGlobalData->m_statsUrl.str());
						fflush(stdout);
						UploadStatsToServer(TheGlobalData->m_statsUrl, fileData, static_cast<unsigned int>(fileSize), GetGameLogicRandomSeed());
					}
					free(fileData);
				}
			}
			fclose(f);
		}
		else
		{
			printf("[stats] ERROR: Failed to read %s for upload\n", statsPath.str());
			fflush(stdout);
		}
	}

	s_state.resetData();
	s_state.exportingActive = FALSE;
}
