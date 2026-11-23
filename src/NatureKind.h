// Which nature kind a spell's art is - shared by izzydoingit's lighting plugins (master: Tools\Shared Plugin Code).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// His answer 2026-11-22 ("all 5 named"): frost, earth and stone, water, wind and plant spells each have a switch that leaves them unlit.
// A spell's kind is read off its model's name: the first kind whose words the name holds, never with a fire word or one of that kind's
// not-words (names that hold a kind's word by chance - "voice" holds "ice"). One list for both mods: Illuminated's build (lagen
// NATURE_KINDS) reads these arrays off this file, RELight's plugin calls KindOf. Game-free, tested off the game.

#pragma once

#include "GroundElement.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace NatureKind
{
	enum class Kind : std::uint8_t
	{
		kNone,
		kFrost,
		kEarth,
		kWater,
		kWind,
		kPlants
	};

	inline constexpr std::array<std::string_view, 4>  kFrostNot{ "voice", "galvanic", "psychic", "practice" };
	inline constexpr std::array<std::string_view, 3>  kEarthWords{ "earth", "stone", "boulder" };
	inline constexpr std::array<std::string_view, 3>  kEarthNot{ "seidstone", "thunderstone", "shalidor" };
	inline constexpr std::array<std::string_view, 2>  kWaterWords{ "water", "hydro" };
	inline constexpr std::array<std::string_view, 6>  kWindWords{ "wind", "whirl", "cyclone", "gale", "tornado", "tempest" };
	inline constexpr std::array<std::string_view, 1>  kWindNot{ "nightingale" };
	inline constexpr std::array<std::string_view, 16> kPlantWords{ "plant", "vine", "thorn", "flower", "bloom", "leaf", "leaves", "petal",
		"moss", "bramble", "briar", "spore", "pollen", "mushroom", "wildgrowth", "nature" };
	inline constexpr std::array<std::string_view, 5>  kPlantNot{ "divine", "diviner", "bloodthorn", "shockbloom", "profanebloom" };

	// the settings' ids in the menu and the ini, one per kind (both mods name them alike: the sync check reads them here)
	inline constexpr std::array<std::string_view, 5> kSettingKeys{ "FrostLights", "EarthLights", "WaterLights", "WindLights", "PlantLights" };

	[[nodiscard]] inline bool HoldsAny(std::string_view a_low, std::span<const std::string_view> a_words) noexcept
	{
		for (const auto w : a_words) {
			if (a_low.find(w) != std::string_view::npos) {
				return true;
			}
		}
		return false;
	}

	// a model path, any case -> its kind
	[[nodiscard]] inline Kind KindOf(std::string_view a_path)
	{
		// lowered into a buffer on the stack: called on the option walk and the hand dress, so no allocation (a longer path is cut,
		// its kind words sit in the file name at the end, so the end is kept)
		std::array<char, 260> buffer{};
		const auto            keep = (std::min)(a_path.size(), buffer.size());
		const auto            tail = a_path.substr(a_path.size() - keep);
		for (std::size_t i = 0; i < keep; ++i) {
			const char c = tail[i];
			buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}
		const std::string_view low(buffer.data(), keep);
		if (HoldsAny(low, GroundElement::kFireWords)) {
			return Kind::kNone;
		}
		if (HoldsAny(low, GroundElement::kFrostWords) && !HoldsAny(low, kFrostNot)) {
			return Kind::kFrost;
		}
		if (HoldsAny(low, kEarthWords) && !HoldsAny(low, kEarthNot)) {
			return Kind::kEarth;
		}
		if (HoldsAny(low, kWaterWords)) {
			return Kind::kWater;
		}
		if (HoldsAny(low, kWindWords) && !HoldsAny(low, kWindNot)) {
			return Kind::kWind;
		}
		if (HoldsAny(low, kPlantWords) && !HoldsAny(low, kPlantNot)) {
			return Kind::kPlants;
		}
		return Kind::kNone;
	}

	// the index into kSettingKeys of a kind, or -1 for none
	[[nodiscard]] inline int IndexOf(Kind a_kind) noexcept { return a_kind == Kind::kNone ? -1 : static_cast<int>(a_kind) - 1; }
}
