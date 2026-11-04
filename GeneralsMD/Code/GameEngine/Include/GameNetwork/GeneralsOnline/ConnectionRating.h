#pragma once

#include "Lib/BaseType.h"
#include "GameNetwork/ConnectionStats.h"

// One overall rating of a connection, shared by the lobby slot indicator and the in-game overlay.
// It is the worse of two axes: delay (the input delay the link forces, in ms) and stability (stalls per minute).
// 5 bars: delay <= 150 ms and no stall in the last minute; 1 bar: delay above 500 ms or 6 or more stalls per minute.
// Colors: 4-5 green, 2-3 amber, 1 red. Red needs one of (RedReason): blackout, loss, delay or stalls, see RedReason in the .cpp.
// Stalls a healthy link does not explain wait on the peer's frame rate: slow PC, at most 3 bars and never red for it.
// Too few samples (10 probes, 20 command acks) give no rating.
class ConnectionRating
{
public:
	enum { BAR_COUNT = 5 };
	enum { SEVERITY_OK = 0, SEVERITY_WARN = 1, SEVERITY_BAD = 2 };

	struct Inputs
	{
		Inputs();

		Real delayMs;				///< Input delay the link forces.
		Real stallsPerMinute;		///< Measured and predicted, whichever is higher.
		Real measuredStallsPerMinute;	///< Stalls of the last minute in a match, negative in the lobby.
		Real lossPercent;			///< Packet loss over the window from enough feedback reports, negative if unknown.
		Int silentMs;				///< Since the last packet of the peer, negative if unknown.
		Int silentLimitMs;			///< Silence that makes the link red, at least 2.5 probe intervals.
		Bool stallInWindow;			///< A stall in the last 10 s stats window.
		Bool slowPc;				///< The measured stalls are the peer's frame rate, not the link.
	};

	struct State
	{
		State() : bars(0), delayBars(0), stabilityBars(0), delayMs(0), stallsPerMinute(0.0f), raiseSinceMs(0), redReason(nullptr), slowPc(FALSE) {}

		Int bars;					///< 1..BAR_COUNT, 0 until rated.
		Int delayBars;
		Int stabilityBars;
		Int delayMs;
		Real stallsPerMinute;
		UnsignedInt raiseSinceMs;	///< Since when the rating asks for more bars than shown, 0 if it does not.
		const char *redReason;		///< Why the link is red, nullptr if it is not.
		Bool slowPc;				///< The peer's PC is the bottleneck, the link is fine.
	};

	/// lossPercent is the packet loss from the packet feedback, negative to use the loss of the stats.
	static Inputs InputsFromProbe(const ConnectionStats &probe, Int slackPercent, Int sendIntervalMs, Real lossPercent);

	static Inputs InputsFromMatch(const ConnectionStats &cmd, const NetworkOverlayInfo &info, Int slackPercent, Int sendIntervalMs, Real lossPercent);

	/// Fewer bars show at once, more only after the rating asked for them for a few seconds.
	static void Update(State &state, const Inputs &inputs, UnsignedInt nowMs);

	static Int Severity(Int bars);

	static const char *RedReason(const Inputs &inputs);

	/// Draws the signal bars, level is bars - 1.
	static void DrawBars(Int left, Int top, Int size, Int level);

	/// The spinner shown while a connection is not rated yet.
	static void DrawSpinner(Int left, Int top, Int size);

	/// The indicator of a rating as ComputeConnectionRating returns it: the spinner below 1 bar, else the bars.
	static void DrawIndicator(Int left, Int top, Int size, Int bars);
};

// Draws between begin and end of a display batch, unless the caller already batches.
class ScopedDisplayBatch
{
public:
	ScopedDisplayBatch();
	~ScopedDisplayBatch();

private:
	Bool m_ownBatch;
};
