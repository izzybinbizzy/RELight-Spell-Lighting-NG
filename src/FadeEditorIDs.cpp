// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Editor IDs come from powerofthree's Tweaks, which the mod REQUIRES (rule file 2 rule 19c, requirements over rebuilds): the game
// throws most editor IDs away and Tweaks' Load EditorIDs (on by default) keeps them, exporting GetFormEditorID. Our own table
// holds only the names of our in-memory copies (RememberEditorID); after the data has loaded ForgetPassEditorIDs() lets go of
// those the passes needed. Tweaks is asked through the shared Po3Tweaks.h (our mods' one way), judged once at data load
// (Po3Tweaks::Judge, FadeMain.cpp): the game naming its own forms (Native EditorID Fix) counts too; with neither, the log says so
// and one box names Tweaks. EditorID() hands out a copy.
// Credit: powerofthree (his Tweaks; his yes to all of his work, shipping topic sp.3).

#include "Fade.h"
#include "Po3Tweaks.h"

namespace Fade
{
	namespace
	{
		std::unordered_map<const RE::TESForm*, std::string> gEditorIDs;
		RE::BSSpinLock                                      gLock;

		// the form types kept for the whole session: weapons and enchantments (rule files name them), magic effects (read
		// while the game runs: the soul trap test and the plugins' own readers)
		[[nodiscard]] bool RuleType(const RE::TESForm* a_form) noexcept
		{
			return a_form->Is(RE::FormType::Weapon, RE::FormType::Enchantment, RE::FormType::MagicEffect);
		}
	}

	// our own copies' names (any thread): out of memory, the name is not kept and nothing leaves into the game
	void RememberEditorID(const RE::TESForm* a_form, const char* a_id) noexcept
	{
		if (!a_form || !a_id || !*a_id) {
			return;
		}
		try {
			RE::BSSpinLockGuard guard(gLock);
			gEditorIDs[a_form] = a_id;
		} catch (...) {
			return;
		}
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
				return it->second;  // our own in-memory copies' names
			}
		}
		return Po3Tweaks::EditorID(a_form);
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
