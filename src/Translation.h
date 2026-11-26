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

#include <cstdint>
#include <format>
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
	using TranslationText::Shown;

	// a translation file past this is not read (ours are a few KB): the game would hold its every line, and the menu stays English
	inline constexpr std::uint64_t kMostFileBytes = 4ull << 20;

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
			std::unordered_map<std::string, std::string, Hash, std::equal_to<>> lines;   // a line's cache key (CacheKey) -> what the menu shows
			std::thread::id                                                     main{};  // Load's thread: the game's main thread
			std::unordered_set<std::string>                                     asked;   // cache keys asked for off it (copied: a caller's text may not outlive the call)
			bool                                                                queued{ false };
		};

		[[nodiscard]] inline State& Get()
		{
			static State state;
			return state;
		}

		// a line the caller says how it shows is kept apart from the same line guessed (the verdict may differ): a control mark in
		// front, which no English line of ours starts with
		[[nodiscard]] inline std::string CacheKey(std::string_view a_english, Shown a_shown)
		{
			if (a_shown == Shown::kGuess) {
				return std::string(a_english);
			}
			return std::string(1, a_shown == Shown::kText ? '\x01' : '\x02') + std::string(a_english);
		}

		[[nodiscard]] inline std::pair<std::string_view, Shown> FromCacheKey(std::string_view a_key) noexcept
		{
			if (!a_key.empty() && (a_key.front() == '\x01' || a_key.front() == '\x02')) {
				return { a_key.substr(1), a_key.front() == '\x01' ? Shown::kText : Shown::kPrintf };
			}
			return { a_key, Shown::kGuess };
		}

		// CommonLib's SKSE::Translation::ParseTranslation and ::Translate (our pin, Translation.cpp 46 / 129) each take a reference on
		// the game's translator (GetStateAddRef) and never give it back, on every path once the loader is there. Called right after
		// either: one reference of our own is taken and released with the one the helper left (none when the loader is not there,
		// as the helper took none). The game keeps its own references, so the translator never goes. Main thread
		inline void GiveBackTranslator()
		{
			const auto* manager = RE::BSScaleformManager::GetSingleton();
			auto*       loader = manager ? manager->loader : nullptr;
			if (auto* translator = loader ? loader->GetStateAddRef<RE::GFxTranslator>(RE::GFxState::StateType::kTranslator) : nullptr) {
				translator->Release();
				translator->Release();
			}
		}

		// the file ParseTranslation opens (its own path and language, Translation.cpp 55-63), through the game's loader, so a file
		// packed in an archive is measured too; 0 when there is none
		[[nodiscard]] inline std::uint64_t FileBytes(std::string_view a_name, std::string& a_path)
		{
			auto*       ini = RE::INISettingCollection::GetSingleton();
			const auto* setting = ini ? ini->GetSetting("sLanguage:General") : nullptr;
			const char* language = setting && setting->GetType() == RE::Setting::Type::kString && setting->data.s ? setting->data.s : "ENGLISH";
			a_path = std::format("Interface\\Translations\\{}_{}.txt", a_name, language);
			RE::BSResourceNiBinaryStream file{ a_path };
			return file.good() && file.stream ? static_cast<std::uint64_t>(file.stream->totalSize) : 0;
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
		std::string path;
		if (const auto bytes = Detail::FileBytes(a_name, path); bytes > kMostFileBytes) {
			return std::format("translation: {} is {} bytes, more than {} - it is not read and the menu stays English", path, bytes, kMostFileBytes);
		}
		SKSE::Translation::ParseTranslation(std::string(a_name));
		Detail::GiveBackTranslator();
		return "translation: the menu's lines come from Interface\\Translations\\" + std::string(a_name) +
		       "_<the game's language>.txt; a missing file or line stays English";
	}

	namespace Detail
	{
		// under state.lock, on the main thread only: the game's translator read for one line, kept
		inline const char* Resolve(State& a_state, std::string_view a_key);

		// on the main thread: every line asked for off it, each taken out only once it is read, so a line whose read throws stays
		// for the next task; no exception leaves the task (SKSE's queue runs it inside the game)
		inline void ResolveAsked() noexcept
		{
			auto& s = Get();
			try {
				std::lock_guard g{ s.lock };
				try {
					while (!s.asked.empty()) {
						const std::string key = *s.asked.begin();
						static_cast<void>(Resolve(s, key));
						s.asked.erase(key);
					}
				} catch (...) {
				}
				s.queued = false;
			} catch (...) {
			}
		}
	}

	// the line in the reader's language, or the English itself. Each line is asked of the game's translator once, on the main
	// thread: asked first from another thread (the menu draws on the render thread) it shows its English until one task has read it.
	// a_shown: how the caller shows it (a printf format, plain text), else guessed from the line
	inline const char* T(const char* a_english, Shown a_shown = Shown::kGuess)
	{
		auto&           state = Detail::Get();
		std::lock_guard l{ state.lock };
		if (state.name.empty()) {
			return a_english;
		}
		// a guessed line is looked up by its own text, so a draw makes no string
		const std::string      marked = a_shown == Shown::kGuess ? std::string{} : Detail::CacheKey(a_english, a_shown);
		const std::string_view key = a_shown == Shown::kGuess ? std::string_view(a_english) : std::string_view(marked);
		if (const auto it = state.lines.find(key); it != state.lines.end()) {
			return it->second.c_str();
		}
		if (std::this_thread::get_id() == state.main) {
			return Detail::Resolve(state, key);
		}
		state.asked.emplace(key);
		if (!state.queued) {
			if (auto* tasks = SKSE::GetTaskInterface()) {
				// marked only once the task is in: a task that could not be added leaves the lines asked for the next draw to queue
				tasks->AddTask([]() { Detail::ResolveAsked(); });
				state.queued = true;
			}
		}
		return a_english;
	}

	inline const char* Detail::Resolve(State& a_state, std::string_view a_key)
	{
		auto& state = a_state;
		if (const auto it = state.lines.find(a_key); it != state.lines.end()) {
			return it->second.c_str();
		}
		const auto [englishView, shownAs] = FromCacheKey(a_key);
		const std::string english(englishView);
		std::string       shown = english;
		std::string       read;
		const auto        key = TranslationText::KeyOf(state.name, english);
		const bool        translated = SKSE::Translation::Translate(key, read);
		GiveBackTranslator();  // our keys carry no {}: Translate took exactly one reference
		if (translated && read != key) {
			read = TranslationText::Unescape(read);
			if (TranslationText::Usable(english, read, shownAs)) {
				shown = std::move(read);
			} else if (!read.empty() && read != english) {
				SKSE::log::warn("translation: placeholders differ, English kept: \"{}\"", english);
			}
		}
		return state.lines.emplace(std::string(a_key), std::move(shown)).first->second.c_str();
	}
}
