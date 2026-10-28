// Illuminated / RELight - Spell Lighting NG - the Auto art picks, read in game (Illuminated and RELight carry identical copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Which art pack each pick's hand art is, off the files the game really loads: opened through the game's own resource system
// (loose or in an archive - the winning one), each fingerprinted and told apart by ArtSignatures.h (ArtAuto.h decides).
// Once, at data load, on the main thread; each mod's settings keep what it found and never save it (the pick stays the backup).

#include "ArtSignatures.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		[[nodiscard]] std::optional<ArtAuto::Fingerprint> Winning(std::string_view a_file)
		{
			RE::BSResourceNiBinaryStream stream(std::string{ a_file });
			const auto*                  source = stream.good() ? stream.stream.get() : nullptr;
			if (!source) {
				return std::nullopt;
			}
			ArtAuto::Fingerprint      print;
			std::vector<std::uint8_t> chunk(65536);
			for (std::uint64_t got = 0; source->DoRead(chunk.data(), chunk.size(), got) == RE::BSResource::ErrorCode::kNone && got > 0;) {
				print.Add(std::span<const std::uint8_t>(chunk.data(), static_cast<std::size_t>(got)));
			}
			return print;
		}
	}

	std::array<ArtAuto::Found, 4> FindArtPacks()
	{
		std::array<ArtAuto::Found, 4> found;
		for (std::size_t e = 0; e < ArtAuto::kElements.size(); ++e) {
			found[e] = ArtAuto::Decide(ArtAuto::kElements[e], ArtAuto::kSignatures, Winning);
		}
		return found;
	}
}
