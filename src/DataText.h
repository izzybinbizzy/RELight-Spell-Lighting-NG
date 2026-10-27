// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The plain-text part of reading the installer's data files (Data.cpp has the line format): every line of one file into
// one File of plain values. A form ("0x1540E~Dawnguard.esm") stays text here; Data.cpp resolves it in the load order. Nothing
// of the game in here, so tests/test_fade.cpp builds and checks it off the game (PC Runner\fade tests.py).

#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace DataText
{
	// the TAB-separated fields of one line (an empty field is kept)
	[[nodiscard]] inline std::vector<std::string_view> Split(std::string_view a_line)
	{
		std::vector<std::string_view> out;
		for (std::size_t start = 0;;) {
			const auto tab = a_line.find('\t', start);
			out.push_back(a_line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
			if (tab == std::string_view::npos) {
				return out;
			}
			start = tab + 1;
		}
	}

	// a whole field as a number: "12abc" is not 12
	template <class T>
	[[nodiscard]] bool Number(std::string_view a_text, T& a_out)
	{
		T          v{};
		const auto end = a_text.data() + a_text.size();
		const auto r = std::from_chars(a_text.data(), end, v);
		if (a_text.empty() || r.ec != std::errc{} || r.ptr != end) {
			return false;
		}
		if constexpr (std::is_floating_point_v<T>) {
			if (!std::isfinite(v)) {
				return false;  // "nan" / "inf" read as whole fields: the line does not read
			}
		}
		a_out = v;
		return true;
	}

	// n numbers from a_fields[a_first..]; false when any does not read
	template <std::size_t N>
	[[nodiscard]] bool Numbers(const std::vector<std::string_view>& a_fields, std::size_t a_first, std::array<float, N>& a_out)
	{
		for (std::size_t i = 0; i < N; ++i) {
			if (a_first + i >= a_fields.size() || !Number(a_fields[a_first + i], a_out[i])) {
				return false;
			}
		}
		return true;
	}

	struct OptionText
	{
		std::string download, name, id, category, author, desc;
		std::string pickGroup, pickName;                                      // `pick <group> <name>`: an art pick's file, read only while that pick is chosen
		bool        weapons{ false }, switchable{ false }, dynamic{ false };  // `dynamic`: read only while Dynamic Lighting is on
		int         order{ 0 }, menu{ 0 };
	};

	// the art picks the menu offers (the installer's four groups until 2.6); choice 0 of each is the game's own art, no file
	inline constexpr std::array<std::string_view, 4>                kPickGroups{ "fire", "frost", "shock", "healing" };
	inline constexpr std::array<std::array<std::string_view, 4>, 4> kPickNames{ {
		{ "Vanilla", "Inferno", "Embers XD", "Kittytail Flame" },
		{ "Vanilla", "Kittytail", "", "" },
		{ "Vanilla", "Kittytail Lightning", "Voltage", "" },
		{ "Vanilla", "Kittytail healing", "", "" },
	} };

	// how many choices group a_group has, the game's own art included (0 for no such group)
	[[nodiscard]] constexpr int PickCount(int a_group)
	{
		if (a_group < 0 || a_group >= static_cast<int>(kPickNames.size())) {
			return 0;
		}
		int n = 0;
		for (const auto name : kPickNames[static_cast<std::size_t>(a_group)]) {
			n += name.empty() ? 0 : 1;
		}
		return n;
	}

	// the group's index, or -1
	[[nodiscard]] inline int PickGroup(std::string_view a_group)
	{
		for (std::size_t g = 0; g < kPickGroups.size(); ++g) {
			if (kPickGroups[g] == a_group) {
				return static_cast<int>(g);
			}
		}
		return -1;
	}

	// the choice's index in its group (1 or more for a pick with a file), or -1
	[[nodiscard]] inline int PickChoice(int a_group, std::string_view a_name)
	{
		if (a_group < 0 || a_group >= static_cast<int>(kPickNames.size()) || a_name.empty()) {
			return -1;
		}
		const auto& names = kPickNames[static_cast<std::size_t>(a_group)];
		for (std::size_t i = 1; i < names.size(); ++i) {
			if (names[i] == a_name) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	// what a 2.5 install chose in its installer, read off the data file names it put down (only the chosen pick's files)
	struct OldPicks
	{
		bool               found = false;  // an old install is there (its Core file)
		bool               dynamic = false;
		std::array<int, 4> art{};
	};

	[[nodiscard]] inline OldPicks PicksFromOldFiles(const std::vector<std::string>& a_fileNames)
	{
		const auto has = [&](std::string_view a_name) {
			for (const auto& f : a_fileNames) {
				if (f == a_name) {
					return true;
				}
			}
			return false;
		};
		OldPicks out;
		out.found = has("Spells - Core.txt");
		if (!out.found) {
			return out;
		}
		out.dynamic = has("Spells - Core - Dynamic Lighting.txt");
		for (std::size_t g = 0; g < kPickNames.size(); ++g) {
			for (std::size_t i = 1; i < kPickNames[g].size(); ++i) {
				if (!kPickNames[g][i].empty() && has(std::string("Spells - ").append(kPickNames[g][i]).append(".txt"))) {
					out.art[g] = static_cast<int>(i);
				}
			}
		}
		return out;
	}

	// a light on a mesh key: a `stream` (node + r g b fade radius size cutoff x y z), a `held` or a `body` (no node)
	struct LightText
	{
		std::string           key, node;
		std::array<float, 10> v{};
	};

	struct HandText
	{
		std::string          key;
		std::array<float, 9> v{};  // r g b fade radius size cutoff inverseSquare portalStrict
	};

	struct FxText
	{
		std::string key;
		bool        flicker{ false };
		float       perSecond{ 0.0f }, intensity{ 0.0f };
	};

	struct TintText
	{
		std::string          key;  // a mesh key, or a form "0xID~Plugin"
		std::array<float, 3> rgb{};
	};

	// everything one file holds
	struct File
	{
		OptionText               option;
		std::vector<std::string> meshes, bases, sprays, beams;  // bases / sprays / beams: forms as written
		// a spray's or beam's light record that carries his color (2.3): the form as written, and its color 0-255
		std::vector<std::pair<std::string, std::array<int, 3>>> recordColors;
		std::vector<LightText>                                  streams, helds, bodies, clouds;
		std::vector<HandText>                                   hands;
		std::vector<FxText>                                     fx;
		std::vector<TintText>                                   tints;
		bool                                                    sawVersion{ false }, sawFile{ false }, sawMenu{ false }, sawDownload{ false };
	};

	// one line into `a_file`; false when it does not read
	[[nodiscard]] inline bool ParseLine(const std::vector<std::string_view>& p, File& a_file)
	{
		if (p.empty()) {
			return false;
		}
		const auto kind = p[0];
		auto&      opt = a_file.option;
		if (kind == "version" && p.size() >= 2) {
			int v = 0;
			a_file.sawVersion = Number(p[1], v) && v == 1;
			return a_file.sawVersion;
		}
		if (kind == "file" && p.size() >= 3) {
			opt.download = std::string(p[1]);
			opt.name = std::string(p[2]);
			opt.id = opt.download + " - " + opt.name;
			a_file.sawFile = true;
			return true;
		}
		if (kind == "order" && p.size() >= 2) {
			if (!Number(p[1], opt.order)) {
				return false;
			}
			if (!a_file.sawMenu) {
				opt.menu = opt.order;  // a file written before `menu` existed sorts by its order
			}
			return true;
		}
		if (kind == "menu" && p.size() >= 2) {
			a_file.sawMenu = true;
			return Number(p[1], opt.menu);  // menu position, separate from `order` (which decides mesh ownership)
		}
		if (kind == "switch" && p.size() >= 2) {
			opt.switchable = p[1] == "1";
			return true;
		}
		if (kind == "menugroup" && p.size() == 3) {
			opt.category = std::string(p[1]);
			opt.author = p[2] == "-" ? std::string() : std::string(p[2]);
			return true;
		}
		if (kind == "pick" && p.size() == 3 && PickChoice(PickGroup(p[1]), p[2]) > 0) {
			opt.pickGroup = std::string(p[1]);
			opt.pickName = std::string(p[2]);
			return true;
		}
		if (kind == "dynamic" && p.size() == 1) {
			opt.dynamic = true;
			return true;
		}
		if (kind == "desc" && p.size() == 2 && !p[1].empty()) {
			opt.desc = std::string(p[1]);
			return true;
		}
		if (kind == "download" && p.size() == 2 && (p[1] == "Spells" || p[1] == "Weapons")) {
			opt.weapons = p[1] == "Weapons";
			a_file.sawDownload = true;
			return true;
		}
		if (kind == "mesh" && p.size() >= 2) {
			a_file.meshes.emplace_back(p[1]);
			return true;
		}
		if (kind == "base" && p.size() >= 2) {
			a_file.bases.emplace_back(p[1]);
			return true;
		}
		// spraylight / beamlight <0xID~Plugin> [<r> <g> <b>] - the color only where his eye set one (2.3)
		if ((kind == "spraylight" || kind == "beamlight") && (p.size() == 2 || p.size() == 5)) {
			if (p.size() == 5) {
				std::array<int, 3> c{};
				for (std::size_t i = 0; i < 3; ++i) {
					if (!Number(p[2 + i], c[i]) || c[i] < 0 || c[i] > 255) {
						return false;
					}
				}
				a_file.recordColors.emplace_back(std::string(p[1]), c);
			}
			(kind == "spraylight" ? a_file.sprays : a_file.beams).emplace_back(p[1]);
			return true;
		}
		if (kind == "stream" && p.size() == 13) {
			LightText s{ std::string(p[1]), std::string(p[2]), {} };
			if (!Numbers(p, 3, s.v)) {
				return false;
			}
			a_file.streams.push_back(std::move(s));
			return true;
		}
		if (kind == "held" && p.size() == 12) {
			LightText s{ std::string(p[1]), "-", {} };
			if (!Numbers(p, 2, s.v)) {
				return false;
			}
			a_file.helds.push_back(std::move(s));
			return true;
		}
		// body <key> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>: the light for that art model on an actor (BodyLights.cpp)
		if (kind == "body" && p.size() == 12) {
			LightText s{ std::string(p[1]), "-", {} };
			if (!Numbers(p, 2, s.v)) {
				return false;
			}
			a_file.bodies.push_back(std::move(s));
			return true;
		}
		// cloud <key> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>: a cone whose one light stays where its cloud is (Rows.cpp)
		if (kind == "cloud" && p.size() == 12) {
			LightText s{ std::string(p[1]), "-", {} };
			if (!Numbers(p, 2, s.v)) {
				return false;
			}
			a_file.clouds.push_back(std::move(s));
			return true;
		}
		if (kind == "hand" && p.size() == 11) {
			HandText h{ std::string(p[1]), {} };
			if (!Numbers(p, 2, h.v)) {
				return false;
			}
			a_file.hands.push_back(std::move(h));
			return true;
		}
		if (kind == "handfx" && p.size() == 5 && (p[2] == "pulse" || p[2] == "flicker")) {
			FxText f{ std::string(p[1]), p[2] == "flicker", 0.0f, 0.0f };
			if (!Number(p[3], f.perSecond) || !Number(p[4], f.intensity)) {
				return false;
			}
			a_file.fx.push_back(std::move(f));
			return true;
		}
		if (kind == "tint" && p.size() == 5) {
			TintText t{ std::string(p[1]), {} };
			if (!Numbers(p, 2, t.rgb)) {
				return false;
			}
			a_file.tints.push_back(std::move(t));
			return true;
		}
		return false;
	}

	// What reading one file's text came to: the File when it read, else the first line that did not (0: no `version 1` or no
	// `file` line). A line may end in CR; empty lines and lines starting with '#' are skipped.
	struct Result
	{
		File        file;
		bool        ok{ false };
		std::size_t badLine{ 0 };
	};

	[[nodiscard]] inline Result ReadText(std::string_view a_text)
	{
		Result      r;
		std::size_t n = 0;
		for (std::size_t start = 0; start <= a_text.size();) {
			const auto nl = a_text.find('\n', start);
			auto       line = a_text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
			start = nl == std::string_view::npos ? a_text.size() + 1 : nl + 1;
			++n;
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (!ParseLine(Split(line), r.file)) {
				r.badLine = n;
				return r;
			}
		}
		r.ok = r.file.sawVersion && r.file.sawFile;
		return r;
	}
}
