// RELight - Spell Lighting NG - the ground twins' own names and hooks (GroundLights.cpp is one file in Illuminated and RELight,
// master in Tools\Shared Plugin Code\; what differs between the two mods is here)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.

#pragma once

#include "CreatureSpells.h"
#include "Plugin.h"

namespace Plugin::Ground
{
	inline constexpr const char* kLightName = "RELightSpellLightingNGGroundLight";

	// off Community Shaders only: under it, inverse square or linear, the casting light lights the ground itself
	[[nodiscard]] inline bool Wanted() { return !LightLimitFix(); }

	// Performance's switch: every light of the mod off
	[[nodiscard]] inline bool AllOff() { return AllLightsOff(); }

	// a light the mod keeps dark now (a switched-off option, a hand light held out) keeps its twin dark too
	[[nodiscard]] inline bool WantedDark(RE::NiLight* a_light) { return Plugin::WantedDark(a_light); }

	// a far caster's light kept out by the budget: no twin
	[[nodiscard]] inline bool KeptOut(RE::NiLight* a_light) { return KeptOutByBudget(a_light); }

	// RE::Light's flicker prevention lets a marked light reach every surface (Plugin.h)
	inline void Mark(RE::NiPointLight* a_light) { a_light->fadeAmount = kMovingLightMark; }

	// a hold may have outlasted the moment that ended the light: without a look now, Keep.cpp holds it until the next one
	inline void LetGo() { NoteSweep(SweepWhy::kMagicDrop); }

	// anyone with hands out, from the draw events (Held.cpp keeps the list)
	// anyone besides the player with hands out who gets a ground light (not a creature: ForEachDrawn skips those)
	[[nodiscard]] inline bool OthersDrawn()
	{
		return std::ranges::any_of(DrawnActorIds(), [](RE::FormID a_id) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			return actor && !actor->IsPlayerRef() && !CreatureSpells::IsCreature(actor);
		});
	}

	// the player, then the actors with their hands out (Held.cpp keeps the list, earlier in this update) still in high process; a
	// creature's lights are Creatures', so it gets no ground light
	template <class F>
	void ForEachDrawn(F&& a_visit)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		a_visit(player);
		for (const auto id : DrawnActorIds()) {
			auto*       actor = RE::TESForm::LookupByID<RE::Actor>(id);
			const auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
			if (actor && actor != player && process && process->InHighProcess() && !CreatureSpells::IsCreature(actor)) {
				a_visit(actor);
			}
		}
	}
}
