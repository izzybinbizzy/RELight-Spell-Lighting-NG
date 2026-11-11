// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Which kept lights only we and the scene still hold. The scene's own pass takes a light out once its entry there is the
// light's last holder, so a light we keep past that point never leaves: each side waits for the other. And which kept lights
// nothing can draw again: no node and in none of the scene's lists, though a scene light the scene let go of still holds them.
// The game-free part of letting such a light go (Keep.cpp); tested off the game.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace KeptLights
{
	// looks in a row before it goes: a light just made on a loader thread has no node for a moment
	inline constexpr int kLooks = 2;
	// a light in no list at all may be one not hung yet: about half a second of looks, far past that moment
	inline constexpr int kLooksOffScene = 30;

	// a kept light that has lost its node
	struct Seen
	{
		const void*   key{ nullptr };
		std::uint32_t refs{ 0 };
		std::uint32_t entries{ 0 };    // its entries in the scene's active lists and remove queue; each holds one reference
		std::uint32_t elsewhere{ 0 };  // its entries in the scene's other lists (the add queue, the unnamed one)
	};

	// one reference is ours and each entry holds one: nobody else has it. A light with a node never gets here: its parent holds one too
	[[nodiscard]] constexpr bool Unowned(const Seen& a_seen) noexcept { return a_seen.key && a_seen.entries > 0 && a_seen.refs == 1 + a_seen.entries; }

	// no node and no entry in any of the scene's lists: nothing draws it again, whoever else still holds it
	[[nodiscard]] constexpr bool OffScene(const Seen& a_seen) noexcept { return a_seen.key && a_seen.entries == 0 && a_seen.elsewhere == 0; }

	class Watch
	{
	public:
		// the lights seen now -> those to let go; a light not unowned in this look starts again
		[[nodiscard]] std::vector<const void*> Look(std::span<const Seen> a_now)
		{
			std::vector<std::pair<const void*, int>> next;
			std::vector<const void*>                 out;
			for (const auto& seen : a_now) {
				const bool unowned = Unowned(seen);
				if (!unowned && !OffScene(seen)) {
					continue;
				}
				const auto it = std::ranges::find(counts_, seen.key, &std::pair<const void*, int>::first);
				const int  looks = it != counts_.end() ? it->second + 1 : 1;
				if (looks >= (unowned ? kLooks : kLooksOffScene)) {
					out.push_back(seen.key);
				} else {
					next.emplace_back(seen.key, looks);
				}
			}
			counts_ = std::move(next);
			return out;
		}

		[[nodiscard]] bool Waiting() const noexcept { return !counts_.empty(); }

	private:
		std::vector<std::pair<const void*, int>> counts_;
	};
}
