// powerofthree's ENB Lights For Effect Shaders - the off-game checks of EnbShaderLightsText.h (master: Tools\Shared Plugin Code, every copy
// kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file.
//
// Run by each carrying plugin's own tests; Run adds kCount to the test's floor.
#pragma once

#include "EnbShaderLightsText.h"

#include <sstream>

namespace EnbShaderLightsChecks
{
	using Check = void (*)(bool a_ok, const char* a_what, int a_line);

	inline constexpr int kCount = 3;

#define ENBL_CHECK(x) a_check((x), #x, __LINE__)

	inline void Run(Check a_check)
	{
		using namespace EnbShaderLightsText;
		const auto valid = [](const char* a_text) {
			std::istringstream in(a_text);
			return ValidActors(in);
		};
		// a missing file or key is everyone (its default); a note is no value; the last line read wins
		ENBL_CHECK(valid("") == 2 && valid("[Settings]\nValid Actors = 0\n") == 0 && valid("Valid Actors = 1 ; followers\n") == 1 &&
				   valid("[Settings]\n;Valid Actors = 0\nValid Actors = 2\n") == 2);
		// its own shader test
		ENBL_CHECK(TakesShader(false, 40.0f) && !TakesShader(true, 40.0f) && !TakesShader(false, 0.0f));
		// its light models, any case and slash; not another model
		ENBL_CHECK(IsItsModel("Effects\\ENBLightForEffectShaders\\Ward.nif") && IsItsModel("meshes/effects/enblightforeffectshaders/fire.nif") &&
				   !IsItsModel("Effects\\GhostFX.nif"));
	}

#undef ENBL_CHECK
}
