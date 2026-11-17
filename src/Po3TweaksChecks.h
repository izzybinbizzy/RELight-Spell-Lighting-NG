// powerofthree's Tweaks - the off-game checks of Po3TweaksText.h (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file.
//
// Run by each carrying plugin's own tests; Run adds kCount to the test's floor. Both paths: with Tweaks keeping editor IDs, and the
// missing requirement named.
#pragma once

#include "Po3TweaksText.h"

#include <sstream>
#include <string>

namespace Po3TweaksChecks
{
	using Check = void (*)(bool a_ok, const char* a_what, int a_line);

	// the checks Run makes (raise it when checks are added)
	inline constexpr int kCount = 24;

#define PO3_CHECK(x) a_check((x), #x, __LINE__)

	inline void Run(Check a_check)
	{
		using namespace Po3TweaksText;
		// a form id in its plugin, with or without 0x, spaces round it; a light plugin's short id
		const auto a = ParseKey(" 0x12EB5 ~ Skyrim.esm ");
		PO3_CHECK(a.kind == Key::kFormID && a.local == 0x12EB5 && a.plugin == "Skyrim.esm");
		const auto b = ParseKey("801~ccBGSSSE001-Fish.esm");
		PO3_CHECK(b.kind == Key::kFormID && b.local == 0x801 && b.plugin == "ccBGSSSE001-Fish.esm");
		// an editor ID: one word with no ~
		const auto c = ParseKey("  MAGFireboltProjectile ");
		PO3_CHECK(c.kind == Key::kEditorID && c.editorID == "MAGFireboltProjectile");
		// not a form: empty, no plugin, a bad number, a space or a | inside, a quote
		PO3_CHECK(ParseKey("").kind == Key::kBad && ParseKey("   ").kind == Key::kBad);
		PO3_CHECK(ParseKey("0x12EB5~").kind == Key::kBad && ParseKey("~Skyrim.esm").kind == Key::kBad);
		PO3_CHECK(ParseKey("0x1G~Skyrim.esm").kind == Key::kBad && ParseKey("0x~Skyrim.esm").kind == Key::kBad);
		PO3_CHECK(ParseKey("Fire Bolt").kind == Key::kBad && ParseKey("Skyrim.esm|12EB5").kind == Key::kBad && ParseKey("\"Fire\"").kind == Key::kBad);
		// its ini: on when the file or the key is missing; off by its reader's words; the current key over the old one; a note is no value
		const auto ini = [](const char* a_text) {
			std::istringstream in(a_text);
			return LoadsEditorIDs(in);
		};
		PO3_CHECK(ini("") && ini("[Fixes]\nbDistantRefLoadCrash = true\n"));
		PO3_CHECK(!ini("[Fixes]\nbLoadEditorIDs = false\n") && !ini("[Fixes]\nbLoadEditorIDs=0\n") && !ini("[Fixes]\nbLoadEditorIDs = Off\n"));
		PO3_CHECK(ini("[Fixes]\nbLoadEditorIDs = true\n") && ini("[Fixes]\nbLoadEditorIDs = 1 ; off\n"));
		PO3_CHECK(!ini("[Fixes]\nLoad EditorIDs = false\n") && ini("[Fixes]\nLoad EditorIDs = false\nbLoadEditorIDs = true\n"));
		PO3_CHECK(!ini("[Fixes]\nbLoadEditorIDs = true\n;bLoadEditorIDs = false\nbLoadEditorIDs = no\n"));
		// the proof: the player's base form named "Player", any case; nothing else
		PO3_CHECK(ProofHolds("Player") && ProofHolds("PLAYER") && !ProofHolds("") && !ProofHolds(nullptr) && !ProofHolds("PlayerRef"));
		// the verdict, each step that can stop it
		PO3_CHECK(Judge(false, false, false, nullptr) == State::kAbsent && Judge(true, false, true, "Player") == State::kNoExport);
		PO3_CHECK(Judge(true, true, false, "Player") == State::kOff && Judge(true, true, true, "") == State::kNoProof);
		PO3_CHECK(Judge(true, true, true, "Player") == State::kKeepsThem);
		// the game naming its own forms (Native EditorID Fix and the like) keeps names without Tweaks; Tweaks proven still comes first
		PO3_CHECK(Judge(false, false, false, nullptr, "Player") == State::kNativeNames && Judge(true, true, false, nullptr, "player") == State::kNativeNames &&
				  Judge(true, true, true, "Player", "Player") == State::kKeepsThem && Judge(false, false, false, nullptr, "") == State::kAbsent);
		PO3_CHECK(NamesKept(State::kKeepsThem) && NamesKept(State::kNativeNames) && !NamesKept(State::kOff) && !NamesKept(State::kAbsent) &&
				  Describe(State::kNativeNames) != Describe(State::kKeepsThem));
		// each verdict has its own log line, naming the mod
		PO3_CHECK(Describe(State::kKeepsThem).find("powerofthree's Tweaks") != std::string_view::npos &&
				  Describe(State::kKeepsThem) != Describe(State::kAbsent) && Describe(State::kOff) != Describe(State::kNoExport));
		// a form named by editor ID and not found: the hint only while Tweaks is not used; never for a form id
		PO3_CHECK(!MissingHint(c, false).empty() && MissingHint(c, true).empty());
		PO3_CHECK(MissingHint(a, false).empty() && MissingHint(ParseKey(""), false).empty());
		PO3_CHECK(kProofForm == 0x7);
		// the box at data load: the mod, what is missing, where to get it
		PO3_CHECK(BoxText("A Mod", State::kAbsent) == "A Mod needs powerofthree's Tweaks. Install powerofthree's Tweaks from Nexus (mod 51073).");
		PO3_CHECK(BoxText("A Mod", State::kOff).find("Tweaks with its Load EditorIDs setting on.") != std::string::npos &&
				  BoxText("A Mod", State::kNoExport).find("Tweaks in a newer version.") != std::string::npos);
	}

#undef PO3_CHECK
}
