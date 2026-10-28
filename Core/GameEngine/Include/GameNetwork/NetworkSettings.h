#pragma once

#if defined(GENERALS_ONLINE)

#include <string>

// Per-match game networking policy, independent of online-service lifetime.
struct NetworkSettings
{
	struct Send
	{
		int frames_per_packet = 2;      // send interval in frames
		bool flush_every_frame = true;  // flush GNS after each frame
	} send;
	struct InputDelay
	{
		int min_frames = 4;
		int decision_interval_ms = 5000;
		int fps_history_samples = 30;   // per-second FPS samples averaged
		int slack_percent = 30;         // percent added to the measured round trip, also sets the start run ahead and the rating

		struct Formula
		{
			int latency_history_samples = 200; // in-band ping samples averaged
		} formula;
	} input_delay;

	static NetworkSettings LocalDefaults()
	{
		return NetworkSettings();
	}
};

#endif
