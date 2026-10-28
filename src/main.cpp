// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version. See LICENSE.
//
// What it does: RE::Light lights the spells, sprays, beams and enchantments through the configs this mod's installer puts
// down; this DLL does what RE::Light has no route for - once, when the game has finished loading its plugins, and then every
// frame through its hooks (Hooks.cpp):
//   - a hand light for every spell whose casting art a layer lights, and an automatic one for a spell no layer lights
//     (HandLights.cpp); off Community Shaders a twin of each lights the ground (GroundLights.cpp);
//   - a weapon's or staff's own light while it is drawn (Held.cpp); the sprays' and beams' light records kept to them (Streams.cpp);
//   - the body glow while casting: a light on art the game hangs on an actor - a cloak, a heal (BodyLights.cpp);
//   - one ward, one dome (Wards.cpp); VAER Reborn's swirls given back (VaerSwirls.cpp);
//   - the menu's switches, Brightness and Reach, lights fading while sneaking, "Hand lights for", and the fading module.
// Every choice is a setting in the menu (SKSE Menu Framework - the only menu this mod has); the few values the menu does not
// show are in the one advanced settings file (Fade::Tuning, registered below).
//
// THE FILES, AND WHAT EACH ONE IS FOR
//   main.cpp        this file - the order of work, and the SKSE messages
//   Hooks.cpp       the hooks, and spell lights fading while you sneak
//   Lighting.cpp    Community Shaders, ENB or Vanilla - found by itself (Illuminated's, the same tests)
//   Data.cpp        the data files the installer put down: what this mod lights, its switches, its streams
//   Settings.cpp    the settings file
//   Options.cpp     the switches - an option's lights put out while the game runs
//   Brightness.cpp  our own brightness slider, which scales this mod's lights and nothing else
//   Streams.cpp     sprays, breath shouts and beams: RE::Light lights them, the menu reaches their lights
//   Held.cpp        a weapon's own light while it is drawn (RE::Light reaches a weapon in the hand only by enchantment),
//                   made and registered the ReLight way (MakeOwnLight)
//   BodyLights.cpp  the body glow while casting: a light on art the game hangs on an actor (Truman's way, the held lights' code)
//   Wards.cpp       one ward, one dome, and the ward color pick - all left to Dynamic Wards when it is loaded
//   HandLights.cpp  the light on the caster's hands, made in memory - no plugin, no script
//   GroundLights.cpp off Community Shaders, a twin of each hand light that lights the ground (Illuminated's)
//   Keep.cpp        the one place a light the game made is held for the lists above, and let go when it leaves the game
//   DataText.h      the data files' line reader, game-free (tested; relightgen.py read_data_text is its twin)
//   Plugin.h        what the files share; PCH.h what they all include; LightKit.h and LightMath.h the small helpers
//                   Illuminated carries too
//   MenuStyle.h, Translation.h, SKSEMenuFramework.h  the shared menu look and translation (word for word in our other
//                   plugins), and SKSE Menu Framework's own header
//   VaerSwirls.cpp  once at data loaded: VAER Reborn's own effects get its art and shader back where a later plugin
//                   replaced them, Thaumaturgy's own enchantment effects wear VAER's swirl, and .dds.dds shader textures
//                   find their file
//   Fade*.cpp       the fading module (the same code as Illuminated's; FadeConfig.h and FadeDevBench.cpp are this mod's
//                   parts): weapon and staff lights follow their charge, a spell's hand light your magicka - its own
//                   settings, rules, pages
//   Menu.cpp        the settings page, in SKSE Menu Framework's Mod Control Panel
//   DevBench.cpp    inspect kind=relight and the tool relight.control, when DevBench is loaded (optional; Illuminated's way);
//                   DevBenchGlue.h and DevBenchAPI.* are the shared copies (master: Dynamic Wards' src\)

#include "Fade.h"
#include "Plugin.h"
#include "RuntimeGuard.h"

namespace
{
	// ------------------------------------------------------------------ order of work
	void LoadEverything()
	{
		const auto loadStarted = std::chrono::steady_clock::now();
		// the advanced settings file's own values (his rule) - read with the fading module's, in Fade::OnDataLoaded
		Fade::Tuning::Register("Lights", "NearbyDistance", 2800.0f, 300.0f, 30000.0f, "Hand lights for - Everyone nearby: how far from you, in game units",
			Plugin::gNearby);
		Fade::Tuning::Register("Vanilla and ENB", "GroundLights", 1.0f, 0.0f, 1.0f,
			"1: off Community Shaders a hand light lights the ground too (its twin, made with land lighting on); 0: off", Plugin::gGroundLightsOn);
		if (REL::Module::IsVR()) {
			// only the VR build's file carries the line, as Illuminated's
			Fade::Tuning::Register("Vanilla and ENB", "GroundLightAtVRHand", 0.0f, 0.0f, 1.0f,
				"VR only - 1: a hand's ground light hangs at your VR hand; 0: at the body's hand (the default)", Plugin::gGroundLightVRHand);
		}
		Plugin::LoadSettings(false);  // the art picks and Dynamic Lighting decide which data files count
		Plugin::NoteArtPacks();       // the art packs the game's own files are: "Find my art pack" picks before the data is read
		Plugin::FixDynamicLighting();
		Plugin::LoadData();
		Plugin::LoadSettings();             // again: the option switches need the options the data made
		Plugin::ReadLighting();             // before the hand lights: off Community Shaders they are drawn plain
		Plugin::RaiseStreamLights();        // 2.3: the sprays' and bolts' raised copies - before the hand lights remember each effect's own light
		Plugin::ApplyWards("data loaded");  // before the hand lights: a silenced ward effect must not get one
		Plugin::MakeHandLights();
		Plugin::FindSpellBases();   // before the late hooks: the 3D-load hooks read it from loader threads
		Plugin::VaerSwirls();       // after LoadData: it needs to know whether the VAER Reborn option is installed
		LightKit::FleshFx::Find();  // which flesh spells wear FleshFX's look: their lights take its hue (Brightness.cpp)
		Plugin::InstallLateHooks();
		Plugin::RegisterMenu();
		Fade::OnDataLoaded();  // after the menu: its two pages go under our section, after ours
		Plugin::RegisterMenuTail();
		SKSE::log::info("done in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStarted).count());
	}

	// an exception never leaves into the game: one thrown by the load work (a file it cannot read, memory) is logged, and
	// what already ran keeps its work
	void OnDataLoaded()
	{
		try {
			LoadEverything();
		} catch (const std::exception& e) {
			SKSE::log::critical(
				"the data-load work stopped part way: {} - what ran before it keeps its work, the rest is not "
				"done this session",
				e.what());
		}
	}

	// the old game's twins, body, held and row lights and the fading let go: at a load, and at a new game (it has no
	// kPreLoadGame)
	void ForgetOldGame()
	{
		Plugin::DropGroundLights();
		Plugin::DropBodyLights();
		Plugin::DropHeldLights();
		Plugin::DropRows();
		Fade::OnGameLoading();
	}

	void Handle(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Fade::OnPluginLoad();       // the fading module: editor IDs recorded before the game reads its plugins (still before)
			Plugin::OfferToDevBench();  // inspect kind=relight and relight.control, when DevBench is loaded (optional)
			Fade::OnPostLoad();
			// after every plugin has loaded: the hook written last runs first, and ours must run before RE::Light's
			Plugin::InstallSpellLightHooks();
			// Dynamic Wards 2.0 changes a ward's casting art from its menu; its hand light follows. Only when it is loaded:
			// SKSE refuses a listener for a plugin that is not, and CommonLib logs that as an error players report (2.1)
			if (Plugin::WardsSteppedDown()) {
				SKSE::GetMessagingInterface()->RegisterListener("DynamicWards", [](SKSE::MessagingInterface::Message* a_m) {
					// handled on the main thread, whichever thread Dynamic Wards sends from
					if (auto* tasks = a_m && a_m->type == 'DWAC' ? SKSE::GetTaskInterface() : nullptr) {
						tasks->AddTask([]() {
							LightKit::Guarded("Dynamic Wards message", []() {
								Plugin::RefindHandLights();
								Plugin::ForgetBodyArt();  // the casting art the body lights leave out moved too
							});
						});
					}
				});
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			ForgetOldGame();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
				ForgetOldGame();
			}
			Plugin::ApplyWards("a save loaded");  // a ward mod that resets art at load may have put it back
			Plugin::RefindHandLights();           // Dynamic Wards 2.0's ranked hand art, set at data load in whichever order
			break;
		default:
			break;
		}
	}

	// an exception never leaves into the game from any message either (kDataLoaded's work has its own, more telling, guard)
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg) {
			LightKit::Guarded("SKSE message", [a_msg]() { Handle(a_msg); });
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// ONE trampoline for every call this DLL wraps (the spell-light call sites, up to 4, + the cell animations + the fading
	// module's two charge-bar calls, 14 bytes each, room to spare): a second SKSE::AllocTrampoline would replace the buffer
	// under live hooks
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 160 });
	// the wrong game: loaded but idle - a false here makes SKSE add its own "exit game?" box over the guard's
	if (!RuntimeGuard::RightGame("RELight - Spell Lighting NG", "RELight - Spell Lighting NG", "RELight - Spell Lighting NG - VR")) {
		return true;
	}
	// the spell-light hooks are installed at SKSE's post-load (OnMessage), after every plugin has loaded
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	SKSE::log::info("RELight - Spell Lighting NG plugin loaded; waiting for the game's data");
	return true;
}
