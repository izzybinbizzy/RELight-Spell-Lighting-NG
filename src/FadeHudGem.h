#pragma once

#include <array>
#include <cstdint>

// The charge gems in the game's own HUD layer: a small Flash movie of ours
// (Interface\InfinityUI\HUDMenu\HUDMovieBaseInstance\<IlluminatedGem|RELightGem>.swf) that Infinity UI loads into the HUD's
// own movie, so the HUD hides and fades the gems with itself. This file drives that movie; when it is not there (no Infinity
// UI, a HUD without the clip, VR) the ImGui gems of FadeMenu.cpp draw as before.

namespace Fade::HudGem
{
	struct Gem
	{
		bool          shown{ false };
		float         fill{ 1.0f };     // 0..1
		std::uint32_t rgb{ 0xFFFFFF };  // 0xRRGGBB
		bool          operator==(const Gem&) const = default;
	};
	struct Look
	{
		float scale{ 1.0f };  // the HUD page's Gem size
		float alpha{ 0.9f };  // Gem opacity, 0..1
		bool  percent{ true };
		bool  operator==(const Look&) const = default;
	};

	// the HUD-layer gems answered since the HUD last opened: the ImGui gems stand down
	[[nodiscard]] bool Live() noexcept;
	// render thread: what the gems should show now. Sends only a change, on the UI thread. While not Live it asks the movie a
	// few times after the HUD opens, then stops until the HUD opens again
	void Publish(const std::array<Gem, 2>& a_gems, const Look& a_look, bool a_hide);
	// data load: the HUD menu opening / closing starts and ends a session with the movie
	void Register();
	// HUD NG places the gems: the movie is never asked again, and one that already answered is hidden. Any thread
	void StandDown();
}
