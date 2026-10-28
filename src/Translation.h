// Translation of the settings pages - shared by izzydoingit's SKSE plugins (the same file in each).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// SKSE Menu Framework translates only its own menu, so each plugin reads its own file:
//   Data/SKSE/Plugins/<plugin folder>/Translation.json
// a flat JSON object, every line "English text": "translated text". The mod ships it with the English on both sides (it
// changes nothing); a translator replaces the right-hand side. A missing file, a missing line or an empty translation keeps
// the English. A translation whose placeholders (%d, %s, %.0f ... or {}) do not match its English line is not used - it would
// print garbage or crash - and the log says which.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace Translation
{
	namespace Detail
	{
		inline std::unordered_map<std::string, std::string>& Map()
		{
			static std::unordered_map<std::string, std::string> map;
			return map;
		}

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
						out.emplace_back("{");
						break;
					}
					out.emplace_back(a_s.substr(i, j - i + 1));
					i = j;
				}
			}
			return out;
		}

		// the file's lines, or false with what is wrong. As the reader before it: what follows the closing brace is not read,
		// and a comma after the last line is forgiven (translators leave one; the parser itself refuses both); anything but one
		// object of text lines is refused whole.
		inline bool Read(std::string a_text, std::unordered_map<std::string, std::string>& a_out, std::string& a_error)
		{
			if (const auto close = a_text.find_last_of('}'); close != std::string::npos && close > 0) {
				a_text.resize(close + 1);
				const auto comma = a_text.find_last_not_of(" \t\r\n", close - 1);
				const auto before = comma != std::string::npos && comma > 0 ? a_text.find_last_not_of(" \t\r\n", comma - 1) : std::string::npos;
				if (before != std::string::npos && a_text[comma] == ',' && a_text[before] != '{') {
					a_text.erase(comma, 1);
				}
			}
			try {
				const auto json = nlohmann::json::parse(a_text);
				if (!json.is_object()) {
					a_error = "the file must be one { } of \"English\": \"translation\" lines";
					return false;
				}
				for (const auto& [key, value] : json.items()) {
					if (!value.is_string()) {
						a_error = "expected a quoted translation for \"" + key + "\"";
						return false;
					}
					a_out[key] = value.get<std::string>();
				}
				return true;
			} catch (const nlohmann::json::exception& e) {
				a_error = e.what();
				return false;
			}
		}
	}

	// Reads the file (call once, at data load, before the menu is registered). Returns what to log.
	inline std::string Load(const std::filesystem::path& a_file)
	{
		auto& map = Detail::Map();
		map.clear();
		std::ifstream in(a_file, std::ios::binary);
		if (!in) {
			// the name as UTF-8: path::string() throws for a name the ANSI code page cannot show
			const auto name = a_file.filename().generic_u8string();
			return "no translation file (" + std::string(name.begin(), name.end()) + "): the pages are in English";
		}
		std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		if (text.starts_with("\xEF\xBB\xBF")) {
			text.erase(0, 3);
		}
		std::unordered_map<std::string, std::string> read;
		std::string                                  error;
		if (!Detail::Read(std::move(text), read, error)) {
			return "translation file not used - " + error;
		}
		std::size_t used = 0, same = 0, bad = 0;
		std::string badLines;
		for (auto& [en, tr] : read) {
			if (tr.empty() || tr == en) {
				++same;
				continue;
			}
			// an English line with a conversion or a "%%" is printed through printf: read both sides by printf's own rules
			const auto lenient = Detail::Placeholders(en);
			const bool format = en.find("%%") != std::string::npos ||
			                    std::ranges::any_of(lenient, [](const std::string& p) { return p.starts_with('%'); });
			if (Detail::Placeholders(en, format) != Detail::Placeholders(tr, format)) {
				++bad;
				if (badLines.size() < 400) {
					badLines += "\n  placeholders differ, English kept: \"" + en + "\"";
				}
				continue;
			}
			map.emplace(en, std::move(tr));
			++used;
		}
		return "translation: " + std::to_string(used) + " line(s) translated, " + std::to_string(same) + " left in English, " +
		       std::to_string(bad) + " not used" + badLines;
	}

	// the line in the reader's language, or the English itself
	inline const char* T(const char* a_english)
	{
		const auto& map = Detail::Map();
		if (map.empty()) {
			return a_english;
		}
		const auto it = map.find(a_english);
		return it != map.end() ? it->second.c_str() : a_english;
	}

	// a (translated) line's {} filled in order with the given words - never std::format on a translated line, where a stray
	// brace would throw. Load already refused a translation whose {} count differs from its English.
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
