// FadePickerAPI.h - shared word for word by Illuminated, RELight and Illuminated - Creature Lighting NG (master: Tools\Shared Plugin Code).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, as the plugin that carries it.
//
// Illuminated's and RELight's Choose Your Own Color page, offered to another DLL of ours: it adds its own items to the page and reads
// back what the player picked. Plain C types only, so the two DLLs need not share a CommonLib build. The provider exports kRequest.
#pragma once

#include <cstdint>

namespace FadePicker
{
	inline constexpr std::uint32_t kVersion = 1;
	inline constexpr const char*   kRequest = "RequestFadePickerAPI";

	struct Item
	{
		const char* key{ nullptr };   // the item's line in the saved file: the same in every load order, no '='
		const char* name{ nullptr };  // as the page shows it
	};

	struct API
	{
		// main thread, data load only; the text is copied
		void (*addItems)(const Item* a_items, std::uint32_t a_count, const char* a_group);
		// main thread: false for Auto. a_rgb is the picked color (0..1) when *a_colored
		bool (*choiceFor)(const char* a_key, float* a_rgb, bool* a_off, bool* a_colored);
		// one listener, called on the main thread when an added item's choice changes; it must only note and return
		void (*listen)(void (*a_changed)());
	};

	using Request = const API* (*)(std::uint32_t a_version);
}
