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
	void update(time_t now);								///< Drop the samples that have left the window.

	Real getLatency() const { return m_srtt; }
	Real getMinLatency() const;
	Real getLatencyPercentile(Int percent, time_t since = 0) const;	///< Round trip that this share of the samples in the window, from since on, stays within
	Real getJitter() const { return m_jitter; }
	Real getLossPercent() const;						///< Share of retries the earlier send was not acked for
	Real getLateAckPercent() const;					///< Share of retries whose earlier send was acked after the retry
	Real getRetryPercent() const;						///< Share of retries, lost and late acked together
	Real getLatePercent() const;						///< Share of acks that took more than twice the smoothed round trip time.
	Int getSampleCount() const { return m_samples.size(); }

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

// Frames the game wanted to advance but had to wait for commands, over the same sliding window.
class StallStats
{
public:
	StallStats();

	void reset();
	void note(Int durationMs, UnsignedInt missingSlotMask, time_t now);	///< One finished stall and the slots whose commands were missing when it began.
	void update(time_t now);

	Int getCount() const { return m_events.size(); }
	Int getTotalMs() const;
	Int getCountForSlot(Int slot) const;

private:
	enum { MAX_EVENTS = 512 };

	struct Event
	{
		time_t time;
		Int ms;
		UnsignedInt slotMask;
	};

	TimedRing<Event, MAX_EVENTS> m_events;
};
