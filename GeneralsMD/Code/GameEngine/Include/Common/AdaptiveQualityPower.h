#pragma once
// What the Windows power state says about the speed of this PC. Pure classification, no system calls.

struct AqGuid
{
	unsigned long d1;
	unsigned short d2;
	unsigned short d3;
	unsigned char d4[8];
};

inline bool aqGuidEqual(const AqGuid& a, const AqGuid& b)
{
	if (a.d1 != b.d1 || a.d2 != b.d2 || a.d3 != b.d3)
	{
		return false;
	}
	for (int i = 0; i < 8; ++i)
	{
		if (a.d4[i] != b.d4[i])
		{
			return false;
		}
	}
	return true;
}

namespace AdaptiveQualityPower
{
	// power plan "Power saver"
	static const AqGuid kPowerSaverPlan = { 0xA1841308, 0x3541, 0x4FAB, { 0xBC, 0x81, 0xF7, 0x15, 0x56, 0xF2, 0x0B, 0x4A } };
	// power mode overlay "Best power efficiency"
	static const AqGuid kBestEfficiencyOverlay = { 0x961CC777, 0x2547, 0x4F9D, { 0x81, 0x74, 0x7D, 0x86, 0x18, 0x1B, 0x8A, 0x7A } };
	static const AqGuid kBalancedPlan = { 0x381B4222, 0xF694, 0x41F0, { 0x96, 0x85, 0xFF, 0x5B, 0xB2, 0x60, 0xDF, 0x2E } };
	static const AqGuid kHighPerformancePlan = { 0x8C5E7FDA, 0xE8BF, 0x4A96, { 0x9A, 0x85, 0xA6, 0xE2, 0x3A, 0x8C, 0x63, 0x5C } };
	static const AqGuid kUltimatePlan = { 0xE9A42B02, 0xD5DF, 0x448D, { 0xAA, 0x00, 0x03, 0xF1, 0x47, 0x49, 0xEB, 0x61 } };

	struct State
	{
		bool onBattery;
		bool batterySaver;
		bool powerSaverPlan;
		bool bestEfficiencyMode;
		bool savesPower() const { return batterySaver || powerSaverPlan || bestEfficiencyMode; }
	};

	// the three fields of SYSTEM_POWER_STATUS plus the active plan and the effective power mode overlay, either may be unknown (null)
	inline State classify(unsigned char acLineStatus, unsigned char batteryFlag, unsigned char systemStatusFlag, const AqGuid* plan, const AqGuid* overlay)
	{
		State s;
		// AC line 0 is offline; battery flag 128 is no battery and 255 is unknown, a desktop is never on battery
		s.onBattery = acLineStatus == 0 && batteryFlag != 128 && batteryFlag != 255;
		s.batterySaver = (systemStatusFlag & 1) != 0;
		s.powerSaverPlan = plan != nullptr && aqGuidEqual(*plan, kPowerSaverPlan);
		s.bestEfficiencyMode = overlay != nullptr && aqGuidEqual(*overlay, kBestEfficiencyOverlay);
		return s;
	}

	inline const char* planName(const AqGuid* plan)
	{
		if (plan == nullptr)
		{
			return "unknown";
		}
		if (aqGuidEqual(*plan, kPowerSaverPlan))
		{
			return "power saver";
		}
		if (aqGuidEqual(*plan, kBalancedPlan))
		{
			return "balanced";
		}
		if (aqGuidEqual(*plan, kHighPerformancePlan))
		{
			return "high performance";
		}
		if (aqGuidEqual(*plan, kUltimatePlan))
		{
			return "ultimate performance";
		}
		return "custom";
	}
}
