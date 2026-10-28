// Illuminated / RELight - Spell Lighting NG - the Auto art picks (Illuminated and RELight carry identical copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Which spell art pack the files the game really loads belong to. Each pick (fire, frost, shock, healing) has a few voter
// files (its hand art); each file the game serves is known by its size and the 64-bit FNV-1a hash of its bytes
// (ArtSignatures.h, written by the generators from Tools\art-pack-signatures.txt). Every voter the game's own, or every
// pack voter naming the same pick: that pick. An unknown file (another pack, an updated one) or two picks (a mix): no
// answer - the menu's pick is used. Game-free: the caller reads the files (each mod's ArtPacks.cpp), so the tests run it.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ArtAuto
{
	inline constexpr int kGame = -1;  // a voter file in the game's own version: no pack

	// the four art picks, in RELight's pick-group order (DataText kPickGroups); a pick's choices are the same in both mods
	inline constexpr std::array<std::string_view, 4> kElements{ "fire", "frost", "shock", "healing" };

	struct Signature
	{
		std::string_view element;  // fire, frost, shock, healing
		int              pick{ kGame };
		std::string_view file;  // as the game opens it: "meshes\..." or "textures\..."
		std::uint64_t    size{ 0 };
		std::uint64_t    hash{ 0 };
	};

	// FNV-1a 64 over a file read piece by piece, and its length
	struct Fingerprint
	{
		std::uint64_t hash{ 0xCBF29CE484222325ULL };
		std::uint64_t size{ 0 };

		void Add(std::span<const std::uint8_t> a_bytes) noexcept
		{
			for (const auto b : a_bytes) {
				hash = (hash ^ b) * 0x100000001B3ULL;
			}
			size += a_bytes.size();
		}
	};

	struct Found
	{
		std::optional<int> pick;  // empty: not recognised - the menu's pick holds
		std::string        why;   // for the log
	};

	// a_read(file) -> std::optional<Fingerprint> of the file the game loads (empty when the game has none by that name)
	template <class Read>
	[[nodiscard]] Found Decide(std::string_view a_element, std::span<const Signature> a_table, Read&& a_read)
	{
		std::vector<std::string_view> files;
		for (const auto& s : a_table) {
			if (s.element == a_element && std::ranges::find(files, s.file) == files.end()) {
				files.push_back(s.file);
			}
		}
		if (files.empty()) {
			return { std::nullopt, "no signature names this art" };
		}
		std::optional<int> pick;
		for (const auto file : files) {
			const std::optional<Fingerprint> got = a_read(file);
			if (!got) {
				return { std::nullopt, std::format("the game has no {}", file) };
			}
			const auto it = std::ranges::find_if(a_table, [&](const Signature& s) {
				return s.element == a_element && s.file == file && s.size == got->size && s.hash == got->hash;
			});
			if (it == a_table.end()) {
				return { std::nullopt, std::format("{} is a version no signature knows (another or an updated pack)", file) };
			}
			if (it->pick == kGame) {
				continue;
			}
			if (pick && *pick != it->pick) {
				return { std::nullopt, "a mix of packs" };
			}
			pick = it->pick;
		}
		return { pick.value_or(0), pick ? std::format("pick {}", *pick) : std::string("the game's own art") };
	}
}
