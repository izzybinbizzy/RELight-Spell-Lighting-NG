// Illuminated / RELight - Spell Lighting NG - the Auto art picks' signatures (Illuminated and RELight carry identical copies)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// The rows are Tools\art-pack-signatures.txt, measured off the packs' own files and the game's archives: lagen.py latables
// checks them and --write puts them back. Never edit a row by hand.

#pragma once

#include "ArtAuto.h"

namespace ArtAuto
{
	inline constexpr Signature kSignatures[] = {
		{ "shock", -1, "meshes\\magic\\shockhandeffects.nif", 15566, 0x68334922AFFC5AE6ULL },
		{ "shock", 1, "meshes\\magic\\shockhandeffects.nif", 23362, 0xD004248766C9B0FDULL },
		{ "shock", -1, "textures\\effects\\gradients\\gradviobright.dds", 21984, 0x3F698E9CCA04E56AULL },
		{ "shock", 2, "textures\\effects\\gradients\\gradviobright.dds", 22032, 0x0FCC7F5C078318B7ULL },
		{ "fire", -1, "meshes\\magic\\fireball01handeffects.nif", 19973, 0x37FB5DA551C3A7C8ULL },
		{ "fire", 1, "meshes\\magic\\fireball01handeffects.nif", 19953, 0x427D3B76EBDE1580ULL },
		{ "fire", 2, "meshes\\magic\\fireball01handeffects.nif", 20342, 0x336C6617F7FCE842ULL },
		{ "fire", 3, "meshes\\magic\\fireball01handeffects.nif", 25110, 0x359921526B4D6D79ULL },
		{ "frost", -1, "meshes\\magic\\frostbolthandfx01.nif", 13849, 0xFE6F57348621CB87ULL },
		{ "frost", 0, "meshes\\magic\\frostbolthandfx01.nif", 13850, 0x0D54B24B02F0DCCBULL },
		{ "frost", 1, "meshes\\magic\\frostbolthandfx01.nif", 13744, 0xC6E6E483F6F5E17EULL },
		{ "healing", -1, "meshes\\magic\\healfxhand01.nif", 36237, 0xDAF9D12CD370A72CULL },
		{ "healing", 1, "meshes\\magic\\healfxhand01.nif", 56412, 0x2184B187B8B56AABULL },
	};
}
