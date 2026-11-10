// The Floor Light Fix, shared by our lighting mods (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.
//
// Where the 1.6.1170 exe has what the fix hooks, and the read of a call site - game-free, so the off-game tests reach it
// (FloorLightChecks.h).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace FloorLightMath
{
	// offsets from the base of the 1.6.1170 exe (Address Library ids in brackets)
	namespace Game1170
	{
		// the array erase (106419)
		inline constexpr std::uintptr_t kErase = 0x14A83F0;
		// the per-light pass (106335) and its call onto the erase
		inline constexpr std::uintptr_t kPass = 0x14A1540;
		inline constexpr std::uintptr_t kPassCall = 0x732;
		// the drain of RemoveLight's queue (106333) and its call onto the erase
		inline constexpr std::uintptr_t kDrain = 0x14A1370;
		inline constexpr std::uintptr_t kDrainCall = 0x134;
		// BSLight::ClearGeometries (108285)
		inline constexpr std::uintptr_t kClear = 0x1509A00;
	}

	// where the 5-byte relative call (E8 rel32) at a_site goes; nothing when the bytes are not one
	[[nodiscard]] inline std::optional<std::uintptr_t> CallTarget(const std::array<std::uint8_t, 5>& a_bytes, std::uintptr_t a_site) noexcept
	{
		if (a_bytes[0] != 0xE8) {
			return std::nullopt;
		}
		std::int32_t rel = 0;
		std::memcpy(&rel, a_bytes.data() + 1, sizeof(rel));
		return a_site + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
	}

	// the first of the sites that no longer holds the game's call onto the erase, or nothing when every one does: the first of
	// our mods to load hooks them all, so each later copy finds them taken and stands down (as it does behind another plugin's hook)
	template <std::size_t N, class Read>
	[[nodiscard]] std::optional<std::size_t> FirstTaken(const std::array<std::uintptr_t, N>& a_sites, std::uintptr_t a_erase, Read a_read)
	{
		for (std::size_t i = 0; i < N; ++i) {
			if (CallTarget(a_read(a_sites[i]), a_sites[i]) != a_erase) {
				return i;
			}
		}
		return std::nullopt;
	}

	// [first, last) of the lights the game's erase takes: a count equal to the size takes them all, whatever the index (read in the
	// exe); otherwise the range from the index, clamped to the array here (the game's erase does not clamp it)
	[[nodiscard]] constexpr std::pair<std::uint32_t, std::uint32_t> ErasedRange(std::uint32_t a_index, std::uint32_t a_count, std::uint32_t a_size) noexcept
	{
		if (a_count == a_size) {
			return { 0, a_size };
		}
		if (a_index >= a_size) {
			return { a_size, a_size };
		}
		return { a_index, a_index + (std::min)(a_count, a_size - a_index) };
	}
}
