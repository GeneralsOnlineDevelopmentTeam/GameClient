#include "GameNetwork/GeneralsOnline/PacketFeedback.h"
#include "GameNetwork/GeneralsOnline/NetBytes.h"

#include <algorithm>

PacketFeedback::PacketFeedback()
{
	Reset();
}

void PacketFeedback::Reset()
{
	m_nextSequence = 0;
	m_sentCount = 0;
	m_haveHighest = false;
	m_highest = 0;
	m_mask = 0;
	m_span = 0;
	m_lastFeedbackMs = 0;
	m_outgoing.clear();
	m_incoming.clear();
}

size_t PacketFeedback::WriteEnvelope(uint8_t* out, int64_t nowMs)
{
	WriteU16(out, m_nextSequence);
	++m_nextSequence;
	++m_sentCount;
	out[2] = 0x80;

	if (!m_haveHighest || (m_lastFeedbackMs != 0 && nowMs - m_lastFeedbackMs < FEEDBACK_INTERVAL_MS))
	{
		return ENVELOPE_BASE_SIZE;
	}

	int received = 0;
	int longestRun = 0;
	int run = 0;
	for (int bit = 0; bit < m_span; ++bit)
	{
		if (m_mask & (static_cast<uint64_t>(1) << bit))
		{
			++received;
			run = 0;
		}
		else
		{
			++run;
			longestRun = (std::max)(longestRun, run);
		}
	}

	out[2] |= 0x01;
	WriteU16(out + 3, m_highest);
	out[5] = m_span;
	out[6] = static_cast<uint8_t>(received);
	out[7] = static_cast<uint8_t>(longestRun);

	Report report;
	report.timeMs = nowMs;
	report.expected = m_span;
	report.received = static_cast<uint8_t>(received);
	report.longestRun = static_cast<uint8_t>(longestRun);
	Push(m_incoming, report, nowMs);

	m_lastFeedbackMs = nowMs;
	return ENVELOPE_MAX_SIZE;
}

void PacketFeedback::CancelEnvelope()
{
	if (m_sentCount > 0)
	{
		--m_nextSequence;
		--m_sentCount;
	}
}

size_t PacketFeedback::ReadEnvelope(const uint8_t* in, size_t len, int64_t nowMs)
{
	if (in == nullptr || len < ENVELOPE_BASE_SIZE || (in[2] & 0x80) == 0)
	{
		return 0;
	}

	const bool hasFeedback = (in[2] & 0x01) != 0;
	if (hasFeedback && len < ENVELOPE_MAX_SIZE)
	{
		return 0;
	}

	const uint16_t sequence = ReadU16(in);
	if (!m_haveHighest)
	{
		m_haveHighest = true;
		m_highest = sequence;
		m_mask = 1;
		m_span = 1;
	}
	else
	{
		const int diff = static_cast<int16_t>(static_cast<uint16_t>(sequence - m_highest));
		if (diff > 0)
		{
			m_mask = (diff >= 64) ? 0 : (m_mask << diff);
			m_mask |= 1;
			m_highest = sequence;
			m_span = static_cast<uint8_t>((std::min)(64, m_span + diff));
		}
		else if (diff < 0 && -diff < 64)
		{
			m_mask |= static_cast<uint64_t>(1) << (-diff);
		}
	}

	if (!hasFeedback)
	{
		return ENVELOPE_BASE_SIZE;
	}

	Report report;
	report.timeMs = nowMs;
	report.expected = in[5];
	report.received = in[6];
	report.longestRun = in[7];
	if (report.expected > 64 || report.received > report.expected)
	{
		return ENVELOPE_MAX_SIZE; // implausible, the packet is still consumed
	}

	// the peer cannot have expected more of our packets than we sent
	report.expected = static_cast<uint8_t>((std::min<uint32_t>)(report.expected, m_sentCount));
	report.received = (std::min)(report.received, report.expected);
	Push(m_outgoing, report, nowMs);
	return ENVELOPE_MAX_SIZE;
}

void PacketFeedback::Push(std::deque<Report>& reports, const Report& report, int64_t nowMs)
{
	reports.push_back(report);
	while (!reports.empty() && nowMs - reports.front().timeMs > MAX_WINDOW_MS)
	{
		reports.pop_front();
	}
}

PacketFeedback::Summary PacketFeedback::Summarize(const std::deque<Report>& reports, int64_t nowMs, int windowMs)
{
	Summary summary;
	int expected = 0;
	int received = 0;
	for (const Report& report : reports)
	{
		if (nowMs - report.timeMs > windowMs)
		{
			continue;
		}
		++summary.samples;
		expected += report.expected;
		received += report.received;
		summary.longestRun = (std::max)(summary.longestRun, static_cast<int>(report.longestRun));
	}

	if (expected > 0)
	{
		summary.lossPercent = 100.0f * static_cast<float>(expected - received) / static_cast<float>(expected);
	}
	return summary;
}

PacketFeedback::Summary PacketFeedback::GetOutgoing(int64_t nowMs, int windowMs) const
{
	return Summarize(m_outgoing, nowMs, windowMs);
}

PacketFeedback::Summary PacketFeedback::GetIncoming(int64_t nowMs, int windowMs) const
{
	return Summarize(m_incoming, nowMs, windowMs);
}
