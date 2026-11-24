// GroundElement - which element a spell left on the ground is, and its strength and reach from the Advanced Settings file
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Game-free (tests\test_fade.cpp), one file in Illuminated and RELight - Spell Lighting NG (master in Tools\Shared Plugin Code\).
// A ground spell's element is read off its model's (or record's) name: fire first, then frost, then shock, else other - so Fire
// Storm stays fire and Ice Storm frost. lagen.py reads the same word lists from this file for Illuminated's light copies; RELight
// asks it for each hazard's model. The values are the Advanced Settings file's [Ground Effects] section, on top of the menu's
// Ground effects strength and the Reach slider; a plugin that registers none keeps 1 everywhere.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace GroundElement
{
	enum class Element : std::uint8_t
	{
		kOther,
		kFire,
		kFrost,
		kShock
	};
	inline constexpr std::size_t kElements = 4;

	inline constexpr std::array<std::string_view, 10> kFireWords{ "fire", "flame", "burn", "inferno", "ember", "magma", "lava", "blaze", "pyro", "immolat" };
	inline constexpr std::array<std::string_view, 8>  kFrostWords{ "frost", "ice", "cold", "snow", "blizzard", "freez", "chill", "glaci" };
	inline constexpr std::array<std::string_view, 8>  kShockWords{ "shock", "lightning", "spark", "storm", "thunder", "electr", "volt", "static" };

	// the element a name says (a model path or an editor ID), case ignored: fire, then frost, then shock, else other
	[[nodiscard]] inline Element Of(std::string_view a_name)
	{
		std::string low(a_name);
		for (auto& c : low) {
			c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}
		const auto any = [&low](const auto& a_words) {
			for (const auto w : a_words) {
				if (low.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		};
		if (any(kFireWords)) {
			return Element::kFire;
		}
		if (any(kFrostWords)) {
			return Element::kFrost;
		}
		return any(kShockWords) ? Element::kShock : Element::kOther;
	}

	// a light copy's kind= text (lagen.row_kind): "ground-fire", "ground-frost", "ground-shock"; anything else is other
	[[nodiscard]] inline Element OfKind(std::string_view a_kind)
	{
		if (a_kind == "ground-fire") {
			return Element::kFire;
		}
		if (a_kind == "ground-frost") {
			return Element::kFrost;
		}
		return a_kind == "ground-shock" ? Element::kShock : Element::kOther;
	}

	// [Ground Effects] in the Advanced Settings file: one reach for every ground spell, then a strength and a reach per element
	struct Tune
	{
		std::atomic<float>                        reach{ 1.0f };
		std::array<std::atomic<float>, kElements> strength{ 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<std::atomic<float>, kElements> elementReach{ 1.0f, 1.0f, 1.0f, 1.0f };
	};
	inline Tune gTune;

	[[nodiscard]] inline float StrengthOf(Element a_e) { return gTune.strength[static_cast<std::size_t>(a_e)].load(std::memory_order_relaxed); }

	[[nodiscard]] inline float ReachOf(Element a_e)
	{
		return gTune.reach.load(std::memory_order_relaxed) * gTune.elementReach[static_cast<std::size_t>(a_e)].load(std::memory_order_relaxed);
	}
}
