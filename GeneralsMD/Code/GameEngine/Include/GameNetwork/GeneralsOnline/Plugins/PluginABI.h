#pragma once

// The contract between the game client (host) and any plugin DLL. Must stay pure C - POD structs,
// function pointers and primitives only, no STL or engine types - so a plugin compiles against it
// without engine headers and both sides always agree on layout. The host loads any number of
// plugins; each opts into hook categories at GO_Plugin_Initialize via the table it is handed.

#include <stdint.h>

#ifdef _WIN32
#define GO_PLUGIN_API extern "C" __declspec(dllexport)
#define GO_PLUGIN_IMPORT extern "C" __declspec(dllimport)
#else
#define GO_PLUGIN_API extern "C"
#define GO_PLUGIN_IMPORT extern "C"
#endif

// Bump when the layout of any struct below changes. GOPluginInfo::abiVersion and
// GOPluginHostAPI::abiVersion are checked against this on load; a mismatch fails the plugin load
// rather than risk silently misreading a function-pointer table with a different layout.
#define GO_PLUGIN_ABI_VERSION 1

// Compile-time layout check, spelled without static_assert/_Static_assert so this header still
// compiles as C89 as well as C++. Both sides fail to build the moment a struct below lays out
// differently from what this contract says, which is the one failure GO_PLUGIN_ABI_VERSION cannot
// catch. The expected sizes are the 32-bit (x86) ones - host and plugin are both Win32 builds.
#define GO_ABI_ASSERT_CONCAT_(a, b) a##b
#define GO_ABI_ASSERT_CONCAT(a, b) GO_ABI_ASSERT_CONCAT_(a, b)
#define GO_ABI_ASSERT_LAYOUT(cond) typedef char GO_ABI_ASSERT_CONCAT(goAbiLayoutAssert_, __LINE__)[(cond) ? 1 : -1]

// Hook categories. GOPluginInfo::hookCategories is informational only; actual use requires the
// matching register* call during GO_Plugin_Initialize. Only the categories plugins consume are
// exposed, keeping the surface the engine must guarantee small. Carried as uint32_t, never as the
// enum type - the enum has no base-type specifier.
enum EGOPluginHookCategory
{
	GO_HOOK_NONE                 = 0,
	GO_HOOK_GAMEPLAY_EVENTS      = 1 << 0, // unit/upgrade/power/building gameplay events
	GO_HOOK_RENDER               = 1 << 1, // per-frame overlay draw + raw hotkey passthrough
};

// Production/power/building events. Payloads are plain data rather than engine pointers, since
// they cross a DLL boundary and must not depend on the host's C++ class layout.
struct GOUnitEvent
{
	uint32_t playerIndex;
	const char* templateName;     // ThingTemplate name, e.g. "AmericaVehicleCrusader"
	uint32_t producerObjectId;    // producing building's ObjectID (0 if unavailable)
	float percentComplete;        // 0..100, valid for queued/completed; ignored for cancelled
	int32_t productionID;         // engine ProductionID, stable per queue entry until completion/cancel
	float producerPositionX;      // producing building's world position at the moment of this event
	float producerPositionY;
	float producerPositionZ;
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOUnitEvent) == 32);

struct GOUpgradeEvent
{
	uint32_t playerIndex;
	const char* templateName;     // UpgradeTemplate name
	uint32_t producerObjectId;
	float percentComplete;
	float producerPositionX;
	float producerPositionY;
	float producerPositionZ;
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOUpgradeEvent) == 28);

struct GOBuildingEvent
{
	uint32_t objectId;             // the destroyed production structure's ObjectID
	uint32_t playerIndex;
	float positionX;               // world position at the moment of destruction
	float positionY;
	float positionZ;
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOBuildingEvent) == 20);

struct GOSpecialPowerEvent
{
	uint32_t playerIndex;
	const char* powerTemplateName;
	float locationX;
	float locationY;
	float locationZ;
	float rechargeTimeSeconds;     // static reload duration for this power template (0 if unknown)
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOSpecialPowerEvent) == 24);

// Shared payload for onObjectDamaged and onObjectHealed; which callback fired implies the
// direction. See plans\plugin-framework\design-notes.md for where it is raised and why.
// All four-byte members come first so the three flags pack contiguously at the end.
struct GOCombatEvent
{
	uint32_t objectId;         // the object whose health changed
	uint32_t sourceObjectId;   // attacker/healer ObjectID (0 if unavailable, e.g. environmental damage)
	uint32_t playerIndex;      // owner of objectId
	int32_t amount;            // always positive; onObjectDamaged vs onObjectHealed implies the sign
	float positionX;           // objectId's world position at the moment of this event
	float positionY;
	float positionZ;
	uint8_t isBuilding;        // KINDOF_STRUCTURE at the moment of this event
	uint8_t isUnit;            // KINDOF_INFANTRY || KINDOF_VEHICLE - deliberately not "!isBuilding"
	uint8_t isFlame;           // DAMAGE_FLAME, a continuous per-frame tick source (onObjectDamaged only)
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOCombatEvent) == 32);

struct GOGameplayEventCallbacks
{
	void (*onUnitQueued)(const GOUnitEvent* ev);
	void (*onUnitCancelled)(const GOUnitEvent* ev);
	void (*onUnitCompleted)(const GOUnitEvent* ev);
	void (*onUpgradeQueued)(const GOUpgradeEvent* ev);
	void (*onUpgradeCancelled)(const GOUpgradeEvent* ev);
	void (*onUpgradeCompleted)(const GOUpgradeEvent* ev);
	// Production structures only - the hook lives in ProductionUpdate, so a destroyed power plant
	// or defensive structure does not raise it. Enough to retire a building's queue entries.
	void (*onBuildingDestroyed)(const GOBuildingEvent* ev);
	void (*onSpecialPowerTriggered)(const GOSpecialPowerEvent* ev);
	void (*onObjectDamaged)(const GOCombatEvent* ev);
	void (*onObjectHealed)(const GOCombatEvent* ev);
};

// One entry in a player's general-power roster (see getPlayerGeneralPowers). templateName points
// into engine-owned static storage and is valid for the duration of the call.
struct GOGeneralPowerInfo
{
	const char* templateName;    // SpecialPowerTemplate name, e.g. "AmericaSuperweaponParticleCannon"
	uint32_t rechargeFrames;     // full reload duration in logic frames (0 = recharges instantly)
	uint32_t framesUntilReady;   // logic frames until it can be triggered again (0 = ready now)
	uint32_t buildingObjectId;   // ObjectID of the building carrying this power's module (0 if none)
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOGeneralPowerInfo) == 16);

struct GOContainedObjectInfo
{
	uint32_t objectId;
	const char* templateName;
	uint32_t playerIndex;
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOContainedObjectInfo) == 12);

// Per-frame overlay draw plus raw input passthrough. onDrawOverlay runs once a frame from
// InGameUI's draw path, in 2D screen space with the HUD's ortho projection already active, so a
// plugin can issue the host's 2D primitives without touching 3D state.
struct GORenderCallbacks
{
	void (*onDrawOverlay)();
	// scanCode is a DirectInput scan code (DIK_*, e.g. DIK_F9 == 0x43), which is what the engine
	// itself keys off - NOT a Windows virtual-key code. Comparing against VK_* silently never
	// matches. modifierFlags: bit0 = CTRL, bit1 = SHIFT, bit2 = ALT.
	void (*onRawKeyUp)(uint32_t scanCode, uint32_t modifierFlags);

	// Side-channel notification like onRawKeyUp: never gates or consumes the input. Fires more often
	// than any other hook here, so keep handlers cheap. Screen-space pixels, as drawRect2D.
	void (*onMouseMove)(int32_t x, int32_t y);
	// Mouse-button passthrough for clickable plugin UI. Same side-channel
	// discipline as onMouseMove. buttonIndex: 0 = left, 1 = middle, 2 = right. modifierFlags: bit0 =
	// CTRL, bit1 = SHIFT, bit2 = ALT. The mouse-button cases exist so a plugin can act on clicks on
	// its own drawn UI (e.g. jump the viewport to what was clicked).
	void (*onMouseButtonDown)(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);
	void (*onMouseButtonUp)(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags);
};

// Production-building purposes a plugin might treat differently. Deliberately closed: the five
// there was a need to distinguish, not a general taxonomy. Returned as uint8_t, never as the enum.
enum EGOBuildingCategory
{
	GO_BUILDING_CATEGORY_NONE = 0,          // not a building, or a building outside the categories below
	GO_BUILDING_CATEGORY_COMMAND_CENTER = 1,
	GO_BUILDING_CATEGORY_WAR_FACTORY = 2,
	GO_BUILDING_CATEGORY_BARRACKS = 3,
	GO_BUILDING_CATEGORY_AIRFIELD = 4,
	GO_BUILDING_CATEGORY_SUPPLY_STASH = 5,
};

// Handed to the plugin at GO_Plugin_Initialize. Registration functions are independent and
// optional, and may each be called more than once. LIFETIME: the pointer has process lifetime, so a
// plugin may retain it - but copying the struct by value costs nothing and does not rely on that.
struct GOPluginHostAPI
{
	uint32_t abiVersion;

	// sizeof(GOPluginHostAPI) as the host compiled it, at a fixed offset so a plugin can read it
	// before touching anything further down the table. A plugin must refuse to initialize when this
	// is smaller than its own sizeof - see plans\plugin-framework\design-notes.md.
	uint32_t structSize;

	void (*log)(const char* msg);

	void (*registerGameplayEventHooks)(const GOGameplayEventCallbacks* cb);
	void (*registerRenderHooks)(const GORenderCallbacks* cb);

	// --- Player roster queries. Pure queries, no side effects. playerIndex matches the
	// GOUnitEvent/GOUpgradeEvent/GOBuildingEvent/GOSpecialPowerEvent field of the same name. Return
	// 0 / empty string / false if playerIndex doesn't currently resolve to an active player (no
	// match currently loaded, index out of range, observer slot, etc). ---
	uint32_t (*getPlayerColor)(uint32_t playerIndex);      // colorARGB, same packing as drawText2D/drawRect2D

	// Active, non-observer participants; returns how many were written, never more than maxCount.
	// Excludes the neutral/civilian player, and the values are the same playerIndex the event
	// structs carry. Returns 0 when no match is loaded.
	uint32_t (*getActivePlayers)(uint32_t* outPlayerIndices, uint32_t maxCount);

	// TRUE if the local client is spectating rather than playing (dead counts as spectating; replay
	// playback is always true). A plugin showing other players' information MUST gate on this: not
	// doing so exposes it to a live participant, which is a cheat vector, not a display bug. The host
	// also enforces it - while false, no callbacks are delivered and GO_Plugin_Tick is not called.
	uint8_t (*isLocalPlayerObserver)();

	// General powers the player owns: required science plus a live object carrying the module, so a
	// superweapon only counts once its building exists. The event hooks fire only on use, so this is
	// the only way to show owned-but-unused powers. Cooldowns come from the module's own clock and
	// stay correct across pause and fast-forward. Returns the number written, at most maxCount.
	uint32_t (*getPlayerGeneralPowers)(uint32_t playerIndex, GOGeneralPowerInfo* outPowers, uint32_t maxCount);

	// --- Icon drawing. Looks up the named template server-side (where the engine's ThingTemplate/
	// UpgradeTemplate/CommandButton data safely lives) and draws its button art; the plugin never
	// needs the Image/ThingTemplate/CommandButton types themselves. Does nothing if
	// the name doesn't resolve. ---

	// Draws a unit or player-upgrade's button icon. Tries a ThingTemplate lookup by templateName
	// first, then an UpgradeTemplate lookup if that fails - matches GOUnitEvent::templateName or
	// GOUpgradeEvent::templateName respectively, so you don't need to track which kind it was.
	void (*drawTemplateIcon2D)(const char* templateName, int32_t x, int32_t y, int32_t width, int32_t height);

	// Draws a special power's button icon for the given player (a power's icon is defined on the
	// CommandButton that exposes it, which is per-faction - hence needing playerIndex, unlike
	// drawTemplateIcon2D). Matches GOSpecialPowerEvent::powerTemplateName.
	void (*drawPowerIcon2D)(uint32_t playerIndex, const char* powerTemplateName, int32_t x, int32_t y, int32_t width, int32_t height);

	// Minimal 2D draw primitives for render-hook plugins, screen-space pixels, top-left origin.
	// Valid only from onDrawOverlay. Text is 7-bit ASCII: the host widens it byte by byte, so any
	// multi-byte sequence renders as one garbage character per byte.
	void (*drawText2D)(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB);
	void (*drawRect2D)(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colorARGB, uint8_t filled);

	// Like drawText2D, but sizeScale multiplies the host's own message font point size (1.0 matches
	// drawText2D exactly, values outside the host's supported range are clamped) and bold is
	// explicit. See plans\plugin-framework\design-notes.md for why this is a separate function.
	void (*drawText2DScaled)(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB, float sizeScale, uint8_t bold);

	// Current render target size in pixels, same coordinate space as drawText2D/drawRect2D/
	// drawTemplateIcon2D/drawPowerIcon2D/onMouseMove. Lets a render-hook plugin anchor drawn UI to
	// a screen edge (bottom/right) instead of only a fixed top-left-relative offset. Safe to call
	// from onDrawOverlay every frame.
	void (*getScreenSize)(int32_t* outWidth, int32_t* outHeight);

	// Simulation clock, for ageing anything recorded from the events - wall-clock time drifts against
	// a paused, delayed or fast-forwarded simulation. Return 0 when no game is running.
	uint32_t (*getLogicFrame)();
	uint32_t (*getLogicFramesPerSecond)();

	// Live production progress, read from the engine's ProductionEntry on demand. The event structs
	// carry percentComplete only as of the moment they fire - essentially always 0 for a queued
	// event - and nothing reports progress changing. Return 0..100, or -1 if the entry is gone.
	float (*getUnitProductionProgress)(uint32_t producerObjectId, int32_t productionID);
	float (*getUpgradeProductionProgress)(uint32_t producerObjectId, const char* upgradeTemplateName);

	// --- World-space anchoring. GOUnitEvent/GOUpgradeEvent/GOBuildingEvent carry world
	// positions, but there was no way to turn one into a screen position, so world-anchored UI was
	// unreachable from a plugin. worldToScreen returns 0 (and leaves the outputs untouched) when
	// the point is outside the view frustum. ---
	uint8_t (*worldToScreen)(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY);

	// Screen-space bounding box of a live object, derived from its world position and bounding
	// radius - for sizing/placing UI relative to the thing it belongs to, rather than guessing a
	// scale from the screen resolution. Returns 0 if the object no longer exists or is off screen.
	uint8_t (*getObjectScreenBounds)(uint32_t objectId, int32_t* outX, int32_t* outY, int32_t* outWidth, int32_t* outHeight);

	// Same point the engine projects its own health bar onto (Object::getHealthBoxPosition:
	// top-of-model + 10 world units + the object's healthBoxOffset), projected via worldToScreen.
	// Returns 0 if the object no longer exists or the point is off screen.
	uint8_t (*getObjectHealthBarScreenPosition)(uint32_t objectId, int32_t* outX, int32_t* outY);

	// Moves the observer's viewport to a world position, by the same camera path the engine's own
	// observer look-at actions use - for jumping to where a gameplay event happened.
	void (*teleportViewportTo)(float worldX, float worldY, float worldZ);

	// The engine's own radial "pie" fill, as the command bar uses for build progress and recharge.
	// percent is 0..100; drawRectClock2D fills the elapsed wedge, the other the remaining one.
	void (*drawRectClock2D)(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB);
	void (*drawRemainingRectClock2D)(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB);

	// D3D8 handles for a plugin with its own UI backend, opaque so no engine types cross the
	// boundary. The device is the engine's own live IDirect3DDevice8, not a copy: a plugin MUST save
	// and restore every state it touches. The engine assumes its state survives and cannot recover
	// from a dirty device.
	void* (*getD3DDevice8)();
	void* (*getGameWindow)();
	uint32_t (*enumeratePlayerObjects)(uint32_t playerIndex,
		void (*callback)(uint32_t objectId, float posX, float posY, float posZ, void* userData),
		void* userData);
	uint32_t (*getContainedObjects)(uint32_t containerObjectId,
		GOContainedObjectInfo* outObjects, uint32_t maxCount);

	// Target tracking: an object's current attack/move target position (current victim's position
	// when attacking an object, otherwise the AI state machine's goal position). Returns 0 when the
	// object is idle or has no target. isObjectAirborne reports Object::isAirborneTarget.
	uint8_t (*getObjectTargetPosition)(uint32_t objectId, float* outTargetX, float* outTargetY, float* outTargetZ);
	uint8_t (*isObjectAirborne)(uint32_t objectId);

	// 1 for a KINDOF_VEHICLE. Identifies an upgrade queued on an already-built vehicle, which
	// getObjectBuildingCategory cannot - a vehicle always reports GO_BUILDING_CATEGORY_NONE.
	uint8_t (*isObjectVehicle)(uint32_t objectId);

	// Like worldToScreen, but clamps an off-viewport point to just inside the screen edge instead of
	// dropping it; returns 0 only when no projection is possible at all. For anything that should
	// survive as an edge indicator when its world anchor pans off-screen.
	uint8_t (*worldToScreenClamped)(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY);

	// Which of the five categories in EGOBuildingCategory a producing building belongs to (or
	// GO_BUILDING_CATEGORY_NONE if it isn't a building, or isn't one of the five). For per-category
	// UI treatment - e.g. a different queue-panel offset for airfields vs war factories.
	uint8_t (*getObjectBuildingCategory)(uint32_t objectId);

	// Player card queries. Pure, same playerIndex semantics as getPlayerColor, returning 0 / empty
	// string / -1 when it does not resolve. Each string-returning function owns one buffer, so only
	// its own most recent return value is live - copy it out immediately. Not thread-safe; the whole
	// ABI runs on the engine's single thread.

	// Player display name. Returns empty string if the player slot is not active.
	const char* (*getPlayerName)(uint32_t playerIndex);

	// Faction template name (e.g. "America", "China", "GLA", or sub-faction variants like
	// "AmericaAirForce"). Returns empty string on failure.
	const char* (*getPlayerFactionTemplate)(uint32_t playerIndex);

	// Current credits (money). Returns 0 if unavailable.
	uint32_t (*getPlayerMoney)(uint32_t playerIndex);

	// General rank (0-based: 0 == the player's first rank) and skill-point progress toward the
	// next rank. outCurrentXP is the player's cumulative skill points; outNextXP is the total
	// skill points required for the next rank (0 at max rank - there is no "next"). Returns -1 if
	// playerIndex doesn't resolve or the player has no rank yet (rank 0/uninitialized).
	int32_t (*getPlayerRank)(uint32_t playerIndex, uint32_t* outCurrentXP, uint32_t* outNextXP);

	// Power state: outPowerGenerated / outPowerDrain, in the engine's own energy units. Returns 1
	// if power data is available, 0 otherwise (outputs left untouched).
	uint8_t (*getPlayerPowerState)(uint32_t playerIndex, uint32_t* outPowerGenerated, uint32_t* outPowerDrain);

	// Count of the player's live construction units - USA/China Dozer, GLA Worker - identified by
	// the shared DozerAIInterface (AIUpdateInterface::getDozerAIInterface() != nullptr) rather than
	// a per-faction template/KindOf list, so it stays correct across all three factions uniformly.
	// Returns 0 if playerIndex doesn't resolve.
	uint32_t (*getPlayerBuilderCount)(uint32_t playerIndex);

	// Supply units actually ferrying right now, not merely idle or empty-moving - backed by
	// SupplyTruckAIInterface::isCurrentlyFerryingSupplies().
	uint32_t (*getPlayerActiveGathererCount)(uint32_t playerIndex);

	// Cumulative gross money earned this match, not net of spending - the score screen's own figure.
	// Deliberately the raw counter rather than a rate, so a plugin picks its own averaging window.
	uint32_t (*getPlayerTotalMoneyEarned)(uint32_t playerIndex);

	// Display name for a template ("AmericaInfantryRifle" -> "Ranger"), via the same lookup the
	// game's own tooltips use. Empty string if it does not resolve.
	const char* (*getTemplateDisplayName)(const char* templateName);

	// A sample template name from the same qualifying checks as getPlayerBuilderCount /
	// getPlayerActiveGathererCount, so a plugin can draw the player's actual builder icon rather
	// than guess a per-faction template. Empty string if no qualifying unit exists.
	const char* (*getPlayerBuilderTemplateName)(uint32_t playerIndex);

	// Per-type builder breakdown, which the summed count and the single sample name above cannot
	// give - neither tells a native Dozer from a captured GLA Worker. Writes up to maxCount parallel
	// entries and returns how many distinct templates were found. outNames is call-lifetime only.
	uint32_t (*getPlayerBuilderTemplateCounts)(uint32_t playerIndex, const char** outNames, uint32_t* outCounts, uint32_t maxCount);

	// The gatherer counterpart of getPlayerBuilderTemplateName above, sampling one of the player's
	// currently supply-ferrying units instead. Same return and storage discipline.
	const char* (*getPlayerGathererTemplateName)(uint32_t playerIndex);

	// Lobby-configured alliance/team number (GameSlot::getTeamNumber - the same value the skirmish/
	// lobby "Team" dropdown sets, and what the game's own alliance/enemy checks are based on), NOT a
	// display-position or player-count index. Returns -1 if playerIndex doesn't resolve or the
	// player has no team assigned (free-for-all / no alliance).
	int32_t (*getPlayerTeamNumber)(uint32_t playerIndex);

	// Every real participant including defeated and resigned ones: filters on isPlayerObserver() but
	// not isPlayerActive(), so the roster does not shrink or reindex when someone dies. Use this
	// rather than getActivePlayers for roster tracking.
	uint32_t (*getMatchPlayers)(uint32_t* outPlayerIndices, uint32_t maxCount);

	// 1 if the player is no longer playing (dead or resigned - !Player::isPlayerActive()), 0 if
	// they're still an active match participant or playerIndex doesn't resolve. Covers both causes
	// of "no longer playing" with one boolean rather than requiring a plugin to distinguish them.
	uint8_t (*getPlayerIsDefeated)(uint32_t playerIndex);

	// The engine's per-user data directory, trailing backslash included - where options.ini, Replays
	// and Maps live, and so where a plugin's user-owned files belong. The plugin's own folder is
	// under Program Files and is not writable. Process lifetime, never null; empty means unresolved.
	const char* (*getUserDataPath)();

	// One straight line as a single rotated quad (Render2DClass::Add_Line), same coordinate space and
	// colour packing as drawRect2D. drawRect2D is axis-aligned, so faking a diagonal through it costs
	// a quad every few pixels; this costs one whatever the length.
	void (*drawLine2D)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, float thickness, uint32_t colorARGB);

	// 1 for a KINDOF_PROJECTILE. enumeratePlayerObjects walks everything a player owns, and a missile
	// or bomb in flight owns a target like any attacker does, so target-tracking overlays draw a line
	// for every shot unless they filter these out.
	uint8_t (*isObjectProjectile)(uint32_t objectId);

	// The id of the object this one is inside (Object::getContainedBy), or 0 when it is not contained.
	// enumeratePlayerObjects returns garrisoned occupants alongside everything else, and each of them
	// carries its own target from the same world position, so a full building emits one identical line
	// per occupant. Collapse on this id to draw the container once.
	uint32_t (*getObjectContainerId)(uint32_t objectId);
};

struct GOPluginInfo
{
	uint32_t abiVersion;
	const char* name;
	const char* version;
	uint32_t hookCategories; // bitmask of EGOPluginHookCategory, informational only
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOPluginInfo) == 16);

// ------------------------------------------------------------------------------------------------
// Fixed export names every plugin DLL must implement, resolved via GetProcAddress so the DLL
// interface is plain C - no link-time coupling between host and plugin.
// ------------------------------------------------------------------------------------------------
typedef void (*GOPluginGetInfoFunc)(GOPluginInfo* outInfo);
typedef bool (*GOPluginInitializeFunc)(const GOPluginHostAPI* hostAPI);
typedef void (*GOPluginShutdownFunc)();
typedef void (*GOPluginTickFunc)(); // optional; may be null

#define GO_PLUGIN_EXPORT_GETINFO_NAME "GO_Plugin_GetInfo"
#define GO_PLUGIN_EXPORT_INITIALIZE_NAME "GO_Plugin_Initialize"
#define GO_PLUGIN_EXPORT_SHUTDOWN_NAME "GO_Plugin_Shutdown"
#define GO_PLUGIN_EXPORT_TICK_NAME "GO_Plugin_Tick"
