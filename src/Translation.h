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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

		inline void Utf8(std::string& a_out, std::uint32_t a_cp)
		{
			if (a_cp < 0x80) {
				a_out += static_cast<char>(a_cp);
			} else if (a_cp < 0x800) {
				a_out += static_cast<char>(0xC0 | (a_cp >> 6));
				a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
			} else if (a_cp < 0x10000) {
				a_out += static_cast<char>(0xE0 | (a_cp >> 12));
				a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
				a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
			} else {
				a_out += static_cast<char>(0xF0 | (a_cp >> 18));
				a_out += static_cast<char>(0x80 | ((a_cp >> 12) & 0x3F));
				a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
				a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
			}
		}

		// a minimal reader for exactly this file: one JSON object of string values. False on anything else.
		class Reader
		{
		public:
			explicit Reader(std::string_view a_text) :
				s(a_text) {}

			bool Object(std::unordered_map<std::string, std::string>& a_out, std::string& a_error)
			{
				Space();
				if (!Eat('{')) {
					return Fail(a_error, "the file must start with {");
				}
				Space();
				if (Eat('}')) {
					return true;
				}
				while (true) {
					std::string key, value;
					Space();
					if (!String(key)) {
						return Fail(a_error, "expected a quoted English line");
					}
					Space();
					if (!Eat(':')) {
						return Fail(a_error, "expected : after \"" + key + "\"");
					}
					Space();
					if (!String(value)) {
						return Fail(a_error, "expected a quoted translation for \"" + key + "\"");
					}
					a_out[key] = value;
					Space();
					if (Eat(',')) {
						Space();
						if (Eat('}')) {
							return true;  // a trailing comma is forgiven
						}
						continue;
					}
					if (Eat('}')) {
						return true;
					}
					return Fail(a_error, "expected , or } after \"" + key + "\"");
				}
			}

		private:
			bool Fail(std::string& a_error, const std::string& a_what)
			{
				std::size_t line = 1;
				for (std::size_t k = 0; k < i && k < s.size(); ++k) {
					line += s[k] == '\n' ? 1 : 0;
				}
				a_error = "line " + std::to_string(line) + ": " + a_what;
				return false;
			}
			void Space()
			{
				while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
					++i;
				}
			}
			bool Eat(char a_c)
			{
				if (i < s.size() && s[i] == a_c) {
					++i;
					return true;
				}
				return false;
			}
			bool Hex4(std::uint32_t& a_v)
			{
				if (i + 4 > s.size()) {
					return false;
				}
				a_v = 0;
				for (int k = 0; k < 4; ++k) {
					const char c = s[i++];
					a_v <<= 4;
					if (c >= '0' && c <= '9') {
						a_v |= static_cast<std::uint32_t>(c - '0');
					} else if (c >= 'a' && c <= 'f') {
						a_v |= static_cast<std::uint32_t>(c - 'a' + 10);
					} else if (c >= 'A' && c <= 'F') {
						a_v |= static_cast<std::uint32_t>(c - 'A' + 10);
					} else {
						return false;
					}
				}
				return true;
			}
			bool String(std::string& a_out)
			{
				if (!Eat('"')) {
					return false;
				}
				while (i < s.size()) {
					const char c = s[i++];
					if (c == '"') {
						return true;
					}
					if (c != '\\') {
						a_out += c;
						continue;
					}
					if (i >= s.size()) {
						return false;
					}
					switch (const char e = s[i++]) {
					case '"':
					case '\\':
					case '/':
						a_out += e;
						break;
					case 'n':
						a_out += '\n';
						break;
					case 't':
						a_out += '\t';
						break;
					case 'r':
						a_out += '\r';
						break;
					case 'b':
						a_out += '\b';
						break;
					case 'f':
						a_out += '\f';
						break;
					case 'u':
						{
							std::uint32_t cp = 0;
							if (!Hex4(cp)) {
								return false;
							}
							if (cp >= 0xDC00 && cp <= 0xDFFF) {
								return false;  // a low surrogate with no high one before it
							}
							if (cp >= 0xD800 && cp <= 0xDBFF) {
								// a high surrogate is only half a character: a low one must follow
								if (i + 6 > s.size() || s[i] != '\\' || s[i + 1] != 'u') {
									return false;
								}
								i += 2;
								std::uint32_t lo = 0;
								if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
									return false;
								}
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
							Utf8(a_out, cp);
							break;
						}
					default:
						return false;
					}
				}
				return false;
			}

			std::string_view s;
			std::size_t      i{ 0 };
		};
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
		std::stringstream ss;
		ss << in.rdbuf();
		std::string text = ss.str();
		if (text.starts_with("\xEF\xBB\xBF")) {
			text.erase(0, 3);
		}
		std::unordered_map<std::string, std::string> read;
		std::string                                  error;
		if (!Detail::Reader(text).Object(read, error)) {
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
