// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// An enchanted weapon's light follows its charge, a spell hand's light and glow follow the caster's magicka: full when
// charged, dimmer as it is used, a sputter near empty, a pulse when a hit spends charge, a flare when a soul gem refills
// it, a cooler color near empty. A bound weapon follows the time its spell has left. Its settings: the menu's "Fading"
// pages and Mod::kSettingsPath; its rule files: Mod::kRulesDir\*.json. The plugin's main.cpp calls the four entry points below.
//
// THE FILES: FadeConfig.h (the mod's own names) and FadeDevBench.cpp (its DevBench glue) - the two each mod has its own
// of; FadeGlow.h (the behaviour as plain numbers), FadeFormText.h / FadeSettingsText.h / FadeRulesText.h (text),
// FadeCharge.cpp (what a hand holds), FadeSpells.cpp (spells in hand), FadeLights.cpp (the lights found and scaled every
// frame; the HUD state), FadeItems.cpp (Lights by Item), FadeTuning.cpp (the all-in-one advanced settings file),
// FadeRules.cpp, FadeSettings.cpp, FadeEditorIDs.cpp, FadeMenu.cpp, FadeArcs.cpp (the bars offered to Reticle Arcs),
// Fade.h (what they share), FadeMain.cpp (this file: its own hooks and the order).

#include "Fade.h"
#include "LightKit.h"
#include "Po3Tweaks.h"

namespace
{
	// The player's update, each reference effect's update (0x3B) and the cell's animation call are wrapped ONCE, by the
	// plugin, which calls UpdateHands / AfterReferenceEffect / ReapplyAll after its own Brightness work (two wrappers on one
	// slot would leave their order to who installed last).

	// ------------------------------------------------------------------ after the game animates an effect shader
	// ShaderReferenceEffect::Update (0x28) writes this frame's fill and rim alpha; the shader dimming runs after it.
	// the game's Update calls UpdatePosition (0x3B) before it writes the alpha, so this cannot move there
	struct ShaderUpdate
	{
		static bool thunk(RE::ShaderReferenceEffect* a_this, float a_delta)
		{
			const bool result = func(a_this, a_delta);
			// every effect shader in the loaded world, every frame: this one test before anything else
			if (Fade::ShaderGlowsWanted()) {
				LightKit::Guarded("fading: effect shader update", [a_this]() { Fade::AfterShaderEffect(a_this); });
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::ShaderReferenceEffect::VTABLE[0] };
			func = vtbl.write_vfunc(0x28, thunk);
		}
	};

}

namespace Fade
{
	namespace
	{
		// Reticle Arcs is loaded: the module's bars are offered to it (FadeArcs.cpp) - since his order of 2026-10-19 the only
		// reticle there is. Written once on the main thread at SKSE's post-load
		std::atomic<bool> gOtherReticle{ false };
	}

	bool OtherReticleLoaded() { return gOtherReticle.load(std::memory_order_relaxed); }

	void OnPluginLoad()
	{
		// every SKSE plugin is loaded by now, so another mod's DLL is either here or not coming
		gOtherReticle.store(REX::W32::GetModuleHandleA("ReticleArcs.dll") != nullptr, std::memory_order_relaxed);
		if (OtherReticleLoaded()) {
			SKSE::log::info("Reticle Arcs is loaded: the fading module's bars are offered to it");
		}
	}

	void OnPostLoad()
	{
		OfferToDevBench();
		RegisterArcBars();  // after OnPluginLoad read whether Reticle Arcs is here
	}

	void OnDataLoaded()
	{
		Po3Tweaks::Judge(Mod::kName);  // powerofthree's Tweaks is required for every editor ID: the log line, and one box without it
		LoadSettings();
		// the advanced settings file (his rule): the fading module's own values, then the file and the player's Custom file read
		// (the mod registered its own values before this - its main.cpp)
		using namespace Tuning;
		Register("Fading with Magicka", "MinReach", 0.35f, 0.0f, 1.0f, "How much of its reach a fading light keeps at empty charge or magicka, as a share (0.35 is about a third).", gMinReach);
		Register("Fading with Magicka", "PulseSeconds", 0.30f, 0.05f, 2.0f, "How long the flash lasts when a hit spends a weapon's charge, in seconds.", gPulseSeconds);
		Register("Fading with Magicka", "FlareSeconds", 0.70f, 0.1f, 3.0f, "How long the light swells when a soul gem recharges a weapon, in seconds.", gFlareSeconds);
		Register("Fading with Magicka", "FallSeconds", 0.15f, 0.0f, 2.0f, "How quickly a light dims as the charge or magicka drops, in seconds.", gFallSeconds);
		Register("Fading with Magicka", "RiseSeconds", 0.35f, 0.0f, 3.0f, "How quickly a light comes back as the charge or magicka returns, in seconds.", gRiseSeconds);
		Register("HUD", "GemDistance", 215.0f, 40.0f, 900.0f, "How far each charge gem sits from the middle of the screen when the HUD shows no charge meters or bars to sit by, in pixels at 1080p.", gGemDistance);
		Register("HUD", "GemHeight", 62.0f, 0.0f, 1000.0f, "How high above the bottom of the screen the charge gems sit when the HUD shows no charge meters or bars to sit by, in pixels at 1080p.", gGemHeight);
		// the log's level (spdlog's order: 0 trace, 1 debug, 2 info, 3 warn, 4 error); the plugins write no trace or debug
		// lines, so 0 and 1 log the same as 2
		static std::atomic<float> logLevel{ 2.0f };
		Register("General", "LogLevel", 2.0f, 0.0f, 4.0f,
			"How much the log file writes: 2 or lower everything (the usual), 3 warnings only, 4 errors only. Keep it at 2 when you send a log with a bug report.",
			logLevel);
		Tuning::Load();
		spdlog::set_level(static_cast<spdlog::level::level_enum>(std::clamp(static_cast<int>(std::lround(Get(logLevel))), 0, 4)));
		{
			auto s = Config();
			s.tuning.minReach = Get(gMinReach);
			s.tuning.pulseSeconds = Get(gPulseSeconds);
			s.tuning.flareSeconds = Get(gFlareSeconds);
			s.tuning.fallSeconds = Get(gFallSeconds);
			s.tuning.riseSeconds = Get(gRiseSeconds);
			SetConfig(s);
		}
		LoadRules();
		Items::BuildCatalog();
		Items::Load();
		ShaderUpdate::Install();
		WatchHands();
		SKSE::log::info("fading: hooked effect shaders; the player update, effect updates and cell animations are the plugin's wrappers");
		RegisterMenu();
	}

	void OnGameLoading()
	{
		// the weapons about to load (or a new game's) are new copies: forget the old ones, putting their lights back first,
		// and the rule matches of enchantments made in play
		ReleaseAll();
		ForgetRuleMatches();
	}
}
