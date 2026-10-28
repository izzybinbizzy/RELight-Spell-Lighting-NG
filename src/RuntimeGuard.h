// RuntimeGuard.h - shared word for word by our SKSE plugins (master: Soul Gem Hub's src\, as DevBenchGlue.h is Dynamic Wards').
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, as the plugin that carries it.
//
// A DLL built for Skyrim SE / AE that Skyrim VR loads - or the VR build that SE / AE loads - reads the other game's addresses and
// crashes the game while it loads, and CommonLib cannot tell (a build for one game has IsVR fixed). This reads only the game's
// own file version (CommonLib's reading: minor 4 = VR, 5 = SE, 6 and up = AE), never a game address. Call it first thing in
// SKSEPluginLoad, right after SKSE::Init: on the wrong game it names the file to install in a Windows box and in the log; the
// plugin's load then returns TRUE and does nothing else (a false makes SKSE add its own "exit game?" box over this one).
#pragma once

#include "REX/W32/USER32.h"

namespace RuntimeGuard
{
	// the words for the wrong game; a_vrFile is empty when the mod has no VR version
	[[nodiscard]] inline std::string WrongGameText(bool a_gameIsVr, std::string_view a_mod, std::string_view a_mainFile, std::string_view a_vrFile)
	{
		if (!a_gameIsVr) {
			return std::format(
				"This is the Skyrim VR version of {}, but your game is Skyrim SE / AE.\n\nIt stays off, so your game "
				"does not crash. Remove the VR download \"{}\" and install only the main download \"{}\".",
				a_mod, a_vrFile, a_mainFile);
		}
		if (a_vrFile.empty()) {
			return std::format(
				"This is the Skyrim SE / AE version of {}, but your game is Skyrim VR.\n\n{} has no Skyrim VR version "
				"yet, so it stays off and your game does not crash. Remove it from your Skyrim VR setup.",
				a_mod, a_mod);
		}
		return std::format(
			"This is the Skyrim SE / AE version of {}, but your game is Skyrim VR.\n\nIt stays off, so your game does "
			"not crash. Install the optional VR download \"{}\" over the main download.",
			a_mod, a_vrFile);
	}

	// true when this DLL was built for the game that runs; otherwise the box and the log line, and false
	[[nodiscard]] inline bool RightGame(std::string_view a_mod, std::string_view a_mainFile, std::string_view a_vrFile) noexcept
	{
#if defined(ENABLE_SKYRIM_VR) && (defined(ENABLE_SKYRIM_SE) || defined(ENABLE_SKYRIM_AE))
		return true;  // one DLL built for every game
#else
#	ifdef ENABLE_SKYRIM_VR
		constexpr bool builtForVr = true;
#	else
		constexpr bool builtForVr = false;
#	endif
		const bool gameIsVr = REL::Module::get().version()[1] == 4;
		if (gameIsVr == builtForVr) {
			return true;
		}
		try {
			const auto text = WrongGameText(gameIsVr, a_mod, a_mainFile, a_vrFile);
			SKSE::log::error("{}", text);
			const auto              title = std::format("{} - the wrong version for your game", a_mod);
			constexpr std::uint32_t kWarningOnTop = 0x30 | 0x40000;  // MB_ICONWARNING | MB_TOPMOST, one OK button
			REX::W32::MessageBoxA(nullptr, text.c_str(), title.c_str(), kWarningOnTop);
		} catch (...) {
		}
		return false;
#endif
	}
}
