// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings, and the settings file's text both ways, with nothing of the game in it (tests/test_fade.cpp reads and
// writes it off the game). FadeSettings.cpp only opens the file and keeps the one shared copy.
//
//   [Settings]
//   Defaults=2                a stamp, not a setting: the defaults the file was last moved to; a file without it
//                             (written before 2) is moved once to Curve=0 and ReachFollows=100, then saved
//   Enabled=1
//   EmptyBrightness=10        percent, 0 to 50 - what is left of the light at 0% charge
//   Curve=0                   0 linear, 1 gentle (stays bright longer)
//   ReachFollows=100          percent, 0 to 100 - how much of the fade the reach follows too
//   Sputter=1
//   SputterBelow=15           percent of charge, 1 to 50
//   SputterStrength=60        percent - the deepest dip, at empty
//   EmptySteady=1             at exactly 0% the light holds still
//   HitPulse=1
//   HitPulseStrength=60       percent, 0 to 200
//   RechargeFlare=1
//   RechargeFlareStrength=80  percent, 0 to 200
//   ColorCooling=1
//   ColorCoolingAmount=50     percent - toward grey; a fire spell or enchantment with an orange light cools to ember
//   Weapons=1                 enchanted weapons' lights and glow follow their charge
//   Staves=1
//   BoundWeapons=1
//   Spells=1                  a spell in hand: its hand's lights and glow follow the caster's magicka
//   BoundFadeSeconds=10       1 to 60: the last seconds of a bound weapon's spell, over which its light fades
//   Who=0                     0 the player, 1 the player and followers, 2 everyone nearby (each actor the game fully runs near you)
//   DimShader=1               the enchantment's glow (its shader and its art's swirls) follows the charge too
//   HideChargeBar=0           the HUD's enchantment charge bar is hidden (vanilla HUD, SkyHUD, TrueHUD) - while Fading is on
//   HudGems=0                 a soul gem per weapon hand beside the health bar, shown while its charge is down (needs SKSE Menu Framework)
//   HudGemSize=100            percent, 50 to 200
//   HudGemOpacity=90          percent, 10 to 100
//   HudGemPercent=1           the percent under each gem
//   HudGemFull=0              a gem for every enchanted or bound weapon in hand, full ones too (0: once its charge drops)
//   DebugLog=0
//   Fire=1 Frost=1 Shock=1 Absorb=1 SoulTrap=1 Paralyze=1 FearTurnBanish=1 OtherEffects=1
//                             by effect: an enchantment's or a spell's strongest effect, for weapons and spells alike
//   Destruction=1 Restoration=1 Conjuration=1 Alteration=1 Illusion=1
//                             by school: the same effect's school (an effect of no school is never held back by these)
//
// Every value is a whole number. The file is read the way people edit it: a byte order mark (Notepad's), any case in
// section and key names, spaces, Windows line ends, and ; or # comments, on their own line or after a value. A line
// that is not a whole number, or names no setting, keeps the default and is reported; a retired key (kRetired) is
// skipped quietly.

#pragma once

#include "FadeConfig.h"
#include "FadeGlow.h"
#include "LightMath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace Fade
{
	enum class Who : int
	{
		kPlayer = 0,
		kPlayerAndFollowers = 1,
		kEveryone = 2
	};

	inline constexpr int kDefaultsVersion = 2;  // 2: Curve linear, Reach follows 100 %

	struct Settings
	{
		int          defaults{ kDefaultsVersion };  // a file without it predates kDefaultsVersion
		bool         enabled{ true };
		Glow::Tuning tuning{};
		Who          who{ Who::kPlayer };
		bool         weapons{ true };  // enchanted weapons (staves and bound weapons have their own switches too)
		bool         staves{ true };
		bool         bound{ true };
		bool         spells{ true };  // a spell in hand fades with the caster's magicka
		// what kind of magic fades, by the strongest effect (Kind), for weapons and spells alike: [element] and [school]
		std::array<bool, 8> elements{ true, true, true, true, true, true, true, true };
		std::array<bool, 5> schools{ true, true, true, true, true };
		float               boundFadeSeconds{ 10.0f };
		bool                dimShader{ true };  // the enchantment's glow shader and its art's swirls follow the charge too
		bool                hideChargeBar{ false };
		bool                hudGems{ false };  // FadeMenu.cpp's HUD element: one charge gem per tracked hand of the player
		float               hudGemSize{ 1.0f };
		float               hudGemOpacity{ 0.9f };
		bool                hudGemPercent{ true };  // the percent under each gem
		bool                hudGemFull{ false };    // a gem at full charge too (his ask 2026-10-16); off: once the charge drops
		bool                debugLog{ false };

		bool operator==(const Settings&) const = default;
	};

	// the kind of an enchantment's or a spell's strongest effect, as FadeSpells.cpp reads it off the game
	enum class Element : int
	{
		kFire = 0,
		kFrost,
		kShock,
		kAbsorb,
		kSoulTrap,
		kParalyze,
		kFearTurnBanish,  // fear, turn undead, banish
		kOther
	};
	inline constexpr const char* kElementKeys[]{ "Fire", "Frost", "Shock", "Absorb", "SoulTrap", "Paralyze", "FearTurnBanish", "OtherEffects" };
	inline constexpr const char* kSchoolKeys[]{ "Destruction", "Restoration", "Conjuration", "Alteration", "Illusion" };

	struct Kind
	{
		Element element{ Element::kOther };
		int     school{ -1 };  // 0 Destruction .. 4 Illusion, as kSchoolKeys; -1 no school
	};

	// "" when this kind fades, else why it does not (for the log and the Debug page)
	[[nodiscard]] inline std::string KindOff(const Settings& a_s, Kind a_k)
	{
		const auto e = static_cast<std::size_t>(a_k.element);
		if (e < a_s.elements.size() && !a_s.elements[e]) {
			return std::string(kElementKeys[e]) + " off";
		}
		if (a_k.school >= 0 && a_k.school < 5 && !a_s.schools[static_cast<std::size_t>(a_k.school)]) {
			return std::string(kSchoolKeys[a_k.school]) + " off";
		}
		return {};
	}

	// what a weapon ends up with once the rule files have had their say (FadeRules.cpp, FadeRulesText.h)
	enum class Mode : int
	{
		kCharge = 0,  // the light follows the enchantment's charge
		kExempt = 1,  // the light is left alone
		kBound = 2    // the light follows the time a bound-weapon spell has left
	};

	struct Verdict
	{
		Mode         mode{ Mode::kCharge };
		Glow::Tuning tuning{};
		float        boundFadeSeconds{ 10.0f };
		std::string  why;  // the rule(s) that decided it, for the menu's debug page
	};

	namespace SettingsText
	{
		[[nodiscard]] inline float Pct(int a_v, int a_lo, int a_hi) { return static_cast<float>(std::clamp(a_v, a_lo, a_hi)) / 100.0f; }
		[[nodiscard]] inline int   ToPct(float a_v) { return static_cast<int>(std::lround(a_v * 100.0f)); }

		// every setting: its key in the file, how it reads back as a whole number, and how a whole number sets it (clamped to
		// what the menu allows). One table, so the file, the menu and DevBench can never disagree.
		struct Key
		{
			const char* name;
			int (*get)(const Settings&);
			void (*set)(Settings&, int);
		};

		inline constexpr Key kKeys[]{
			{ "Enabled", [](const Settings& s) { return s.enabled ? 1 : 0; }, [](Settings& s, int v) { s.enabled = v != 0; } },
			{ "EmptyBrightness", [](const Settings& s) { return ToPct(s.tuning.floor); }, [](Settings& s, int v) { s.tuning.floor = Pct(v, 0, 50); } },
			{ "Curve", [](const Settings& s) { return static_cast<int>(s.tuning.curve); },
				[](Settings& s, int v) { s.tuning.curve = v <= 0 ? Glow::Curve::kLinear : Glow::Curve::kGentle; } },
			{ "ReachFollows", [](const Settings& s) { return ToPct(s.tuning.reachFollows); },
				[](Settings& s, int v) { s.tuning.reachFollows = Pct(v, 0, 100); } },
			{ "Sputter", [](const Settings& s) { return s.tuning.sputter ? 1 : 0; }, [](Settings& s, int v) { s.tuning.sputter = v != 0; } },
			{ "SputterBelow", [](const Settings& s) { return ToPct(s.tuning.sputterBelow); },
				[](Settings& s, int v) { s.tuning.sputterBelow = Pct(v, 1, 50); } },
			{ "SputterStrength", [](const Settings& s) { return ToPct(s.tuning.sputterStrength); },
				[](Settings& s, int v) { s.tuning.sputterStrength = Pct(v, 0, 100); } },
			{ "EmptySteady", [](const Settings& s) { return s.tuning.emptySteady ? 1 : 0; }, [](Settings& s, int v) { s.tuning.emptySteady = v != 0; } },
			{ "HitPulse", [](const Settings& s) { return s.tuning.pulse ? 1 : 0; }, [](Settings& s, int v) { s.tuning.pulse = v != 0; } },
			{ "HitPulseStrength", [](const Settings& s) { return ToPct(s.tuning.pulseStrength); },
				[](Settings& s, int v) { s.tuning.pulseStrength = Pct(v, 0, 200); } },
			{ "RechargeFlare", [](const Settings& s) { return s.tuning.flare ? 1 : 0; }, [](Settings& s, int v) { s.tuning.flare = v != 0; } },
			{ "RechargeFlareStrength", [](const Settings& s) { return ToPct(s.tuning.flareStrength); },
				[](Settings& s, int v) { s.tuning.flareStrength = Pct(v, 0, 200); } },
			{ "ColorCooling", [](const Settings& s) { return s.tuning.cool ? 1 : 0; }, [](Settings& s, int v) { s.tuning.cool = v != 0; } },
			{ "ColorCoolingAmount", [](const Settings& s) { return ToPct(s.tuning.coolAmount); },
				[](Settings& s, int v) { s.tuning.coolAmount = Pct(v, 0, 100); } },
			{ "Weapons", [](const Settings& s) { return s.weapons ? 1 : 0; }, [](Settings& s, int v) { s.weapons = v != 0; } },
			{ "Staves", [](const Settings& s) { return s.staves ? 1 : 0; }, [](Settings& s, int v) { s.staves = v != 0; } },
			{ "BoundWeapons", [](const Settings& s) { return s.bound ? 1 : 0; }, [](Settings& s, int v) { s.bound = v != 0; } },
			{ "Spells", [](const Settings& s) { return s.spells ? 1 : 0; }, [](Settings& s, int v) { s.spells = v != 0; } },
			{ "BoundFadeSeconds", [](const Settings& s) { return static_cast<int>(std::lround(s.boundFadeSeconds)); },
				[](Settings& s, int v) { s.boundFadeSeconds = static_cast<float>(std::clamp(v, 1, 60)); } },
			{ "Who", [](const Settings& s) { return static_cast<int>(s.who); }, [](Settings& s, int v) { s.who = static_cast<Who>(std::clamp(v, 0, 2)); } },
			{ "DimShader", [](const Settings& s) { return s.dimShader ? 1 : 0; }, [](Settings& s, int v) { s.dimShader = v != 0; } },
			{ "HideChargeBar", [](const Settings& s) { return s.hideChargeBar ? 1 : 0; }, [](Settings& s, int v) { s.hideChargeBar = v != 0; } },
			{ "HudGems", [](const Settings& s) { return s.hudGems ? 1 : 0; }, [](Settings& s, int v) { s.hudGems = v != 0; } },
			{ "HudGemSize", [](const Settings& s) { return ToPct(s.hudGemSize); }, [](Settings& s, int v) { s.hudGemSize = Pct(v, 50, 200); } },
			{ "HudGemOpacity", [](const Settings& s) { return ToPct(s.hudGemOpacity); },
				[](Settings& s, int v) { s.hudGemOpacity = Pct(v, 10, 100); } },
			{ "HudGemPercent", [](const Settings& s) { return s.hudGemPercent ? 1 : 0; }, [](Settings& s, int v) { s.hudGemPercent = v != 0; } },
			{ "HudGemFull", [](const Settings& s) { return s.hudGemFull ? 1 : 0; }, [](Settings& s, int v) { s.hudGemFull = v != 0; } },
			{ "DebugLog", [](const Settings& s) { return s.debugLog ? 1 : 0; }, [](Settings& s, int v) { s.debugLog = v != 0; } },
			{ "Fire", [](const Settings& s) { return s.elements[0] ? 1 : 0; }, [](Settings& s, int v) { s.elements[0] = v != 0; } },
			{ "Frost", [](const Settings& s) { return s.elements[1] ? 1 : 0; }, [](Settings& s, int v) { s.elements[1] = v != 0; } },
			{ "Shock", [](const Settings& s) { return s.elements[2] ? 1 : 0; }, [](Settings& s, int v) { s.elements[2] = v != 0; } },
			{ "Absorb", [](const Settings& s) { return s.elements[3] ? 1 : 0; }, [](Settings& s, int v) { s.elements[3] = v != 0; } },
			{ "SoulTrap", [](const Settings& s) { return s.elements[4] ? 1 : 0; }, [](Settings& s, int v) { s.elements[4] = v != 0; } },
			{ "Paralyze", [](const Settings& s) { return s.elements[5] ? 1 : 0; }, [](Settings& s, int v) { s.elements[5] = v != 0; } },
			{ "FearTurnBanish", [](const Settings& s) { return s.elements[6] ? 1 : 0; }, [](Settings& s, int v) { s.elements[6] = v != 0; } },
			{ "OtherEffects", [](const Settings& s) { return s.elements[7] ? 1 : 0; }, [](Settings& s, int v) { s.elements[7] = v != 0; } },
			{ "Destruction", [](const Settings& s) { return s.schools[0] ? 1 : 0; }, [](Settings& s, int v) { s.schools[0] = v != 0; } },
			{ "Restoration", [](const Settings& s) { return s.schools[1] ? 1 : 0; }, [](Settings& s, int v) { s.schools[1] = v != 0; } },
			{ "Conjuration", [](const Settings& s) { return s.schools[2] ? 1 : 0; }, [](Settings& s, int v) { s.schools[2] = v != 0; } },
			{ "Alteration", [](const Settings& s) { return s.schools[3] ? 1 : 0; }, [](Settings& s, int v) { s.schools[3] = v != 0; } },
			{ "Illusion", [](const Settings& s) { return s.schools[4] ? 1 : 0; }, [](Settings& s, int v) { s.schools[4] = v != 0; } },
		};

		// keys an older version wrote: read past without a warning, never written again (Glow::TintFor decides the cooling tint)
		inline constexpr std::string_view kRetired[]{ "OwnLight", "OwnLightReach", "ColorCoolingTint", "Reticle", "ReticleStyle", "ReticleSize",
			"ReticleOpacity", "ReticleDotSize", "ReticleDotClear" };

		using LightKit::SameText;  // LightMath.h: the one case-blind compare

		[[nodiscard]] inline const Key* Find(std::string_view a_name) noexcept
		{
			for (const auto& k : kKeys) {
				if (SameText(a_name, k.name)) {
					return &k;
				}
			}
			return nullptr;
		}

		[[nodiscard]] inline std::string_view Trim(std::string_view a_s) noexcept
		{
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
				a_s.remove_prefix(1);
			}
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
				a_s.remove_suffix(1);
			}
			return a_s;
		}

		// a whole number and nothing else ("15abc" and "1.5" are not)
		[[nodiscard]] inline bool ReadInt(std::string_view a_v, int& a_out) noexcept
		{
			int v = 0;
			const auto [end, ec] = std::from_chars(a_v.data(), a_v.data() + a_v.size(), v);
			if (a_v.empty() || ec != std::errc{} || end != a_v.data() + a_v.size()) {
				return false;
			}
			a_out = v;
			return true;
		}

		// one setting by its key (any case), clamped as the menu clamps it; false for a key it does not know
		inline bool Apply(Settings& a_s, std::string_view a_key, int a_value)
		{
			if (const auto* k = Find(a_key)) {
				k->set(a_s, a_value);
				return true;
			}
			return false;
		}

		struct ReadResult
		{
			int                      taken{ 0 };
			bool                     moved{ false };  // an older file moved to the new defaults (save it)
			std::vector<std::string> problems;        // "Key=value: why", for the log
		};

		// reads the [Settings] lines into a_s; a line that cannot be used keeps the default and is reported
		inline ReadResult Read(std::istream& a_in, Settings& a_s)
		{
			ReadResult  r;
			bool        versioned = false, setFade = false;  // setFade: the file set Curve or ReachFollows
			std::string line;
			bool        inSettings = false, first = true;
			while (std::getline(a_in, line)) {
				std::string_view l = line;
				if (first && l.starts_with("\xEF\xBB\xBF")) {
					l.remove_prefix(3);  // a UTF-8 byte order mark
				}
				first = false;
				if (const auto c = l.find_first_of(";#"); c != std::string_view::npos) {
					l = l.substr(0, c);
				}
				l = Trim(l);
				if (l.empty()) {
					continue;
				}
				if (l.front() == '[' && l.back() == ']') {
					inSettings = SameText(Trim(l.substr(1, l.size() - 2)), "Settings");
					continue;
				}
				const auto eq = l.find('=');
				if (!inSettings || eq == std::string_view::npos) {
					continue;
				}
				const auto key = Trim(l.substr(0, eq));
				const auto val = Trim(l.substr(eq + 1));
				int        v = 0;
				if (std::ranges::any_of(kRetired, [key](std::string_view k) { return SameText(key, k); })) {
					continue;
				}
				if (SameText(key, "Defaults")) {
					if (ReadInt(val, v)) {
						a_s.defaults = (std::max)(v, 0);  // a newer build's stamp is kept, never walked back
						versioned = true;
					} else {
						r.problems.push_back(std::string(key) + "=" + std::string(val) + ": not a whole number; read as unstamped");
					}
					continue;
				}
				if (!Find(key)) {
					r.problems.push_back(std::string(key) + ": not a setting");
				} else if (!ReadInt(val, v)) {
					r.problems.push_back(std::string(key) + "=" + std::string(val) + ": not a whole number; the default stays");
				} else {
					Apply(a_s, key, v);
					setFade = setFade || SameText(key, "Curve") || SameText(key, "ReachFollows");
					++r.taken;
				}
			}
			// a file from before a defaults change is moved once, one step per version, so a later pick of the player stays
			if (r.taken > 0 && !versioned) {
				a_s.defaults = 0;
			}
			if (r.taken > 0 && a_s.defaults < kDefaultsVersion) {
				if (a_s.defaults < 2 && setFade) {
					a_s.tuning.curve = Glow::Curve::kLinear;
					a_s.tuning.reachFollows = 1.0f;
				}
				r.moved = setFade || a_s.defaults != 0;
				a_s.defaults = kDefaultsVersion;
			}
			return r;
		}

		// The file to read: a_path, or - while a_path is not there yet - the same file under the mod's old name (RELight - Spell
		// Lighting NG kept its settings as RelightSpellAddon up to 2.5). The old file is only ever read - never written, moved or
		// deleted: every write goes to a_path, so the first save makes the new file and the old one stays as the player left it. An empty a_old (Illuminated, never
		// renamed) is a_path itself.
		[[nodiscard]] inline std::string ReadFrom(std::string_view a_path, std::string_view a_old)
		{
			namespace fs = std::filesystem;
			std::error_code ec;
			if (a_old.empty() || fs::exists(fs::path(a_path), ec) || ec) {
				return std::string(a_path);  // an error asking about the new file never falls back to the old one
			}
			return fs::is_regular_file(fs::path(a_old), ec) && !ec ? std::string(a_old) : std::string(a_path);
		}

		inline void Write(std::ostream& a_out, const Settings& a_s)
		{
			a_out << "; " << Mod::kName << " - fading - written by its menu (SKSE Menu Framework). Percent values are whole numbers.\n[Settings]\n";
			a_out << "Defaults=" << a_s.defaults << '\n';
			for (const auto& k : kKeys) {
				a_out << k.name << '=' << k.get(a_s) << '\n';
			}
		}
	}
}
