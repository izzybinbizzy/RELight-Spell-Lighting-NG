// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// VAER Reborn's swirls, two passes, once at data loaded, only when VAEReborn.esp is loaded and the VAER Reborn option of the
// Weapons download is installed (its enchantment lights are keyed by VAER's shaders, so without it a swap would leave the
// weapon unlit):
//
// 1. VAER's own effects put back (his report, "vaer swirls are still not working for relight"). A plugin
// loaded after VAEReborn.esp that edits the same vanilla effect (Thaumaturgy, Artificer, ...) carries the effect's old art, so
// VAER's swirl is lost: 21 of VAER's 46 in his game, measured. Each gets VAER's Enchant Art and Enchant Shader back. The table
// is relightgen.VAER_VANILLA_FORMS, read off VAEReborn.esp's own records; relightgen's selftest refuses one that differs.
//
// 2. VAER Reborn on Thaumaturgy's own effects. Thaumaturgy points the Fear, Paralyze, Turn Undead, Banish, Silent Moons and
// second Absorb enchantments at new magic effects of its own, and VAER Reborn only dresses the vanilla ones, so those
// weapons had no swirl. Each copy is given the enchant art and the enchant shader VAER made for the vanilla effect. Once, at
// data loaded, and only when VAEReborn.esp and Thaumaturgy.esp are both loaded and the VAER Reborn option of the Weapons
// download is installed: its enchantment lights are keyed by VAER's shaders, so without it the swap would leave the weapon
// unlit.
//
// The table is gen.VAER_SWIRL_FORMS (Illuminated carries the same rows); relightgen's selftest refuses a table here that is
// not that list, row for row.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		struct SwirlCopy
		{
			std::string_view effectPlugin;
			RE::FormID       effect;
			std::string_view artPlugin;
			RE::FormID       art;
			std::string_view shaderPlugin;
			RE::FormID       shader;
			std::string_view name;
		};

		// VAER_SWIRL_FORMS begin
		constexpr SwirlCopy kSwirlCopies[] = {
			{ "Thaumaturgy.esp", 0x1904A2, "VAEReborn.esp", 0x000807, "VAEReborn.esp", 0x00082C, "MAG_EnchFearFFContact" },
			{ "Thaumaturgy.esp", 0x1904A0, "VAEReborn.esp", 0x00080B, "VAEReborn.esp", 0x000823, "MAG_EnchParalyzeFFContact" },
			{ "Thaumaturgy.esp", 0x1904A3, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact01" },
			{ "Thaumaturgy.esp", 0x30C4C4, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact02" },
			{ "Thaumaturgy.esp", 0x30C4C5, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact03" },
			{ "Thaumaturgy.esp", 0x1904A4, "VAEReborn.esp", 0x000803, "VAEReborn.esp", 0x000827, "MAG_EnchBanishFFContact01" },
			{ "Thaumaturgy.esp", 0x3115CA, "VAEReborn.esp", 0x00080A, "VAEReborn.esp", 0x000826, "MAG_EnchSilentMoonsEnchFFContact02" },
			{ "Thaumaturgy.esp", 0x427D9E, "VAEReborn.esp", 0x000801, "VAEReborn.esp", 0x00082A, "MAG_EnchAbsorbMagickaFFContact02" },
			{ "Thaumaturgy.esp", 0x42CEA1, "VAEReborn.esp", 0x000802, "VAEReborn.esp", 0x000826, "MAG_EnchAbsorbStaminaFFContact02" },
		};
		// VAER_SWIRL_FORMS end

		// an empty art plugin: VAER leaves that effect's art alone and sets only the shader
		// VAER_VANILLA_FORMS begin
		constexpr SwirlCopy kVaerOwn[] = {
			{ "Skyrim.esm", 0x0F23F9, "VAEReborn.esp", 0x000817, "VAEReborn.esp", 0x000848, "C06BladeOfYsgramorEnchEffect" },
			{ "ccbgssse004-ruinsedge.esl", 0x000802, "VAEReborn.esp", 0x000833, "VAEReborn.esp", 0x00083C, "ccBGSSSE004_BowBaseEffect" },
			{ "ccbgssse006-stendarshammer.esl", 0x000808, "VAEReborn.esp", 0x000834, "VAEReborn.esp", 0x00083D, "ccBGSSSE006_EnchDamageFFContact" },
			{ "ccbgssse007-chrysamere.esl", 0x000801, "VAEReborn.esp", 0x000835, "VAEReborn.esp", 0x00083E, "ccBGSSSE007_Chrysamere_Effect" },
			{ "ccbgssse013-dawnfang.esl", 0x000D18, "VAEReborn.esp", 0x000836, "VAEReborn.esp", 0x00083F, "ccBGSSSE013_EnchBloodthirst" },
			{ "ccbgssse013-dawnfang.esl", 0x000803, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchDawnfangDescriptionFX" },
			{ "ccbgssse013-dawnfang.esl", 0x000804, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchDawnfangKillIncrementFX" },
			{ "ccbgssse013-dawnfang.esl", 0x000819, "ccbgssse013-dawnfang.esl", 0x00080B, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchFireDamageFFContact" },
			{ "ccbgssse013-dawnfang.esl", 0x00081A, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchFrostDamageFFContact" },
			{ "ccbgssse016-umbra.esm", 0x00C831, "VAEReborn.esp", 0x000838, "VAEReborn.esp", 0x000841, "ccBGSSSE016_EnchAbsorbHealthFFContact_SoulTrapFX" },
			{ "ccbgssse020-graycowl.esl", 0x00086F, "VAEReborn.esp", 0x000839, "VAEReborn.esp", 0x000842, "ccBGSSSE020_EnchTurnUndeadFFContact" },
			{ "ccbgssse067-daedinv.esm", 0x17094E, "VAEReborn.esp", 0x00083A, "VAEReborn.esp", 0x000844, "ccBGSSSE067_EnchBanishFFContact" },
			{ "ccbgssse067-daedinv.esm", 0x175AC0, "VAEReborn.esp", 0x00083B, "VAEReborn.esp", 0x000843, "ccBGSSSE067_EnchFireDamageFFContact_NoShader" },
			{ "Skyrim.esm", 0x106617, "VAEReborn.esp", 0x000814, "Skyrim.esm", 0x10A043, "ChillrendEnchFrostDamageFFContact" },
			{ "Skyrim.esm", 0x106616, "VAEReborn.esp", 0x000814, "Skyrim.esm", 0x10A043, "ChillrendParalysisFFContact" },
			{ "Skyrim.esm", 0x091AE5, "VAEReborn.esp", 0x00081A, "VAEReborn.esp", 0x00082E, "DA07MehrunesRazorMagicEffect" },
			{ "Skyrim.esm", 0x10FAF1, "VAEReborn.esp", 0x000811, "VAEReborn.esp", 0x000828, "DA08EnchAbsorbHealthFFContact" },
			{ "Skyrim.esm", 0x0FEE38, "", 0x000000, "VAEReborn.esp", 0x00084B, "DA09EncDawnbreakeScriptEffect" },
			{ "Skyrim.esm", 0x0FEFBC, "", 0x000000, "VAEReborn.esp", 0x00084B, "DA09EnchDawnbreakerEnchFireDamageFFContact" },
			{ "Dawnguard.esm", 0x016696, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneAxeDamageEffect" },
			{ "Dawnguard.esm", 0x016695, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneAxeIncrementKills" },
			{ "Dawnguard.esm", 0x006924, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneVisualsEffect" },
			{ "Dawnguard.esm", 0x015719, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1EnchSunDamage" },
			{ "Dawnguard.esm", 0x01571B, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1EnchSunDamageUndead" },
			{ "Dawnguard.esm", 0x014557, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1RuneHammerVisualEffect" },
			{ "Dragonborn.esm", 0x03570C, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbHealthFFContact50" },
			{ "Dragonborn.esm", 0x03570E, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbMagickaFFContact50" },
			{ "Dragonborn.esm", 0x03570F, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbStaminaFFContact50" },
			{ "Dragonborn.esm", 0x02C46B, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchFireDamageFFContact50" },
			{ "Dragonborn.esm", 0x02C46D, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchFrostDamageFFContact50" },
			{ "Dragonborn.esm", 0x02C46C, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchShockDamageFFContact50" },
			{ "Skyrim.esm", 0x03B0B1, "VAEReborn.esp", 0x00080A, "VAEReborn.esp", 0x000826, "dunSilentMoonsEnchFFContact" },
			{ "Skyrim.esm", 0x0AA155, "VAEReborn.esp", 0x000800, "VAEReborn.esp", 0x000828, "EnchAbsorbHealthFFContact" },
			{ "Skyrim.esm", 0x0AA156, "VAEReborn.esp", 0x000801, "VAEReborn.esp", 0x00082A, "EnchAbsorbMagickaFFContact" },
			{ "Skyrim.esm", 0x0AA157, "VAEReborn.esp", 0x000802, "VAEReborn.esp", 0x000826, "EnchAbsorbStaminaFFContact" },
			{ "Skyrim.esm", 0x0ACBB5, "VAEReborn.esp", 0x000803, "VAEReborn.esp", 0x000827, "EnchBanishFFContact" },
			{ "Skyrim.esm", 0x04605A, "VAEReborn.esp", 0x000808, "VAEReborn.esp", 0x000824, "EnchFireDamageFFContact" },
			{ "Skyrim.esm", 0x04605B, "VAEReborn.esp", 0x000809, "VAEReborn.esp", 0x000825, "EnchFrostDamageFFContact" },
			{ "Skyrim.esm", 0x05B451, "VAEReborn.esp", 0x000807, "VAEReborn.esp", 0x00082C, "EnchInfluenceConfDownFFContactLow" },
			{ "Skyrim.esm", 0x05B44F, "VAEReborn.esp", 0x000805, "VAEReborn.esp", 0x000830, "EnchMagickaDamageFFContact" },
			{ "Skyrim.esm", 0x0ACBB6, "VAEReborn.esp", 0x00080B, "VAEReborn.esp", 0x000823, "EnchParalysisFFContact" },
			{ "Skyrim.esm", 0x04605C, "VAEReborn.esp", 0x00080D, "VAEReborn.esp", 0x000829, "EnchShockDamageFFContact" },
			{ "Skyrim.esm", 0x05B452, "VAEReborn.esp", 0x00080F, "VAEReborn.esp", 0x00082E, "EnchSoulTrapFFContact" },
			{ "Skyrim.esm", 0x05B450, "VAEReborn.esp", 0x000806, "VAEReborn.esp", 0x000831, "EnchStaminaDamageFFContact" },
			{ "Skyrim.esm", 0x05B46B, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "EnchTurnUndeadFFContact" },
			{ "Skyrim.esm", 0x0F1AC2, "VAEReborn.esp", 0x000810, "VAEReborn.esp", 0x000829, "MQ203DragonDamageFFContact" },
		};
		// VAER_VANILLA_FORMS end

		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		[[nodiscard]] bool Loaded(std::string_view a_name)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName(a_name) || dh->LookupLoadedLightModByName(a_name));
		}

		// A patch a tool writes (Synthesis, Mator Smash, Wrye Bash, xEdit's merged patch) lists every plugin before it as a
		// master, VAER's too, without knowing VAER: its change is the winning plugin's carried forward, never one made for VAER
		[[nodiscard]] bool GeneratedPatch(std::string_view a_file)
		{
			const auto low = LightKit::Lower(a_file);
			return low == "synthesis.esp" || low == "smashed patch.esp" || low.starts_with("bashed patch") || low == "merged patch.esp";
		}

		// The plugin that changed this effect last, when it knows VAER (VAEReborn.esp is one of its masters) - its change is
		// deliberate and stays. Measured (his Precision magic trails report, Illuminated's same pass): VAER's own Vibrant
		// Weapons patch (VAER_EAE.esp) takes the swirl off six fire / frost / shock effects and gives them the vanilla shader so
		// Enchantment Art Extender dresses the weapon, and this pass was putting VAER's swirl back over it.
		// A generated patch that won only carries an earlier plugin's change forward, so the judge is the newest plugin before it
		// that is not one (the code review of 2.2: VAER_EAE.esp's change carried by Synthesis.esp is still VAER's own patch's).
		[[nodiscard]] const RE::TESFile* VaerAwareChange(const RE::EffectSetting* a_effect)
		{
			const auto* files = a_effect ? a_effect->sourceFiles.array : nullptr;
			if (!files) {
				return nullptr;
			}
			for (auto n = files->size(); n-- > 0;) {
				const auto* last = (*files)[n];
				if (!last || GeneratedPatch(last->fileName)) {
					continue;
				}
				if (_stricmp(last->fileName, "VAEReborn.esp") == 0) {
					return nullptr;
				}
				for (std::uint32_t i = 0; last->masterPtrs && i < last->masterCount; ++i) {
					if (const auto* master = last->masterPtrs[i]; master && _stricmp(master->fileName, "VAEReborn.esp") == 0) {
						return last;
					}
				}
				return nullptr;
			}
			return nullptr;
		}

		// the VAER Reborn option's data file is there only when the player ticked the option in the installer
		[[nodiscard]] bool VaerOptionInstalled()
		{
			for (const auto& option : Options()) {
				if (option.id == "Weapons - VAER Reborn") {
					return true;
				}
			}
			return false;
		}
	}

	void VaerSwirls()
	{
		if (!Loaded("VAEReborn.esp")) {
			SKSE::log::info("VAER: VAEReborn.esp is not loaded; nothing to do");
			return;
		}
		if (!VaerOptionInstalled()) {
			SKSE::log::info("VAER: the VAER Reborn option is not installed; every effect keeps the art its winning plugin gives it");
			return;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}
		// 1. VAER's own effects: a plugin that is not loaded (a Creation Club file he does not have) is simply skipped
		std::size_t back = 0, already = 0, absent = 0, patched = 0;
		for (const auto& c : kVaerOwn) {
			auto* effect = dh->LookupForm<RE::EffectSetting>(c.effect, c.effectPlugin);
			auto* art = c.artPlugin.empty() ? nullptr : dh->LookupForm<RE::BGSArtObject>(c.art, c.artPlugin);
			auto* shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
			if (!effect || (!c.artPlugin.empty() && !art) || !shader) {
				++absent;
				continue;
			}
			if ((c.artPlugin.empty() || effect->data.enchantEffectArt == art) && effect->data.enchantShader == shader) {
				++already;
				continue;
			}
			if (const auto* patch = VaerAwareChange(effect)) {
				++patched;
				SKSE::log::info("[VAER] {} was changed by {}, a patch made for VAER: left as it set it", c.name, patch->fileName);
				continue;
			}
			if (!c.artPlugin.empty()) {
				effect->data.enchantEffectArt = art;
			}
			effect->data.enchantShader = shader;
			++back;
			SKSE::log::info("[VAER] {} had lost VAER's swirl; given back", c.name);
		}
		SKSE::log::info(
			"VAER: {} of VAER's effect(s) given their swirl back, {} still had it, {} left to a VAER patch, {} not in this load "
			"order",
			back, already, patched, absent);
		// his call: two of VAER's shaders (Stendarr's Hammer, Dawnfang's Bloodthirst) name their fill
		// texture "....dds.dds", a file that does not exist, so they draw nothing - point any such name at the real file
		std::size_t typos = 0;
		for (const auto& c : kVaerOwn) {
			auto*             shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
			const char*       tex = shader ? shader->fillTexture.textureName.c_str() : nullptr;
			const std::size_t len = tex ? std::strlen(tex) : 0;
			if (len > 8 && _stricmp(tex + len - 8, ".dds.dds") == 0) {
				const std::string fixed(tex, len - 4);
				SKSE::log::info("[VAER] {}: shader texture {} -> {}", c.name, tex, fixed);
				shader->fillTexture.textureName = fixed;
				++typos;
			}
		}
		SKSE::log::info("VAER: {} shader texture name(s) ending .dds.dds fixed", typos);
		if (!Loaded("Thaumaturgy.esp")) {
			SKSE::log::info("VAER on Thaumaturgy: Thaumaturgy.esp is not loaded; nothing more to do");
			return;
		}
		// 2. Thaumaturgy's own copies
		std::size_t set = 0, missing = 0, kept = 0;
		for (const auto& c : kSwirlCopies) {
			auto* effect = dh->LookupForm<RE::EffectSetting>(c.effect, c.effectPlugin);
			auto* art = dh->LookupForm<RE::BGSArtObject>(c.art, c.artPlugin);
			auto* shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
			if (!effect || !art || !shader) {
				++missing;
				SKSE::log::warn("[VAER] {}: a form is not in this load order (effect {}, art {}, shader {})", c.name,
					effect != nullptr, art != nullptr, shader != nullptr);
				continue;
			}
			// a plugin that masters VAER (and so Thaumaturgy, whose record it changes) set this one on purpose, as VaerOwnBack keeps
			if (const auto* patch = VaerAwareChange(effect)) {
				++kept;
				SKSE::log::info("[VAER] {} was changed by {}, a patch made for VAER: left as it set it", c.name, patch->fileName);
				continue;
			}
			effect->data.enchantEffectArt = art;
			effect->data.enchantShader = shader;
			++set;
			SKSE::log::info("[VAER] {} now wears VAER's swirl {:08X} and shader {:08X}", c.name, art->GetFormID(),
				shader->GetFormID());
		}
		SKSE::log::info("VAER on Thaumaturgy: {} of {} effect(s) given VAER's swirl, {} left to a VAER patch, {} not found", set, std::size(kSwirlCopies),
			kept, missing);
	}
}
