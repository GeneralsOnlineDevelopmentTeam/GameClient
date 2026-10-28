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
#include "PreRTS.h"

#include "Common/FramePacer.h"

#include "GameClient/View.h"

#include "Common/GameEngine.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"

#if defined(GENERALS_ONLINE)
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#endif


FramePacer* TheFramePacer = nullptr;

FramePacer::FramePacer()
{
	// Set the time slice size to 1 ms.
	timeBeginPeriod(1);

	m_maxFPS = BaseFps;
	m_logicTimeScaleFPS = LOGICFRAMES_PER_SECOND;
	m_updateTime = 1.0f / (Real)BaseFps; // initialized to something to avoid division by zero on first use
	m_enableFpsLimit = FALSE;
	m_enableLogicTimeScale = FALSE;
	m_isTimeFrozen = FALSE;
	m_isGameHalted = FALSE;
#if defined(GENERALS_ONLINE)
	m_windowRefreshRate = 60;
	m_windowRefreshTime = 0;
	m_windowRefreshDirty = TRUE;
#endif
}

FramePacer::~FramePacer()
{
	// Restore the previous time slice for Windows.
	timeEndPeriod(1);
}

void FramePacer::update()
{
	// TheSuperHackers @bugfix xezon 05/08/2025 Re-implements the frame rate limiter
	// with higher resolution counters to cap the frame rate more accurately to the desired limit.
	const UnsignedInt maxFps = getActualFramesPerSecondLimit();// allowFpsLimit ? getFramesPerSecondLimit() : RenderFpsPreset::UncappedFpsValue;
	m_updateTime = m_frameRateLimit.wait(maxFps);
}

void FramePacer::reset()
{
	m_frameRateLimit.reset();
	m_updateTime = 1.0f / (Real)getActualFramesPerSecondLimit();
}

void FramePacer::setFramesPerSecondLimit( Int fps )
{
	DEBUG_LOG(("FramePacer::setFramesPerSecondLimit() - setting max fps to %d (TheGlobalData->m_useFpsLimit == %d)", fps, TheGlobalData->m_useFpsLimit));

	// clamp FPS to at least render at same as logic
#if defined(GENERALS_ONLINE)
	fps = std::max<int>(fps, GENERALS_ONLINE_HIGH_FPS_LIMIT);
#endif

	m_maxFPS = fps;
}

Int FramePacer::getFramesPerSecondLimit()  const
{
	return m_maxFPS;
}

void FramePacer::enableFramesPerSecondLimit( Bool enable )
{
	m_enableFpsLimit = enable;
}

Bool FramePacer::isFramesPerSecondLimitEnabled() const
{
	return m_enableFpsLimit;
}

Bool FramePacer::isActualFramesPerSecondLimitEnabled() const
{
	Bool allowFpsLimit = true;

	if (TheTacticalView != nullptr)
	{
		allowFpsLimit &= TheTacticalView->getTimeMultiplier()<=1 && !TheScriptEngine->isTimeFast();
	}

	if (TheGameLogic != nullptr)
	{
#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode);
#else	//always allow this cheat key if we're in a replay game.
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame());
#endif
	}

	allowFpsLimit &= TheGlobalData->m_useFpsLimit;
	allowFpsLimit &= isFramesPerSecondLimitEnabled();

	return allowFpsLimit;
}

Int FramePacer::getActualFramesPerSecondLimit() const
{
	const Int fps = isActualFramesPerSecondLimitEnabled() ? getFramesPerSecondLimit() : RenderFpsPreset::UncappedFpsValue;
#if defined(GENERALS_ONLINE)
	return getWindowCappedFramesPerSecondLimit(fps);
#else
	return fps;
#endif
}

#if defined(GENERALS_ONLINE)
void FramePacer::invalidateWindowRefreshRate()
{
	m_windowRefreshDirty = TRUE;
}

UnsignedInt FramePacer::getWindowRefreshRate() const
{
	// Re-query at most every 100 ms after a move or display change, and every second otherwise.
	const DWORD now = timeGetTime();
	const DWORD age = now - m_windowRefreshTime;
	if (m_windowRefreshTime == 0 || age >= 1000 || (m_windowRefreshDirty && age >= 100))
	{
		m_windowRefreshRate = DX8Wrapper::Get_Window_Monitor_Refresh_Rate();
		m_windowRefreshTime = now;
		m_windowRefreshDirty = FALSE;
	}
	return m_windowRefreshRate;
}

Int FramePacer::getWindowCappedFramesPerSecondLimit(Int fps) const
{
	// Presenting a windowed device faster than the monitor refreshes only floods the GPU and the desktop compositor.
	// Explicit limits stay as chosen, only the uncapped limit maps to the refresh rate. Exclusive fullscreen stays uncapped.
	if (fps >= (Int)RenderFpsPreset::UncappedFpsValue && WW3D::Is_Windowed())
	{
		fps = (Int)getWindowRefreshRate();
	}

	// Without focus nobody watches the match at a high frame rate.
	if (TheGameEngine != nullptr && !TheGameEngine->isActive())
	{
		fps = std::min<Int>(fps, GENERALS_ONLINE_HIGH_FPS_LIMIT);
	}

	// Logic is capped by the render frame rate, so never render slower than the logic time scale or the network rate.
	if (TheNetwork != nullptr || isLogicTimeScaleEnabled())
	{
		fps = std::max<Int>(fps, getActualLogicTimeScaleFps(IgnoreFrozenTime | IgnoreHaltedGame));
	}
	return fps;
}
#endif

Real FramePacer::getUpdateTime()  const
{
	return m_updateTime;
}

Real FramePacer::getUpdateFps()  const
{
	return 1.0f / m_updateTime;
}

Real FramePacer::getBaseOverUpdateFpsRatio(Real minUpdateFps)
{
	// Update fps is floored to default 5 fps, 200 ms.
	// Useful to prevent insane ratios on frame spikes/stalls.
	return (Real)BaseFps / std::max(getUpdateFps(), minUpdateFps);
}

void FramePacer::setTimeFrozen(Bool frozen)
{
	m_isTimeFrozen = frozen;
}

void FramePacer::setGameHalted(Bool halted)
{
	m_isGameHalted = halted;
}

Bool FramePacer::isTimeFrozen() const
{
	return m_isTimeFrozen;
}

Bool FramePacer::isGameHalted() const
{
	return m_isGameHalted;
}

void FramePacer::setLogicTimeScaleFps( Int fps )
{
	m_logicTimeScaleFPS = fps;
}

Int FramePacer::getLogicTimeScaleFps() const
{
	return m_logicTimeScaleFPS;
}

void FramePacer::enableLogicTimeScale( Bool enable )
{
	m_enableLogicTimeScale = enable;
}

Bool FramePacer::isLogicTimeScaleEnabled() const
{
	return m_enableLogicTimeScale;
}

#if defined(GENERALS_ONLINE)
void FramePacer::setGameSpeedFps( Int fps )
{
	const Int logicFps = fps >= 1000 ? (Int)RenderFpsPreset::UncappedFpsValue : std::max<Int>(fps, 1);
	m_enableLogicTimeScale = TRUE;
	m_logicTimeScaleFPS = logicFps;
}

Int FramePacer::getGameSpeedFps() const
{
	return m_logicTimeScaleFPS >= (Int)RenderFpsPreset::UncappedFpsValue ? 1000 : m_logicTimeScaleFPS;
}

void FramePacer::resetGameSpeed()
{
	m_enableLogicTimeScale = TRUE;
	m_logicTimeScaleFPS = LOGICFRAMES_PER_SECOND;
}
#endif

Int FramePacer::getActualLogicTimeScaleFps(LogicTimeQueryFlags flags) const
{
	if (m_isTimeFrozen && (flags & IgnoreFrozenTime) == 0)
	{
		return 0;
	}

	if (m_isGameHalted && (flags & IgnoreHaltedGame) == 0)
	{
		return 0;
	}

	if (TheNetwork != nullptr)
	{
		return TheNetwork->getFrameRate();
	}

	if (isLogicTimeScaleEnabled())
	{
		return getLogicTimeScaleFps();
	}

	// Returns uncapped value to align with the render update as per the original game behavior.
	return RenderFpsPreset::UncappedFpsValue;
}

Real FramePacer::getActualLogicTimeScaleRatio(LogicTimeQueryFlags flags) const
{
	return (Real)getActualLogicTimeScaleFps(flags) / LOGICFRAMES_PER_SECONDS_REAL;
}

Real FramePacer::getActualLogicTimeScaleOverFpsRatio(LogicTimeQueryFlags flags) const
{
	// TheSuperHackers @info Clamps ratio to min 1, because the logic
	// frame rate is currently capped by the render frame rate.
	return min(1.0f, (Real)getActualLogicTimeScaleFps(flags) / getUpdateFps());
}

Real FramePacer::getLogicTimeStepSeconds(LogicTimeQueryFlags flags) const
{
	return SECONDS_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}

Real FramePacer::getLogicTimeStepMilliseconds(LogicTimeQueryFlags flags) const
{
	return MSEC_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}
