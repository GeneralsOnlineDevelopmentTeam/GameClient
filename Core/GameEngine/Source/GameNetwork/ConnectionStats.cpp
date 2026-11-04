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

// Rules run on window time, so any decision interval behaves the same.
static const Int SPIKE_LAMBDA_MS = 10000;			// a player gives 1 frame of permanent delay for 1/lambda less freeze, the cover pays for spikes recurring faster than this
static const Int SPIKE_FORGET_MS = 500;
static const Real SPIKE_FORGET = 0.983f;			// per SPIKE_FORGET_MS of window time, about a 30 s memory: up after 1.5 s of high windows, down after 90 s
static const Int SPIKE_WINDOW_MIN_MS = 100;
static const Int SPIKE_WINDOW_MAX_MS = 5000;		// a decision interval is 2 x 128 frames at most, a longer window counts as this much
static const Int SPIKE_HEAD_MIN_FRAMES = 4;			// over the baseline that opens an episode, above the normal tail
static const Int SPIKE_HEAD_JITTER_FACTOR = 3;		// or 3 x the round trip jitter when that is more
static const Int SPIKE_TAIL_FRAMES = 1;				// over the baseline that counts as calm again
static const Int SPIKE_TAIL_MS = 1000;				// calm window time in a row that ends an episode
static const Int SPIKE_SUSTAIN_MS = 2000;			// episode window time after its first window that makes it a level shift
static const Int SPIKE_UNDO_MS = SPIKE_SUSTAIN_MS;	// window time after an adoption in which a return to the old baseline undoes it
static const Int SPIKE_WARMUP_MS = SPIKE_SUSTAIN_MS;	// normal window time before the quantile is trusted, the seed stands until then
static const Int SPIKE_HORIZON_MS = 120000;			// the spike rate memory, 3 periods of a 34 s spike
static const Int SPIKE_ALLOWED = SPIKE_HORIZON_MS / SPIKE_LAMBDA_MS;	// spikes taller than the cover allowed in the horizon, a rate of 1 / lambda
static const Int SPIKE_EXTRA_MAX_FRAMES = 12;		// the cover buys 200 ms at most, a taller spike is not affordable
static const Int SAFE_CLEAN_MS = 3000;				// window time the need stays within the target + 1 before a fast drop is safe
static const Int SAFE_HISTORY_MS = 120000;			// a fast drop needs the current spike to be the only one in this much time before it
static const Int SPIKE_DOWN_STEP_FRAMES = 2;		// per decision going down, at least: the engine drains the queued frames without a stall, the step keeps the delay change gradual
static const Int SPIKE_MAX_RAISE_FRAMES = 24;		// most a single decision raises while a slow producer is suspected, loss stalls raise without a bound

// Adds value to an ascending array of count entries.
static void insertSorted(Int *array, Int &count, Int value)
{
	Int pos = count;
	while (pos > 0 && array[pos - 1] > value)
	{
		array[pos] = array[pos - 1];
		--pos;
	}
	array[pos] = value;
	++count;
}

RunAheadSpikeController::RunAheadSpikeController()
{
	reset();
}

void RunAheadSpikeController::reset()
{
	for (Int bucket = 0; bucket < BUCKETS; ++bucket)
	{
		m_hist[bucket] = 0.0f;
		m_saved[bucket] = 0.0f;
	}
	m_mass = 0.0f;
	m_timeMs = 0.0f;
	m_savedMass = 0.0f;
	m_savedTimeMs = 0.0f;
	m_savedBase = 0;
	m_undoMs = 0;
	m_undoCalmMs = 0;
	m_pending.start = 0;
	m_pending.height = 0;
	m_inSpike = FALSE;
	m_spikeStart = 0;
	m_peakNeed = 0;
	m_episodeStallMs = 0;
	m_episodeMs = 0;
	m_calmMs = 0;
	m_sampleCount = 0;
	m_episodeCount = 0;
	m_lastCover = 0;
	m_recentCount = 0;
	m_gateCleanMs = 0;
	m_gateClusterStart = 0;
	m_gatePrevClusterStart = 0;
}

void RunAheadSpikeController::dropEpisode()
{
	m_inSpike = FALSE;
	m_episodeMs = 0;
	m_calmMs = 0;
	m_sampleCount = 0;
	m_undoMs = 0;
	m_gateCleanMs = 0;
}

void RunAheadSpikeController::forget(Real *hist, Real &mass, Real factor)
{
	for (Int bucket = 0; bucket < BUCKETS; ++bucket)
	{
		hist[bucket] *= factor;
	}
	mass *= factor;
}

void RunAheadSpikeController::addSample(Real *hist, Real &mass, Int need)
{
	hist[clamp<Int>(0, need, BUCKETS - 1)] += 1.0f;
	mass += 1.0f;
}

Int RunAheadSpikeController::quantile(const Real *hist, Real mass, Real q)
{
	const Real wanted = q * mass;
	Real sum = 0.0f;
	for (Int bucket = 0; bucket < BUCKETS; ++bucket)
	{
		sum += hist[bucket];
		if (sum >= wanted && sum > 0.0f)
		{
			return bucket;
		}
	}
	return BUCKETS - 1;
}

Int RunAheadSpikeController::baseline(Int runAhead, Int marginFrames) const
{
	if (m_timeMs < SPIKE_WARMUP_MS)
	{
		return runAhead - marginFrames;
	}
	// q = 1 - W / lambda
	const Real q = 1.0f - m_timeMs / (m_mass * (Real)SPIKE_LAMBDA_MS);
	return quantile(m_hist, m_mass, q);
}

void RunAheadSpikeController::recordEpisode(const Episode &episode)
{
	m_episodes[m_episodeCount % EPISODE_RING] = episode;
	++m_episodeCount;
}

RunAheadSpikeController::Decision RunAheadSpikeController::onWindow(time_t now, Int windowMs, Int need, Bool stalled, Int stallMs, Real jitterMs, Int runAhead, Int marginFrames, Real frameMs, Int floorFrames, Int capFrames, Bool safeFastDrop, Bool boundRaise)
{
	Decision d;
	d.runAhead = runAhead;
	d.reason = REASON_HOLD;
	d.fastDrop = FALSE;

	const Int window = clamp<Int>(SPIKE_WINDOW_MIN_MS, windowMs, SPIKE_WINDOW_MAX_MS);
	const Real factor = (Real)pow((double)SPIKE_FORGET, (double)window / (double)SPIKE_FORGET_MS);
	need = clamp<Int>(0, need, BUCKETS - 1);
	m_recentNeed[m_recentCount % RECENT_RING] = need;
	m_recentMs[m_recentCount % RECENT_RING] = window;
	++m_recentCount;

	// every window ages the histograms by the time it covers, spike windows too
	forget(m_hist, m_mass, factor);
	m_timeMs *= factor;
	if (m_undoMs > 0)
	{
		forget(m_saved, m_savedMass, factor);
		m_savedTimeMs *= factor;
	}

	Int base = baseline(runAhead, marginFrames);
	const Int head = max(SPIKE_HEAD_MIN_FRAMES, (Int)ceil(SPIKE_HEAD_JITTER_FACTOR * jitterMs / frameMs));
	Bool undone = FALSE;

	// an adopted level the need does not stay at was a spike after all, the old baseline comes back at once
	if (m_undoMs > 0)
	{
		m_undoMs -= window;
		if (need <= m_savedBase + SPIKE_TAIL_FRAMES && !stalled)
		{
			m_undoCalmMs += window;
			if (m_undoCalmMs >= SPIKE_TAIL_MS)
			{
				for (Int restore = 0; restore < BUCKETS; ++restore)
				{
					m_hist[restore] = m_saved[restore];
				}
				m_mass = m_savedMass;
				m_timeMs = m_savedTimeMs;
				m_undoMs = 0;
				undone = TRUE;
				d.reason = REASON_BASELINE;
				recordEpisode(m_pending);
				base = baseline(runAhead, marginFrames);
			}
		}
		else
		{
			m_undoCalmMs = 0;
		}
	}

	if (!m_inSpike)
	{
		// only normal windows feed the baseline
		if (need > base + head || stalled)
		{
			m_inSpike = TRUE;
			m_spikeStart = now;
			// a spike that opens before SAFE_CLEAN_MS of calm belongs to the same cluster of loss
			if (m_gateClusterStart == 0 || m_gateCleanMs >= SAFE_CLEAN_MS)
			{
				m_gatePrevClusterStart = m_gateClusterStart;
				m_gateClusterStart = now;
			}
			m_peakNeed = need;
			m_episodeStallMs = stallMs;
			m_episodeMs = 0;
			m_calmMs = 0;
			m_sampleTime[0] = now;
			m_sampleNeed[0] = need;
			m_sampleMs[0] = window;
			m_sampleCount = 1;
		}
		else
		{
			addSample(m_hist, m_mass, need);
			m_timeMs += window;
		}
	}
	else
	{
		m_peakNeed = max(m_peakNeed, need);
		m_episodeStallMs += stallMs;
		m_episodeMs += window;
		m_sampleTime[m_sampleCount % SAMPLE_RING] = now;
		m_sampleNeed[m_sampleCount % SAMPLE_RING] = need;
		m_sampleMs[m_sampleCount % SAMPLE_RING] = window;
		++m_sampleCount;
		m_calmMs = (need <= base + SPIKE_TAIL_FRAMES && !stalled) ? (m_calmMs + window) : 0;
		// the height a run ahead would need to cover the whole episode, its stalls split over windows add up
		Episode episode;
		episode.start = m_spikeStart;
		episode.height = max<Int>(0, max(m_peakNeed, runAhead + marginFrames + (Int)ceil(m_episodeStallMs / frameMs)) - base);
		if (m_calmMs >= SPIKE_TAIL_MS)
		{
			recordEpisode(episode);
			m_inSpike = FALSE;
			addSample(m_hist, m_mass, need);
			m_timeMs += window;
		}
		else if (m_episodeMs >= SPIKE_SUSTAIN_MS)
		{
			// level shift: rebase on the median need of the last SUSTAIN
			Int recent[SAMPLE_RING];
			Int recentCount = 0;
			Int recentMs = 0;
			const Int kept = min<Int>(m_sampleCount, SAMPLE_RING);
			for (Int k = 0; k < kept; ++k)
			{
				const Int at = (m_sampleCount - 1 - k) % SAMPLE_RING;
				if ((now - m_sampleTime[at]) < (time_t)SPIKE_SUSTAIN_MS)
				{
					insertSorted(recent, recentCount, m_sampleNeed[at]);
					recentMs += m_sampleMs[at];
				}
			}
			const Int level = (recentCount > 0) ? recent[recentCount / 2] : need;
			for (Int save = 0; save < BUCKETS; ++save)
			{
				m_saved[save] = m_hist[save];
				m_hist[save] = 0.0f;
			}
			m_savedMass = m_mass;
			m_savedTimeMs = m_timeMs;
			m_savedBase = base;
			m_mass = 0.0f;
			m_pending = episode;
			for (Int fill = 0; fill < max<Int>(recentCount, 1); ++fill)
			{
				addSample(m_hist, m_mass, level);
			}
			m_timeMs = (Real)max<Int>(recentMs, SPIKE_WARMUP_MS);
			m_undoMs = SPIKE_UNDO_MS;
			m_undoCalmMs = 0;
			m_inSpike = FALSE;
			d.reason = REASON_CONGESTION;
		}
	}

	const Bool warm = m_timeMs >= SPIKE_WARMUP_MS;
	base = baseline(runAhead, marginFrames);

	// cover = the tallest spike height that more than horizon / lambda episodes reach
	Int heights[EPISODE_RING];
	Int inHorizon = 0;
	const Int stored = min<Int>(m_episodeCount, EPISODE_RING);
	for (Int e = 0; e < stored; ++e)
	{
		const Episode &episode = m_episodes[e];
		if ((now - episode.start) <= SPIKE_HORIZON_MS)
		{
			Int pos = inHorizon;
			while (pos > 0 && heights[pos - 1] < episode.height)
			{
				heights[pos] = heights[pos - 1];
				--pos;
			}
			heights[pos] = episode.height;
			++inHorizon;
		}
	}
	const Int cover = (inHorizon > SPIKE_ALLOWED) ? min(SPIKE_EXTRA_MAX_FRAMES, heights[SPIKE_ALLOWED]) : 0;
	const Int target = clamp<Int>(floorFrames, base + cover, capFrames);

	m_gateCleanMs = (!m_inSpike && !stalled && need <= target + 1) ? (m_gateCleanMs + window) : 0;
	const Bool gateClean = m_gateCleanMs >= SAFE_CLEAN_MS;
	const Bool gateIsolated = m_gatePrevClusterStart == 0 || (m_gateClusterStart - m_gatePrevClusterStart) > SAFE_HISTORY_MS;

	// fast drop: target the recent median need
	Int goal = target;
	Bool fastDrop = FALSE;
	// the gate stays open through our own steps down: they do not touch the clean counter and the reseeded baseline only moves the target
	const Bool gateOpen = safeFastDrop && gateClean && gateIsolated && !m_inSpike && !undone && warm;
	if (gateOpen)
	{
		Int recent[RECENT_RING];
		Int recentCount = 0;
		Int recentMs = 0;
		const Int kept = min<Int>(m_recentCount, RECENT_RING);
		for (Int k = 0; k < kept && recentMs < SAFE_CLEAN_MS; ++k)
		{
			const Int at = (m_recentCount - 1 - k) % RECENT_RING;
			insertSorted(recent, recentCount, m_recentNeed[at]);
			recentMs += m_recentMs[at];
		}
		const Int level = clamp<Int>(floorFrames, recent[recentCount / 2], capFrames);
		if (level < target)
		{
			goal = level;
			fastDrop = TRUE;
		}
	}

	// no change during a spike, a raise decided now arrives after it ended
	if (!m_inSpike && warm)
	{
		if (target > runAhead)
		{
			d.runAhead = boundRaise ? min(target, runAhead + SPIKE_MAX_RAISE_FRAMES) : target;
			if (d.reason != REASON_CONGESTION)
			{
				d.reason = (cover > m_lastCover) ? REASON_COVER : REASON_BASELINE;
			}
		}
		else if (goal < runAhead)
		{
			// fast recovery: a third of the excess per step, never below the goal; with the safe gate open the goal is the recent need,
			// the baseline is reseeded from it and the next step does not wait the long dead time
			const Int step = safeFastDrop ? max(SPIKE_DOWN_STEP_FRAMES, (runAhead - goal + 2) / 3) : SPIKE_DOWN_STEP_FRAMES;
			if (gateOpen)
			{
				d.fastDrop = TRUE;
			}
			if (fastDrop)
			{
				for (Int reseed = 0; reseed < BUCKETS; ++reseed)
				{
					m_hist[reseed] = 0.0f;
				}
				m_mass = 0.0f;
				addSample(m_hist, m_mass, goal);
				m_timeMs = (Real)SPIKE_WARMUP_MS;
			}
			d.runAhead = undone ? target : max(goal, runAhead - step);
			if (d.reason != REASON_CONGESTION && !undone)
			{
				d.reason = (cover < m_lastCover) ? REASON_UNCOVER : REASON_BASELINE;
			}
		}
	}
	m_lastCover = cover;

	const Int sizeFrames = (inHorizon > 0) ? heights[inHorizon / 2] : 0;
	d.base = base;
	d.cover = cover;
	d.spikesPerMin10 = inHorizon * 10 * 60000 / SPIKE_HORIZON_MS;
	d.spikeSizeMs = (Int)(sizeFrames * frameMs + 0.5f);
	d.inSpike = m_inSpike;
	return d;
}
