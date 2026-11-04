#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AdaptiveQuality.h"

#include "Common/FramePacer.h"
#include "Common/GameLOD.h"
#include "Common/GlobalData.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"

extern HWND ApplicationHWnd;

static const double BIN_MS = 0.05;

// from system32 only; without KB2533623 the search flag is rejected, so retry by the full system path
static HMODULE loadSystemLibrary(const char* name)
{
	HMODULE module = ::LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (module == nullptr)
	{
		char path[MAX_PATH];
		const UINT len = ::GetSystemDirectoryA(path, MAX_PATH);
		if (len > 0 && len + 1 + strlen(name) < MAX_PATH)
		{
			strcat(path, "\\");
			strcat(path, name);
			module = ::LoadLibraryA(path);
		}
	}
	return module;
}

// windowed Present goes through DWM composition, which waits for the display; always on since Windows 8
static Bool dwmComposing()
{
	typedef HRESULT (WINAPI *DwmIsCompositionEnabledFn)(BOOL*);
	static HMODULE s_dwm = loadSystemLibrary("dwmapi.dll");
	static DwmIsCompositionEnabledFn s_enabled = s_dwm != nullptr ? (DwmIsCompositionEnabledFn)::GetProcAddress(s_dwm, "DwmIsCompositionEnabled") : nullptr;
	BOOL enabled = FALSE;
	return s_enabled != nullptr && SUCCEEDED(s_enabled(&enabled)) && enabled;
}

AdaptiveQuality& AdaptiveQuality::get()
{
	static AdaptiveQuality s_instance;
	return s_instance;
}

AdaptiveQuality::AdaptiveQuality()
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	m_freq = f.QuadPart;
	m_ownTicks = 0;
	m_presentTicks = 0;
	m_secondStart = 0;
	m_matchStart = 0;
	m_networkWait = FALSE;
	m_inMatch = FALSE;
	m_renderSkipPhase = FALSE;
	m_pairPending = FALSE;
	m_pairMs = 0.0;
	m_pairDeadlineMs = 0.0;
	m_cpuSumMs = m_presentSumMs = 0.0;
	m_splitFrames = 0;
	m_lastCpuMs = m_lastPresentMs = 0.0;
	m_lastDeadlineMs = AdaptiveQualityTuning::kOverFrameMs;
	m_refreshMs = 1000.0 / 60.0;
	m_windowed = FALSE;
	m_intervalSynced = TRUE;
	m_presentSynced = TRUE;
	memset(m_hist, 0, sizeof(m_hist));
	m_frames = m_over = m_waitFrames = m_iterations = 0;
	m_sumMs = 0.0;
	m_minimized = FALSE;
	m_paused = FALSE;
	m_lastMeanMs = 0.0;
	m_stepDowns = m_stepUps = 0;
	m_matchSeconds = 0.0;
	m_lastP50Ms = m_lastP95Ms = 0.0f;
	m_deadlineKind = AdaptiveQualityTuning::DEADLINE_FLOOR;
	m_lastFrom = m_lastTo = -1;
	m_lastReason = AdaptiveQualityModel::REASON_NONE;
	m_lastStepAt = 0.0;
	m_powerChecked = FALSE;
	m_power = {};
	strcpy(m_planName, "unknown");
}

long long AdaptiveQuality::now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

void AdaptiveQuality::frameBegin()
{
	m_ownTicks = 0;
	m_presentTicks = 0;
	m_networkWait = FALSE;
}

Bool AdaptiveQuality::shouldSkipRender()
{
	if (!m_inMatch || !AdaptiveQualityLevels::rendersSkipped(m_model.level()))
	{
		m_renderSkipPhase = FALSE;
		return FALSE;
	}

	m_renderSkipPhase = !m_renderSkipPhase;
	return m_renderSkipPhase;
}

void AdaptiveQuality::frameEnd()
{
	const Bool enabled = NGMP_OnlineServicesManager::Settings.Graphics_AdaptiveQuality();
	const Bool inMatch = enabled && TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isInReplayGame() && TheGameLogic->getFrame() > 0;
	if (inMatch != m_inMatch)
	{
		if (inMatch)
		{
			startMatch();
		}
		else
		{
			endMatch();
		}
	}
	if (!m_inMatch)
	{
		return;
	}

	if (ApplicationHWnd != nullptr && ::IsIconic(ApplicationHWnd))
	{
		m_minimized = TRUE;
	}
	if (TheGameLogic->isGamePaused())
	{
		m_paused = TRUE;
	}
	++m_iterations;
	if (m_networkWait)
	{
		++m_waitFrames;
	}

	// own ticks include the Present wait, split it off; the split is logged but only the deadline decides
	const long long cpuTicks = m_ownTicks > m_presentTicks ? m_ownTicks - m_presentTicks : 0;
	const double cpuMs = (double)cpuTicks * 1000.0 / (double)m_freq;
	const double presentMs = (double)(m_ownTicks - cpuTicks) * 1000.0 / (double)m_freq;
	const Int fpsCap = TheFramePacer != nullptr && TheFramePacer->isActualFramesPerSecondLimitEnabled() ? TheFramePacer->getActualFramesPerSecondLimit() : 0;
	const double deadlineMs = AdaptiveQualityTuning::frameDeadlineMs(fpsCap, m_refreshMs, m_presentSynced != FALSE);
	m_lastDeadlineMs = deadlineMs;
	m_deadlineKind = AdaptiveQualityTuning::frameDeadlineKind(fpsCap, m_refreshMs, m_presentSynced != FALSE);
	const double ms = AdaptiveQualityTuning::frameCostMs(cpuMs, presentMs, deadlineMs);
	if (!m_networkWait)
	{
		m_cpuSumMs += cpuMs;
		m_presentSumMs += presentMs;
		++m_splitFrames;
	}
	if (m_networkWait)
	{
		// held for another player's commands: waiting is no cost, and a frame cut short by it says nothing about the pair
		m_pairPending = FALSE;
	}
	else if (AdaptiveQualityLevels::rendersSkipped(m_model.level()))
	{
		// rendered and skipped frames come in pairs, judge the pair
		if (!m_pairPending)
		{
			m_pairPending = TRUE;
			m_pairMs = ms;
			m_pairDeadlineMs = deadlineMs;
		}
		else
		{
			m_pairPending = FALSE;
			recordFrame((m_pairMs + ms) * 0.5, (m_pairDeadlineMs + deadlineMs) * 0.5);
		}
	}
	else
	{
		m_pairPending = FALSE;
		recordFrame(ms, deadlineMs);
	}

	const long long t = now();
	if (!m_powerChecked && (t - m_matchStart) >= 3 * m_freq)
	{
		checkPower();
	}
	if (t - m_secondStart >= m_freq)
	{
		closeSecond(t);
	}
}

// Asks Windows once per match and fails silently when it cannot be asked.
void AdaptiveQuality::checkPower()
{
	m_powerChecked = TRUE;

	static_assert(sizeof(AqGuid) == sizeof(GUID), "AqGuid must match GUID");
	SYSTEM_POWER_STATUS status = {};
	if (!::GetSystemPowerStatus(&status))
	{
		return;
	}

	AqGuid plan = {};
	AqGuid overlay = {};
	Bool havePlan = FALSE;
	Bool haveOverlay = FALSE;
	HMODULE powrprof = loadSystemLibrary("powrprof.dll");
	if (powrprof != nullptr)
	{
		typedef DWORD (WINAPI *GetActiveSchemeFn)(HKEY, GUID**);
		typedef DWORD (WINAPI *GetOverlayFn)(GUID*);
		GetActiveSchemeFn getActiveScheme = (GetActiveSchemeFn)::GetProcAddress(powrprof, "PowerGetActiveScheme");
		GUID* activeScheme = nullptr;
		if (getActiveScheme != nullptr && getActiveScheme(nullptr, &activeScheme) == ERROR_SUCCESS && activeScheme != nullptr)
		{
			memcpy(&plan, activeScheme, sizeof(plan));
			havePlan = TRUE;
			::LocalFree(activeScheme);
		}
		GetOverlayFn getOverlay = (GetOverlayFn)::GetProcAddress(powrprof, "PowerGetEffectiveOverlayScheme");
		GUID overlayScheme = {};
		if (getOverlay != nullptr && getOverlay(&overlayScheme) == ERROR_SUCCESS)
		{
			memcpy(&overlay, &overlayScheme, sizeof(overlay));
			haveOverlay = TRUE;
		}
		::FreeLibrary(powrprof);
	}

	m_power = AdaptiveQualityPower::classify(status.ACLineStatus, status.BatteryFlag, status.SystemStatusFlag, havePlan ? &plan : nullptr, haveOverlay ? &overlay : nullptr);
	_snprintf_s(m_planName, sizeof(m_planName), _TRUNCATE, "%s", AdaptiveQualityPower::planName(havePlan ? &plan : nullptr));

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[ADAPTIVE] power: battery=%d saver=%d plan=%s efficiency=%d", (int)m_power.onBattery, (int)(m_power.batterySaver || m_power.powerSaverPlan), m_planName, (int)m_power.bestEfficiencyMode);
	if (m_power.onBattery)
	{
		showMessage(TheGameText->FETCH_OR_SUBSTITUTE("GUI:AdaptiveQualityBattery", L"On battery power: plug in for better performance."));
	}
	if (m_power.savesPower())
	{
		showMessage(TheGameText->FETCH_OR_SUBSTITUTE("GUI:AdaptiveQualityPowerSaving", L"Power saving is on: set Windows to Best performance."));
	}
}

void AdaptiveQuality::recordFrame(double ms, double deadlineMs)
{
	const int bin = (int)(ms / BIN_MS);
	++m_hist[bin < BINS ? bin : BINS - 1];
	++m_frames;
	if (ms > deadlineMs)
	{
		++m_over;
	}
	m_sumMs += ms;
}

// the refresh period of the window's monitor, 60 Hz when unknown, and whether Present waits for it
void AdaptiveQuality::checkDisplay()
{
	double refreshMs = 1000.0 / 60.0;
	HMONITOR monitor = ApplicationHWnd != nullptr ? ::MonitorFromWindow(ApplicationHWnd, MONITOR_DEFAULTTONEAREST) : nullptr;
	MONITORINFOEXA info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	DEVMODEA mode;
	memset(&mode, 0, sizeof(mode));
	mode.dmSize = sizeof(mode);
	if (monitor != nullptr && ::GetMonitorInfoA(monitor, &info) && ::EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
	{
		refreshMs = 1000.0 / mode.dmDisplayFrequency;
	}
	const Bool synced = m_windowed ? dwmComposing() : m_intervalSynced;
	if (refreshMs != m_refreshMs || synced != m_presentSynced)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[ADAPTIVE] display: %.0f Hz synced=%d windowed=%d", 1000.0 / refreshMs, (int)synced, (int)m_windowed);
	}
	m_refreshMs = refreshMs;
	m_presentSynced = synced;
}

void AdaptiveQuality::startMatch()
{
	m_inMatch = TRUE;
	m_model.reset();
	m_matchStart = now();
	m_secondStart = m_matchStart;
	memset(m_hist, 0, sizeof(m_hist));
	m_frames = m_over = m_waitFrames = m_iterations = 0;
	m_sumMs = 0.0;
	m_cpuSumMs = m_presentSumMs = 0.0;
	m_splitFrames = 0;
	m_lastCpuMs = m_lastPresentMs = 0.0;
	m_minimized = m_paused = FALSE;
	m_pairPending = FALSE;
	m_stepDowns = m_stepUps = 0;
	m_matchSeconds = 0.0;
	m_lastP50Ms = m_lastP95Ms = 0.0f;
	m_lastFrom = m_lastTo = -1;
	m_lastReason = AdaptiveQualityModel::REASON_NONE;
	m_lastStepAt = 0.0;
	m_powerChecked = FALSE;
	checkDisplay();
}

void AdaptiveQuality::endMatch()
{
	if (m_stepDowns > 0 || m_stepUps > 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[ADAPTIVE] match ended at level %d, %d steps down, %d steps up", (int)m_model.level(), (int)m_stepDowns, (int)m_stepUps);
	}

	m_inMatch = FALSE;
	m_renderSkipPhase = FALSE;
	TheWritableGlobalData->m_adaptiveQualityLevel = 0;
	if (TheGameLODManager != nullptr)
	{
		TheGameLODManager->setAdaptiveParticleLevel(0);
	}
	m_model.reset();
}

void AdaptiveQuality::closeSecond(long long t)
{
	const double matchSeconds = (double)(t - m_matchStart) / (double)m_freq;
	m_matchSeconds = matchSeconds;
	checkDisplay();

	AdaptiveSecond s = {};
	s.frames = m_frames;
	s.over = m_over;
	if (m_frames > 0)
	{
		const unsigned t50 = (m_frames - 1) / 2;
		const unsigned t95 = (m_frames * 95 + 99) / 100 - 1;
		unsigned seen = 0;
		bool got50 = false;
		for (int b = 0; b < BINS; ++b)
		{
			seen += m_hist[b];
			if (!got50 && seen > t50)
			{
				s.p50Ms = (float)((b + 0.5) * BIN_MS);
				got50 = true;
			}
			if (seen > t95)
			{
				s.p95Ms = (float)((b + 0.5) * BIN_MS);
				break;
			}
		}
		s.meanMs = (float)(m_sumMs / m_frames);
	}

	const Bool ignore = m_frames == 0 || matchSeconds < AdaptiveQualityTuning::kWarmupSeconds || m_minimized || m_paused
		|| (double)m_waitFrames > AdaptiveQualityTuning::kNetworkWaitIgnoreFraction * m_iterations;

	if (!ignore)
	{
		m_lastMeanMs = s.meanMs;
		m_lastP50Ms = s.p50Ms;
		m_lastP95Ms = s.p95Ms;
		m_lastCpuMs = m_splitFrames ? m_cpuSumMs / m_splitFrames : 0.0;
		m_lastPresentMs = m_splitFrames ? m_presentSumMs / m_splitFrames : 0.0;
		const Int from = m_model.level();
		if (m_model.onSecond(s, matchSeconds) != AdaptiveQualityModel::DECISION_NONE)
		{
			m_lastFrom = from;
			m_lastTo = m_model.level();
			m_lastReason = m_model.reason();
			m_lastStepAt = matchSeconds;
			setLevel(from, m_model.level(), s);
		}
	}

	memset(m_hist, 0, sizeof(m_hist));
	m_frames = m_over = m_waitFrames = m_iterations = 0;
	m_sumMs = 0.0;
	m_cpuSumMs = m_presentSumMs = 0.0;
	m_splitFrames = 0;
	m_minimized = m_paused = FALSE;
	m_secondStart = t;
}

void AdaptiveQuality::showMessage(const UnicodeString& text)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[ADAPTIVE] message: %s", to_utf8(text.str()).c_str());
	if (TheInGameUI != nullptr)
	{
		TheInGameUI->messageNoFormat(text);
	}
}

void AdaptiveQuality::setLevel(Int from, Int level, const AdaptiveSecond& s)
{
	TheWritableGlobalData->m_adaptiveQualityLevel = level;
	if (TheGameLODManager != nullptr)
	{
		TheGameLODManager->setAdaptiveParticleLevel(level);
	}

	static const char* const reasons[] = { "none", "over", "p50", "calm" };
	const Int changed = level > from ? level : from;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[ADAPTIVE] level %d -> %d reason=%s cost p50=%.2f p95=%.2f missed=%.1f%% deadline=%.2f cpu=%.2f present=%.2f refresh=%.0fHz synced=%d delta(L%d)=%.2f%s upwait=%.0fs", (int)from, (int)level, reasons[m_model.reason()],
		s.p50Ms, s.p95Ms, s.frames ? 100.0 * s.over / s.frames : 0.0, m_lastDeadlineMs, m_lastCpuMs, m_lastPresentMs, 1000.0 / m_refreshMs, (int)m_presentSynced, (int)changed, m_model.delta(changed), m_model.deltaLearned(changed) ? "" : " (default)", m_model.stepUpWait());

	if (level > from)
	{
		++m_stepDowns;
		showMessage(TheGameText->FETCH_OR_SUBSTITUTE("GUI:AdaptiveQualityLowered", L"Effects lowered for better performance."));
	}
	else
	{
		++m_stepUps;
	}
}

Bool AdaptiveQuality::getOverlay(AdaptiveQualityOverlay& out) const
{
	if (!m_inMatch)
	{
		return FALSE;
	}

	out.level = m_model.level();
	out.lastFrom = m_lastFrom;
	out.lastTo = m_lastTo;
	out.lastReason = m_lastReason;
	out.lastAgoSec = m_lastFrom >= 0 ? (float)(m_matchSeconds - m_lastStepAt) : -1.0f;
	out.p50Ms = m_lastP50Ms;
	out.p95Ms = m_lastP95Ms;
	out.cpuMs = (float)m_lastCpuMs;
	out.presentMs = (float)m_lastPresentMs;
	out.stepDownP50Ms = (float)AdaptiveQualityTuning::kStepDownP50Ms;
	out.stepUpP95Ms = (float)AdaptiveQualityTuning::kStepUpP95Ms;
	out.deadlineMs = (float)m_lastDeadlineMs;
	out.deadlineKind = m_deadlineKind;
	out.refreshHz = (float)(1000.0 / m_refreshMs);
	for (Int l = 0; l < AdaptiveQualityOverlay::LEVELS; ++l)
	{
		out.savingMs[l] = (float)m_model.delta(l);
		out.savingLearned[l] = m_model.deltaLearned(l);
	}
	const double cooldown = m_model.cooldownLeft(m_matchSeconds);
	out.downInSec = m_model.canStepDown() ? (float)cooldown : -1.0f;
	const double calmLeft = m_model.stepUpRequired() - (double)m_model.calmSeconds();
	out.upInSec = m_model.level() > 0 ? (float)(std::max)(cooldown, calmLeft > 0.0 ? calmLeft : 0.0) : -1.0f;
	return TRUE;
}
