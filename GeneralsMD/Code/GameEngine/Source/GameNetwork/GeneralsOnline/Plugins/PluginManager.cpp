#include "GameNetwork/GeneralsOnline/Plugins/PluginManager.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Upgrade.h"
#include "Common/Geometry.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameNetwork/NetworkDefs.h" // MAX_SLOTS
#include "GameNetwork/GameInfo.h" // GameSlot::getTeamNumber

#include <cctype>
#include <climits> // INT_MAX
#include <stdio.h>
#include <string>

std::vector<GOPluginManager::LoadedPlugin> GOPluginManager::s_plugins;
std::vector<GOGameplayEventCallbacks> GOPluginManager::s_gameplayEventHooks;
std::vector<GORenderCallbacks> GOPluginManager::s_renderHooks;
GOPluginManager::NativeHandleProvider GOPluginManager::s_d3dDevice8Provider = nullptr;
GOPluginManager::NativeHandleProvider GOPluginManager::s_gameWindowProvider = nullptr;

// The free functions the host API table points at. All dispatch is gated on
// IsLocalPlayerObserver(), so plugins only ever run for observers.
namespace
{
	void HostAPI_Log(const char* msg)
	{
		GOPluginManager::Log(msg);
	}

	void HostAPI_RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
	{
		GOPluginManager::RegisterGameplayEventHooks(cb);
	}

	void HostAPI_RegisterRenderHooks(const GORenderCallbacks* cb)
	{
		GOPluginManager::RegisterRenderHooks(cb);
	}

	// ---- Player roster queries. Matches playerIndex against Player::getPlayerIndex(), not list
	// position - those aren't guaranteed to be the same thing. ----

	Player* FindPlayerByIndex(uint32_t playerIndex)
	{
		if (ThePlayerList == nullptr)
			return nullptr;
		Int count = ThePlayerList->getPlayerCount();
		for (Int i = 0; i < count; ++i)
		{
			Player* p = ThePlayerList->getNthPlayer(i);
			if (p != nullptr && (uint32_t)p->getPlayerIndex() == playerIndex)
				return p;
		}
		return nullptr;
	}

	uint32_t HostAPI_GetPlayerColor(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr) ? (uint32_t)p->getPlayerColor() : 0;
	}

	// Walks the engine's player-slot name keys; the callback returns false to stop early. Not free
	// per call - see plans\plugin-framework\design-notes.md.
	typedef bool (*SlotPlayerCallback)(Int slot, Player* player, void* userData);

	void ForEachSlotPlayer(SlotPlayerCallback callback, void* userData)
	{
		if (callback == nullptr || ThePlayerList == nullptr || TheNameKeyGenerator == nullptr)
			return;

		for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		{
			AsciiString nameKeyStr;
			nameKeyStr.format("player%d", slot);

			Player* p = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey(nameKeyStr));
			if (p == nullptr)
				continue;
			if (!callback(slot, p, userData))
				return;
		}
	}

	struct SlotRosterContext
	{
		uint32_t* outPlayerIndices;
		uint32_t maxCount;
		uint32_t count;
		bool activeOnly;    // false keeps defeated/resigned players in the roster
	};

	bool CollectSlotPlayer(Int slot, Player* player, void* userData)
	{
		SlotRosterContext* ctx = (SlotRosterContext*)userData;
		if (player->isPlayerObserver())
			return true;
		if (ctx->activeOnly && !player->isPlayerActive())
			return true;

		ctx->outPlayerIndices[ctx->count++] = (uint32_t)player->getPlayerIndex();
		return ctx->count < ctx->maxCount;
	}

	uint32_t HostAPI_GetActivePlayers(uint32_t* outPlayerIndices, uint32_t maxCount)
	{
		if (outPlayerIndices == nullptr || maxCount == 0)
			return 0;

		SlotRosterContext ctx = { outPlayerIndices, maxCount, 0, true };
		ForEachSlotPlayer(CollectSlotPlayer, &ctx);
		return ctx.count;
	}

	// Mirrors the same check the engine's own observer-only UI uses (e.g.
	// InGameUI's observer stats/notifications gating) - dead also counts as observing, since a dead
	// player becomes spectator-like. Plugins that display information about other players must gate
	// on this.
	uint8_t HostAPI_IsLocalPlayerObserver()
	{
		return GOPluginManager::IsLocalPlayerObserver() ? 1 : 0;
	}

	// General-power roster. A power counts as owned when the player has the required science and at
	// least one live object carries its module - the same test the engine's own observer UI uses.

	struct PowerModuleFind
	{
		const SpecialPowerTemplate* powerTemplate;
		SpecialPowerModuleInterface* module;
		Object* object; // the STRUCTURE carrying the module, for GOGeneralPowerInfo::buildingObjectId
	};

	// Prefers a structure as the anchor but accepts any carrier - see
	// plans\plugin-framework\design-notes.md.
	static void FindPowerModule(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		PowerModuleFind* find = (PowerModuleFind*)userData;
		// Step 1: a structure is the best possible anchor, so stop once one has been found.
		if (find->object != nullptr)
			return;

		SpecialPowerModuleInterface* module = obj->getSpecialPowerModule(find->powerTemplate);
		if (module == nullptr)
			return;

		// Step 2: a structure wins outright and also becomes the reported building.
		if (obj->isKindOf(KINDOF_STRUCTURE))
		{
			find->module = module;
			find->object = obj;
			return;
		}

		// Step 3: otherwise keep the first carrier of any kind, with no building to anchor to.
		if (find->module == nullptr)
			find->module = module;
	}

	uint32_t HostAPI_GetPlayerGeneralPowers(uint32_t playerIndex, GOGeneralPowerInfo* outPowers, uint32_t maxCount)
	{
		if (outPowers == nullptr || maxCount == 0)
			return 0;
		if (ThePlayerList == nullptr || TheControlBar == nullptr)
			return 0;

		Player* player = FindPlayerByIndex(playerIndex);
		if (player == nullptr)
			return 0;

		const PlayerTemplate* pt = player->getPlayerTemplate();
		if (pt == nullptr)
			return 0;
		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return 0;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return 0;

		uint32_t count = 0;
		for (Int i = 0; i < MAX_COMMANDS_PER_SET && count < maxCount; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr)
				continue;
			const SpecialPowerTemplate* sp = btn->getSpecialPowerTemplate();
			if (sp == nullptr)
				continue;

			ScienceType required = sp->getRequiredScience();
			if (required != SCIENCE_INVALID && !player->hasScience(required))
				continue;

			PowerModuleFind find;
			find.powerTemplate = sp;
			find.module = nullptr;
			find.object = nullptr;
			player->iterateObjects(FindPowerModule, &find);
			if (find.module == nullptr)
				continue;

			GOGeneralPowerInfo& info = outPowers[count];
			info.templateName = sp->getName().str();
			info.rechargeFrames = sp->getReloadTime();
			info.buildingObjectId = (find.object != nullptr) ? (uint32_t)find.object->getID() : 0;
			const UnsignedInt readyFrame = find.module->getReadyFrame();
			const UnsignedInt now = (TheGameLogic != nullptr) ? (UnsignedInt)TheGameLogic->getFrame() : 0;
			info.framesUntilReady = (readyFrame > now) ? (readyFrame - now) : 0;
			++count;
		}
		return count;
	}

	// ---- Icon drawing. Only valid to call from within a GORenderCallbacks::onDrawOverlay
	// callback - same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawTemplateIcon2D(const char* templateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (templateName == nullptr || TheDisplay == nullptr)
			return;

		const Image* img = nullptr;
		if (TheThingFactory != nullptr)
		{
			const ThingTemplate* tmpl = TheThingFactory->findTemplate(AsciiString(templateName), FALSE);
			if (tmpl != nullptr)
				img = tmpl->getButtonImage();
		}
		if (img == nullptr && TheUpgradeCenter != nullptr)
		{
			const UpgradeTemplate* upgrade = TheUpgradeCenter->findUpgrade(templateName);
			if (upgrade != nullptr)
				img = upgrade->getButtonImage();
		}
		if (img != nullptr)
			TheDisplay->drawImage(img, x, y, x + width, y + height);
	}

	void HostAPI_DrawPowerIcon2D(uint32_t playerIndex, const char* powerTemplateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (powerTemplateName == nullptr || TheDisplay == nullptr || TheControlBar == nullptr)
			return;

		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return;

		const PlayerTemplate* pt = p->getPlayerTemplate();
		if (pt == nullptr)
			return;

		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return;

		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr || btn->getSpecialPowerTemplate() == nullptr)
				continue;
			if (strcmp(btn->getSpecialPowerTemplate()->getName().str(), powerTemplateName) != 0)
				continue;

			const Image* img = btn->getButtonImage();
			if (img != nullptr)
				TheDisplay->drawImage(img, x, y, x + width, y + height);
			return;
		}
	}

	void HostAPI_DrawText2D(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginText2D((Int)x, (Int)y, asciiText, (Color)colorARGB);
	}

	void HostAPI_DrawText2DScaled(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB, float sizeScale, uint8_t bold)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginText2DScaled((Int)x, (Int)y, asciiText, (Color)colorARGB, (Real)sizeScale, bold != 0);
	}

	void HostAPI_DrawRect2D(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colorARGB, uint8_t filled)
	{
		if (TheDisplay == nullptr)
			return;

		if (filled != 0)
			TheDisplay->drawFillRect((Int)x, (Int)y, (Int)width, (Int)height, (Color)colorARGB);
		else
			TheDisplay->drawOpenRect((Int)x, (Int)y, (Int)width, (Int)height, 1.0f, (Color)colorARGB);
	}

	// One rotated quad however long the line is, unlike DrawRect2D which is axis-aligned.
	void HostAPI_DrawLine2D(int32_t x1, int32_t y1, int32_t x2, int32_t y2, float thickness, uint32_t colorARGB)
	{
		if (TheDisplay == nullptr)
			return;

		TheDisplay->drawLine((Int)x1, (Int)y1, (Int)x2, (Int)y2, (Real)thickness, (Color)colorARGB);
	}

	void HostAPI_GetScreenSize(int32_t* outWidth, int32_t* outHeight)
	{
		UnsignedInt w = (TheDisplay != nullptr) ? TheDisplay->getWidth() : 0;
		UnsignedInt h = (TheDisplay != nullptr) ? TheDisplay->getHeight() : 0;
		if (outWidth != nullptr)
			*outWidth = (int32_t)w;
		if (outHeight != nullptr)
			*outHeight = (int32_t)h;
	}

	// ---- Simulation clock. ----

	uint32_t HostAPI_GetLogicFrame()
	{
		return (TheGameLogic != nullptr) ? (uint32_t)TheGameLogic->getFrame() : 0;
	}

	uint32_t HostAPI_GetLogicFramesPerSecond()
	{
		return (uint32_t)LOGICFRAMES_PER_SECOND;
	}

	// ---- Live production progress. Reads the producer's own queue, so the value is whatever the
	// simulation currently believes rather than a plugin-side reconstruction. ----

	ProductionUpdateInterface* FindProducer(uint32_t producerObjectId)
	{
		if (TheGameLogic == nullptr || producerObjectId == 0)
			return nullptr;
		Object* producer = TheGameLogic->findObjectByID((ObjectID)producerObjectId);
		return (producer != nullptr) ? producer->getProductionUpdateInterface() : nullptr;
	}

	float HostAPI_GetUnitProductionProgress(uint32_t producerObjectId, int32_t productionID)
	{
		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() == PRODUCTION_UNIT && (int32_t)entry->getProductionID() == productionID)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	float HostAPI_GetUpgradeProductionProgress(uint32_t producerObjectId, const char* upgradeTemplateName)
	{
		if (upgradeTemplateName == nullptr)
			return -1.0f;

		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() != PRODUCTION_UPGRADE)
				continue;
			const UpgradeTemplate* upgrade = entry->getProductionUpgrade();
			if (upgrade != nullptr && strcmp(upgrade->getUpgradeName().str(), upgradeTemplateName) == 0)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	// ---- World-space anchoring. ----

	uint8_t HostAPI_WorldToScreen(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY)
	{
		if (TheTacticalView == nullptr)
			return 0;

		Coord3D world;
		world.x = worldX;
		world.y = worldY;
		world.z = worldZ;

		ICoord2D screen;
		if (!TheTacticalView->worldToScreen(&world, &screen))
			return 0;

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Edge inset for clamped off-screen indicators, as a fraction of view height so it scales with
	// resolution (0.0333 is the original 24 pixels at 720p).
	const float kEdgeIndicatorMarginFraction = 0.0333f;

	// Like HostAPI_WorldToScreen, but clamps an off-screen point to the view edge instead of
	// dropping it - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_WorldToScreenClamped(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY)
	{
		if (TheTacticalView == nullptr)
			return 0;

		Coord3D world;
		world.x = worldX;
		world.y = worldY;
		world.z = worldZ;

		// Step 1: project, accepting a point beyond the far clip plane as well as an off-frustum one.
		ICoord2D screen;
		const View::WorldToScreenReturn result = TheTacticalView->worldToScreenTriReturnAllowFarClip(&world, &screen);
		if (result == View::WTS_INVALID)
			return 0;

		if (result == View::WTS_OUTSIDE_FRUSTUM)
		{
			// Step 2: clamp against the tactical view's own rectangle, which is not always the
			// whole display - the projected position is already in full-display coordinates.
			Int originX = 0;
			Int originY = 0;
			TheTacticalView->getOrigin(&originX, &originY);
			const float viewW = (float)TheTacticalView->getWidth();
			const float viewH = (float)TheTacticalView->getHeight();
			const float margin = viewH * kEdgeIndicatorMarginFraction;
			if (viewW > margin * 2.0f && viewH > margin * 2.0f)
			{
				// Step 3: clamp along the ray from the view's centre to the raw (possibly far
				// off-screen) projected point, so the indicator sits on the nearest edge.
				const float cx = (float)originX + viewW * 0.5f;
				const float cy = (float)originY + viewH * 0.5f;
				float dx = (float)screen.x - cx;
				float dy = (float)screen.y - cy;
				const float halfW = viewW * 0.5f - margin;
				const float halfH = viewH * 0.5f - margin;
				const float absDx = (dx < 0.0f) ? -dx : dx;
				const float absDy = (dy < 0.0f) ? -dy : dy;
				float scale = 1.0f;
				if (absDx > halfW && absDx > 0.0f)
					scale = halfW / absDx;
				if (absDy > halfH && absDy > 0.0f)
				{
					const float scaleY = halfH / absDy;
					if (scaleY < scale)
						scale = scaleY;
				}
				if (scale < 1.0f)
				{
					dx *= scale;
					dy *= scale;
				}
				screen.x = (Int)(cx + dx);
				screen.y = (Int)(cy + dy);
			}
		}

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Classifies by the same KindOf flags the engine's own faction-agnostic code uses, so no
	// per-faction cases are needed - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_GetObjectBuildingCategory(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return GO_BUILDING_CATEGORY_NONE;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr || !obj->isKindOf(KINDOF_STRUCTURE))
			return GO_BUILDING_CATEGORY_NONE;

		if (obj->isKindOf(KINDOF_COMMANDCENTER))
			return GO_BUILDING_CATEGORY_COMMAND_CENTER;
		if (obj->isKindOf(KINDOF_FS_WARFACTORY))
			return GO_BUILDING_CATEGORY_WAR_FACTORY;
		if (obj->isKindOf(KINDOF_FS_BARRACKS))
			return GO_BUILDING_CATEGORY_BARRACKS;
		if (obj->isKindOf(KINDOF_FS_AIRFIELD))
			return GO_BUILDING_CATEGORY_AIRFIELD;
		if (obj->isKindOf(KINDOF_FS_SUPPLY_CENTER))
			return GO_BUILDING_CATEGORY_SUPPLY_STASH;
		return GO_BUILDING_CATEGORY_NONE;
	}

	uint8_t HostAPI_GetObjectScreenBounds(uint32_t objectId, int32_t* outX, int32_t* outY, int32_t* outWidth, int32_t* outHeight)
	{
		if (TheGameLogic == nullptr || TheTacticalView == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		const Coord3D* pos = obj->getPosition();
		if (pos == nullptr)
			return 0;

		ICoord2D centre;
		if (!TheTacticalView->worldToScreen(pos, &centre))
			return 0;

		// Project a point one bounding radius to the side of the object as well; the horizontal
		// distance between the two projections is the object's on-screen size, which already
		// accounts for camera zoom and pitch without exposing any camera state to the plugin.
		const Real radius = obj->getGeometryInfo().getBoundingCircleRadius();
		Coord3D edgeWorld = *pos;
		edgeWorld.x += radius;

		ICoord2D edge;
		Int halfWidth = 0;
		if (TheTacticalView->worldToScreen(&edgeWorld, &edge))
			halfWidth = abs(edge.x - centre.x);
		if (halfWidth <= 0)
			halfWidth = 1;

		if (outX != nullptr)
			*outX = (int32_t)(centre.x - halfWidth);
		if (outY != nullptr)
			*outY = (int32_t)(centre.y - halfWidth);
		if (outWidth != nullptr)
			*outWidth = (int32_t)(halfWidth * 2);
		if (outHeight != nullptr)
			*outHeight = (int32_t)(halfWidth * 2);
		return 1;
	}

	// Projects the same point the engine draws its own health bar at - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_GetObjectHealthBarScreenPosition(uint32_t objectId, int32_t* outX, int32_t* outY)
	{
		if (TheGameLogic == nullptr || TheTacticalView == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		Coord3D pos;
		obj->getHealthBoxPosition(pos);

		ICoord2D screen;
		if (!TheTacticalView->worldToScreen(&pos, &screen))
			return 0;

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Camera control. Same look-at path the engine's own observer actions
	// use, so a plugin can jump the viewport to where a gameplay event happened when the user
	// clicks on it.

	void HostAPI_TeleportViewportTo(float worldX, float worldY, float worldZ)
	{
		if (TheTacticalView == nullptr)
			return;
		Coord3D target;
		target.x = worldX;
		target.y = worldY;
		target.z = worldZ;
		TheTacticalView->userLookAt(&target);
	}

	// ---- Clock-wedge draw primitives. Same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}

	void HostAPI_DrawRemainingRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRemainingRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}

	// Both handles come from the device layer through GOPluginManager's provider seam.

	void* HostAPI_GetD3DDevice8()
	{
		return GOPluginManager::GetD3DDevice8();
	}

	void* HostAPI_GetGameWindow()
	{
		return GOPluginManager::GetGameWindow();
	}

	struct EnumerateObjectsContext
	{
		void (*callback)(uint32_t, float, float, float, void*);
		void* userData;
		uint32_t count;
	};

	void EnumerateObjectsCallback(Object* object, void* userData)
	{
		EnumerateObjectsContext* context = (EnumerateObjectsContext*)userData;
		if (object == nullptr || context->callback == nullptr)
			return;
		const Coord3D* position = object->getPosition();
		if (position == nullptr)
			return;
		context->callback((uint32_t)object->getID(), position->x, position->y, position->z, context->userData);
		++context->count;
	}

	uint32_t HostAPI_EnumeratePlayerObjects(uint32_t playerIndex,
		void (*callback)(uint32_t, float, float, float, void*), void* userData)
	{
		Player* player = FindPlayerByIndex(playerIndex);
		if (player == nullptr || callback == nullptr)
			return 0;
		EnumerateObjectsContext context = { callback, userData, 0 };
		player->iterateObjects(EnumerateObjectsCallback, &context);
		return context.count;
	}

	uint32_t HostAPI_GetContainedObjects(uint32_t containerObjectId,
		GOContainedObjectInfo* outObjects, uint32_t maxCount)
	{
		if (TheGameLogic == nullptr || containerObjectId == 0 || outObjects == nullptr || maxCount == 0)
			return 0;
		Object* container = TheGameLogic->findObjectByID((ObjectID)containerObjectId);
		if (container == nullptr || container->getContain() == nullptr)
			return 0;
		const ContainedItemsList* items = container->getContain()->getContainedItemsList();
		if (items == nullptr)
			return 0;
		uint32_t count = 0;
		for (ContainedItemsList::const_iterator it = items->begin(); it != items->end() && count < maxCount; ++it)
		{
			Object* object = *it;
			if (object == nullptr || object->getTemplate() == nullptr)
				continue;
			GOContainedObjectInfo& info = outObjects[count++];
			info.objectId = (uint32_t)object->getID();
			info.templateName = object->getTemplate()->getName().str();
			info.playerIndex = object->getControllingPlayer() != nullptr
				? (uint32_t)object->getControllingPlayer()->getPlayerIndex() : 0;
		}
		return count;
	}

	uint8_t HostAPI_GetObjectTargetPosition(uint32_t objectId, float* outX, float* outY, float* outZ)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return 0;

		// Attacking a specific object: point at that object's current position.
		Object* victim = ai->getCurrentVictim();
		if (victim != nullptr)
		{
			const Coord3D* pos = victim->getPosition();
			if (pos != nullptr)
			{
				if (outX != nullptr) *outX = pos->x;
				if (outY != nullptr) *outY = pos->y;
				if (outZ != nullptr) *outZ = pos->z;
				return 1;
			}
		}

		// A goal position only counts for an explicit attack order, never a move-to or guard - see
		// plans\plugin-framework\design-notes.md.
		const StateID state = ai->getCurrentStateID();
		if (state != AI_ATTACK_MOVE_TO && state != AI_ATTACK_POSITION)
			return 0;

		const Coord3D* goal = ai->getGoalPosition();
		if (goal == nullptr)
			return 0;

		if (outX != nullptr) *outX = goal->x;
		if (outY != nullptr) *outY = goal->y;
		if (outZ != nullptr) *outZ = goal->z;
		return 1;
	}

	uint8_t HostAPI_IsObjectAirborne(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isAirborneTarget()) ? 1 : 0;
	}

	uint8_t HostAPI_IsObjectVehicle(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isKindOf(KINDOF_VEHICLE)) ? 1 : 0;
	}

	uint8_t HostAPI_IsObjectProjectile(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isKindOf(KINDOF_PROJECTILE)) ? 1 : 0;
	}

	uint32_t HostAPI_GetObjectContainerId(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;
		const Object* container = obj->getContainedBy();
		return (container != nullptr) ? (uint32_t)container->getID() : 0;
	}

	// ---- Player card queries (Plan 5). Same FindPlayerByIndex resolution as every other
	// per-player query above. ----

	// Each of the string-returning queries below owns one distinct function-local buffer, so only
	// its own most recent return value is live - see plans\plugin-framework\design-notes.md.
	const char* HostAPI_GetPlayerName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		// getPlayerDisplayName() is a UnicodeString; the ABI only carries UTF-8/ASCII.
		static AsciiString s_playerName;
		s_playerName.translate(p->getPlayerDisplayName());
		return s_playerName.str();
	}

	const char* HostAPI_GetPlayerFactionTemplate(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		// getSide() is the template's "Side" INI field ("AmericaLaserGeneral"), not
		// getPlayerTemplate()->getName(), which carries an unreadable "Faction" prefix.
		return p->getSide().str();
	}

	uint32_t HostAPI_GetPlayerMoney(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr) ? (uint32_t)p->getMoney()->countMoney() : 0;
	}

	// Both skill-point figures are absolute totals, so they are already a progress bar's numerator
	// and denominator - see plans\plugin-framework\design-notes.md.
	int32_t HostAPI_GetPlayerRank(uint32_t playerIndex, uint32_t* outCurrentXP, uint32_t* outNextXP)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return -1;
		const Int rankLevel = p->getRankLevel();
		if (rankLevel <= 0)
			return -1;
		if (outCurrentXP != nullptr)
			*outCurrentXP = (uint32_t)p->getSkillPoints();
		if (outNextXP != nullptr)
		{
			// At the rank cap there is no next rank, and the threshold is INT_MAX - report 0
			// rather than a meaningless huge denominator.
			const Int levelUp = p->getSkillPointsLevelUp();
			*outNextXP = (levelUp >= INT_MAX) ? 0 : (uint32_t)levelUp;
		}
		return (int32_t)(rankLevel - 1);
	}

	// getEnergy(), like getMoney() and getScoreKeeper(), returns the address of a Player member and
	// is never null once the Player itself resolved, so none of the three is null-checked.
	uint8_t HostAPI_GetPlayerPowerState(uint32_t playerIndex, uint32_t* outPowerGenerated, uint32_t* outPowerDrain)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		const Energy* energy = p->getEnergy();
		if (outPowerGenerated != nullptr)
			*outPowerGenerated = (uint32_t)energy->getProduction();
		if (outPowerDrain != nullptr)
			*outPowerDrain = (uint32_t)energy->getConsumption();
		return 1;
	}

	struct CountContext { uint32_t count; };

	// A builder is anything carrying a DozerAIInterface, which covers all three factions - see
	// plans\plugin-framework\design-notes.md.
	void CountBuilderCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai != nullptr && ai->getDozerAIInterface() != nullptr)
			((CountContext*)userData)->count++;
	}

	uint32_t HostAPI_GetPlayerBuilderCount(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		CountContext ctx = { 0 };
		p->iterateObjects(CountBuilderCallback, &ctx);
		return ctx.count;
	}

	// Counts units actually working the supply line right now, not every supply unit - see
	// plans\plugin-framework\design-notes.md.
	void CountActiveGathererCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return;
		SupplyTruckAIInterface* supplyAI = ai->getSupplyTruckAIInterface();
		if (supplyAI != nullptr && supplyAI->isCurrentlyFerryingSupplies())
			((CountContext*)userData)->count++;
	}

	uint32_t HostAPI_GetPlayerActiveGathererCount(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		CountContext ctx = { 0 };
		p->iterateObjects(CountActiveGathererCallback, &ctx);
		return ctx.count;
	}

	// The score screen's cumulative figure, deliberately raw rather than a rate - see plans\plugin-framework\design-notes.md.
	uint32_t HostAPI_GetPlayerTotalMoneyEarned(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		return (uint32_t)p->getScoreKeeper()->getTotalMoneyEarned();
	}

	// Looks up by raw template name rather than a live object, so it also resolves a template a
	// plugin only knows the name of (e.g. GOContainedObjectInfo::templateName from a garrison query).
	const char* HostAPI_GetTemplateDisplayName(const char* templateName)
	{
		if (templateName == nullptr || templateName[0] == '\0' || TheThingFactory == nullptr)
			return "";
		const ThingTemplate* tt = TheThingFactory->findTemplate(AsciiString(templateName));
		if (tt == nullptr)
			return "";
		static AsciiString s_templateDisplayName;
		s_templateDisplayName.translate(tt->getDisplayName());
		return s_templateDisplayName.str();
	}

	struct SampleTemplateContext { AsciiString templateName; bool found; };

	// Same qualifying check as CountBuilderCallback, but stops recording once one match is found -
	// this only needs a representative icon, not an exact count.
	void SampleBuilderTemplateCallback(Object* obj, void* userData)
	{
		SampleTemplateContext* ctx = (SampleTemplateContext*)userData;
		if (ctx->found || obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai != nullptr && ai->getDozerAIInterface() != nullptr && obj->getTemplate() != nullptr)
		{
			ctx->templateName = obj->getTemplate()->getName();
			ctx->found = true;
		}
	}

	// Same qualifying check as CountActiveGathererCallback (isCurrentlyFerryingSupplies, not merely
	// "is a supply unit"), so the sampled icon matches what getPlayerActiveGathererCount is counting.
	void SampleGathererTemplateCallback(Object* obj, void* userData)
	{
		SampleTemplateContext* ctx = (SampleTemplateContext*)userData;
		if (ctx->found || obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return;
		SupplyTruckAIInterface* supplyAI = ai->getSupplyTruckAIInterface();
		if (supplyAI != nullptr && supplyAI->isCurrentlyFerryingSupplies() && obj->getTemplate() != nullptr)
		{
			ctx->templateName = obj->getTemplate()->getName();
			ctx->found = true;
		}
	}

	const char* HostAPI_GetPlayerBuilderTemplateName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		SampleTemplateContext ctx = {};
		p->iterateObjects(SampleBuilderTemplateCallback, &ctx);
		static AsciiString s_builderTemplateName;
		s_builderTemplateName = ctx.templateName;
		return s_builderTemplateName.str();
	}

	// The base game has three distinct DozerAIInterface-carrying templates (USA Dozer, China Dozer,
	// GLA Worker); the extra headroom is for mods that add their own - see
	// plans\plugin-framework\design-notes.md.
	const uint32_t kMaxBuilderTypeBuckets = 8;

	struct BuilderTypeBucket { AsciiString name; uint32_t count; };
	struct BuilderTypeCountContext
	{
		BuilderTypeBucket buckets[kMaxBuilderTypeBuckets];
		uint32_t bucketCount = 0;
	};

	// As CountBuilderCallback, but keyed by template so a captured Worker is not collapsed into a
	// native Dozer - see plans\plugin-framework\design-notes.md.
	void CountBuilderTypeCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr || ai->getDozerAIInterface() == nullptr)
			return;
		const ThingTemplate* tt = obj->getTemplate();
		if (tt == nullptr)
			return;

		BuilderTypeCountContext* ctx = (BuilderTypeCountContext*)userData;
		const AsciiString& name = tt->getName();
		for (uint32_t i = 0; i < ctx->bucketCount; ++i)
		{
			if (ctx->buckets[i].name == name)
			{
				ctx->buckets[i].count++;
				return;
			}
		}
		if (ctx->bucketCount < kMaxBuilderTypeBuckets)
		{
			ctx->buckets[ctx->bucketCount].name = name;
			ctx->buckets[ctx->bucketCount].count = 1;
			ctx->bucketCount++;
		}
	}

	// Per-type breakdown of the player's live builder units - see PluginABI.h for the contract.
	uint32_t HostAPI_GetPlayerBuilderTemplateCounts(uint32_t playerIndex, const char** outNames, uint32_t* outCounts, uint32_t maxCount)
	{
		if (outNames == nullptr || outCounts == nullptr || maxCount == 0)
			return 0;
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;

		BuilderTypeCountContext ctx;
		p->iterateObjects(CountBuilderTypeCallback, &ctx);

		// This function's own backing storage for the returned pointers.
		static AsciiString s_builderTemplateNames[kMaxBuilderTypeBuckets];
		const uint32_t n = (ctx.bucketCount < maxCount) ? ctx.bucketCount : maxCount;
		for (uint32_t i = 0; i < n; ++i)
		{
			s_builderTemplateNames[i] = ctx.buckets[i].name;
			outNames[i] = s_builderTemplateNames[i].str();
			outCounts[i] = ctx.buckets[i].count;
		}
		return n;
	}

	const char* HostAPI_GetPlayerGathererTemplateName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		SampleTemplateContext ctx = {};
		p->iterateObjects(SampleGathererTemplateCallback, &ctx);
		static AsciiString s_gathererTemplateName;
		s_gathererTemplateName = ctx.templateName;
		return s_gathererTemplateName.str();
	}

	struct SlotLookupContext { uint32_t playerIndex; Int slot; };

	bool MatchSlotByPlayerIndex(Int slot, Player* player, void* userData)
	{
		SlotLookupContext* ctx = (SlotLookupContext*)userData;
		if ((uint32_t)player->getPlayerIndex() != ctx->playerIndex)
			return true;
		ctx->slot = slot;
		return false;
	}

	// playerIndex (Player::getPlayerIndex()) and the lobby slot index GameSlot is keyed by are two
	// different numbering spaces, so the slot has to be found first and then looked up in TheGameInfo.
	int32_t HostAPI_GetPlayerTeamNumber(uint32_t playerIndex)
	{
		if (TheGameInfo == nullptr)
			return -1;

		SlotLookupContext ctx = { playerIndex, -1 };
		ForEachSlotPlayer(MatchSlotByPlayerIndex, &ctx);
		if (ctx.slot < 0)
			return -1;

		const GameSlot* gameSlot = TheGameInfo->getConstSlot(ctx.slot);
		return (gameSlot != nullptr) ? gameSlot->getTeamNumber() : -1;
	}

	// As HostAPI_GetActivePlayers but without the isPlayerActive() filter, so the roster is stable
	// for the whole match - see plans\plugin-framework\design-notes.md.
	uint32_t HostAPI_GetMatchPlayers(uint32_t* outPlayerIndices, uint32_t maxCount)
	{
		if (outPlayerIndices == nullptr || maxCount == 0)
			return 0;

		SlotRosterContext ctx = { outPlayerIndices, maxCount, 0, false };
		ForEachSlotPlayer(CollectSlotPlayer, &ctx);
		return ctx.count;
	}

	uint8_t HostAPI_GetPlayerIsDefeated(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr && !p->isPlayerActive()) ? 1 : 0;
	}

	const char* HostAPI_GetUserDataPath()
	{
		// Cached in a function-local static because the ABI promises a pointer that stays valid for
		// the process lifetime, while getPath_UserData() hands back a temporary.
		static std::string s_userDataPath;
		if (s_userDataPath.empty() && TheGlobalData != nullptr)
			s_userDataPath = TheGlobalData->getPath_UserData().str();
		return s_userDataPath.c_str();
	}
} // anonymous namespace

// ------------------------------------------------------------------------------------------------

void GOPluginManager::Log(const char* msg)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s", msg);
}

// Called by GameEngineDevice once its display is up, before any plugin is loaded.
void GOPluginManager::SetNativeHandleProviders(NativeHandleProvider d3dDevice8, NativeHandleProvider gameWindow)
{
	s_d3dDevice8Provider = d3dDevice8;
	s_gameWindowProvider = gameWindow;
}

void* GOPluginManager::GetD3DDevice8()
{
	return (s_d3dDevice8Provider != nullptr) ? s_d3dDevice8Provider() : nullptr;
}

void* GOPluginManager::GetGameWindow()
{
	return (s_gameWindowProvider != nullptr) ? s_gameWindowProvider() : nullptr;
}

GOPluginHostAPI GOPluginManager::BuildHostAPI()
{
	GOPluginHostAPI api = {};
	api.abiVersion = GO_PLUGIN_ABI_VERSION;
	api.structSize = (uint32_t)sizeof(GOPluginHostAPI);
	api.log = HostAPI_Log;
	api.registerGameplayEventHooks = HostAPI_RegisterGameplayEventHooks;
	api.registerRenderHooks = HostAPI_RegisterRenderHooks;
	api.getPlayerColor = HostAPI_GetPlayerColor;
	api.getActivePlayers = HostAPI_GetActivePlayers;
	api.isLocalPlayerObserver = HostAPI_IsLocalPlayerObserver;
	api.getPlayerGeneralPowers = HostAPI_GetPlayerGeneralPowers;
	api.drawTemplateIcon2D = HostAPI_DrawTemplateIcon2D;
	api.drawPowerIcon2D = HostAPI_DrawPowerIcon2D;
	api.drawText2D = HostAPI_DrawText2D;
	api.drawText2DScaled = HostAPI_DrawText2DScaled;
	api.drawRect2D = HostAPI_DrawRect2D;
	api.getScreenSize = HostAPI_GetScreenSize;
	api.getLogicFrame = HostAPI_GetLogicFrame;
	api.getLogicFramesPerSecond = HostAPI_GetLogicFramesPerSecond;
	api.getUnitProductionProgress = HostAPI_GetUnitProductionProgress;
	api.getUpgradeProductionProgress = HostAPI_GetUpgradeProductionProgress;
	api.worldToScreen = HostAPI_WorldToScreen;
	api.worldToScreenClamped = HostAPI_WorldToScreenClamped;
	api.getObjectBuildingCategory = HostAPI_GetObjectBuildingCategory;
	api.getObjectScreenBounds = HostAPI_GetObjectScreenBounds;
	api.getObjectHealthBarScreenPosition = HostAPI_GetObjectHealthBarScreenPosition;
	api.teleportViewportTo = HostAPI_TeleportViewportTo;
	api.drawRectClock2D = HostAPI_DrawRectClock2D;
	api.drawRemainingRectClock2D = HostAPI_DrawRemainingRectClock2D;
	api.getD3DDevice8 = HostAPI_GetD3DDevice8;
	api.getGameWindow = HostAPI_GetGameWindow;
	api.enumeratePlayerObjects = HostAPI_EnumeratePlayerObjects;
	api.getContainedObjects = HostAPI_GetContainedObjects;
	api.getObjectTargetPosition = HostAPI_GetObjectTargetPosition;
	api.isObjectAirborne = HostAPI_IsObjectAirborne;
	api.isObjectVehicle = HostAPI_IsObjectVehicle;
	api.getPlayerName = HostAPI_GetPlayerName;
	api.getPlayerFactionTemplate = HostAPI_GetPlayerFactionTemplate;
	api.getPlayerMoney = HostAPI_GetPlayerMoney;
	api.getPlayerRank = HostAPI_GetPlayerRank;
	api.getPlayerPowerState = HostAPI_GetPlayerPowerState;
	api.getPlayerBuilderCount = HostAPI_GetPlayerBuilderCount;
	api.getPlayerActiveGathererCount = HostAPI_GetPlayerActiveGathererCount;
	api.getPlayerTotalMoneyEarned = HostAPI_GetPlayerTotalMoneyEarned;
	api.getTemplateDisplayName = HostAPI_GetTemplateDisplayName;
	api.getPlayerBuilderTemplateName = HostAPI_GetPlayerBuilderTemplateName;
	api.getPlayerBuilderTemplateCounts = HostAPI_GetPlayerBuilderTemplateCounts;
	api.getPlayerGathererTemplateName = HostAPI_GetPlayerGathererTemplateName;
	api.getPlayerTeamNumber = HostAPI_GetPlayerTeamNumber;
	api.getMatchPlayers = HostAPI_GetMatchPlayers;
	api.getPlayerIsDefeated = HostAPI_GetPlayerIsDefeated;
	api.getUserDataPath = HostAPI_GetUserDataPath;
	api.drawLine2D = HostAPI_DrawLine2D;
	api.isObjectProjectile = HostAPI_IsObjectProjectile;
	api.getObjectContainerId = HostAPI_GetObjectContainerId;
	return api;
}

// The one table handed to every plugin. A function-local static because plugins retain the pointer
// indefinitely, so it must not be a temporary; every entry is a free function, so one instance is
// correct for all of them.
const GOPluginHostAPI& GOPluginManager::GetHostAPI()
{
	static const GOPluginHostAPI s_hostAPI = BuildHostAPI();
	return s_hostAPI;
}

void GOPluginManager::RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
{
	if (cb != nullptr)
		s_gameplayEventHooks.push_back(*cb);
}

void GOPluginManager::RegisterRenderHooks(const GORenderCallbacks* cb)
{
	if (cb != nullptr)
		s_renderHooks.push_back(*cb);
}

// Scans for a single flat "key": "value" string field rather than
// pulling the full json.hpp parser into this file for two optional log-line fields. Not a general
// JSON parser - no nested objects/arrays, only \" and \\ are unescaped - but the manifest below is
// purely informational, so anything this doesn't understand just yields an empty value.
static std::string ExtractJsonStringField(const std::string& json, const char* key)
{
	const std::string quotedKey = std::string("\"") + key + "\"";
	size_t pos = json.find(quotedKey);
	if (pos == std::string::npos)
		return std::string();

	pos = json.find(':', pos + quotedKey.size());
	if (pos == std::string::npos)
		return std::string();
	++pos;

	while (pos < json.size() && isspace((unsigned char)json[pos]))
		++pos;
	if (pos >= json.size() || json[pos] != '"')
		return std::string();
	++pos;

	std::string value;
	while (pos < json.size() && json[pos] != '"')
	{
		if (json[pos] == '\\' && pos + 1 < json.size())
			++pos;
		value += json[pos];
		++pos;
	}
	return value;
}

// Optional sidecar manifest (foo.goplugin.dll -> foo.goplugin.json) with author info for the load
// log. Purely informational - GOPluginInfo stays authoritative, so a missing or malformed manifest
// never refuses a plugin. Returns a log fragment, or "".
static std::string ReadManifestSummary(const char* dllPath)
{
	std::string path(dllPath);
	const size_t dot = path.rfind(".dll");
	if (dot == std::string::npos)
		return std::string();
	path.replace(dot, 4, ".json");

	FILE* f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return std::string();

	std::string text;
	char buffer[512];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
		text.append(buffer, got);
	fclose(f);

	const std::string author = ExtractJsonStringField(text, "plugin_author");
	const std::string website = ExtractJsonStringField(text, "website");
	if (author.empty() && website.empty())
		return std::string();

	std::string summary = " [by " + (author.empty() ? std::string("unknown") : author);
	if (!website.empty())
		summary += ", " + website;
	return summary + "]";
}

bool GOPluginManager::LoadPlugin(const char* dllPath)
{
	if (dllPath == nullptr)
		return false;

	HMODULE hModule = LoadLibraryA(dllPath);
	if (hModule == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Failed to load %s (err=%u)", dllPath, GetLastError());
		return false;
	}

	GOPluginGetInfoFunc fnGetInfo = (GOPluginGetInfoFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_GETINFO_NAME);
	GOPluginInitializeFunc fnInitialize = (GOPluginInitializeFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_INITIALIZE_NAME);
	GOPluginShutdownFunc fnShutdown = (GOPluginShutdownFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_SHUTDOWN_NAME);
	GOPluginTickFunc fnTick = (GOPluginTickFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_TICK_NAME); // optional

	if (fnGetInfo == nullptr || fnInitialize == nullptr || fnShutdown == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s is missing required exports (GetInfo/Initialize/Shutdown)", dllPath);
		FreeLibrary(hModule);
		return false;
	}

	GOPluginInfo info = {};
	fnGetInfo(&info);

	if (info.abiVersion != GO_PLUGIN_ABI_VERSION)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s ABI version mismatch (plugin=%u, host=%u)",
			dllPath, info.abiVersion, (uint32_t)GO_PLUGIN_ABI_VERSION);
		FreeLibrary(hModule);
		return false;
	}

	LoadedPlugin loaded;
	loaded.module = hModule;
	loaded.path = dllPath;
	loaded.name = (info.name != nullptr) ? info.name : "(unnamed)";
	loaded.version = (info.version != nullptr) ? info.version : "(unknown)";
	loaded.shutdown = fnShutdown;
	loaded.tick = fnTick;
	loaded.info = info;

	// A plugin may register hooks during Initialize() and then still return false. Snapshot both
	// vector sizes and roll back on failure, or those callbacks keep pointing into the DLL we are
	// about to FreeLibrary() and the next dispatch calls through freed memory.
	const size_t gameplayHooksBefore = s_gameplayEventHooks.size();
	const size_t renderHooksBefore = s_renderHooks.size();
	if (!fnInitialize(&GetHostAPI()))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s Initialize() returned failure", dllPath);
		s_gameplayEventHooks.resize(gameplayHooksBefore);
		s_renderHooks.resize(renderHooksBefore);
		FreeLibrary(hModule);
		return false;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Loaded %s v%s (%s) hooks=0x%X%s",
		loaded.name.c_str(), loaded.version.c_str(), dllPath, info.hookCategories,
		ReadManifestSummary(dllPath).c_str());

	s_plugins.push_back(loaded);
	return true;
}

// Loads every *.goplugin.dll directly inside pluginDir. Returns how many loaded.
static int LoadPluginsInFolder(const std::string& pluginDir)
{
	const std::string searchPattern = pluginDir + "\\*.goplugin.dll";

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;

	int loaded = 0;
	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		const std::string fullPath = pluginDir + "\\" + findData.cFileName;
		if (GOPluginManager::LoadPlugin(fullPath.c_str()))
			++loaded;
	}
	while (FindNextFileA(hFind, &findData) != 0);

	FindClose(hFind);
	return loaded;
}

void GOPluginManager::LoadPluginsFromDirectory(const char* directoryPath)
{
	if (directoryPath == nullptr)
		return;

	const std::string root(directoryPath);

	// One folder per plugin, holding its DLL plus an optional .json manifest, so each plugin's
	// files (DLL, manifest, any of its own data) stay together and the plugins directory does not
	// become a pile of loose DLLs.
	int loaded = 0;
	int folders = 0;

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA((root + "\\*").c_str(), &findData);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				continue;
			if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0)
				continue;

			++folders;
			loaded += LoadPluginsInFolder(root + "\\" + findData.cFileName);
		}
		while (FindNextFileA(hFind, &findData) != 0);

		FindClose(hFind);
	}

	// A DLL dropped loose in plugins\ is not picked up. Say so loudly rather than silently doing
	// nothing: a plugin that never runs looks exactly like a plugin that runs and draws nothing,
	// and telling those apart cost a full round of testing once already.
	WIN32_FIND_DATAA strayData;
	HANDLE hStray = FindFirstFileA((root + "\\*.goplugin.dll").c_str(), &strayData);
	if (hStray != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strayData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[Plugin] IGNORED %s\\%s - plugins live in their own folder. Move it to %s\\<name>\\%s",
				root.c_str(), strayData.cFileName, root.c_str(), strayData.cFileName);
		}
		while (FindNextFileA(hStray, &strayData) != 0);

		FindClose(hStray);
	}

	if (loaded == 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] No plugins loaded (%s, %d folder(s) scanned)",
			root.c_str(), folders);
	}
}

void GOPluginManager::UnloadAll()
{
	for (auto it = s_plugins.rbegin(); it != s_plugins.rend(); ++it)
	{
		if (it->shutdown != nullptr)
			it->shutdown();
		if (it->module != nullptr)
			FreeLibrary(it->module);
	}

	s_plugins.clear();
	s_gameplayEventHooks.clear();
	s_renderHooks.clear();
}

// One gate for the whole framework: callbacks carry other players' queues, powers and buildings, so
// none of it may reach a plugin while the local client is a participant. Same condition the engine's
// own observer-only UI uses; replay playback passes, its local player being the observer.
bool GOPluginManager::IsLocalPlayerObserver()
{
	if (ThePlayerList == nullptr)
		return false;
	Player* localPlayer = ThePlayerList->getLocalPlayer();
	return (localPlayer != nullptr && (localPlayer->isPlayerObserver() || localPlayer->isPlayerDead())) ? true : false;
}

void GOPluginManager::Tick()
{
	if (!IsLocalPlayerObserver())
		return;

	for (LoadedPlugin& plugin : s_plugins)
	{
		if (plugin.tick != nullptr)
			plugin.tick();
	}
}

// ---- IGameplayEventHooks dispatch. All no-ops unless IsLocalPlayerObserver() - a match
// participant never receives plugin callbacks. ----
void GOPluginManager::DispatchUnitQueued(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitQueued != nullptr) cb.onUnitQueued(&ev);
}

void GOPluginManager::DispatchUnitCancelled(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCancelled != nullptr) cb.onUnitCancelled(&ev);
}

void GOPluginManager::DispatchUnitCompleted(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCompleted != nullptr) cb.onUnitCompleted(&ev);
}

void GOPluginManager::DispatchUpgradeQueued(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeQueued != nullptr) cb.onUpgradeQueued(&ev);
}

void GOPluginManager::DispatchUpgradeCancelled(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCancelled != nullptr) cb.onUpgradeCancelled(&ev);
}

void GOPluginManager::DispatchUpgradeCompleted(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCompleted != nullptr) cb.onUpgradeCompleted(&ev);
}

void GOPluginManager::DispatchBuildingDestroyed(const GOBuildingEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onBuildingDestroyed != nullptr) cb.onBuildingDestroyed(&ev);
}

void GOPluginManager::DispatchSpecialPowerTriggered(const GOSpecialPowerEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onSpecialPowerTriggered != nullptr) cb.onSpecialPowerTriggered(&ev);
}

void GOPluginManager::DispatchObjectDamaged(const GOCombatEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onObjectDamaged != nullptr) cb.onObjectDamaged(&ev);
}

void GOPluginManager::DispatchObjectHealed(const GOCombatEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onObjectHealed != nullptr) cb.onObjectHealed(&ev);
}

// ---- IRenderHooks dispatch. Same observer gate as the gameplay events. ----

void GOPluginManager::DispatchDrawOverlay()
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onDrawOverlay != nullptr) cb.onDrawOverlay();
}

void GOPluginManager::DispatchRawKeyUp(uint32_t scanCode, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onRawKeyUp != nullptr) cb.onRawKeyUp(scanCode, modifierFlags);
}

void GOPluginManager::DispatchMouseMove(int32_t x, int32_t y)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseMove != nullptr) cb.onMouseMove(x, y);
}

void GOPluginManager::DispatchMouseButtonDown(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonDown != nullptr) cb.onMouseButtonDown(buttonIndex, x, y, modifierFlags);
}

void GOPluginManager::DispatchMouseButtonUp(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonUp != nullptr) cb.onMouseButtonUp(buttonIndex, x, y, modifierFlags);
}
