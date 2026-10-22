#pragma once
// Lowers client only effects step by step when this machine cannot keep up, and gives them back when it can.

#include "Common/AdaptiveQualityModel.h"

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
	// the F10 overlay line, false when there is nothing to show
	Bool formatOverlayLine(char* out, size_t cap) const;

private:
	AdaptiveQuality();

	void startMatch();
	void endMatch();
	void recordFrame(double ms, double deadlineMs);
	void checkDisplay();
	void closeSecond(long long t);
	void setLevel(Int from, Int level, const AdaptiveSecond& s);
	void showMessage(const UnicodeString& text);

	enum { BINS = 2000 };

	AdaptiveQualityModel m_model;
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
};
