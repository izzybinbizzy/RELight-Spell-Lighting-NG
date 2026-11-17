// powerofthree's ENB Lights For Effect Shaders - the parts that need no game (master: Tools\Shared Plugin Code; shared code sync.py keeps the copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// ENB Lights For Effect Shaders (powerof3, GPL-3.0, credited; its source read: powerof3/ENBLightsForEffectShaders, Manager.cpp, Hooks.cpp)
// gives every particle-emitting effect shader a light at data load: the shader's addon models point at one of its debris records, each a
// small model under Effects\ENBLightForEffectShaders with a particle light in it (drawn by ENB's complex particle lights, or Light Limit
// Fix's). It is never a requirement of ours; our lights are the default where both light a shader (rule file 6 1.3), and a switch gives
// that shader back to it. EnbShaderLights.h does the game side; here its tests.

#pragma once

#include <cctype>
#include <charconv>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace EnbShaderLightsText
{
	// its ini, [Settings] Valid Actors: 0 the player, 1 and followers, 2 everyone (its default, so a missing file or key is everyone)
	[[nodiscard]] inline int ValidActors(std::istream& a_ini)
	{
		std::optional<int> value;
		for (std::string line; std::getline(a_ini, line);) {
			std::string key;
			for (const char c : line) {
				if (c == ';' || c == '#') {
					break;
				}
				if (!std::isspace(static_cast<unsigned char>(c))) {
					key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
			if (key.starts_with("validactors=")) {
				int        v = 0;
				const auto digits = std::string_view(key).substr(12);
				if (const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v); ec == std::errc{}) {
					value = v;
				}
			}
		}
		return value.value_or(2);
	}

	// its own test for a shader it lights: the particle shader on, with persistent particles
	[[nodiscard]] inline bool TakesShader(bool a_particlesOff, float a_persistentParticles) noexcept
	{
		return !a_particlesOff && a_persistentParticles > 0.0f;
	}

	// one of its light models, as an addon's file name names it (any case, either slash)
	[[nodiscard]] inline bool IsItsModel(std::string_view a_path)
	{
		std::string p;
		for (const char c : a_path) {
			p += c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return p.find("effects\\enblightforeffectshaders\\") != std::string::npos;
	}
}
