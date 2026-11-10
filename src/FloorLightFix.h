// The Floor Light Fix, shared by our lighting mods (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.
//
// The game erases a light from the scene's active list in two places without taking it off the floors it lit; only the light's
// destructor does that, so while anything else still holds the light the floors keep drawing it. Before each of those erases this
// calls the game's own ClearGeometries, the call the same pass already makes for a hidden light. Every mod of ours carries it and
// calls Install; the first to load hooks both erase calls, every later copy finds them taken and stands down.
#pragma once

#include <cstddef>

namespace FloorLightFix
{
	// the room its two call hooks take in the plugin's SKSE trampoline (a 14-byte jump each): added to the plugin's SKSE::InitInfo
	inline constexpr std::size_t kTrampolineBytes = 2 * 14;

	// once, at SKSE load, after RuntimeGuard::RightGame; never throws, and logs one line: where it went in, or why not
	void Install() noexcept;
}
