#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>

// Per-peer packet envelope after the channel byte: u16 sequence, u8 flags (bit 7 always set, bit 0 = feedback record follows),
// optional feedback { u16 highest seen, u8 expected, u8 received, u8 longest lost run } over the last 64 sequence numbers,
// sent at most every 250 ms.
class PacketFeedback
{
public:
	enum
	{
		ENVELOPE_BASE_SIZE = 3,
		FEEDBACK_SIZE = 5,
		ENVELOPE_MAX_SIZE = ENVELOPE_BASE_SIZE + FEEDBACK_SIZE
	};

	struct Summary
	{
		Summary() : samples(0), lossPercent(0.0f), longestRun(0) {}

		int samples;			///< Reports in the window, 0 when there is nothing to say.
		float lossPercent;
		int longestRun;			///< Longest run of consecutive lost packets in any report.
	};

	PacketFeedback();

	void Reset();

	size_t WriteEnvelope(uint8_t* out, int64_t nowMs);

	/// The envelope written last never left this machine, its sequence number is not a lost packet.
	void CancelEnvelope();

	/// Returns the envelope size, 0 if malformed.
	size_t ReadEnvelope(const uint8_t* in, size_t len, int64_t nowMs);

	Summary GetOutgoing(int64_t nowMs, int windowMs) const;

	Summary GetIncoming(int64_t nowMs, int windowMs) const;

private:
	struct Report
	{
		int64_t timeMs;
		uint8_t expected;
		uint8_t received;
		uint8_t longestRun;
	};

	static void Push(std::deque<Report>& reports, const Report& report, int64_t nowMs);
	static Summary Summarize(const std::deque<Report>& reports, int64_t nowMs, int windowMs);

	enum { MAX_WINDOW_MS = 60000, FEEDBACK_INTERVAL_MS = 250 };

	uint16_t m_nextSequence;
	uint32_t m_sentCount;

	bool m_haveHighest;
	uint16_t m_highest;
	uint64_t m_mask;			///< Bit n is set when highest - n was received.
	uint8_t m_span;			///< How many sequence numbers of the window have been seen, at most 64.
	int64_t m_lastFeedbackMs;

	std::deque<Report> m_outgoing;	///< reports from the peer about our packets
	std::deque<Report> m_incoming;	///< taken when we send our record
};
