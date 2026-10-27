// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Rule files: Mod::kRulesDir\*.json (FadeConfig.h), read in file-name order (case ignored), rules in the order written.
// Every rule that matches a weapon applies, and a later rule's setting replaces an earlier one's - so a mod author's
// file can be overridden by a player's "zz_mine.json". Every key, with its range: Fading\Examples.json.txt.
//
//   { "rules": [ { "name": "Dawnbreaker always glows", "weapons": ["Skyrim.esm|0x04E4EE"], "mode": "exempt" } ] }
//
// Match fields (a rule with none matches everything):
//   weapons, enchantments   "Plugin.esp|0x123" (or SPID's "0x123~Plugin.esp") or an editor ID
//   weaponKeywords          keyword editor IDs on the weapon
//   effectKeywords          keyword editor IDs on any magic effect of the enchantment (MagicDamageFire, ...)
//   bound, staff            true or false
// Within one field any entry matches; every field given must match.
//
// Before any file: a weapon with the keyword Illuminated_NoFade (a keyword distributor can hand it out; the one name in
// both mods, so a patch written for one serves the other) is left alone, a bound weapon follows its spell's time if Bound
// weapons is on, and a staff is left alone if Staves is off.

#include "Fade.h"

#include "FadeRulesText.h"

namespace Fade
{
	namespace
	{
		constexpr const char*      kDir = Mod::kRulesDir;
		constexpr std::string_view kNoFadeKeyword = "illuminated_nofade";

		using RulesText::FormRef;
		using RulesText::Rule;

		// gRules is read and rewritten on the main thread only (the per-frame pass, and a reload queued as a task).
		// The menu, on the render thread, sees only the counts and the problems, under gInfoLock.
		std::vector<Rule>          gRules;
		std::mutex                 gInfoLock;
		std::size_t                gRuleCount = 0, gFiles = 0;
		std::vector<std::string>   gProblems;
		std::atomic<std::uint32_t> gGeneration{ 1 };

		// which rules match a (weapon, enchantment) pair never changes while a game is loaded: remembered. Keyed by the
		// forms AND their IDs, and forgotten when a game loads: an enchantment made at the table is freed with the game it
		// belongs to, and another can be made at the same address.
		struct PairKey
		{
			const void* weapon{ nullptr };
			const void* ench{ nullptr };
			RE::FormID  weaponID{ 0 }, enchID{ 0 };
			bool        operator==(const PairKey&) const = default;
		};
		struct PairHash
		{
			std::size_t operator()(const PairKey& a_k) const noexcept
			{
				return std::hash<const void*>{}(a_k.weapon) * 31u ^ std::hash<const void*>{}(a_k.ench) ^
				       (std::size_t{ a_k.enchID } * 0x9E3779B97F4A7C15ull);
			}
		};
		struct Match
		{
			bool                     noFade{ false };  // the weapon carries Illuminated_NoFade
			std::vector<std::size_t> rules;            // the rules that match, in order
		};
		std::unordered_map<PairKey, Match, PairHash> gMatches;
		std::mutex                                   gMatchLock;

		// "Plugin.esp|0x123" in this load order
		std::optional<std::uint32_t> ResolveForm(std::string_view a_plugin, std::uint32_t a_id)
		{
			auto*       dh = RE::TESDataHandler::GetSingleton();
			const auto* file = dh ? dh->LookupModByName(a_plugin) : nullptr;
			// LookupForm adds the local ID to the file's index, so it must be the plugin's own part only
			const auto* form = file ? dh->LookupForm(FormText::LocalID(a_id, file->IsLight()), a_plugin) : nullptr;
			return form ? std::optional<std::uint32_t>(form->GetFormID()) : std::nullopt;
		}

		bool FormMatches(const std::vector<FormRef>& a_refs, const RE::TESForm* a_form)
		{
			if (a_refs.empty()) {
				return true;
			}
			if (!a_form) {
				return false;
			}
			std::string id;  // lower-cased once, only if an entry needs it
			for (const auto& ref : a_refs) {
				if (ref.id) {
					if (ref.id == a_form->GetFormID()) {
						return true;
					}
				} else {
					if (id.empty()) {
						id = Lower(EditorID(a_form));
					}
					if (!id.empty() && id == ref.editorID) {
						return true;
					}
				}
			}
			return false;
		}

		bool HasKeyword(const RE::BGSKeywordForm* a_form, const std::vector<std::string>& a_wanted)
		{
			if (!a_form) {
				return false;
			}
			for (const auto* kw : a_form->GetKeywords()) {
				const char* raw = kw ? kw->GetFormEditorID() : nullptr;
				if (raw && *raw) {
					const auto id = Lower(raw);
					if (std::ranges::find(a_wanted, id) != a_wanted.end()) {
						return true;
					}
				}
			}
			return false;
		}

		bool EffectsHaveKeyword(const RE::EnchantmentItem* a_ench, const std::vector<std::string>& a_wanted)
		{
			if (!a_ench) {
				return false;
			}
			for (const auto* eff : a_ench->effects) {
				if (eff && eff->baseEffect && HasKeyword(eff->baseEffect, a_wanted)) {
					return true;
				}
			}
			return false;
		}

		bool Matches(const Rule& a_r, const RE::TESObjectWEAP* a_weapon, const RE::EnchantmentItem* a_ench)
		{
			if (!FormMatches(a_r.weapons, a_weapon) || !FormMatches(a_r.enchantments, a_ench)) {
				return false;
			}
			if (!a_r.weaponKeywords.empty() && !HasKeyword(a_weapon, a_r.weaponKeywords)) {
				return false;
			}
			if (!a_r.effectKeywords.empty() && !EffectsHaveKeyword(a_ench, a_r.effectKeywords)) {
				return false;
			}
			if (a_r.bound && (!a_weapon || a_weapon->IsBound() != *a_r.bound)) {
				return false;
			}
			if (a_r.staff && (!a_weapon || a_weapon->IsStaff() != *a_r.staff)) {
				return false;
			}
			return true;
		}

		// what never changes for a (weapon, enchantment) pair after load, worked out once: keyword names are lower-cased
		// only here, not every frame. A copy is handed out (a handful of indices): nothing points into the table once its
		// lock is let go, whatever thread asks.
		Match MatchesFor(const RE::TESObjectWEAP* a_weapon, const RE::EnchantmentItem* a_ench, bool a_log)
		{
			std::lock_guard lock(gMatchLock);
			const PairKey   key{ a_weapon, a_ench, a_weapon ? a_weapon->GetFormID() : 0, a_ench ? a_ench->GetFormID() : 0 };
			if (const auto it = gMatches.find(key); it != gMatches.end()) {
				return it->second;
			}
			static const std::vector<std::string> noFade{ std::string(kNoFadeKeyword) };
			Match                                 m;
			m.noFade = HasKeyword(a_weapon, noFade);
			for (std::size_t i = 0; i < gRules.size(); ++i) {
				if (Matches(gRules[i], a_weapon, a_ench)) {
					m.rules.push_back(i);
				}
			}
			if ((m.noFade || !m.rules.empty()) && a_log) {
				SKSE::log::info("rules: {} with {} - {} rule(s){}", Label(a_weapon), Label(a_ench), m.rules.size(),
					m.noFade ? ", keyword Illuminated_NoFade" : "");
			}
			return gMatches.emplace(key, std::move(m)).first->second;
		}
	}

	void LoadRules()
	{
		{
			std::lock_guard lock(gMatchLock);
			gMatches.clear();
		}
		RulesText::Loaded loaded;
		std::size_t       filesRead = 0;
		std::error_code   ec;
		if (!std::filesystem::is_directory(kDir, ec)) {
			SKSE::log::info("rules: no {} folder; the settings apply to every weapon", kDir);
		} else {
			std::vector<std::filesystem::path> files;
			// increment(ec), not a range-for: the range-for's ++ throws on an error reading the folder
			for (auto it = std::filesystem::directory_iterator(kDir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
				std::error_code fileEc;
				if (it->is_regular_file(fileEc) && Lower(PathText(it->path().extension())) == ".json") {
					files.push_back(it->path());
				}
			}
			std::ranges::sort(files, {}, [](const auto& a_p) { return Lower(PathText(a_p.filename())); });
			for (const auto& path : files) {
				const auto    file = PathText(path.filename());
				std::ifstream in(path, std::ios::binary);
				if (!in) {
					loaded.problems.push_back(file + ": cannot be read");
					continue;
				}
				const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
				const auto        before = loaded.rules.size();
				if (RulesText::ReadFile(text, file, ResolveForm, loaded)) {
					++filesRead;
					SKSE::log::info("rules: {} - {} rule(s) in effect", file, loaded.rules.size() - before);
				}
			}
		}
		for (const auto& n : loaded.notes) {
			SKSE::log::info("rules: {}", n);
		}
		for (const auto& p : loaded.problems) {
			SKSE::log::warn("rules: {}", p);
		}
		gRules = std::move(loaded.rules);
		++gGeneration;
		{
			std::lock_guard lock(gInfoLock);
			gProblems = std::move(loaded.problems);
			gRuleCount = gRules.size();
			gFiles = filesRead;
			SKSE::log::info("rules: {} rule(s) from {} file(s), {} problem(s)", gRuleCount, gFiles, gProblems.size());
		}
		RulesLoaded();  // after the lock: it reads the counts through RuleCount() / RuleProblems(), which take it again
	}

	Verdict Judge(const RE::TESObjectWEAP* a_weapon, const RE::EnchantmentItem* a_ench, const Settings& a_settings)
	{
		Verdict v{ .mode = Mode::kCharge, .tuning = a_settings.tuning, .boundFadeSeconds = a_settings.boundFadeSeconds, .why = "settings" };
		if (a_weapon) {
			if (a_weapon->IsBound()) {
				v.mode = a_settings.bound ? Mode::kBound : Mode::kExempt;
				v.why = a_settings.bound ? "bound weapon" : "bound weapons off";
			} else if (!a_settings.weapons) {
				v.mode = Mode::kExempt;
				v.why = "weapons off";
			} else if (a_weapon->IsStaff() && !a_settings.staves) {
				v.mode = Mode::kExempt;
				v.why = "staves off";
			} else if (const auto off = KindOff(a_settings, KindOf(a_ench)); !off.empty()) {
				v.mode = Mode::kExempt;
				v.why = off;
			}
		}
		const auto match = MatchesFor(a_weapon, a_ench, a_settings.debugLog);
		if (match.noFade) {
			v.mode = Mode::kExempt;
			v.why = "keyword Illuminated_NoFade";
			return v;  // the keyword is the last word: no file overrides it
		}
		for (const auto i : match.rules) {
			RulesText::Overlay(gRules[i], v);
		}
		return v;
	}

	std::uint32_t RulesGeneration() { return gGeneration.load(); }

	void ForgetRuleMatches()
	{
		std::lock_guard lock(gMatchLock);
		gMatches.clear();
	}

	std::size_t RuleCount()
	{
		std::lock_guard lock(gInfoLock);
		return gRuleCount;
	}

	std::size_t RuleFileCount()
	{
		std::lock_guard lock(gInfoLock);
		return gFiles;
	}

	std::vector<std::string> RuleProblems()
	{
		std::lock_guard lock(gInfoLock);
		return gProblems;
	}
}
