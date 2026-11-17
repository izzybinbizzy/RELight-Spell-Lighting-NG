// powerofthree's ENB Lights For Effect Shaders, where it lights an effect shader one of our mods lights (master: Tools\Shared Plugin Code;
// shared code sync.py keeps the copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Its light on a shader is the shader's addon models pointing at one of its debris records (EnbShaderLightsText.h). Ours is the default
// there (rule file 6 1.3): TakeOff points the addon back at what the shader had (nothing, for the shaders it gives one), GiveBack restores
// its addon when the player's switch hands that shader back to it. Run on the main thread, after its own data-load pass has run (a task
// queued at data load), and again when that switch changes. Its addon records are shared by every shader it lights, so only each
// shader's own pointer is changed, never a record of its.

#pragma once

#include "EnbShaderLightsText.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace EnbShaderLights
{
	[[nodiscard]] inline bool Installed() { return REX::W32::GetModuleHandleW(L"po3_ENBLightForEffectShaders") != nullptr; }

	// the characters its ini lets it light: 2 = everyone (its default)
	[[nodiscard]] inline int ValidActors()
	{
		std::ifstream in(std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "po3_ENBLightsForEffectShaders.ini");
		return EnbShaderLightsText::ValidActors(in);
	}

	// a shader it gives a light, by its own test (read before its data-load pass may have run)
	[[nodiscard]] inline bool Takes(const RE::TESEffectShader* a_shader)
	{
		return a_shader && EnbShaderLightsText::TakesShader(a_shader->data.flags.any(RE::EffectShaderData::Flags::kDisableParticleShader),
							   a_shader->data.particleShaderPersistantParticleCount);
	}

	namespace Detail
	{
		// each shader whose addon was its: the addon it set (main thread only)
		inline std::unordered_map<RE::TESEffectShader*, RE::BGSDebris*>& Saved()
		{
			static std::unordered_map<RE::TESEffectShader*, RE::BGSDebris*> saved;
			return saved;
		}

		// not const: CommonLib's BSSimpleList cannot be walked through a const list
		[[nodiscard]] inline bool AllIts(RE::BGSDebris* a_debris)
		{
			if (!a_debris || a_debris->data.empty()) {
				return false;
			}
			for (const auto* d : a_debris->data) {
				if (!d || !d->fileName || !EnbShaderLightsText::IsItsModel(d->fileName)) {
					return false;
				}
			}
			return true;
		}
	}

	// its light on this shader is on: the shader's addon is wholly its
	[[nodiscard]] inline bool Lit(const RE::TESEffectShader* a_shader) { return a_shader && Detail::AllIts(a_shader->data.addonModels); }

	// ours is the default: its addon comes off the shader (the shaders it lights carry no addon of their own), remembered for GiveBack
	inline bool TakeOff(RE::TESEffectShader* a_shader)
	{
		if (!Lit(a_shader)) {
			return false;
		}
		Detail::Saved()[a_shader] = a_shader->data.addonModels;
		a_shader->data.addonModels = nullptr;
		return true;
	}

	// the player's switch gives the shader back to it
	inline bool GiveBack(RE::TESEffectShader* a_shader)
	{
		auto&      saved = Detail::Saved();
		const auto it = saved.find(a_shader);
		if (it == saved.end() || a_shader->data.addonModels) {
			return false;
		}
		a_shader->data.addonModels = it->second;
		saved.erase(it);
		return true;
	}
}
