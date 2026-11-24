// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; master in Tools\Shared Plugin Code\)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// THE ADVANCED SETTINGS FILE AS TEXT - game-free, tested off the game (tests\test_fade.cpp): reading its lines, a value as a
// number, the version a file says it is, and the whole file written out. FadeTuning.cpp does the files and the log.
// The file ships in the main download, every line explained; a second file beside it ("... - Custom.ini") that no download
// carries keeps a player's own lines through updates (SSE Display Tweaks' _custom.ini way - its method only, no code of it).

#pragma once

#include "AdvancedLayout.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Fade::TuningText
{
	// the layout this code writes; a file that says less (or nothing) is written again with its values kept
	inline constexpr int kFileVersion = 3;

	struct Entry
	{
		std::string value;  // as written
		std::string shown;  // "[Section] Key" as written, for a log line
		bool        used{ false };
	};
	using Entries = std::map<std::string, Entry>;  // "section|key", lower case

	struct Knob
	{
		std::string section, key, note;
		float       def{ 0.0f }, lo{ 0.0f }, hi{ 0.0f };
		bool        common{ false };  // listed under "most people only change these"
	};

	[[nodiscard]] inline std::string_view Trim(std::string_view a_s)
	{
		constexpr std::string_view kSpace = " \t\r\n";
		const auto                 first = a_s.find_first_not_of(kSpace);
		if (first == std::string_view::npos) {
			return {};
		}
		return a_s.substr(first, a_s.find_last_not_of(kSpace) - first + 1);
	}

	[[nodiscard]] inline std::string Lower(std::string_view a_s)
	{
		std::string out(a_s);
		std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
		return out;
	}

	[[nodiscard]] inline std::string KeyOf(std::string_view a_section, std::string_view a_key) { return Lower(a_section) + "|" + Lower(a_key); }

	// every "Key=value" under its [Section]; ';' or '#' starts a note; a UTF-8 byte order mark (Notepad's) is passed over; a key
	// written twice keeps its last value
	[[nodiscard]] inline Entries Parse(std::string_view a_text)
	{
		Entries     found;
		std::string section, sectionShown;
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);
		}
		while (!a_text.empty()) {
			const auto       nl = a_text.find('\n');
			std::string_view t = a_text.substr(0, nl);
			a_text = nl == std::string_view::npos ? std::string_view{} : a_text.substr(nl + 1);
			if (const auto c = t.find_first_of(";#"); c != std::string_view::npos) {
				t = t.substr(0, c);
			}
			t = Trim(t);
			if (t.empty()) {
				continue;
			}
			if (t.front() == '[' && t.back() == ']') {
				sectionShown = std::string(Trim(t.substr(1, t.size() - 2)));
				section = Lower(sectionShown);
				continue;
			}
			if (const auto eq = t.find('='); eq != std::string_view::npos) {
				const auto key = Trim(t.substr(0, eq));
				if (!key.empty()) {
					found[section + "|" + Lower(key)] = { std::string(Trim(t.substr(eq + 1))), "[" + sectionShown + "] " + std::string(key) };
				}
			}
		}
		return found;
	}

	// the whole value a number: "0,35" or "300px" is not (from_chars stops at the first character it cannot use)
	[[nodiscard]] inline bool Number(std::string_view a_s, float& a_out)
	{
		float v = 0.0f;
		if (a_s.starts_with('+')) {
			a_s.remove_prefix(1);
			if (a_s.starts_with('+') || a_s.starts_with('-')) {
				return false;  // "+-1" is not a number: the main file's value stays
			}
		}
		const auto [end, ec] = std::from_chars(a_s.data(), a_s.data() + a_s.size(), v);
		if (a_s.empty() || ec != std::errc{} || end != a_s.data() + a_s.size() || !std::isfinite(v)) {
			return false;
		}
		a_out = v;
		return true;
	}

	// the "(file version N)" a file says in its first five lines; 0 for a file without one (written before versions, or by hand)
	[[nodiscard]] inline int FileVersion(std::string_view a_text)
	{
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);
		}
		constexpr std::string_view kMark = "(file version ";
		for (int line = 0; line < 5 && !a_text.empty(); ++line) {
			const auto nl = a_text.find('\n');
			const auto l = a_text.substr(0, nl);
			a_text = nl == std::string_view::npos ? std::string_view{} : a_text.substr(nl + 1);
			const auto at = l.find(kMark);
			if (at == std::string_view::npos) {
				continue;
			}
			int        v = 0;
			const auto from = l.data() + at + kMark.size();
			const auto [end, ec] = std::from_chars(from, l.data() + l.size(), v);
			return ec == std::errc{} && end != from ? v : 0;
		}
		return 0;
	}

	// a file saved as UTF-16 (Notepad's "Unicode"), its byte order mark either way round: its lines cannot be read here
	[[nodiscard]] inline bool Utf16(std::string_view a_text) { return a_text.starts_with("\xFF\xFE") || a_text.starts_with("\xFE\xFF"); }

	// a knob whose own line ([Section] Key) the file does not hold: the file is written again, every value it has kept
	[[nodiscard]] inline bool LacksAny(const Entries& a_found, const std::vector<Knob>& a_knobs)
	{
		return std::ranges::any_of(a_knobs, [&a_found](const Knob& a_k) { return !a_found.contains(KeyOf(a_k.section, a_k.key)); });
	}

	// letters apart: one added, removed or changed counts one; case is ignored
	[[nodiscard]] inline std::size_t Distance(std::string_view a_a, std::string_view a_b)
	{
		const auto               a = Lower(a_a), b = Lower(a_b);
		std::vector<std::size_t> row(b.size() + 1);
		for (std::size_t j = 0; j <= b.size(); ++j) {
			row[j] = j;
		}
		for (std::size_t i = 1; i <= a.size(); ++i) {
			std::size_t diag = row[0];
			row[0] = i;
			for (std::size_t j = 1; j <= b.size(); ++j) {
				const std::size_t up = row[j];
				row[j] = (std::min)((std::min)(row[j] + 1, row[j - 1] + 1), diag + (a[i - 1] == b[j - 1] ? std::size_t{ 0 } : std::size_t{ 1 }));
				diag = up;
			}
		}
		return row[b.size()];
	}

	// the knob a misspelled key most likely means: within 1 letter for a key of up to 4 letters, 2 up to 8, else 3; of two as near, the
	// one in the key's own section; none when still tied. Its index in a_knobs, or a_knobs.size() for none
	[[nodiscard]] inline std::size_t Nearest(std::string_view a_section, std::string_view a_key, const std::vector<Knob>& a_knobs)
	{
		const std::size_t most = a_key.size() <= 4 ? 1 : (a_key.size() <= 8 ? 2 : 3);
		std::size_t       best = most + 1;
		for (const auto& k : a_knobs) {
			best = (std::min)(best, Distance(a_key, k.key));
		}
		if (best > most) {
			return a_knobs.size();
		}
		std::vector<std::size_t> nearest, sameSection;
		for (std::size_t i = 0; i < a_knobs.size(); ++i) {
			if (Distance(a_key, a_knobs[i].key) == best) {
				nearest.push_back(i);
				if (Lower(a_knobs[i].section) == Lower(a_section)) {
					sameSection.push_back(i);
				}
			}
		}
		if (nearest.size() == 1) {
			return nearest.front();
		}
		return sameSection.size() == 1 ? sameSection.front() : a_knobs.size();
	}

	// a knob's key (or its old name) written under a section no knob has - an older or misspelled one: that one entry when exactly one
	// such entry holds it, else none
	[[nodiscard]] inline Entries::iterator Moved(Entries& a_found, std::string_view a_key, std::string_view a_was, const std::vector<Knob>& a_knobs)
	{
		auto       out = a_found.end();
		const auto key = Lower(a_key), was = Lower(a_was);
		for (auto it = a_found.begin(); it != a_found.end(); ++it) {
			const auto bar = it->first.find('|');
			const auto section = it->first.substr(0, bar), name = it->first.substr(bar + 1);
			if (name != key && (was.empty() || name != was)) {
				continue;
			}
			if (std::ranges::any_of(a_knobs, [&](const Knob& k) { return Lower(k.section) == section; })) {
				continue;
			}
			if (out != a_found.end()) {
				return a_found.end();
			}
			out = it;
		}
		return out;
	}

	// a number as the file shows it: 6 significant digits, no trailing zeros (2800, 0.35, 1.1) - the generators' %g writes the same
	[[nodiscard]] inline std::string Show(float a_v)
	{
		std::ostringstream s;
		s << a_v;
		return s.str();
	}

	// the whole file in the shared layout (AdvancedLayout.h): a_values holds each knob's value to write, in a_knobs' order
	[[nodiscard]] inline std::string Render(std::string_view a_mod, std::string_view a_customName, const std::vector<Knob>& a_knobs,
		const std::vector<float>& a_values)
	{
		std::vector<AdvancedLayout::Row> rows;
		for (std::size_t i = 0; i < a_knobs.size(); ++i) {
			const auto& k = a_knobs[i];
			rows.push_back({ k.section, k.key, k.note, k.def, k.lo, k.hi, i < a_values.size() ? a_values[i] : k.def, false, k.common });
		}
		return AdvancedLayout::Render(a_mod, a_customName, kFileVersion, rows);
	}
}
