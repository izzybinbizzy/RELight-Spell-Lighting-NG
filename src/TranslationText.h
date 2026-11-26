// Translation of the settings pages - the parts that need no game (shared by izzydoingit's SKSE plugins; master: Tools\Shared Plugin Code)
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// The menu reads the standard Skyrim translation files (Translation.h does the game side). Here what the off-game tests reach: the
// placeholders a line carries and the check that lets a translation stand for its English line (a translation whose placeholders -
// %d, %s, %.0f ... or {} - differ would print garbage or crash, so it is not used), a line's key in the file (translationfiles.py
// makes the same sum), the file's written newline, and Fill.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace TranslationText
{
	// the printf conversions (%% excluded) and the {} fields of a line, in order. A '*' width or precision belongs to its
	// conversion ("%*s" is not "%s" - it takes one more argument). a_format: the line is printed through printf, so the
	// space and ' flags count too ("% d" takes an argument there); otherwise "100% is" stays plain text.
	inline std::vector<std::string> Placeholders(std::string_view a_s, bool a_format = false)
	{
		const std::string_view   flags = a_format ? std::string_view("-+#0123456789.lhzjtLIw* '") : std::string_view("-+#0123456789.lhzjtLIw*");
		std::vector<std::string> out;
		for (std::size_t i = 0; i < a_s.size(); ++i) {
			if (a_s[i] == '%') {
				if (i + 1 < a_s.size() && a_s[i + 1] == '%') {
					++i;
					continue;
				}
				// a conversion is flags/width/length then one conversion letter - MSVC's own %S, %C, %Z and its I, I32,
				// I64 and w sizes included ("%I64u" is one conversion, not "%I" and text)
				std::size_t j = i + 1;
				while (j < a_s.size() && flags.find(a_s[j]) != std::string_view::npos) {
					++j;
				}
				const bool known = j < a_s.size() && std::string_view("diouxXeEfFgGaAcspnSCZ").find(a_s[j]) != std::string_view::npos;
				// printf reads ANY '%' as a conversion: one it does not know (a translated "50 % r") or a '%' at the very
				// end counts too, so it never matches the English and never reaches printf
				if (known || a_format) {
					out.emplace_back(a_s.substr(i, j - i + 1));
					i = j;
				}
			} else if (a_s[i] == '{') {
				const auto j = a_s.find('}', i);
				if (j == std::string_view::npos) {
					// an unmatched brace is a marker of its own; the scan goes on, so a conversion after it is still read
					out.emplace_back("{");
					continue;
				}
				out.emplace_back(a_s.substr(i, j - i + 1));
				i = j;
			}
		}
		return out;
	}

	// how a line reaches the screen: as plain text (a "%s" argument), as a printf format, or not said - then a line with a
	// conversion or a "%%" counts as a format ("% d", a space flag alone, is read as text there; a caller that prints it says so)
	enum class Shown
	{
		kGuess,
		kText,
		kPrintf
	};

	// a translation stands for its English line only when it says something else with the same placeholders; a line printed
	// through printf has both sides read by printf's own rules
	[[nodiscard]] inline bool Usable(std::string_view a_english, std::string_view a_translated, Shown a_shown = Shown::kGuess)
	{
		if (a_translated.empty() || a_translated == a_english) {
			return false;
		}
		const auto lenient = Placeholders(a_english);
		const bool format = a_shown == Shown::kPrintf ||
		                    (a_shown == Shown::kGuess && (a_english.find("%%") != std::string_view::npos ||
															 std::ranges::any_of(lenient, [](const std::string& a_p) { return a_p.starts_with('%'); })));
		if (!format) {
			// a plain line is shown as text, never printed through printf: only its {} fields must match. A translation that brings
			// a conversion of its own (%s, %d) is refused all the same: its English may be a printf line this scan reads as plain
			// ("% d", a space flag), and a conversion with no argument behind it reads garbage
			const auto translated = Placeholders(a_translated);
			if (std::ranges::any_of(translated, [](const std::string& a_p) { return a_p.starts_with('%'); })) {
				return false;
			}
			const auto fields = [](std::vector<std::string> a_all) {
				std::erase_if(a_all, [](const std::string& a_p) { return a_p.starts_with('%'); });
				return a_all;
			};
			return fields(lenient) == fields(translated);
		}
		return Placeholders(a_english, format) == Placeholders(a_translated, format);
	}

	// a line's key in the standard translation file: "$<name>_" and the FNV-1a 32-bit hash of its English text (UTF-8 bytes) in 8
	// hex digits - one short key whatever the line's length, so a line and its translation fit the 510 characters the game's reader
	// takes. translationfiles.py makes the same sum
	[[nodiscard]] inline std::string KeyOf(std::string_view a_name, std::string_view a_english)
	{
		std::uint32_t h = 0x811C9DC5u;
		for (const char c : a_english) {
			h = (h ^ static_cast<unsigned char>(c)) * 0x01000193u;
		}
		char hex[9]{};
		std::snprintf(hex, sizeof(hex), "%08X", static_cast<unsigned int>(h));
		return "$" + std::string(a_name) + "_" + hex;
	}

	// a line of the file holds no line break: the file writes one as \n, read back here
	[[nodiscard]] inline std::string Unescape(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] == '\\' && i + 1 < a_text.size() && a_text[i + 1] == 'n') {
				out += '\n';
				++i;
			} else {
				out += a_text[i];
			}
		}
		return out;
	}

	// a menu page or section name as SKSE Menu Framework reads it: an unescaped '/' there splits a path, so a slash the
	// translation brings is marked with a backslash (its header's rule); an English name with a slash of its own means a folder
	// on purpose and stays as it is
	[[nodiscard]] inline std::string MenuName(std::string_view a_english, std::string_view a_shown)
	{
		if (a_english.find('/') != std::string_view::npos) {
			return std::string(a_shown);
		}
		std::string out;
		out.reserve(a_shown.size());
		for (const char c : a_shown) {
			if (c == '/') {
				out += '\\';
			}
			out += c;
		}
		return out;
	}

	// a (translated) line's {} filled in order with the given words - never std::format on a translated line, where a stray
	// brace would throw. Usable already refused a translation whose {} count differs from its English.
	inline std::string Fill(std::string_view a_line, std::string_view a_one, std::string_view a_two = {})
	{
		std::string out;
		int         n = 0;
		for (std::size_t i = 0; i < a_line.size(); ++i) {
			if (a_line[i] == '{' && i + 1 < a_line.size() && a_line[i + 1] == '}') {
				out += n++ == 0 ? a_one : a_two;
				++i;
			} else {
				out += a_line[i];
			}
		}
		return out;
	}
}
