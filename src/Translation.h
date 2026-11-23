// Translation of the settings pages - shared by izzydoingit's SKSE plugins (master: Tools\Shared Plugin Code).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// The standard Skyrim translation files: each mod ships Data\Interface\Translations\<name>_ENGLISH.txt (UTF-16 with a byte order
// mark, one "$Key<TAB>text" line per menu line, made by translationfiles.py from the menu's own lines). A translator copies it to
// <name>_<LANGUAGE>.txt - the game's own language word: GERMAN, FRENCH, RUSSIAN ... - and changes the text after the tab. At data load
// CommonLib's SKSE::Translation reads the file of the language the game runs in into the game's own translator, and a menu line is
// asked of it by its key the first time it is shown. A missing file or line keeps the English; a translation whose placeholders do
// not match its English line is not used and the log names it (TranslationText.h Usable).
#pragma once

#include "TranslationText.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Translation
{
	using TranslationText::Fill;

	namespace Detail
	{
		struct Hash
		{
			using is_transparent = void;
			[[nodiscard]] std::size_t operator()(std::string_view a_text) const noexcept { return std::hash<std::string_view>{}(a_text); }
		};

		struct State
		{
			std::mutex                                                          lock;
			std::string                                                         name;
			std::unordered_map<std::string, std::string, Hash, std::equal_to<>> lines;   // English -> what the menu shows
			std::thread::id                                                     main{};  // Load's thread: the game's main thread
			std::unordered_set<std::string>                                     asked;   // lines asked for off it (copied: a caller's text may not outlive the call)
			bool                                                                queued{ false };
		};

		[[nodiscard]] inline State& Get()
		{
			static State state;
			return state;
		}
	}

	// at data load, before the menu is registered: the game's translator reads <a_name>_<its language>.txt. Returns what to log
	inline std::string Load(std::string_view a_name)
	{
		auto& state = Detail::Get();
		{
			std::lock_guard l{ state.lock };
			state.name = a_name;
			state.lines.clear();
			state.main = std::this_thread::get_id();
		}
		SKSE::Translation::ParseTranslation(std::string(a_name));
		return "translation: the menu's lines come from Interface\\Translations\\" + std::string(a_name) +
		       "_<the game's language>.txt; a missing file or line stays English";
	}

	namespace Detail
	{
		// under state.lock, on the main thread only: the game's translator read for one line, kept
		inline const char* Resolve(State& a_state, const char* a_english);
	}

	// the line in the reader's language, or the English itself. Each line is asked of the game's translator once, on the main
	// thread: asked first from another thread (the menu draws on the render thread) it shows its English until one task has read it
	inline const char* T(const char* a_english)
	{
		auto&           state = Detail::Get();
		std::lock_guard l{ state.lock };
		if (state.name.empty()) {
			return a_english;
		}
		if (const auto it = state.lines.find(std::string_view(a_english)); it != state.lines.end()) {
			return it->second.c_str();
		}
		if (std::this_thread::get_id() == state.main) {
			return Detail::Resolve(state, a_english);
		}
		state.asked.insert(a_english);
		if (!state.queued) {
			if (auto* tasks = SKSE::GetTaskInterface()) {
				state.queued = true;
				tasks->AddTask([]() {
					auto&           s = Detail::Get();
					std::lock_guard g{ s.lock };
					for (const auto& english : std::exchange(s.asked, {})) {
						static_cast<void>(Detail::Resolve(s, english.c_str()));
					}
					s.queued = false;
				});
			}
		}
		return a_english;
	}

	inline const char* Detail::Resolve(State& a_state, const char* a_english)
	{
		auto& state = a_state;
		if (const auto it = state.lines.find(std::string_view(a_english)); it != state.lines.end()) {
			return it->second.c_str();
		}
		std::string shown = a_english;
		std::string read;
		const auto  key = TranslationText::KeyOf(state.name, a_english);
		if (SKSE::Translation::Translate(key, read) && read != key) {
			read = TranslationText::Unescape(read);
			if (TranslationText::Usable(a_english, read)) {
				shown = std::move(read);
			} else if (!read.empty() && read != a_english) {
				SKSE::log::warn("translation: placeholders differ, English kept: \"{}\"", a_english);
			}
		}
		return state.lines.emplace(a_english, std::move(shown)).first->second.c_str();
	}
}
