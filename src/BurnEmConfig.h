// BurnEmConfig.h - RELight's side of the shared BurningBanners.cpp: the light's name and its numbers under RELight's sliders.
#pragma once

#include "Plugin.h"

namespace Plugin::BurnEm
{
	inline constexpr const char* kLightName = "RELightSpellLightingNGBannerFire";
	inline constexpr const char* kModName = "RELight";

	// Brightness x Ground effect strength; 0 with every light of the mod off (Performance)
	[[nodiscard]] inline float LightStrength() { return AllLightsOff() ? 0.0f : static_cast<float>(BrightnessPercent()) / 100.0f * StrengthOf(Strength::kGround); }
	[[nodiscard]] inline float ReachScale() { return static_cast<float>(ReachPercent()) / 100.0f; }
	// "Burn'em Banners lights its banners" off: our light, theirs put out
	[[nodiscard]] inline bool OursLight() { return !BurnEmBannersLightsItself(); }
}
