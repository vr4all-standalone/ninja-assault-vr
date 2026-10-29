/* One-time migration for the global displayed gun angle. GPL v2 or later. */
#pragma once
#include "cfg/cfg.h"

namespace vr::gun_angle
{
// OptionType is Option<int, false>: old per-game angle entries cannot reappear
// after a game-specific settings load. The headset menu already saves globally.
template<class OptionType>
class Setting : public OptionType
{
public:
	Setting() : OptionType("vr.GunAngle", 0) {}
	using OptionType::operator=;

	void load() override
	{
		OptionType::load();
		if (config::loadInt("config", "vr.GunAngleZeroVersion", 0) >= 1)
			return;
		// Discard every legacy angle, including explicit positive/negative choices.
		// Persist the angle before the marker, so an interrupted write is retryable.
		this->set(0);
		config::saveInt("config", "vr.GunAngle", 0);
		config::saveInt("config", "vr.GunAngleZeroVersion", 1);
	}
};
}
