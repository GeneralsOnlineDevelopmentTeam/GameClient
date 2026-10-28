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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/ConnectionStats.h"
#include "GameNetwork/NetworkDefs.h"

#include <algorithm>

ConnectionStats::ConnectionStats()
{
	reset();
}

void ConnectionStats::reset()
{
	m_samples.clear();
	m_retries.clear();
	m_lateAcks.clear();
	m_srtt = 0.0f;
	m_jitter = 0.0f;
	m_previousRtt = 0.0f;
	m_hasRtt = FALSE;
	m_depth = 0;
	m_depthCap = 0;
}

void ConnectionStats::noteAcked(Real rttMs, time_t now)
{
	if (rttMs < 0.0f)
	{
		rttMs = 0.0f;
	}

	Bool late = FALSE;
	if (m_hasRtt)
	{
		// RFC 3550 interarrival jitter and the TCP smoothed round trip time.
		const Real delta = rttMs - m_previousRtt;
		const Real deviation = delta < 0.0f ? -delta : delta;
		// late = more than 2 x srtt, floored at 20 ms
		const Real lateMargin = m_srtt > 20.0f ? m_srtt : 20.0f;
		late = (rttMs - m_srtt) > lateMargin;
		m_jitter += (deviation - m_jitter) / 16.0f;
		m_srtt += (rttMs - m_srtt) / 8.0f;
	}
	else
	{
		m_srtt = rttMs;
		m_hasRtt = TRUE;
	}
	m_previousRtt = rttMs;

	Sample sample;
	sample.time = now;
	sample.rtt = rttMs;
	sample.late = late;
	m_samples.push(sample);
}

void ConnectionStats::noteRetry(time_t now)
{
	Stamp stamp;
	stamp.time = now;
	m_retries.push(stamp);
}

void ConnectionStats::noteLateAck(time_t now)
{
	Stamp stamp;
	stamp.time = now;
	m_lateAcks.push(stamp);
}

void ConnectionStats::setRedundancyDepth(Int depth, Int cap)
{
	if (depth < 0)
	{
		depth = 0;
	}
	if (depth > MAX_REDUNDANCY_DEPTH)
	{
		depth = MAX_REDUNDANCY_DEPTH;
	}

	m_depth = depth;
	m_depthCap = cap;
}

void ConnectionStats::update(time_t now)
{
	m_samples.prune(now, NETWORK_STATS_WINDOW_MS);
	m_retries.prune(now, NETWORK_STATS_WINDOW_MS);
	m_lateAcks.prune(now, NETWORK_STATS_WINDOW_MS);
}

Real ConnectionStats::getMinLatency() const
{
	if (m_samples.size() == 0)
	{
		return 0.0f;
	}

	Real result = m_samples[0].rtt;
	for (Int i = 1; i < m_samples.size(); ++i)
	{
		if (m_samples[i].rtt < result)
		{
			result = m_samples[i].rtt;
		}
	}
	return result;
}

Real ConnectionStats::getLatencyPercentile(Int percent, time_t since) const
{
	Real sorted[MAX_SAMPLES];
	Int count = 0;
	for (Int i = 0; i < m_samples.size(); ++i)
	{
		if (m_samples[i].time >= since)
		{
			sorted[count] = m_samples[i].rtt;
			++count;
		}
	}
	if (count == 0)
	{
		return 0.0f;
	}
	std::sort(sorted, sorted + count);

	Int index = (count - 1) * percent / 100;
	if (index < 0)
	{
		index = 0;
	}
	if (index >= count)
	{
		index = count - 1;
	}
	return sorted[index];
}

Int ConnectionStats::getSampleCountSince(time_t since) const
{
	Int count = 0;
	for (Int i = m_samples.size() - 1; i >= 0; --i)
	{
		if (m_samples[i].time < since)
		{
			break;
		}
		++count;
	}
	return count;
}

Real ConnectionStats::getRetryPercent() const
{
	const Int total = m_samples.size() + m_retries.size();
	if (total == 0)
	{
		return 0.0f;
	}

	return 100.0f * m_retries.size() / total;
}

Real ConnectionStats::getLateAckPercent() const
{
	const Int total = m_samples.size() + m_retries.size();
	if (total == 0)
	{
		return 0.0f;
	}

	const Int lateAcks = m_lateAcks.size() < m_retries.size() ? m_lateAcks.size() : m_retries.size();
	return 100.0f * lateAcks / total;
}

Real ConnectionStats::getLossPercent() const
{
	const Int total = m_samples.size() + m_retries.size();
	if (total == 0)
	{
		return 0.0f;
	}

	const Int lost = m_retries.size() > m_lateAcks.size() ? m_retries.size() - m_lateAcks.size() : 0;
	return 100.0f * lost / total;
}

Real ConnectionStats::getLatePercent() const
{
	if (m_samples.size() == 0)
	{
		return 0.0f;
	}

	Int lateCount = 0;
	for (Int i = 0; i < m_samples.size(); ++i)
	{
		if (m_samples[i].late)
		{
			++lateCount;
		}
	}
	return 100.0f * lateCount / m_samples.size();
}

ArrivalCushionStats::ArrivalCushionStats()
{
	reset();
}

void ArrivalCushionStats::reset()
{
	m_samples.clear();
}

void ArrivalCushionStats::note(Int cushion, time_t now)
{
	update(now);

	Sample sample;
	sample.time = now;
	sample.cushion = cushion;
	m_samples.push(sample);
}

void ArrivalCushionStats::update(time_t now)
{
	m_samples.prune(now, NETWORK_STATS_WINDOW_MS);
}

Int ArrivalCushionStats::getPercentile(Int percent) const
{
	if (m_samples.size() == 0)
	{
		return 0;
	}

	// counting sort over a clamped range, the values are small frame counts
	enum { LOW = -64, RANGE = 256 };
	Int counts[RANGE];
	for (Int r = 0; r < RANGE; ++r)
	{
		counts[r] = 0;
	}

	for (Int i = 0; i < m_samples.size(); ++i)
	{
		Int cushion = m_samples[i].cushion;
		if (cushion < LOW)
		{
			cushion = LOW;
		}
		if (cushion > LOW + RANGE - 1)
		{
			cushion = LOW + RANGE - 1;
		}
		++counts[cushion - LOW];
	}

	Int target = (m_samples.size() * percent + 99) / 100;
	if (target < 1)
	{
		target = 1;
	}

	Int seen = 0;
	for (Int q = 0; q < RANGE; ++q)
	{
		seen += counts[q];
		if (seen >= target)
		{
			return q + LOW;
		}
	}
	return LOW + RANGE - 1;
}

StallStats::StallStats()
{
	reset();
}

void StallStats::reset()
{
	m_events.clear();
}

void StallStats::note(Int durationMs, UnsignedInt missingSlotMask, time_t now)
{
	update(now);

	Event event;
	event.time = now;
	event.ms = durationMs;
	event.slotMask = missingSlotMask;
	m_events.push(event);
}

void StallStats::update(time_t now)
{
	m_events.prune(now, HISTORY_MS);
}

Int StallStats::getCount(time_t now, Int windowMs) const
{
	Int count = 0;
	for (Int i = 0; i < m_events.size(); ++i)
	{
		if (now - m_events[i].time <= windowMs)
		{
			++count;
		}
	}
	return count;
}

Int StallStats::getCountForSlot(Int slot, time_t now, Int windowMs) const
{
	Int count = 0;
	for (Int i = 0; i < m_events.size(); ++i)
	{
		if ((m_events[i].slotMask & (1u << slot)) && now - m_events[i].time <= windowMs)
		{
			++count;
		}
	}
	return count;
}
