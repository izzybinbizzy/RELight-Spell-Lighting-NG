// powerofthree's Tweaks, a requirement of our mods (master: Tools\Shared Plugin Code; shared code sync.py keeps the copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// powerofthree's Tweaks (powerof3, credited) keeps the editor IDs the game throws away: its SetFormEditorID hooks put each into the
// game's own editor-ID map and its cache, and it exports GetFormEditorID (read in its source, powerof3/po3-Tweaks: Cache.cpp,
// Fixes/CacheEditorIDs.cpp, Settings.h). Nothing of its code is copied: we call its export the way powerof3's own CLibUtil asks it.
// It is REQUIRED (rule file 2 rule 19c, requirements over rebuilds): no editor ID keeping of our own exists. Judged ONCE, after the
// data has loaded: the DLL is loaded, it has the export, its ini leaves Load EditorIDs on, and the export names a known form; when it
// does not, the log says so and one box at data load names what to install. Native EditorID Fix (Kitsuune, credited) is the other source
// the box accepts: at its default it keeps every editor ID in its own table and the game's editor-ID map and gives them through its published
// API (NEIF_GetEditorIDLookupState, NEIF_GetEditorID taking the form ID and type by value - the API's facts, none of its code copied); in
// its native mode the game's own GetFormEditorID answers too. The game-free part, with the off-game tests, is Po3TweaksText.h.

#pragma once

#include "Po3TweaksText.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace Po3Tweaks
{
	using EditorIDFn = const char* (*)(std::uint32_t);

	namespace Detail
	{
		[[nodiscard]] inline EditorIDFn Export()
		{
			static const EditorIDFn fn = [] {
				const auto dll = REX::W32::GetModuleHandleW(L"po3_Tweaks");
				return dll ? reinterpret_cast<EditorIDFn>(REX::W32::GetProcAddress(dll, "GetFormEditorID")) : nullptr;
			}();
			return fn;
		}

		[[nodiscard]] inline bool IniOn()
		{
			std::ifstream in(std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "po3_Tweaks.ini");
			return Po3TweaksText::LoadsEditorIDs(in);
		}

		// Native EditorID Fix's published API: a form named by its ID and type, the lookup's state (0 none, 1 its own table only, 2-3 native)
		struct NeifForm
		{
			std::uint32_t id{ 0 };
			std::uint8_t  type{ 0 };
		};
		using NeifEditorIDFn = const char* (*)(NeifForm);
		using NeifStateFn = std::uint32_t (*)();

		// its export while its lookup is on, else nullptr
		[[nodiscard]] inline NeifEditorIDFn NeifExport()
		{
			static const NeifEditorIDFn fn = []() -> NeifEditorIDFn {
				const auto dll = REX::W32::GetModuleHandleW(L"NativeEditorIDFix");
				if (!dll) {
					return nullptr;
				}
				const auto state = reinterpret_cast<NeifStateFn>(REX::W32::GetProcAddress(dll, "NEIF_GetEditorIDLookupState"));
				const auto get = reinterpret_cast<NeifEditorIDFn>(REX::W32::GetProcAddress(dll, "NEIF_GetEditorID"));
				return state && get && state() >= 1 ? get : nullptr;
			}();
			return fn;
		}

		[[nodiscard]] inline const char* NeifEditorID(const RE::TESForm* a_form)
		{
			const auto fn = NeifExport();
			return fn && a_form ? fn({ a_form->GetFormID(), static_cast<std::uint8_t>(a_form->GetFormType()) }) : nullptr;
		}
	}

	// judged the first time it is asked, which must be after the data has loaded (the proof form must be there): each plugin asks first
	// at its data-loaded message through Judge()
	[[nodiscard]] inline Po3TweaksText::State StateNow()
	{
		static const Po3TweaksText::State state = [] {
			const bool  dll = REX::W32::GetModuleHandleW(L"po3_Tweaks") != nullptr;
			const auto  fn = dll ? Detail::Export() : nullptr;
			const bool  on = fn && Detail::IniOn();
			const auto* proof = RE::TESForm::LookupByID(Po3TweaksText::kProofForm);
			const char* neif = Detail::NeifEditorID(proof);
			const char* native = neif && *neif ? neif : proof ? proof->GetFormEditorID() :
			                                                    nullptr;
			return Po3TweaksText::Judge(dll, fn != nullptr, on, on ? fn(Po3TweaksText::kProofForm) : nullptr, native);
		}();
		return state;
	}

	// editor IDs are there: Tweaks keeps them, or the game names its own forms (Native EditorID Fix and the like)
	[[nodiscard]] inline bool KeepsEditorIDs() { return Po3TweaksText::NamesKept(StateNow()); }

	// at the data-loaded message, before anything reads a form by editor ID: judged once and logged; when Tweaks does not keep them, an
	// error line and one box naming what to install (as Illuminated and RELight do)
	inline void Judge(std::string_view a_mod)
	{
		const auto state = StateNow();
		if (Po3TweaksText::NamesKept(state)) {
			SKSE::log::info("{}", Po3TweaksText::Describe(state));
			return;
		}
		SKSE::log::error("{}", Po3TweaksText::Describe(state));
		const auto text = Po3TweaksText::BoxText(a_mod, state);
		RE::DebugMessageBox(text.c_str());
	}

	// a form's editor ID: Tweaks' while it keeps them, else Native EditorID Fix's, else the form's own (the kinds the game keeps), else ""
	[[nodiscard]] inline std::string EditorID(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (StateNow() == Po3TweaksText::State::kKeepsThem) {
			if (const char* id = Detail::Export()(a_form->GetFormID()); id && *id) {
				return id;
			}
		}
		if (const char* neif = Detail::NeifEditorID(a_form); neif && *neif) {
			return neif;
		}
		const char* own = a_form->GetFormEditorID();
		return own ? std::string(own) : std::string();
	}

	// a form our files name, by either spelling (Po3TweaksText::ParseKey); an editor ID through the game's own map, which Tweaks fills for
	// the kinds it keeps (the game itself keeps races, keywords, globals and a few more)
	[[nodiscard]] inline RE::TESForm* FormOf(const Po3TweaksText::FormKey& a_key)
	{
		switch (a_key.kind) {
		case Po3TweaksText::Key::kFormID:
			{
				auto* dh = RE::TESDataHandler::GetSingleton();
				return dh ? dh->LookupForm(a_key.local, a_key.plugin) : nullptr;
			}
		case Po3TweaksText::Key::kEditorID:
			return RE::TESForm::LookupByEditorID(a_key.editorID);
		case Po3TweaksText::Key::kBad:
		default:
			return nullptr;
		}
	}

	[[nodiscard]] inline RE::TESForm* FormOf(std::string_view a_text) { return FormOf(Po3TweaksText::ParseKey(a_text)); }

	// what a log line adds when a form named by editor ID was not found
	[[nodiscard]] inline std::string_view MissingHint(std::string_view a_text) { return Po3TweaksText::MissingHint(Po3TweaksText::ParseKey(a_text), KeepsEditorIDs()); }
}
