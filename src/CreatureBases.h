#pragma once

// The records Illuminated - Creature Lighting NG lights through RE::Light by base ID (its creature records on the models we
// share). RE::Light looks a base ID up before a model, so such a record is lit by Creatures' config, and no switch, slider or
// tint of ours touches it (Data.cpp). Only Creatures' own folder is read: another author's base-ID config does not take a
// record from us (rule file 6, 1.3: our lights are the default). Without Creatures nothing is read and our model configs and
// switches light the record as before. Game-free, tested off the game (tests\test_fade.cpp).

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace CreatureBases
{
	// Creatures' RE::Light config folder under SKSE\Plugins\ReLight\Configs (its release's RELIGHT_NAME)
	constexpr std::string_view kFolder = "Illuminated Creature Lighting NG";

	// whether a folder of RE::Light's configs is Creatures', by name, any case: only its base IDs are left to it
	[[nodiscard]] inline bool CreaturesFolder(std::string_view a_name)
	{
		return a_name.size() == kFolder.size() && std::ranges::equal(a_name, kFolder, [](char a, char b) {
			const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
			return lower(a) == lower(b);
		});
	}

	// the base ID tokens one RE::Light config file names ("0x1074FD~Skyrim.esm"), from one config or a list of them; a
	// "baseID" may be one string or a list. Nothing when the text does not read as JSON (a comment or a BOM is fine)
	[[nodiscard]] inline std::optional<std::vector<std::string>> BaseTokens(std::string_view a_json)
	{
		const auto json = nlohmann::json::parse(a_json, nullptr, false, true);
		if (json.is_discarded()) {
			return std::nullopt;
		}
		std::vector<std::string> out;
		const auto               take = [&](const nlohmann::json& a_config) {
			const auto found = a_config.is_object() ? a_config.find("baseID") : a_config.end();
			if (found == a_config.end()) {
				return;
			}
			for (const auto& id : found->is_array() ? *found : nlohmann::json::array({ *found })) {
				if (id.is_string()) {
					out.push_back(id.get<std::string>());
				}
			}
		};
		if (json.is_array()) {
			for (const auto& config : json) {
				take(config);
			}
		} else {
			take(json);
		}
		return out;
	}

	// a base is left to Creatures when Creatures' config names it and no data line of ours claims it by base ID
	template <class Creatures, class Ours, class Id>
	[[nodiscard]] bool LeftToCreatures(const Id& a_id, const Creatures& a_creatures, const Ours& a_ours)
	{
		return a_creatures.contains(a_id) && !a_ours.contains(a_id);
	}
}
