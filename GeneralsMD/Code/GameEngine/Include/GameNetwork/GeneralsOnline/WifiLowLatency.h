#pragma once

// Turns on Wi-Fi media streaming mode for the adapter that carries the game traffic while a match runs; best effort, a no-op without Wi-Fi.
class WifiLowLatency
{
public:
	static void Begin();	///< Match start, returns at once.
	static void End();		///< Match end or shutdown, waits 250 ms at most for the worker.
};
