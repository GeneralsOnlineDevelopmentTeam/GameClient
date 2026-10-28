#pragma once

#if defined(GENERALS_ONLINE)

#include <string>

// Per-match game networking policy, independent of online-service lifetime.
struct NetworkSettings
{
	struct Send
	{
		std::string mode = "datagram";  // "reliable" = GNS retransmits, "datagram" = unreliable datagrams
		bool direct = true;             // every player sends commands straight to each peer while the mesh is up
		int frames_per_packet = 2;      // send interval in frames
		int frames_per_packet_lossy = 1; // send interval in frames while the redundancy depth is above 0, 0 = keep the configured one
		bool flush_every_frame = true;  // flush GNS after each frame
		bool redundancy = true;         // repeat un-acked commands in packets that go out anyway
		int redundancy_window_ms = 0;   // wait before a command is repeated, 0 = max(2 x smoothed rtt, 100 ms)
		int redundancy_burst_ms = 150;  // longest burst of lost packets repaired, caps the depth
		bool fast_resend = true;        // resend after srtt + 2 x jitter (at most 400 ms) instead of 4 x jitter (at most 1 s)

		bool IsDatagram() const { return mode == "datagram"; }
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
