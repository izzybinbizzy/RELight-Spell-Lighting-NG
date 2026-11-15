// Shared by Illuminated and RELight - Spell Lighting NG (the master: Tools\Shared Plugin Code\StrangeRunes.h)
// GPL-3.0-or-later; see each plugin's LICENSE.txt.
//
// Strange Runes (powerofthree, Nexus 19456) hangs a rune on a casting hand after a cast: a constant ability whose Light effect
// carries a light of its own for about 30 s. Beside our hand light that is two lights on one hand, so by default ours lights the
// hand and the rune's light is out; the mod's switch gives the hands back to the rune (and the plugin puts its own hand light out
// there). Done on the records once at data load: a Light effect that names no light makes none, so no hook and no pass.

#pragma once

#include "Fade.h"

#include <string>
#include <vector>

namespace StrangeRunes
{
	struct Rune
	{
		RE::EffectSetting* effect{ nullptr };
		RE::TESForm*       light{ nullptr };
	};

	// found once at data load, read and written on the main thread only
	[[nodiscard]] inline std::vector<Rune>& Runes()
	{
		static std::vector<Rune> runes;
		return runes;
	}

	// a hand rune's light is po3_RUNE_<kind>Light; the ritual and ward runes are left as they are
	[[nodiscard]] inline bool HandRuneLight(std::string a_editorId)
	{
		for (auto& c : a_editorId) {
			c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		}
		return a_editorId.starts_with("po3_rune_") && a_editorId.ends_with("light") && !a_editorId.contains("ward") &&
		       !a_editorId.contains("ritual");
	}

	// data load, main thread: every Light effect of StrangeRunes.esp whose light is a hand rune's. -> how many
	inline std::size_t Find()
	{
		auto& runes = Runes();
		runes.clear();
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data || !data->LookupModByName("StrangeRunes.esp")) {
			return 0;
		}
		for (auto* effect : data->GetFormArray<RE::EffectSetting>()) {
			if (!effect || effect->GetArchetype() != RE::EffectSetting::Archetype::kLight) {
				continue;
			}
			auto* light = effect->data.associatedForm;
			if (light && light->Is(RE::FormType::Light) && HandRuneLight(Fade::EditorID(light))) {
				runes.push_back({ effect, light });
			}
		}
		return runes.size();
	}

	[[nodiscard]] inline bool Loaded() { return !Runes().empty(); }

	// main thread: a_theirs = the runes light the hands as Strange Runes made them; false = their light out. A rune already
	// on a hand keeps the light it had until it ends
	inline void Apply(bool a_theirs)
	{
		for (const auto& rune : Runes()) {
			rune.effect->data.associatedForm = a_theirs ? rune.light : nullptr;
		}
	}
}
