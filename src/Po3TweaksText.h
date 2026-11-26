// powerofthree's Tweaks, the parts that need no game (master: Tools\Shared Plugin Code; shared code sync.py keeps the copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// powerofthree's Tweaks (powerof3, credited; its source read on GitHub, powerof3/po3-Tweaks) keeps the editor IDs the game throws away
// (its "Load EditorIDs" fix, on by default): it puts each into the game's own editor-ID map and exports GetFormEditorID. Our mods
// require it (rule file 2 rule 19c, requirements over rebuilds); Po3Tweaks.h judges it once after the data has loaded and names what
// is missing. Here the judging, a form key's two spellings, its ini switch and the box's words, tested off the game.

#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Po3TweaksText
{
	// a form named in our files: "0x<id>~<plugin>" (the 0x may be left out) is a form id in its plugin; one word with no ~ is an editor ID
	enum class Key
	{
		kBad,
		kFormID,
		kEditorID
	};

	struct FormKey
	{
		Key              kind{ Key::kBad };
		std::uint32_t    local{ 0 };
		std::string_view plugin;
		std::string_view editorID;
	};

	namespace Detail
	{
		[[nodiscard]] inline std::string_view Trim(std::string_view a_s) noexcept
		{
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
				a_s.remove_prefix(1);
			}
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
				a_s.remove_suffix(1);
			}
			return a_s;
		}

		[[nodiscard]] inline std::string Lower(std::string_view a_s)
		{
			std::string out(a_s);
			for (auto& c : out) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return out;
		}
	}

	[[nodiscard]] inline FormKey ParseKey(std::string_view a_text) noexcept
	{
		const auto text = Detail::Trim(a_text);
		FormKey    out;
		if (text.empty()) {
			return out;
		}
		if (const auto tilde = text.find('~'); tilde != std::string_view::npos) {
			auto       hex = Detail::Trim(text.substr(0, tilde));
			const auto plugin = Detail::Trim(text.substr(tilde + 1));
			if (hex.starts_with("0x") || hex.starts_with("0X")) {
				hex.remove_prefix(2);
			}
			std::uint32_t local = 0;
			const auto*   end = hex.data() + hex.size();
			if (const auto [stop, ec] = std::from_chars(hex.data(), end, local, 16); hex.empty() || ec != std::errc{} || stop != end || plugin.empty()) {
				return out;
			}
			out.kind = Key::kFormID;
			out.local = local;
			out.plugin = plugin;
			return out;
		}
		for (const char c : text) {
			if (std::isspace(static_cast<unsigned char>(c)) || c == '|' || c == '"') {
				return out;
			}
		}
		out.kind = Key::kEditorID;
		out.editorID = text;
		return out;
	}

	// its ini, [Fixes] bLoadEditorIDs ("Load EditorIDs" in older files): on unless it says off, read as its ini reader reads a switch
	// (f, n, of or 0 is off); only under [Fixes], where Tweaks keeps it (its Settings.h); the current key wins over the old one
	[[nodiscard]] inline bool LoadsEditorIDs(std::istream& a_ini)
	{
		std::optional<bool> current, legacy;
		bool                fixes = false;
		const auto          off = [](std::string_view a_v) { return a_v.starts_with('f') || a_v.starts_with('n') || a_v.starts_with("of") || a_v.starts_with('0'); };
		bool                first = true;
		for (std::string line; std::getline(a_ini, line);) {
			// a UTF-8 mark at the file's start is no part of its first section's name
			if (std::exchange(first, false) && line.starts_with("\xEF\xBB\xBF")) {
				line.erase(0, 3);
			}
			std::string key;
			for (const char c : line) {
				if (c == ';' || c == '#') {
					break;
				}
				if (!std::isspace(static_cast<unsigned char>(c))) {
					key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
			if (key.starts_with('[')) {
				fixes = key.starts_with("[fixes]");
				continue;
			}
			if (!fixes) {
				continue;
			}
			if (key.starts_with("bloadeditorids=")) {
				current = !off(std::string_view(key).substr(15));
			} else if (key.starts_with("loadeditorids=")) {
				legacy = !off(std::string_view(key).substr(14));
			}
		}
		return current.value_or(legacy.value_or(true));
	}

	// the proof that its editor IDs are really there: its export names the player's own base form (Skyrim.esm 0x7, a character, whose
	// editor ID the game throws away) as "Player"
	inline constexpr std::uint32_t kProofForm = 0x7;

	[[nodiscard]] inline bool ProofHolds(const char* a_name) { return a_name && Detail::Lower(a_name) == "player"; }

	// not installed; a version with no GetFormEditorID; its ini switches the editor IDs off; its export did not name the proof form; not
	// Tweaks, but the game names its own forms (another mod restored them, as Native EditorID Fix does); on and proven, so it is used
	enum class State
	{
		kAbsent,
		kNoExport,
		kOff,
		kNoProof,
		kNativeNames,
		kKeepsThem,
	};

	// a_native: the proof form's editor ID as the game itself gives it (GetFormEditorID), which another mod may have restored
	[[nodiscard]] inline State Judge(bool a_dll, bool a_export, bool a_iniOn, const char* a_proof, const char* a_native = nullptr)
	{
		State tweaks = State::kKeepsThem;
		if (!a_dll) {
			tweaks = State::kAbsent;
		} else if (!a_export) {
			tweaks = State::kNoExport;
		} else if (!a_iniOn) {
			tweaks = State::kOff;
		} else if (!ProofHolds(a_proof)) {
			tweaks = State::kNoProof;
		}
		return tweaks != State::kKeepsThem && ProofHolds(a_native) ? State::kNativeNames : tweaks;
	}

	// editor IDs are there, from Tweaks or from the game's own forms
	[[nodiscard]] inline bool NamesKept(State a_state) noexcept { return a_state == State::kKeepsThem || a_state == State::kNativeNames; }

	[[nodiscard]] inline std::string_view Describe(State a_state) noexcept
	{
		switch (a_state) {
		case State::kKeepsThem:
			return "powerofthree's Tweaks keeps editor IDs - forms may be named by editor ID, and the log names them";
		case State::kNativeNames:
			return "Native EditorID Fix (or the game itself) keeps editor IDs - forms may be named by editor ID, and the log names them";
		case State::kOff:
			return "powerofthree's Tweaks is required with its Load EditorIDs on, and its ini has it off - forms named by editor ID are not found";
		case State::kNoExport:
			return "powerofthree's Tweaks is required, and the version installed is too old to share editor IDs - update it";
		case State::kNoProof:
			return "powerofthree's Tweaks is required, and it did not name a known form - its editor IDs are not used";
		case State::kAbsent:
		default:
			return "powerofthree's Tweaks is required and not installed - forms named by editor ID are not found";
		}
	}

	// the one box at data load when Tweaks does not keep editor IDs: what is missing and where to get it (Nexus mod 51073)
	[[nodiscard]] inline std::string BoxText(std::string_view a_mod, State a_state)
	{
		std::string text(a_mod);
		text += " needs powerofthree's Tweaks";
		if (a_state == State::kOff) {
			text += " with its Load EditorIDs setting on";
		} else if (a_state == State::kNoExport) {
			text += " in a newer version";
		}
		text += ". Install powerofthree's Tweaks from Nexus (mod 51073).";
		return text;
	}

	// what a log line adds when a form named by editor ID was not found: the hint that Tweaks, which the mod requires, is not keeping them
	[[nodiscard]] inline std::string_view MissingHint(const FormKey& a_key, bool a_keeps) noexcept
	{
		if (a_key.kind != Key::kEditorID || a_keeps) {
			return {};
		}
		return " (powerofthree's Tweaks, which this mod requires, is not keeping editor IDs)";
	}
}
