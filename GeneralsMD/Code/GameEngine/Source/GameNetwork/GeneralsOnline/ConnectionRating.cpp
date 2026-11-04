#include "GameNetwork/GeneralsOnline/ConnectionRating.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"

#include <algorithm>
#include <cmath>

// upper limit of each band, index 0 is the limit for 5 bars, above the last is 1 bar
static const Real k_delayLimitsMs[ConnectionRating::BAR_COUNT - 1] = { 150.0f, 225.0f, 300.0f, 500.0f };
static const Real k_stallLimitsPerMinute[ConnectionRating::BAR_COUNT - 1] = { 0.05f, 0.5f, 1.0f, 6.0f };

// Red (1 bar) only for a link that breaks the match, every other weakness is amber at worst:
static const Real k_redDelayMs = k_delayLimitsMs[ConnectionRating::BAR_COUNT - 2];	// input delay the run ahead would need
static const Real k_redStallsPerMinute = k_stallLimitsPerMinute[ConnectionRating::BAR_COUNT - 2];	// measured in a match, a prediction alone is never red
static const Real k_redLossPercent = 20.0f;	// sustained packet loss, more than the redundancy repairs
static const Int k_redSilentMs = 1000;		// no packet at all for this long, a blackout

// Measured stalls of a link whose loss and tail predict almost none are the peer's slow frame rate
static const Real k_slowPcMinStalls = k_redStallsPerMinute;
static const Real k_slowPcMaxLinkStalls = 1.0f;
static const Int k_slowPcMaxBars = 3;

static const Real k_hysteresis = 0.1f;	// a band is left only when the value is this much past its limit

static const UnsignedInt k_raiseHoldMs = 3000;	// more bars are shown only after the rating asked for them this long

// predicted stalls/min = 2.1 x loss% x 110 / (rtt + 60) + 0.1 x max(0, tail - max(100 ms, 0.2 x rtt)), tail = p90 round trip - smoothed round trip, up to 500 ms
static Real PredictStallsPerMinute(const ConnectionStats &stats, Real lossPercent)
{
	const Real tail = std::clamp(stats.getLatencyPercentile(90) - stats.getLatency(), 0.0f, 500.0f);
	const Real headroom = std::max(100.0f, 0.2f * stats.getLatency());
	const Real excess = std::max(0.0f, tail - headroom);
	const Real loss = (lossPercent >= 0.0f) ? lossPercent : stats.getRetryPercent();
	return 2.1f * loss * 110.0f / (stats.getLatency() + 60.0f) + 0.1f * excess;
}

static Real EstimateDelayMs(Real roundTripMs, Int slackPercent, Int sendIntervalMs)
{
	return roundTripMs * (1.0f + slackPercent / 100.0f) + sendIntervalMs;
}

ConnectionRating::Inputs::Inputs()
	: delayMs(0.0f), stallsPerMinute(0.0f), measuredStallsPerMinute(-1.0f), lossPercent(-1.0f), silentMs(-1), silentLimitMs(k_redSilentMs), stallInWindow(FALSE), slowPc(FALSE)
{
}

ConnectionRating::Inputs ConnectionRating::InputsFromProbe(const ConnectionStats &probe, Int slackPercent, Int sendIntervalMs, Real lossPercent)
{
	Inputs inputs;
	// the p75 round trip, not the smoothed one a few spikes pull up
	inputs.delayMs = EstimateDelayMs(probe.getLatencyPercentile(75), slackPercent, sendIntervalMs);
	inputs.stallsPerMinute = PredictStallsPerMinute(probe, lossPercent);
	return inputs;
}

ConnectionRating::Inputs ConnectionRating::InputsFromMatch(const ConnectionStats &cmd, const NetworkOverlayInfo &info, Int slackPercent, Int sendIntervalMs, Real lossPercent)
{
	Inputs inputs;
	const Real roundTripMs = (info.slotInBandMs >= 0) ? (Real)info.slotInBandMs : cmd.getLatency();
	inputs.delayMs = EstimateDelayMs(roundTripMs, slackPercent, sendIntervalMs);

	Real stallsPerMinute = PredictStallsPerMinute(cmd, lossPercent);
	if ((Real)info.stallsMinute >= k_slowPcMinStalls && stallsPerMinute < k_slowPcMaxLinkStalls)
	{
		inputs.stallsPerMinute = stallsPerMinute;
		inputs.slowPc = TRUE;
		return inputs;
	}
	if ((Real)info.stallsMinute > stallsPerMinute)
	{
		stallsPerMinute = (Real)info.stallsMinute; // the minute window holds one minute of stalls
	}
	inputs.stallsPerMinute = stallsPerMinute;
	inputs.measuredStallsPerMinute = (Real)info.stallsMinute;
	inputs.stallInWindow = info.stallsWindow > 0;
	return inputs;
}

static Int RateBand(Int previousBars, Real value, const Real *limits)
{
	const Int count = ConnectionRating::BAR_COUNT;

	Int raw = 1;
	for (Int bars = count; bars >= 2; --bars)
	{
		if (value <= limits[count - bars])
		{
			raw = bars;
			break;
		}
	}

	if (previousBars < 1)
	{
		return raw;
	}

	Int bars = previousBars;
	while (bars < count && value <= limits[count - (bars + 1)] * (1.0f - k_hysteresis))
	{
		++bars;
	}
	while (bars > 1 && value > limits[count - bars] * (1.0f + k_hysteresis))
	{
		--bars;
	}
	return bars;
}

void ConnectionRating::Update(State &state, const Inputs &inputs, UnsignedInt nowMs)
{
	Real stalls = inputs.stallsPerMinute;
	if (inputs.stallInWindow)
	{
		// a stall in the 10 s window rules out the two top bands
		stalls = std::max(stalls, 0.6f);
	}
	else if (inputs.measuredStallsPerMinute > 0.0f)
	{
		// a stall in the last minute rules out the top band
		stalls = std::max(stalls, 0.3f);
	}

	state.delayBars = RateBand(state.delayBars, inputs.delayMs, k_delayLimitsMs);
	state.stabilityBars = RateBand(state.stabilityBars, stalls, k_stallLimitsPerMinute);
	Int rated = std::min(state.delayBars, state.stabilityBars);
	state.redReason = RedReason(inputs);
	if (state.redReason != nullptr)
	{
		rated = 1;
	}
	else if (rated < 2)
	{
		rated = 2;
	}
	state.slowPc = inputs.slowPc;
	if (state.slowPc && rated > k_slowPcMaxBars)
	{
		rated = k_slowPcMaxBars;
	}
	if (state.bars < 1 || rated < state.bars)
	{
		state.bars = rated;
		state.raiseSinceMs = 0;
	}
	else if (rated > state.bars)
	{
		// a noisy input crosses a limit back and forth, it must stay better for the whole hold
		if (state.raiseSinceMs == 0)
		{
			state.raiseSinceMs = nowMs | 1;
		}
		else if (nowMs - state.raiseSinceMs >= k_raiseHoldMs)
		{
			state.bars = rated;
			state.raiseSinceMs = 0;
		}
	}
	else
	{
		state.raiseSinceMs = 0;
	}
	state.delayMs = static_cast<Int>(inputs.delayMs);
	state.stallsPerMinute = stalls;
}

const char *ConnectionRating::RedReason(const Inputs &inputs)
{
	if (inputs.silentMs >= inputs.silentLimitMs)
	{
		return "blackout";
	}
	if (inputs.lossPercent >= k_redLossPercent)
	{
		return "loss";
	}
	if (inputs.delayMs > k_redDelayMs)
	{
		return "delay";
	}
	if (inputs.measuredStallsPerMinute >= k_redStallsPerMinute)
	{
		return "stalls";
	}
	return nullptr;
}

Int ConnectionRating::Severity(Int bars)
{
	if (bars >= 4)
	{
		return SEVERITY_OK;
	}
	if (bars >= 2)
	{
		return SEVERITY_WARN;
	}
	return SEVERITY_BAD;
}

void ConnectionRating::DrawSpinner(Int left, Int top, Int size)
{
	// a faint full ring with a solid arc rotating around it, drawn as short segments
	const Real twoPi = 6.28318531f;
	const Int segmentCount = 24;
	const Int arcSegments = 7;
	const Real radius = size * 0.4f;
	const Real centerX = left + size * 0.5f;
	const Real centerY = top + size * 0.5f;
	const Real rotation = (timeGetTime() % 900) * twoPi / 900.0f;
	const Color ringColor = GameMakeColor(47, 55, 168, 70); // Zero Hour UI blue
	const Color arcColor = GameMakeColor(47, 55, 168, 255);
	for (Int segment = 0; segment < segmentCount; ++segment)
	{
		const Real startAngle = rotation + segment * twoPi / segmentCount;
		const Real endAngle = startAngle + twoPi / segmentCount;
		TheDisplay->drawLine((Int)(centerX + std::cos(startAngle) * radius), (Int)(centerY + std::sin(startAngle) * radius),
			(Int)(centerX + std::cos(endAngle) * radius), (Int)(centerY + std::sin(endAngle) * radius),
			3.0f, segment < arcSegments ? arcColor : ringColor);
	}
}

void ConnectionRating::DrawIndicator(Int left, Int top, Int size, Int bars)
{
	if (bars < 1)
	{
		DrawSpinner(left, top, size);
	}
	else
	{
		DrawBars(left, top, size, bars - 1);
	}
}

ScopedDisplayBatch::ScopedDisplayBatch()
	: m_ownBatch(!TheDisplay->isBatching())
{
	if (m_ownBatch)
	{
		TheDisplay->beginBatch();
	}
}

ScopedDisplayBatch::~ScopedDisplayBatch()
{
	if (m_ownBatch)
	{
		TheDisplay->endBatch();
	}
}

void ConnectionRating::DrawBars(Int left, Int top, Int size, Int level)
{
	Color levelColor = GameMakeColor(220, 40, 40, 255);
	const Int severity = Severity(level + 1);
	if (severity == SEVERITY_OK)
	{
		levelColor = GameMakeColor(0, 200, 0, 255);
	}
	else if (severity == SEVERITY_WARN)
	{
		levelColor = GameMakeColor(255, 190, 0, 255);
	}
	const Color emptyColor = GameMakeColor(90, 90, 90, 200);

	const Int gap = 1;
	const Int barWidth = std::max(1, (size - gap * (BAR_COUNT - 1)) / BAR_COUNT);
	for (Int bar = 0; bar < BAR_COUNT; ++bar)
	{
		const Int barHeight = std::max(1, size * (bar + 1) / BAR_COUNT);
		TheDisplay->drawFillRect(left + bar * (barWidth + gap), top + size - barHeight, barWidth, barHeight,
			bar <= level ? levelColor : emptyColor);
	}
}
