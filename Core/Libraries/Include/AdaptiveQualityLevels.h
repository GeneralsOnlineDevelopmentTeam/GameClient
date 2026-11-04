#pragma once
// What each adaptive quality level takes away. Client only: the user's options keep their values.

namespace AdaptiveQualityLevels
{
	enum
	{
		LEVEL_NONE = 0,
		LEVEL_PARTICLES = 1,		// medium particle LOD, no heat effects, 3/4 of the particle cap
		LEVEL_SHADOW_VOLUMES = 2,	// shadow volumes not drawn
		LEVEL_RENDER_SKIP = 3,		// every 2nd frame is not rendered
		LEVEL_MINIMUM = 4,			// low particle LOD, no light pulses, no shadow decals, half the particle cap
		LEVEL_COUNT = 5
	};

	static const int kMinParticleCap = 500;

	inline bool heatEffectsOff(int level) { return level >= LEVEL_PARTICLES; }
	inline bool shadowVolumesOff(int level) { return level >= LEVEL_SHADOW_VOLUMES; }
	inline bool rendersSkipped(int level) { return level >= LEVEL_RENDER_SKIP; }
	inline bool shadowDecalsOff(int level) { return level >= LEVEL_MINIMUM; }
	inline bool lightPulsesOff(int level) { return level >= LEVEL_MINIMUM; }

	// never above the user's own cap, never below kMinParticleCap unless the user's cap is lower
	inline int particleCap(int userCap, int level)
	{
		int scaled = userCap;
		if (level >= LEVEL_MINIMUM)
		{
			scaled = userCap / 2;
		}
		else if (level >= LEVEL_PARTICLES)
		{
			scaled = userCap * 3 / 4;
		}
		const int floor = userCap < kMinParticleCap ? userCap : kMinParticleCap;
		return scaled > floor ? scaled : floor;
	}
}
