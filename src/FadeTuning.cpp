// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// THE ALL-IN-ONE ADVANCED SETTINGS FILE - HIS RULE: "illuminated and relight should also both be dumping inis for all
// settings that can be changed that make sense like illuminated does for fading. nothing that the skse menu already handles
// though. make them aio and seperated by category such as fading." So: ONE file per mod (Mod::kAdvancedPath), a section per
// category, holding the values worth tuning that the menu does NOT show - the menu's settings stay in the menu and its own files.
// The main download ships the file, every line explained (the generators write it from the Register calls). At data load it is
// read, then the player's own file beside it (Mod::kAdvancedCustomPath, in no download) whose lines win; a value out of range is
// clamped and said in the log. The file is written only when it is missing, older than this layout or lacks a value (its values
// kept); a file saved as UTF-16 is neither read nor rewritten. Read once: a change takes effect the next time the game starts.

#include "Fade.h"
#include "FadeTuningText.h"

#include <sstream>

namespace Fade::Tuning
{
	namespace
	{
		struct Knob
		{
			TuningText::Knob    text;
			std::string         was;  // the key's old name, still read (empty: never renamed)
			std::atomic<float>* value{ nullptr };
		};
		std::vector<Knob>& Knobs()
		{
			static std::vector<Knob> knobs;
			return knobs;
		}

		// values an older version wrote ("section|key", lower case): passed over quietly and left out of a rewritten file - the
		// reticle's, his order 2026-10-19 ("completely remove our reticle options ... no settings in our mod")
		constexpr std::string_view kRetired[]{ "hud|reticlegap", "hud|reticlelength", "hud|crosshairbarwidth", "hud|crossfirebarwidth",
			"hud|crosshairbardrop", "hud|crossfirebardrop", "hud|emptypulsespeed", "hud|fullpulseseconds", "hud|reticleholdseconds" };

		[[nodiscard]] std::string ReadWhole(const std::string& a_path)
		{
			std::ifstream      in(a_path, std::ios::binary);
			std::ostringstream s;
			s << in.rdbuf();
			return s.str();
		}

		// sections renamed for the player-ordered layout: a file written before still reads under its old name
		constexpr std::pair<std::string_view, std::string_view> kSectionWas[]{ { "General", "Lights" }, { "Fading with Magicka", "Fading" },
			{ "Streams and Rows", "Streams and sprays" } };

		// listed at the top of the file, under "most people only change these" ("section|key", lower case)
		constexpr std::string_view kCommon[]{ "streams and rows|rowspacing", "vanilla and enb|vanillabrightness", "vanilla and enb|vanillacolorstrength",
			"fading with magicka|minreach", "hud|gemdistance", "hud|gemheight" };

		// a knob's entry in a parsed file: its name, else its old key name, else either under its section's old name; the first found
		// wins and any other one is passed over quietly
		[[nodiscard]] TuningText::Entry* EntryOf(TuningText::Entries& a_found, const Knob& a_k, const std::vector<TuningText::Knob>& a_texts)
		{
			std::vector<std::string> names{ TuningText::KeyOf(a_k.text.section, a_k.text.key) };
			if (!a_k.was.empty()) {
				names.push_back(TuningText::KeyOf(a_k.text.section, a_k.was));
			}
			for (const auto& [now, old] : kSectionWas) {
				if (TuningText::Lower(now) == TuningText::Lower(a_k.text.section)) {
					names.push_back(TuningText::KeyOf(old, a_k.text.key));
					if (!a_k.was.empty()) {
						names.push_back(TuningText::KeyOf(old, a_k.was));
					}
				}
			}
			TuningText::Entry* first = nullptr;
			for (std::size_t i = 0; i < names.size(); ++i) {
				if (auto it = a_found.find(names[i]); it != a_found.end()) {
					if (!first) {
						first = &it->second;
						if (i > 0) {
							SKSE::log::info("advanced settings: {} is read as [{}] {} (its new name)", it->second.shown, a_k.text.section, a_k.text.key);
						}
					} else {
						it->second.used = true;
					}
				}
			}
			if (!first) {
				// under a section no value has (an older or misspelled one): read where it belongs when no other section has it
				if (const auto it = TuningText::Moved(a_found, a_k.text.key, a_k.was, a_texts); it != a_found.end()) {
					SKSE::log::info("advanced settings: {} is read as [{}] {}", it->second.shown, a_k.text.section, a_k.text.key);
					first = &it->second;
				}
			}
			return first;
		}

		// a knob's value from one file's entry: false (and the value untouched) when it is not a number
		bool Take(TuningText::Entry& a_e, const Knob& a_k, const char* a_file, float& a_value)
		{
			a_e.used = true;
			float v = 0.0f;
			if (!TuningText::Number(a_e.value, v)) {
				SKSE::log::warn("advanced settings: {} {}={} is not a number; it is passed over", a_file, a_e.shown, a_e.value);
				return false;
			}
			if (v < a_k.text.lo || v > a_k.text.hi) {
				SKSE::log::warn("advanced settings: {} {}={} is outside {} .. {}; clamped", a_file, a_e.shown, v, a_k.text.lo, a_k.text.hi);
				v = std::clamp(v, a_k.text.lo, a_k.text.hi);
			}
			a_value = v;
			return true;
		}

		// each line that is no value, named; a near spelling of one gets a hint
		void WarnUnused(const TuningText::Entries& a_found, const char* a_file, const std::vector<TuningText::Knob>& a_texts)
		{
			for (const auto& [key, e] : a_found) {
				if (e.used || std::ranges::find(kRetired, key) != std::ranges::end(kRetired)) {
					continue;
				}
				const auto bar = key.find('|');
				if (const auto i = TuningText::Nearest(key.substr(0, bar), key.substr(bar + 1), a_texts); i < a_texts.size()) {
					SKSE::log::warn("advanced settings: {} {} is not a setting; it is passed over - did you mean [{}] {}?", a_file, e.shown,
						a_texts[i].section, a_texts[i].key);
				} else {
					SKSE::log::warn("advanced settings: {} {} is not a setting; it is passed over", a_file, e.shown);
				}
			}
		}
	}

	void Register(std::string_view a_section, std::string_view a_key, float a_default, float a_lo, float a_hi, std::string_view a_note,
		std::atomic<float>& a_value, std::string_view a_was)
	{
		a_value.store(a_default, std::memory_order_relaxed);
		const bool common = std::ranges::find(kCommon, TuningText::KeyOf(a_section, a_key)) != std::ranges::end(kCommon);
		Knobs().push_back({ { std::string(a_section), std::string(a_key), std::string(a_note), a_default, a_lo, a_hi, common }, std::string(a_was), &a_value });
	}

	void Load()
	{
		auto&             knobs = Knobs();
		const auto        from = SettingsText::ReadFrom(Mod::kAdvancedPath, Mod::kOldAdvancedPath);
		const std::string mainText = ReadWhole(from);
		const std::string mineText = ReadWhole(Mod::kAdvancedCustomPath);
		// a file saved as UTF-16 would read as nothing but noise: it is not read, never rewritten, and its defaults hold
		const bool mainWide = TuningText::Utf16(mainText), mineWide = TuningText::Utf16(mineText);
		if (mainWide) {
			SKSE::log::warn("advanced settings: {} is saved as UTF-16 (Unicode); it is not read and left as it is - save it as UTF-8 to use its values", from);
		}
		if (mineWide) {
			SKSE::log::warn("advanced settings: {} is saved as UTF-16 (Unicode); it is not read - save it as UTF-8 to use its values", Mod::kAdvancedCustomPath);
		}
		auto                          found = mainWide ? TuningText::Entries{} : TuningText::Parse(mainText);
		auto                          mine = mineWide ? TuningText::Entries{} : TuningText::Parse(mineText);
		std::vector<float>            fileValues;
		std::size_t                   taken = 0, takenMine = 0;
		std::vector<TuningText::Knob> texts;
		for (const auto& k : knobs) {
			texts.push_back(k.text);
		}
		const bool lacks = TuningText::LacksAny(found, texts);
		for (auto& k : knobs) {
			float v = k.text.def;
			if (auto* e = EntryOf(found, k, texts); e && Take(*e, k, "the file:", v)) {
				++taken;
			}
			fileValues.push_back(v);
			if (auto* e = EntryOf(mine, k, texts); e && Take(*e, k, "Custom:", v)) {
				++takenMine;
			}
			k.value->store(v, std::memory_order_relaxed);
		}
		WarnUnused(found, "the file:", texts);
		WarnUnused(mine, "Custom:", texts);
		SKSE::log::info("advanced settings: {} value(s) of {} read from {}, {} from {}", taken, knobs.size(), from, takenMine, Mod::kAdvancedCustomPath);
		// written only when there is no file, one older than this layout or one that lacks a value (its own values kept, never the
		// Custom file's); a UTF-16 file is left alone
		std::error_code ec;
		const bool      haveNew = std::filesystem::exists(Mod::kAdvancedPath, ec);
		const bool      older = TuningText::FileVersion(mainText) < TuningText::kFileVersion;
		if (mainWide || (haveNew && !older && !lacks)) {
			return;
		}
		// a file the player made read-only stays as it is: its defaults hold for what it lacks
		if (haveNew && (std::filesystem::status(Mod::kAdvancedPath, ec).permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
			SKSE::log::info("advanced settings: {} is read-only, so it is not written again", Mod::kAdvancedPath);
			return;
		}
		const auto customName = std::filesystem::path(Mod::kAdvancedCustomPath).filename().string();
		std::filesystem::create_directories(std::filesystem::path(Mod::kAdvancedPath).parent_path(), ec);
		const std::string tmp = std::string(Mod::kAdvancedPath) + ".tmp";
		{
			std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
			out << TuningText::Render(Mod::kName, customName, texts, fileValues);
			if (!out.flush()) {
				SKSE::log::warn("advanced settings: {} could not be written", Mod::kAdvancedPath);
				return;
			}
		}
		if (std::filesystem::rename(tmp, Mod::kAdvancedPath, ec); ec) {
			SKSE::log::warn("advanced settings: {} could not be replaced ({})", Mod::kAdvancedPath, ec.message());
			std::filesystem::remove(tmp, ec);
			return;
		}
		SKSE::log::info("advanced settings: {} written ({}), its values kept", Mod::kAdvancedPath,
			!haveNew ? "there was none" :
			older    ? "an older layout" :
					   "it lacked a value");
	}
}
