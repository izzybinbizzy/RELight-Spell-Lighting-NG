// RELight - Spell Lighting NG - the fading module's own names
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The fading module is the same code in Illuminated and in RELight - Spell Lighting NG (the two never run together, so each
// carries its own copy). This header is each mod's own: the mod's name, its folders and its DevBench names. Every other Fade*
// file, FadeDevBench.cpp included, is one file with its master in Tools\Shared Plugin Code\ (`PC Runner\shared code sync.py`).
// Game-free: the fading tests include it.

#pragma once

namespace Fade::Mod
{
	inline constexpr const char* kName = "RELight - Spell Lighting NG";                                                                           // the menu section and the log lines
	inline constexpr const char* kSettingsPath = "Data/SKSE/Plugins/RELightSpellLightingNG/Fading.ini";                                           // the settings file
	inline constexpr const char* kRulesDir = "Data/SKSE/Plugins/RELightSpellLightingNG/Fading";                                                   // the rule files (*.json)
	inline constexpr const char* kRulesDirText = "Data\\SKSE\\Plugins\\RELightSpellLightingNG\\Fading";                                           // the same, as a player reads it
	inline constexpr const char* kItemsPath = "Data/SKSE/Plugins/RELightSpellLightingNG/Lights by Item.ini";                                      // FadeItems.cpp (his order)
	inline constexpr const char* kAdvancedPath = "Data/SKSE/Plugins/RELightSpellLightingNG/RELight - Spell Lighting NG - Advanced Settings.ini";  // FadeTuning.cpp (his rule)
	// the player's own lines, in no download: they win over the file and an update never replaces them
	inline constexpr const char* kAdvancedCustomPath = "Data/SKSE/Plugins/RELightSpellLightingNG/RELight - Spell Lighting NG - Advanced Settings - Custom.ini";
	inline constexpr const char* kHudArtDir = "Data/SKSE/Plugins/RELightSpellLightingNG/HUD/";  // FadeMenu.cpp's soul gem sheets
	// the same three files under the old name, read only while the new one is not there yet (SettingsText::ReadFrom; HIS ORDER
	// 2026-10-20: "i want relight update to not affect old settings files") - never written, moved or deleted
	inline constexpr const char* kOldSettingsPath = "Data/SKSE/Plugins/RelightSpellAddon/Fading.ini";
	inline constexpr const char* kOldItemsPath = "Data/SKSE/Plugins/RelightSpellAddon/Lights by Item.ini";
	inline constexpr const char* kOldAdvancedPath = "Data/SKSE/Plugins/RelightSpellAddon/RELight - Spell Addon - Advanced Settings.ini";
	// the fading module's DevBench names (FadeDevBench.cpp): its inspect kind and menu alias, its tool, its event
	inline constexpr const char* kDevBenchKey = "relightfade";
	inline constexpr const char* kDevBenchTool = "relightfade.control";
	inline constexpr const char* kDevBenchRulesEvent = "relightfade.rulesLoaded";
}
