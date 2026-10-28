// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Editor IDs, recorded as each form loads, because the game throws most of them away. The ONE recorder in the plugin:
// SetFormEditorID (vtable slot 0x33) is hooked per form type at SKSE's post-load, before the game reads its plugins.
// The fading module needs weapons and enchantments (rule files may name them by editor ID; keywords keep their own) and
// magic effects (the soul trap test, FadeSpells.cpp, and the plugins' own readers look at them while the game runs); a
// plugin that needs more types asks for them with RecordEditorIDs<T>() (Fade.h). After the data has loaded,
// ForgetPassEditorIDs() lets go of every other editor ID - the passes that needed them have run - so the table holds
// only those three types. EditorID() hands out a copy.
// With powerofthree's Tweaks keeping editor IDs, it does that work: it is asked first and the recorder stays out.

#include "Fade.h"

namespace Fade
{
	namespace
	{
		std::unordered_map<const RE::TESForm*, std::string> gEditorIDs;
		RE::BSSpinLock                                      gLock;

		using TweaksLookup = const char* (*)(std::uint32_t);

		[[nodiscard]] TweaksLookup TweaksFn()
		{
			static const auto fn = [] {
				const auto tweaks = REX::W32::GetModuleHandleW(L"po3_Tweaks");
				return tweaks ? reinterpret_cast<TweaksLookup>(REX::W32::GetProcAddress(tweaks, "GetFormEditorID")) : nullptr;
			}();
			return fn;
		}

		// its ini can switch the cache off ([Fixes] bLoadEditorIDs, "Load EditorIDs" before); on unless it says false or 0
		[[nodiscard]] bool TweaksLoadsEditorIDs()
		{
			std::ifstream in(std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "po3_Tweaks.ini");
			for (std::string line; std::getline(in, line);) {
				std::string key;
				for (const char c : line) {
					if (c != ' ' && c != '\t' && c != '\r') {
						key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
				}
				for (const std::string_view name : { "bloadeditorids=", "loadeditorids=" }) {
					if (key.starts_with(name)) {
						const auto value = std::string_view(key).substr(name.size());
						return !value.starts_with("false") && !value.starts_with('0');
					}
				}
			}
			return true;
		}

		// the form types kept for the whole session: weapons and enchantments (rule files name them), magic effects (read
		// while the game runs: the soul trap test and the plugins' own readers)
		[[nodiscard]] bool RuleType(const RE::TESForm* a_form) noexcept
		{
			return a_form->Is(RE::FormType::Weapon, RE::FormType::Enchantment, RE::FormType::MagicEffect);
		}
	}

	void RememberEditorID(const RE::TESForm* a_form, const char* a_id)
	{
		if (a_form && a_id && *a_id) {
			RE::BSSpinLockGuard guard(gLock);
			gEditorIDs[a_form] = a_id;
		}
	}

	bool TweaksKeepsEditorIDs()
	{
		static const bool keeps = TweaksFn() && TweaksLoadsEditorIDs();
		return keeps;
	}

	const char* TweaksEditorID(RE::FormID a_id)
	{
		const auto fn = TweaksFn();
		return fn ? fn(a_id) : nullptr;
	}

	void InstallEditorIDHooks()
	{
		if (TweaksKeepsEditorIDs()) {
			SKSE::log::info("editor IDs: powerofthree's Tweaks keeps them - asked first, our recorder is not installed");
			return;
		}
		RecordEditorIDs<RE::TESObjectWEAP>();
		RecordEditorIDs<RE::EnchantmentItem>();
		RecordEditorIDs<RE::EffectSetting>();
		SKSE::log::info("editor IDs: weapons, enchantments and magic effects are recorded as they load{}",
			TweaksFn() ? " (powerofthree's Tweaks keeps none: its Load EditorIDs is off)" : "");
	}

	void ForgetPassEditorIDs()
	{
		RE::BSSpinLockGuard guard(gLock);
		std::erase_if(gEditorIDs, [](const auto& a_kv) { return !RuleType(a_kv.first); });
		gEditorIDs.rehash(0);
	}

	std::string EditorID(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		{
			RE::BSSpinLockGuard guard(gLock);
			if (const auto it = gEditorIDs.find(a_form); it != gEditorIDs.end()) {
				return it->second;  // our own copies' names, and every recorded one while Tweaks keeps none
			}
		}
		const char* id = TweaksEditorID(a_form->GetFormID());
		if (!id || !*id) {
			id = a_form->GetFormEditorID();
		}
		return id ? std::string(id) : std::string();
	}

	std::string Label(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "(none)";
		}
		const auto  id = EditorID(a_form);
		const auto* file = a_form->GetFile(0);
		const char* name = a_form->GetName();
		return std::format("{} [{}] {}|{:08X}", name && *name ? name : "(no name)", id.empty() ? "no editor ID" : id,
			file ? file->GetFilename() : "(made in game)", a_form->GetFormID());
	}
}
