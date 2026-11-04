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

#pragma once

#include <time.h>

// Fixed size ring of items with a time member, oldest first, that drops the items older than a window.
template <typename T, Int N>
class TimedRing
{
public:
	TimedRing() { clear(); }

	void clear() { m_head = 0; m_count = 0; }
	Int size() const { return m_count; }
	const T& operator[](Int i) const { return m_items[(m_head + i) % N]; }

	void push(const T& item)
	{
		if (m_count == N)
		{
			m_head = (m_head + 1) % N;
			--m_count;
		}
		m_items[(m_head + m_count) % N] = item;
		++m_count;
	}

	void prune(time_t now, time_t windowMs)
	{
		while (m_count > 0 && now - m_items[m_head].time > windowMs)
		{
			m_head = (m_head + 1) % N;
			--m_count;
		}
	}

private:
	T m_items[N];
	Int m_head;
	Int m_count;
};

// Tracks latency, jitter and loss of one connection over a sliding time window.
class ConnectionStats
{
public:
	ConnectionStats();

	void reset();
	void noteAcked(Real rttMs, time_t now);	///< A command that was acked on its first send.
	void noteRetry(time_t now);							///< A command that was not acked in time and had to be sent again.
	void noteLateAck(time_t now);						///< A retried command was acked by its earlier send after all, the retry was not a loss.
	void setRedundancyDepth(Int depth, Int cap);
	void update(time_t now);								///< Drop the samples that have left the window.

	Real getLatency() const { return m_srtt; }
	Real getMinLatency() const;
	Real getLatencyPercentile(Int percent, time_t since = 0) const;	///< Round trip that this share of the samples in the window, from since on, stays within
	Int getSampleCountSince(time_t since) const;
	Real getJitter() const { return m_jitter; }
	Real getLossPercent() const;						///< Share of retries the earlier send was not acked for
	Real getLateAckPercent() const;					///< Share of retries whose earlier send was acked after the retry
	Real getRetryPercent() const;						///< Share of retries, lost and late acked together
	Real getLatePercent() const;						///< Share of acks that took more than twice the smoothed round trip time.
	Int getSampleCount() const { return m_samples.size(); }
	enum { MAX_REDUNDANCY_DEPTH = 16 };
	Int getRedundancyDepth() const { return m_depth; }
	Int getRedundancyCap() const { return m_depthCap; }

private:
	enum
	{
		MAX_SAMPLES = 1024,	///< Covers the window at 60 acked frames per second.
		MAX_RETRIES = 1024	///< As many as acks, under heavy loss a smaller ring undercounts the retries of the window.
	};

	struct Sample
	{
		time_t time;
		Real rtt;
		Bool late;
	};

	struct Stamp
	{
		time_t time;
	};

	TimedRing<Sample, MAX_SAMPLES> m_samples;
	TimedRing<Stamp, MAX_RETRIES> m_retries;
	TimedRing<Stamp, MAX_RETRIES> m_lateAcks;

	Real m_srtt;
	Real m_jitter;
	Real m_previousRtt;
	Bool m_hasRtt;

	Int m_depth;
	Int m_depthCap;
};

// Tracks how many frames before its execution frame the frame commands of one sender arrive,
// over the same sliding window as ConnectionStats. Zero or less means the frame had to wait.
class ArrivalCushionStats
{
public:
	ArrivalCushionStats();

	void reset();
	void note(Int cushion, time_t now);
	void update(time_t now);

	Int getSampleCount() const { return m_samples.size(); }
	Int getPercentile(Int percent) const;			///< Cushion that percent of the samples are at or above the lowest value of.

private:
	enum { MAX_SAMPLES = 1024 };

	struct Sample
	{
		time_t time;
		Int cushion;
	};

	TimedRing<Sample, MAX_SAMPLES> m_samples;
};

// Frames the game wanted to advance but had to wait for commands, kept for a minute and counted over a window.
class StallStats
{
public:
	StallStats();

	void reset();
	void note(Int durationMs, UnsignedInt missingSlotMask, time_t now);	///< One finished stall and the slots whose commands were missing when it began.
	void update(time_t now);

	Int getCount(time_t now, Int windowMs) const;
	Int getCountForSlot(Int slot, time_t now, Int windowMs) const;

private:
	enum { MAX_EVENTS = 512, HISTORY_MS = 60000 };

	struct Event
	{
		time_t time;
		Int ms;
		UnsignedInt slotMask;
	};

	TimedRing<Event, MAX_EVENTS> m_events;
};

// Run ahead from the per window need: a forgetting quantile of normal windows is the baseline; spike episodes are bought only
// when they recur faster than one per lambda. Rules use window time, so the decision interval does not matter.
class RunAheadSpikeController
{
public:
	enum Reason { REASON_HOLD, REASON_BASELINE, REASON_COVER, REASON_UNCOVER, REASON_CONGESTION };

	struct Decision
	{
		Int runAhead;
		Int reason;				///< Reason.
		Int base;				///< Frames, the q quantile of the normal windows.
		Int cover;				///< Frames bought on top of the baseline for spikes.
		Int spikesPerMin10;		///< Spike episodes per minute over the horizon, times 10.
		Int spikeSizeMs;		///< The median spike height over the baseline in the horizon.
		Bool inSpike;
		Bool fastDrop;			///< The target is the recent need, the next decision does not wait for the long dead time.
	};

	RunAheadSpikeController();

	void reset();
	void dropEpisode();		///< An outage: the open episode is not the network's trend.
	Decision onWindow(time_t now, Int windowMs, Int need, Bool stalled, Int stallMs, Real jitterMs, Int runAhead, Int marginFrames, Real frameMs, Int floorFrames, Int capFrames, Bool safeFastDrop, Bool boundRaise);

private:
	enum
	{
		BUCKETS = 128,			// one frame each, the need is 0 to 127 frames
		SAMPLE_RING = 16,		// need samples of the open episode
		RECENT_RING = 8,		// the last windows, for the median need of a fast drop
		EPISODE_RING = 32		// more than the HORIZON_MS / LAMBDA_MS + 1 the cost rule looks at
	};

	struct Episode
	{
		time_t start;
		Int height;
	};

	Real m_hist[BUCKETS];		///< The normal windows, forgetting.
	Real m_mass;
	Real m_timeMs;				///< The time the normal windows cover, forgetting the same way.
	Real m_saved[BUCKETS];		///< The baseline before a level shift was adopted, for the undo.
	Real m_savedMass;
	Real m_savedTimeMs;
	Int m_savedBase;
	Int m_undoMs;				///< Window time left in which a return to the saved baseline undoes the adopted level.
	Int m_undoCalmMs;
	Episode m_pending;			///< The episode an adoption came from, a spike after all if it is undone.

	Bool m_inSpike;
	time_t m_spikeStart;
	Int m_peakNeed;
	Int m_episodeStallMs;
	Int m_episodeMs;			///< Window time of the open episode after the window that opened it.
	Int m_calmMs;
	time_t m_sampleTime[SAMPLE_RING];
	Int m_sampleNeed[SAMPLE_RING];
	Int m_sampleMs[SAMPLE_RING];
	Int m_sampleCount;

	Episode m_episodes[EPISODE_RING];
	Int m_episodeCount;			///< Recorded so far, the ring holds the last EPISODE_RING.
	Int m_lastCover;
	Int m_recentNeed[RECENT_RING];
	Int m_recentMs[RECENT_RING];
	Int m_recentCount;
	Int m_gateCleanMs;			///< Window time in a row the need stayed within the target + 1.
	time_t m_gateClusterStart;	///< Start of the current spike cluster.
	time_t m_gatePrevClusterStart;	///< Start of the cluster before it.

	static void forget(Real *hist, Real &mass, Real factor);
	static void addSample(Real *hist, Real &mass, Int need);
	static Int quantile(const Real *hist, Real mass, Real q);
	Int baseline(Int runAhead, Int marginFrames) const;
	void recordEpisode(const Episode &episode);
};

// What the network overlay and the connection rating need to know about one slot beyond its ConnectionStats.
struct NetworkOverlayInfo
{
	NetworkOverlayInfo()
		: cushionSamples(0), cushionP5(0), cushionP50(0)
		, stallsWindow(0), stallsMinute(0), stalledNow(FALSE), waitingOnSlot(FALSE)
		, runAhead(0), frameGroupingMs(0)
		, isRouter(FALSE), controllerOn(FALSE), marginFrames(1), changeDir(0), changeReason(CHANGE_NONE), changeAgoMs(-1)
		, ctlReason(CTL_NONE), ctlMinSum(NO_PAIR_SUM), ctlPairA(-1), ctlPairB(-1), ctlBase(0), ctlCover(0), ctlSpikesPerMin10(0), ctlSpikeSizeMs(0), ctlInSpike(FALSE), ctlSinceChangeMs(0) {}

	enum ChangeReason { CHANGE_NONE, CHANGE_START, CHANGE_LAG_SPIKE, CHANGE_STALL, CHANGE_CLEAN };
	enum { NO_PAIR_SUM = 999 };	///< ctlMinSum when no pair reported
	enum ControllerReason { CTL_NONE, CTL_SEED, CTL_HOLD, CTL_BASELINE, CTL_COVER, CTL_UNCOVER, CTL_CONGESTION, CTL_OUTAGE, CTL_FORMULA };

	Int cushionSamples;		///< Frames before execution that the frame commands of this slot arrived, over the stats window.
	Int cushionP5;
	Int cushionP50;
	Int stallsWindow;			///< Stalls the local game had to wait on this slot for, over the stats window.
	Int stallsMinute;			///< The same over the last minute.
	Bool stalledNow;			///< The local game is waiting for commands right now.
	Bool waitingOnSlot;		///< ... and this slot is one it is waiting on.

	Int runAhead;				///< Frames.
	Int frameGroupingMs;		///< The send interval.

	Bool isRouter;				///< This machine decides the run ahead, the controller fields are valid only here.
	Bool controllerOn;
	Int marginFrames;			///< Cushion frames each direction of a pair keeps spare.
	Int changeDir;				///< Direction of the last applied run ahead change, 1 up, -1 down, 0 none yet.
	Int changeReason;			///< ChangeReason, known on every client.
	Int changeAgoMs;			///< Since that change took effect, -1 if none.
	Int ctlReason;				///< ControllerReason of the last decision.
	Int ctlMinSum;				///< Lowest sum of the two cushion directions of a pair, NO_PAIR_SUM if no pair reported.
	Int ctlPairA;
	Int ctlPairB;
	Int ctlBase;				///< Frames, the baseline quantile of the normal windows.
	Int ctlCover;				///< Frames bought for spikes on top of the baseline.
	Int ctlSpikesPerMin10;		///< Spike episodes per minute, times 10.
	Int ctlSpikeSizeMs;			///< The median spike height over the baseline.
	Bool ctlInSpike;			///< A spike episode is open, the run ahead holds.
	Int ctlSinceChangeMs;
};
