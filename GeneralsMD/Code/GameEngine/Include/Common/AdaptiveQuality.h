#pragma once
// Lowers client only effects step by step when this machine cannot keep up, and gives them back when it can.

#include "Common/AdaptiveQualityModel.h"
#include "Common/AdaptiveQualityPower.h"

// Read-only snapshot for the network overlay
struct AdaptiveQualityOverlay
{
	enum { LEVELS = AdaptiveQualityLevels::LEVEL_COUNT };

	Int level;
	Int lastFrom;				// the last step, -1 when there was none this match
	Int lastTo;
	Int lastReason;				// AdaptiveQualityModel::Reason
	float lastAgoSec;
	float p50Ms;				// own frame cost over the last counted second
	float p95Ms;
	float cpuMs;
	float presentMs;
	float stepDownP50Ms;		// the median limit that steps down
	float stepUpP95Ms;			// the p95 limit that allows a step up
	float deadlineMs;
	Int deadlineKind;			// AdaptiveQualityTuning::DeadlineKind
	float refreshHz;
	float savingMs[LEVELS];		// frame cost saved by each level, learned on this machine or the default
	Bool savingLearned[LEVELS];
	float downInSec;			// seconds until a step down is allowed, -1 when no level is left to lower
	float upInSec;				// seconds until a step up is allowed, -1 at level 0
};

class AdaptiveQuality
{
public:
	static AdaptiveQuality& get();

	// the main loop, once per iteration
	void frameBegin();
	void frameEnd();
	// own cost: game logic, client update and client step in QPC ticks
	void addOwn(long long ticks) { m_ownTicks += ticks; }
	// time blocked in Present: counts only when the frame missed its deadline, a met frame was waiting on vsync
	void addPresentWait(long long ticks) { m_presentTicks += ticks; }
	// W3DDisplay::draw: whether the device is windowed and whether fullscreen Present waits for the display
	void notePresentMode(Bool windowed, Bool intervalSynced) { m_windowed = windowed; m_intervalSynced = intervalSynced; }
	// this iteration waited on the other players, its cost is not this machine's
	void noteNetworkWait() { m_networkWait = TRUE; }
	static long long now();

	// W3DDisplay::draw, once per frame outside frozen time: true when this frame is not rendered
	Bool shouldSkipRender();

	Int getLevel() const { return m_model.level(); }
	// the network overlay, false when there is no match to show
	Bool getOverlay(AdaptiveQualityOverlay& out) const;

private:
	AdaptiveQuality();

	void startMatch();
	void endMatch();
	void recordFrame(double ms, double deadlineMs);
	void checkDisplay();
	void closeSecond(long long t);
	void setLevel(Int from, Int level, const AdaptiveSecond& s);
	void showMessage(const UnicodeString& text);
	void checkPower();

	enum { BINS = 2000 };

	AdaptiveQualityModel m_model;
	double m_matchSeconds;
	float m_lastP50Ms;
	float m_lastP95Ms;
	Int m_deadlineKind;
	Int m_lastFrom;
	Int m_lastTo;
	Int m_lastReason;
	double m_lastStepAt;
	long long m_freq;
	long long m_ownTicks;
	long long m_presentTicks;
	long long m_secondStart;
	long long m_matchStart;
	Bool m_networkWait;
	Bool m_inMatch;
	Bool m_renderSkipPhase;
	Bool m_pairPending;
	double m_pairMs;
	double m_pairDeadlineMs;
	double m_cpuSumMs;
	double m_presentSumMs;
	unsigned m_splitFrames;
	double m_lastCpuMs;
	double m_lastPresentMs;
	double m_lastDeadlineMs;
	double m_refreshMs;
	Bool m_windowed;
	Bool m_intervalSynced;
	Bool m_presentSynced;
	unsigned m_hist[BINS];
	unsigned m_frames;
	unsigned m_over;
	unsigned m_waitFrames;
	unsigned m_iterations;
	double m_sumMs;
	Bool m_minimized;
	Bool m_paused;
	double m_lastMeanMs;
	Int m_stepDowns;
	Int m_stepUps;
	Bool m_powerChecked;
	AdaptiveQualityPower::State m_power;
	char m_planName[24];
};
