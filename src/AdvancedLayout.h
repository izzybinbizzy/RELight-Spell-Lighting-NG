// Our plugins' Advanced Settings file - its LAYOUT, one for every mod (master: Tools\Shared Plugin Code; shared code sync.py keeps the copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Game-free. What the file looks like, so every mod's reads alike: a boxed header (what the file is, that it is read when the game
// starts, how to change, reset and keep a value), a contents list, a "most people only change these" block, then one boxed banner per
// section in the order a player thinks (kOrder), each value with a one-line plain note above it and its default and range beside it,
// the = signs lined up. Plain ASCII. Tools\advancedfile.py writes the same text for the files the generators ship; tests\test_fade.cpp
// and advancedfile.py's selftest hold both to one expected text, written out in each.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace AdvancedLayout
{
	struct Row
	{
		std::string section, key, note;
		float       def{ 0.0f }, lo{ 0.0f }, hi{ 0.0f }, value{ 0.0f };
		bool        whole{ false };   // a count: shown with no point
		bool        common{ false };  // listed under "most people only change these"
	};

	// the sections in the order a player thinks; a section not named here follows them, A to Z. Each name's note sits under its banner
	struct Section
	{
		std::string_view name, note;
	};
	inline constexpr std::array<Section, 15> kOrder{ {
		{ "General", "Who gets lights and how far from you." },
		{ "Brightness and Reach", "How bright the lights are and how far they reach, beyond the menu's sliders." },
		{ "Color", "How strong and true the light colors are." },
		{ "Flicker", "How lights flicker and pulse." },
		{ "Fading with Magicka", "How hand and weapon lights dim as magicka or charge runs out." },
		{ "Streams and Rows", "The rows of small lights along streams, sprays and bolts." },
		{ "Explosions", "The lights of explosions and impacts." },
		{ "Ground Effects", "The lights of spells left on the ground." },
		{ "Enchanted Weapons", "The lights of enchanted weapons." },
		{ "Fade on death", "How a dying creature's light fades out." },
		{ "Glow when casting", "How a creature's light brightens while it casts or breathes." },
		{ "Dark until noticed", "How a creature's light waits dark, then fades up when it notices you." },
		{ "Vanilla and ENB", "Values used only without Community Shaders." },
		{ "Performance", "Limits that keep big fights smooth." },
		{ "HUD", "Where the charge gems sit on screen." },
	} };

	inline constexpr std::size_t kWidth = 78;  // a line of the boxes, its "; " included

	// a number as the file shows it: 6 significant digits, no trailing zeros (2800, 0.35, 1.1); a count with no point
	[[nodiscard]] inline std::string Number(float a_v, bool a_whole)
	{
		std::ostringstream s;
		if (a_whole) {
			s << std::lround(a_v);
		} else {
			s << a_v;
		}
		return s.str();
	}

	[[nodiscard]] inline std::string Upper(std::string_view a_s)
	{
		std::string out(a_s);
		std::ranges::transform(out, out.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
		return out;
	}

	[[nodiscard]] inline std::size_t Rank(std::string_view a_section)
	{
		for (std::size_t i = 0; i < kOrder.size(); ++i) {
			if (kOrder[i].name == a_section) {
				return i;
			}
		}
		return kOrder.size();
	}

	[[nodiscard]] inline std::string_view NoteOf(std::string_view a_section)
	{
		const auto r = Rank(a_section);
		return r < kOrder.size() ? kOrder[r].note : std::string_view{};
	}

	[[nodiscard]] inline std::string Pad(std::string_view a_s, std::size_t a_width)
	{
		std::string out(a_s);
		if (out.size() < a_width) {
			out.append(a_width - out.size(), ' ');
		}
		return out;
	}

	// the sections present, in kOrder, then the rest A to Z
	[[nodiscard]] inline std::vector<std::string> SectionsOf(const std::vector<Row>& a_rows)
	{
		std::vector<std::string> out;
		for (const auto& r : a_rows) {
			if (std::ranges::find(out, r.section) == out.end()) {
				out.push_back(r.section);
			}
		}
		std::ranges::stable_sort(out, [](const std::string& a, const std::string& b) {
			const auto ra = Rank(a), rb = Rank(b);
			return ra != rb ? ra < rb : (ra == kOrder.size() && a < b);
		});
		return out;
	}

	// the whole file. a_version is the file version a reader checks (it sits on the title line)
	[[nodiscard]] inline std::string Render(std::string_view a_mod, std::string_view a_customName, int a_version, const std::vector<Row>& a_rows)
	{
		const std::string  bar = "; " + std::string(kWidth - 2, '=');
		const std::string  rule = "; " + std::string(kWidth - 2, '-');
		std::ostringstream t;
		t << bar << "\n"
		  << ";  " << a_mod << " - Advanced Settings (file version " << a_version << ")\n"
		  << bar << "\n"
		  << ";  Fine values the in-game menu does not show. The game reads this file once,\n"
		  << ";  when it starts.\n"
		  << ";\n"
		  << ";  TO CHANGE A VALUE: change the number after the = and save, then start the\n"
		  << ";  game. The note above each value says what it does; its default and allowed\n"
		  << ";  range are written beside it. A value outside its range is held at the\n"
		  << ";  nearest end.\n"
		  << ";\n"
		  << ";  TO RESET A VALUE: delete its line, or the whole file, and the default\n"
		  << ";  comes back - unless your Custom file (below) sets it: take the line out\n"
		  << ";  there too.\n"
		  << ";\n"
		  << ";  TO KEEP YOUR CHANGES THROUGH UPDATES: an update replaces this file. Put\n"
		  << ";  only the lines you change, under their [Section], in a file of their own\n"
		  << ";  beside this one:\n"
		  << ";    " << a_customName << "\n"
		  << ";  Its lines win over this file, and no update touches it.\n"
		  << ";\n"
		  << ";  The menu's own settings (its sliders and switches) stay in the menu.\n"
		  << bar << "\n"
		  << ";\n"
		  << ";  CONTENTS\n";
		const auto sections = SectionsOf(a_rows);
		for (std::size_t i = 0; i < sections.size(); ++i) {
			t << ";    " << (i + 1) << ". " << sections[i] << "\n";
		}
		std::vector<const Row*> common;
		std::size_t             commonWidth = 0;
		for (const auto& s : sections) {
			for (const auto& r : a_rows) {
				if (r.section == s && r.common) {
					common.push_back(&r);
					commonWidth = (std::max)(commonWidth, r.key.size());
				}
			}
		}
		if (!common.empty()) {
			t << ";\n"
			  << ";  MOST PEOPLE ONLY CHANGE THESE\n";
			for (const auto* r : common) {
				t << ";    " << Pad(r->key, commonWidth) << "  in [" << r->section << "]\n";
			}
		}
		t << ";\n"
		  << bar << "\n";
		for (std::size_t i = 0; i < sections.size(); ++i) {
			const auto& s = sections[i];
			std::size_t keyWidth = 0, valueWidth = 0;
			for (const auto& r : a_rows) {
				if (r.section == s) {
					keyWidth = (std::max)(keyWidth, r.key.size());
					valueWidth = (std::max)(valueWidth, Number(r.value, r.whole).size());
				}
			}
			t << "\n"
			  << rule << "\n"
			  << ";  " << (i + 1) << ". " << Upper(s) << "\n";
			if (const auto note = NoteOf(s); !note.empty()) {
				t << ";  " << note << "\n";
			}
			t << rule << "\n"
			  << "[" << s << "]\n";
			for (const auto& r : a_rows) {
				if (r.section != s) {
					continue;
				}
				t << "\n"
				  << "; " << r.note << "\n"
				  << Pad(r.key, keyWidth) << " = " << Pad(Number(r.value, r.whole), valueWidth) << "  ; default " << Number(r.def, r.whole)
				  << " - range " << Number(r.lo, r.whole) << " to " << Number(r.hi, r.whole) << "\n";
			}
		}
		return t.str();
	}
}
