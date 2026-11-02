// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// The settings file, Mod::kSettingsPath (FadeConfig.h; its lines: FadeSettingsText.h). Read once at data load, written
// whenever the menu changes a setting.
//
// Threads: the menu changes the settings on the render thread and DevBench on its own, while the main thread reads them
// every frame. The one copy here is behind a lock, and everyone else works on a copy (Config / SetConfig). The two
// switches hooks read per call are mirrored in atomics whenever the copy changes.

#include "Fade.h"

#include <sstream>

namespace Fade
{
	namespace
	{
		std::mutex        gLock;
		Settings          gSettings;
		std::atomic<bool> gDebugLog{ false }, gHideChargeBar{ false };

		void Mirror(const Settings& a_s) noexcept
		{
			gDebugLog.store(a_s.debugLog, std::memory_order_relaxed);
			// with Fading off nothing else shows a weapon's charge, so the HUD's bar comes back (his answer 2026-10-13)
			gHideChargeBar.store(a_s.hideChargeBar && a_s.enabled, std::memory_order_relaxed);
		}
	}

	Settings Config()
	{
		std::lock_guard lock(gLock);
		return gSettings;
	}

	void SetConfig(const Settings& a_settings)
	{
		std::lock_guard lock(gLock);
		gSettings = a_settings;
		Mirror(gSettings);
	}

	bool ApplySetting(std::string_view a_key, int a_value)
	{
		std::lock_guard lock(gLock);
		const bool      known = SettingsText::Apply(gSettings, a_key, a_value);
		Mirror(gSettings);
		return known;
	}

	bool DebugLogOn() noexcept { return gDebugLog.load(std::memory_order_relaxed); }

	bool HideChargeBarOn() noexcept { return gHideChargeBar.load(std::memory_order_relaxed); }

	Preview& PreviewState()
	{
		static Preview preview;
		return preview;
	}

	void LoadSettings()
	{
		Settings                 s;
		SettingsText::ReadResult read;
		const auto               from = SettingsText::ReadFrom(Mod::kSettingsPath, Mod::kOldSettingsPath);
		if (std::ifstream in(from); in) {
			read = SettingsText::Read(in, s);
		}
		for (const auto& p : read.problems) {
			SKSE::log::warn("settings: {}", p);
		}
		SetConfig(s);
		if (read.moved) {
			SKSE::log::info("settings: moved to the new fading defaults once (curve linear, reach follows 100%)");
			SaveSettings();
		}
		const auto& t = s.tuning;
		SKSE::log::info(
			"settings: {} line(s) read from {}; enabled {}, empty brightness {}%, curve {}, reach follows {}%, "
			"sputter {} below {}%, pulse {}, flare {}, cooling {}, staves {}, bound {} ({} s), who {}, dim glow {}, hide charge bar {}",
			read.taken, from, s.enabled, SettingsText::ToPct(t.floor), static_cast<int>(t.curve), SettingsText::ToPct(t.reachFollows),
			t.sputter, SettingsText::ToPct(t.sputterBelow), t.pulse, t.flare, t.cool, s.staves, s.bound, s.boundFadeSeconds,
			static_cast<int>(s.who), s.dimShader, s.hideChargeBar);
	}

	void SaveSettings()
	{
		std::ostringstream text;
		SettingsText::Write(text, Config());
		// written beside the file, then moved over it: a crash or a full disk mid-write leaves the old settings, never half a file
		const std::string tmp = std::string(Mod::kSettingsPath) + ".tmp";
		std::error_code   ec;
		// the folder the installer ships; made here too, so a missing one never loses a setting
		std::filesystem::create_directories(std::filesystem::path(Mod::kSettingsPath).parent_path(), ec);
		std::ofstream out(tmp, std::ios::trunc);  // text mode: Windows line ends, as Notepad writes
		if (!out) {
			SKSE::log::warn("settings: {} could not be written", Mod::kSettingsPath);
			return;
		}
		out << text.str();
		out.close();
		if (!out) {
			SKSE::log::warn("settings: {} could not be written", Mod::kSettingsPath);
		} else if (std::filesystem::rename(tmp, Mod::kSettingsPath, ec); ec) {
			SKSE::log::warn("settings: {} could not be replaced ({})", Mod::kSettingsPath, ec.message());
		}
		std::filesystem::remove(tmp, ec);  // nothing left behind when the move failed
	}
}
