// Shared by Illuminated and RELight - Spell Lighting NG (the master: Tools\Shared Plugin Code\StrangeRunes.h)
// GPL-3.0-or-later; see each plugin's LICENSE.txt.
//
// Strange Runes (powerofthree, Nexus 19456) hangs a rune on a casting hand after a cast: a constant ability whose Light effect
// carries a light of its own for about 30 s. Beside our hand light that is two lights on one hand, so by default ours lights the
// hand and the rune's effect carries a dark copy of its light; the mod's switch gives the hands back to the rune (and the plugin
// puts its own hand light out there). Done on the records at data load and on a setting change: no hook and no pass.

#pragma once

#include "Fade.h"
#include "LightKit.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace StrangeRunes
{
	struct Rune
	{
		RE::EffectSetting* effect{ nullptr };
		RE::TESForm*       light{ nullptr };
		RE::TESObjectLIGH* dark{ nullptr };
	};

	// found once at data load, read and written on the main thread only
	[[nodiscard]] inline std::vector<Rune>& Runes()
	{
		static std::vector<Rune> runes;
		return runes;
	}

	// a hand rune is named po3_RUNE_<kind>EffectL / EffectR; the ritual runes (...EffectRitual) and the ward dummies are left as
	// they are - two rituals share a hand rune's light, so the effect's name decides, not the light's
	[[nodiscard]] inline bool HandRuneEffect(const std::string& a_editorId)
	{
		const std::string id = LightKit::Lower(a_editorId);
		return id.starts_with("po3_rune_") && (id.ends_with("effectl") || id.ends_with("effectr"));
	}

	// the same light with no strength and the smallest reach: the rune's effect still finds a light to make, it lights nothing
	// (radius 1, never 0, so no attenuation divides by zero)
	[[nodiscard]] inline RE::TESObjectLIGH* DarkCopy(const RE::TESObjectLIGH* a_light)
	{
		auto* dark = LightKit::NewForm<RE::TESObjectLIGH>();
		if (dark) {
			dark->data = a_light->data;
			dark->data.radius = 1;
			dark->fade = 0.0f;
			// a light that lights nothing must not take one of the scene's few shadow slots either
			dark->data.flags.reset(RE::TES_LIGHT_FLAGS::kSpotShadow, RE::TES_LIGHT_FLAGS::kHemiShadow, RE::TES_LIGHT_FLAGS::kOmniShadow);
		}
		return dark;
	}

	// data load, main thread: every hand rune's Light effect of StrangeRunes.esp, with one dark copy per light. -> how many
	inline std::size_t Find()
	{
		auto& runes = Runes();
		runes.clear();
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data || !data->LookupModByName("StrangeRunes.esp")) {
			return 0;
		}
		std::unordered_map<RE::TESForm*, RE::TESObjectLIGH*> darks;
		for (auto* effect : data->GetFormArray<RE::EffectSetting>()) {
			if (!effect || effect->GetArchetype() != RE::EffectSetting::Archetype::kLight || !HandRuneEffect(Fade::EditorID(effect))) {
				continue;
			}
			auto* form = effect->data.associatedForm;
			auto* light = form ? form->As<RE::TESObjectLIGH>() : nullptr;
			if (!light) {
				continue;
			}
			auto [it, made] = darks.try_emplace(form, nullptr);
			if (made) {
				it->second = DarkCopy(light);
			}
			if (it->second) {
				runes.push_back({ effect, form, it->second });
			}
		}
		return runes.size();
	}

	[[nodiscard]] inline bool Loaded() { return !Runes().empty(); }

	// the switch for its hand rune lights asked of Strange Runes' author (no public repo, so asked through the mod page): once a
	// release has it, the global is set and his records are left alone - the dark copies are only the stopgap until then
	inline constexpr const char* kTheirSwitch = "po3_RUNE_HandLightsToggle";

	// main thread: a_theirs = the runes light the hands as Strange Runes made them; false = their dark copies. A rune already
	// on a hand keeps the light it had until it ends
	inline void Apply(bool a_theirs)
	{
		if (auto* theirs = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kTheirSwitch)) {
			theirs->value = a_theirs ? 1.0f : 0.0f;
			return;
		}
		for (const auto& rune : Runes()) {
			rune.effect->data.associatedForm = a_theirs ? rune.light : rune.dark;
		}
	}
}
