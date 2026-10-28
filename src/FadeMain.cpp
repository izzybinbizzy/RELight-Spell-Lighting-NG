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

namespace
{
	// The player's update, each reference effect's update (0x3B) and the cell's animation call are wrapped ONCE, by the
	// plugin, which calls UpdateHands / AfterReferenceEffect / ReapplyAll after its own Brightness work (two wrappers on one
	// slot would leave their order to who installed last).

	// ------------------------------------------------------------------ after the game animates an effect shader
	// ShaderReferenceEffect::Update (0x28) writes this frame's fill and rim alpha; the shader dimming runs after it.
	struct ShaderUpdate
	{
		static bool thunk(RE::ShaderReferenceEffect* a_this, float a_delta)
		{
			const bool result = func(a_this, a_delta);
			LightKit::Guarded("fading: effect shader update", [a_this]() { Fade::AfterShaderEffect(a_this); });
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::ShaderReferenceEffect::VTABLE[0] };
			func = vtbl.write_vfunc(0x28, thunk);
		}
	};

	// ------------------------------------------------------------------ the HUD's enchantment charge bar
	// HUDChargeMeter::Update hands the HUD movie its numbers in two calls to SetChargeMeterPercent(percent, force,
	// leftHand, show) - the two calls TrueHUD also reads for its own bar (ersh1/TrueHUD, Hooks.h). With HideChargeBar on,
	// both go out with show = false, which the vanilla HUD, SkyHUD and TrueHUD all take as "hide the bar". Installed at
	// data load, after TrueHUD hooked the same calls at plugin load, so TrueHUD is handed what we pass on.
	// SE/AE only: the call sites are known for those two. Called every frame: it reads the two lock-free switches only.
	template <int N>
	struct ChargeBar
	{
		static bool thunk(RE::GFxValue::ObjectInterface* a_this, void* a_data, RE::GFxValue* a_result, const char* a_name,
			const RE::GFxValue* a_args, RE::UPInt a_count, bool a_isDisplayObject)
		{
			const bool hide = Fade::HideChargeBarOn();
			if (Fade::DebugLogOn() && a_args && a_count == 4 && a_args[0].IsNumber() && a_args[2].IsBool() && a_args[3].IsBool()) {
				LightKit::Guarded("charge bar log", [&]() {
					// the debug log: each change of what the game hands the HUD (it calls this every frame)
					static std::tuple<int, bool, bool> last[2]{ { -1, false, false }, { -1, false, false } };
					const double                       percent = a_args[0].GetNumber();
					const int                          shown = std::isfinite(percent) ? static_cast<int>(std::clamp(percent, -1.0e6, 1.0e6)) : -1;
					const std::tuple<int, bool, bool>  now{ shown, a_args[2].GetBool(), a_args[3].GetBool() };
					if (std::exchange(last[N], now) != now) {
						SKSE::log::info("charge bar ({}): {}% {} hand, show {}{}{}", N, std::get<0>(now), std::get<1>(now) ? "left" : "right",
							std::get<2>(now), hide ? " - hidden by " : "", hide ? Fade::Mod::kName : "");
					}
				});
			}
			if (a_args && a_count == 4 && hide) {
				RE::GFxValue args[4]{ a_args[0], a_args[1], a_args[2], a_args[3] };
				args[3].SetBoolean(false);
				return func(a_this, a_data, a_result, a_name, args, a_count, a_isDisplayObject);
			}
			return func(a_this, a_data, a_result, a_name, a_args, a_count, a_isDisplayObject);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		[[nodiscard]] static bool                      Present(std::ptrdiff_t a_offset)
		{
			const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50771, 51666), a_offset };  // HUDChargeMeter::Update
			return LightKit::IsCallAt(target.address());
		}
		static void Install(std::ptrdiff_t a_offset)
		{
			const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50771, 51666), a_offset };
			func = SKSE::GetTrampoline().write_call<5>(target.address(), thunk);
		}
	};

	void InstallChargeBar()
	{
		if (REL::Module::IsVR()) {
			SKSE::log::info("VR: the charge bar cannot be hidden");
			return;
		}
		// both calls are checked before either is written: never half the bar hooked
		constexpr std::ptrdiff_t kFirst = 0x168, kSecond = 0x2B3;
		if (!ChargeBar<0>::Present(kFirst) || !ChargeBar<1>::Present(kSecond)) {
			SKSE::log::info("the charge bar's updates are not where expected (another plugin rewrote them?); it cannot be hidden");
			return;
		}
		ChargeBar<0>::Install(kFirst);
		ChargeBar<1>::Install(kSecond);
		SKSE::log::info("the charge bar's two updates hooked");
	}
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
		// the trampoline for the two charge-bar calls is the plugin's (SKSE::Init, made once)
		InstallEditorIDHooks();  // before the game reads its plugins: the rule files may name forms by editor ID
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
		LoadSettings();
		// the advanced settings file (his rule): the fading module's own values, then the file read and written back
		// whole (the mod registered its own values before this - its main.cpp)
		using namespace Tuning;
		Register("Fading", "MinReach", 0.35f, 0.0f, 1.0f, "the least share of its reach a fading light keeps at empty", gMinReach);
		Register("Fading", "PulseSeconds", 0.30f, 0.05f, 2.0f, "how long the flash of a hit that spends charge lasts, in seconds", gPulseSeconds);
		Register("Fading", "FlareSeconds", 0.70f, 0.1f, 3.0f, "how long the swell of a recharge lasts, in seconds", gFlareSeconds);
		Register("Fading", "FallSeconds", 0.15f, 0.0f, 2.0f, "how quickly the light follows a falling charge, in seconds", gFallSeconds);
		Register("Fading", "RiseSeconds", 0.35f, 0.0f, 3.0f, "how quickly the light follows a rising charge, in seconds", gRiseSeconds);
		Register("HUD", "GemDistance", 215.0f, 40.0f, 900.0f, "how far each charge gem sits from the middle of the screen, in pixels at 1080p", gGemDistance);
		Register("HUD", "GemHeight", 62.0f, 0.0f, 1000.0f, "how far above the bottom of the screen the charge gems sit, in pixels at 1080p", gGemHeight);
		Tuning::Load();
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
		InstallChargeBar();
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
