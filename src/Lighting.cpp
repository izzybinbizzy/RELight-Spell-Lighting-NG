// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Which lighting the game draws with: the shared GameLighting.h finds it once, at data load (Community Shaders, ENB or Vanilla,
// never picked); this file keeps the answer and RELight's own Vanilla look. RE::Light stays the main driver: only the lights that
// cannot work without knowing the lighting read it - the hand lights and their ground twins, the held and stream lights, drawn
// plain off Community Shaders (LightKit::PlainOf).

#include "Plugin.h"

#include "GameLighting.h"

namespace Plugin
{
	static_assert(static_cast<int>(Lighting::kShaders) == static_cast<int>(GameLighting::Lighting::kShaders) &&
				  static_cast<int>(Lighting::kEnb) == static_cast<int>(GameLighting::Lighting::kEnb) &&
				  static_cast<int>(Lighting::kVanilla) == static_cast<int>(GameLighting::Lighting::kVanilla));

	namespace
	{
		std::atomic<Lighting> gFound{ Lighting::kVanilla };
		std::atomic<bool>     gInverseSquare{ false }, gCommunityShaders{ false }, gEnb{ false }, gEnbFiles{ false };
	}

	void ReadLighting()
	{
		const auto found = GameLighting::Read();
		gCommunityShaders = found.communityShaders;
		gInverseSquare = found.inverseSquare;
		gEnb = found.enb;
		gEnbFiles = found.enbFilesLeft;
		gFound = static_cast<Lighting>(found.lighting);
		GameLighting::Log(found);
	}

	Lighting LightingFound()
	{
		return gFound.load();
	}

	const char* LightingName(Lighting a_lighting)
	{
		return GameLighting::Name(static_cast<GameLighting::Lighting>(a_lighting));
	}

	bool InverseSquare()
	{
		return gInverseSquare.load();
	}

	bool LightLimitFix()
	{
		return gCommunityShaders.load();
	}

	float VanillaStrength()
	{
		return gFound.load() == Lighting::kVanilla ? gVanillaStrength.load(std::memory_order_relaxed) : 1.0f;
	}

	// each channel pushed away from the color's grey by VanillaColorStrength (1 as made, above 1 deeper), on Vanilla and ENB
	// (ENB drew a blue light green-white)
	RE::NiColor VanillaLook(RE::NiColor a_color)
	{
		const float k = gVanillaSaturation.load(std::memory_order_relaxed);
		const auto  lighting = gFound.load();
		if ((lighting != Lighting::kVanilla && lighting != Lighting::kEnb) || k == 1.0f) {
			return a_color;
		}
		const float grey = (a_color.red + a_color.green + a_color.blue) / 3.0f;
		const auto  push = [&](float a_c) { return std::clamp(grey + (a_c - grey) * k, 0.0f, 1.0f); };
		return { push(a_color.red), push(a_color.green), push(a_color.blue) };
	}

	std::string LightingReport()
	{
		return std::format(
			R"({{"pick":"{}","detected":true,"inverseSquareShader":{},"communityShadersLoaded":{},"enbRunning":{},"enbFiles":{},"inverseSquare":{}}})",
			LightingName(gFound), gInverseSquare.load(), gCommunityShaders.load(), gEnb.load(), gEnbFiles.load(), InverseSquare());
	}
}
