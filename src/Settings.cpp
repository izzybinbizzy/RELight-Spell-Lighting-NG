// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The settings file, Data\SKSE\Plugins\RELightSpellLightingNG.ini. Read once at start, written whenever the menu changes one.
//
//   [Settings]
//   Brightness=100              percent, 10 to 200 - this mod's lights only; the reach is held
//   Reach=100                   percent, 50 to 150 - the peak is held
//   ExplosionStrength=100       percent, 25 to 200 - explosions, on top of Brightness (Brightness.cpp)
//   StreamStrength=100          percent, 25 to 200 - the rows along streams, sprays and bolts and their own lights (Rows.cpp)
//   GlowStrength=100            percent, 25 to 200 - glows on people: cloaks, flesh spells, FEC's bodies (BodyLights.cpp)
//   EnchantedStrength=100       percent, 25 to 200 - enchanted weapons' lights, as the Enchanted weapons switch counts them
//   GroundStrength=100          percent, 25 to 200 - spells left on the ground: a hazard's light (Brightness.cpp)
//   FecFrozenFadesOut=1         1 = a body Frozen Electrocuted Combustion froze glows a few seconds, then fades; 0 = it stays lit
//   FecBurningFadesOut=1        1 = a body it burned glows while its flames die down, then fades; 0 = it stays lit
//   HandLights=1                the light on your hands while you cast (HandLights.cpp)
//   WeaponLights=1              every weapon light, the enchantment lights included (Options.cpp)
//   EnchantedWeapons=1          enchanted weapons' lights; bound weapons, staves and artifacts stay lit (Options.cpp)
//   WardColor=0                 0 vanilla blue, 1 white - the ward's art and light (Wards.cpp); an older file's WardColour is read too
//   HandLightsFor=0             0 everyone, 1 everyone nearby, 2 player and followers, 3 player only (Hooks.cpp)
//   AutoLights=1                spells from mods with no patch get a hand light too (HandLights.cpp)
//   Quality=0                   0 quality, 1 performance: hand, weapon and body lights for the player and followers only (Hooks.cpp)
//   [Switches]
//   Spells - Runes=1            one line per switch; a switch with no line is on. A line for an option that is not
//                               installed now is kept as it is, so reinstalling the option brings its switch back
// Keys and sections match case-insensitively. A value that is not a whole number keeps its default and is named in the log. An old
// [Brightness] section (the per-option sliders, retired) is passed over; any other section is named in the log.
//
// Threads: the menu changes a setting on the render thread while the hooks read it on the main thread and on the game's
// loader threads, so each one is an atomic (relaxed: no setting depends on another being written first). The file is written
// beside itself and moved over it, so a crash or a full disk mid-write leaves the old settings, never half a file.

#include <limits>

#include "DataText.h"
#include "Fade.h"
#include "FadeSettingsText.h"
#include "EnbShaderLights.h"
#include "Plugin.h"
#include "NatureKind.h"
#include "StrangeRunes.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kPath = "Data/SKSE/Plugins/RELightSpellLightingNG.ini";
		// up to 2.5 the same file was RelightSpellAddon.ini: read while the new one is not there yet, never written (HIS ORDER
		// 2026-10-20: "i want relight update to not affect old settings files")
		constexpr const char* kOldPath = "Data/SKSE/Plugins/RelightSpellAddon.ini";
		constexpr const char* kOldFolder = "Data/SKSE/Plugins/RelightSpellAddon";  // a 2.5 install's data files
		constexpr int         kMin = 10, kMax = 200;
		constexpr int         kReachMin = 50, kReachMax = 150;
		constexpr int         kStrengthMin = 25, kStrengthMax = 200;

		// explosions, streams, glows on people, enchanted weapons, ground effects (Strength, one slot each after kNone)
		std::array<std::atomic<int>, 5> gStrength{ 100, 100, 100, 100, 100 };
		std::atomic<int>                gBrightness{ 100 };
		std::atomic<int>                gReach{ 100 };
		std::atomic<bool>               gFecFrozenFades{ true };
		std::atomic<bool>               gFecBurningFades{ true };
		std::atomic<bool>               gHands{ true };
		std::atomic<bool>               gWeapons{ true };
		std::atomic<bool>               gEnchantedWeapons{ true };
		std::atomic<bool>               gStrangeRunes{ false };   // 1 = Strange Runes' runes light the casting hands
		std::atomic<bool>               gHandShadows{ true };     // 1 = both hands lit: each hand casts its own shadow
		std::atomic<bool>               gBurnEm{ false };         // 1 = Burn'em Banners lights its own banners, ours stands down
		RE::TESGlobal*                  gBurnEmGlobal = nullptr;  // RELightBurnEmBannersLight: Burn'em's Light Placer rows wait on it == 1
		// frost, earth and stone, water, wind, plants (NatureKind::kSettingKeys): false leaves that kind of spell unlit
		std::array<std::atomic<bool>, 5>  gNature{ true, true, true, true, true };
		std::atomic<bool>                 gEnbShaderLights{ false };  // 1 = ENB Lights For Effect Shaders lights FEC's bodies
		std::vector<RE::TESEffectShader*> gEnbOurs;                   // our body shaders it lit when the first game loaded (main thread)
		std::atomic<int>                  gWard{ 0 };
		std::atomic<int>                  gHandsFor{ 0 };
		std::atomic<bool>                 gAuto{ true };
		std::atomic<bool>                 gAllOff{ false };  // his Performance switch (Illuminated's CLAUDE.md): every light of this mod off
		std::array<std::atomic<int>, 4>   gArt{};            // fire, frost, shock, healing art: 0 = the game's own (DataText::kPickNames)
		std::atomic<bool>                 gDynamic{ true };
		std::atomic<bool>                 gDynamicSession{ true };  // what this session's data was read with (FixDynamicLighting)
		std::atomic<bool>                 gPulse{ true };           // Dynamic Lighting's Realistic: each hand light pulses in its kind's shape

		// an art pick within its group's choices (DataText::kPickNames - one list, so a new pick needs no second edit)
		[[nodiscard]] int ClampPick(int a_group, int a_v) { return std::clamp(a_v, 0, (std::max)(DataText::PickCount(a_group) - 1, 0)); }
		std::atomic<bool> gPerformance{ false };

		[[nodiscard]] std::string_view Trim(std::string_view a_s) noexcept
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
		[[nodiscard]] std::optional<int> WholeNumber(std::string_view a_v) noexcept
		{
			int v = 0;
			const auto [end, ec] = std::from_chars(a_v.data(), a_v.data() + a_v.size(), v);
			if (a_v.empty() || ec != std::errc{} || end != a_v.data() + a_v.size()) {
				return std::nullopt;
			}
			return v;
		}

		// Every [Settings] key: its name in the file, how it reads back as a whole number, and how a whole number sets it
		// (clamped to what the menu allows). ONE table, so the reader, the writer and the log can never disagree - the
		// fading module's way (FadeSettingsText.h kKeys; the re-score's RELight issue 3). Keys match case-insensitively.
		struct Key
		{
			const char* name;
			int (*get)();
			void (*set)(int);
		};

		inline constexpr Key kKeys[]{
			{ "Brightness", [] { return gBrightness.load(); }, [](int v) { gBrightness = std::clamp(v, kMin, kMax); } },
			{ "Reach", [] { return gReach.load(); }, [](int v) { gReach = std::clamp(v, kReachMin, kReachMax); } },
			{ "ExplosionStrength", [] { return gStrength[0].load(); }, [](int v) { gStrength[0] = std::clamp(v, kStrengthMin, kStrengthMax); } },
			{ "StreamStrength", [] { return gStrength[1].load(); }, [](int v) { gStrength[1] = std::clamp(v, kStrengthMin, kStrengthMax); } },
			{ "GlowStrength", [] { return gStrength[2].load(); }, [](int v) { gStrength[2] = std::clamp(v, kStrengthMin, kStrengthMax); } },
			{ "EnchantedStrength", [] { return gStrength[3].load(); }, [](int v) { gStrength[3] = std::clamp(v, kStrengthMin, kStrengthMax); } },
			{ "GroundStrength", [] { return gStrength[4].load(); }, [](int v) { gStrength[4] = std::clamp(v, kStrengthMin, kStrengthMax); } },
			{ "FecFrozenFadesOut", [] { return gFecFrozenFades ? 1 : 0; }, [](int v) { gFecFrozenFades = v != 0; } },
			{ "FecBurningFadesOut", [] { return gFecBurningFades ? 1 : 0; }, [](int v) { gFecBurningFades = v != 0; } },
			{ "HandLights", [] { return gHands ? 1 : 0; }, [](int v) { gHands = v != 0; } },
			{ "WeaponLights", [] { return gWeapons ? 1 : 0; }, [](int v) { gWeapons = v != 0; } },
			{ "EnchantedWeapons", [] { return gEnchantedWeapons ? 1 : 0; }, [](int v) { gEnchantedWeapons = v != 0; } },
			{ "StrangeRunesLight", [] { return gStrangeRunes ? 1 : 0; }, [](int v) { gStrangeRunes = v != 0; } },
			{ "HandShadows", [] { return gHandShadows ? 1 : 0; }, [](int v) { gHandShadows = v != 0; } },
			{ "VAERSwirls", [] { return VaerEffectsPercent(); }, [](int v) { SetVaerEffectsPercent(v); } },
			{ "BurnEmBannersLight", [] { return gBurnEm ? 1 : 0; }, [](int v) { gBurnEm = v != 0; } },
			{ "FrostLights", [] { return gNature[0] ? 1 : 0; }, [](int v) { gNature[0] = v != 0; } },
			{ "EarthLights", [] { return gNature[1] ? 1 : 0; }, [](int v) { gNature[1] = v != 0; } },
			{ "WaterLights", [] { return gNature[2] ? 1 : 0; }, [](int v) { gNature[2] = v != 0; } },
			{ "WindLights", [] { return gNature[3] ? 1 : 0; }, [](int v) { gNature[3] = v != 0; } },
			{ "PlantLights", [] { return gNature[4] ? 1 : 0; }, [](int v) { gNature[4] = v != 0; } },
			{ "ENBShaderLights", [] { return gEnbShaderLights ? 1 : 0; }, [](int v) { gEnbShaderLights = v != 0; } },
			{ "WardColor", [] { return gWard.load(); }, [](int v) { gWard = std::clamp(v, 0, 1); } },
			{ "HandLightsFor", [] { return gHandsFor.load(); }, [](int v) { gHandsFor = std::clamp(v, 0, 3); } },
			{ "AutoLights", [] { return gAuto ? 1 : 0; }, [](int v) { gAuto = v != 0; } },
			{ "AllLightsOff", [] { return gAllOff ? 1 : 0; }, [](int v) { gAllOff = v != 0; } },
			{ "FireArt", [] { return gArt[0].load(); }, [](int v) { SetArtPick(0, v); } },
			{ "FrostArt", [] { return gArt[1].load(); }, [](int v) { SetArtPick(1, v); } },
			{ "ShockArt", [] { return gArt[2].load(); }, [](int v) { SetArtPick(2, v); } },
			{ "HealingArt", [] { return gArt[3].load(); }, [](int v) { SetArtPick(3, v); } },
			{ "DynamicLighting", [] { return gDynamic ? 1 : 0; }, [](int v) { gDynamic = v != 0; } },
			{ "PulseShapes", [] { return gPulse ? 1 : 0; }, [](int v) { gPulse = v != 0; } },
			{ "Quality", [] { return gPerformance ? 1 : 0; }, [](int v) { gPerformance = v != 0; } },
		};

		// [Switches] lines for options that are not installed now, as read: written back unchanged, so an option taken out
		// and put back keeps its switch (the audit, 2026-10-14: the line was dropped at the next save and the switch came
		// back on). Main thread (LoadSettings, then the save task)
		std::vector<std::pair<std::string, int>> gOtherSwitches;

		// one [Settings] line; false when the key is not one of ours
		bool ApplySetting(std::string_view a_key, int a_v)
		{
			if (SameText(a_key, "WardColour")) {
				a_key = "WardColor";  // the key's spelling before 2.2 (his rule: color, never colour, in a file a player reads)
			}
			for (const auto& k : kKeys) {
				if (SameText(a_key, k.name)) {
					k.set(a_v);
					BumpSettings();
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool IsPickKey(std::string_view a_key)
		{
			for (const auto* k : { "FireArt", "FrostArt", "ShockArt", "HealingArt",
					 "DynamicLighting" }) {
				if (SameText(a_key, k)) {
					return true;
				}
			}
			return false;
		}

		// until 2.5 the art picks and Dynamic Lighting were installer choices: a file with none of their keys takes them from
		// the data files a 2.5 install left in its own folder (a manual update over it keeps that folder)
		void CarryOldPicks(bool a_log)
		{
			std::vector<std::string> names;
			std::error_code          ec;
			for (std::filesystem::directory_iterator it{ kOldFolder, ec }, end; !ec && it != end; it.increment(ec)) {
				names.push_back(Fade::PathText(it->path().filename()));  // never path::string(): it throws on a name the code page cannot show
			}
			const auto old = DataText::PicksFromOldFiles(names);
			if (!old.found) {
				return;
			}
			for (std::size_t g = 0; g < gArt.size(); ++g) {
				gArt[g] = old.art[g];
			}
			gDynamic = old.dynamic;
			if (!a_log) {
				return;
			}
			SKSE::log::info(
				"settings: the art picks and Dynamic Lighting taken from the 2.5 install (fire {}, frost {}, shock {}, healing {}, "
				"Dynamic Lighting {})",
				old.art[0], old.art[1], old.art[2], old.art[3], old.dynamic ? "on" : "off");
		}

		// the Bottled Shaders Shadows installer's pick (his 668): its one-shadow option's marker carries OneShadow=1, where a player
		// who never saved "A shadow from each hand" starts
		[[nodiscard]] bool ShadowInstallerPicksOne()
		{
			std::ifstream in("Data/SKSE/Plugins/RELightSpellLightingNG/Bottled Shaders Shadows.ini");
			std::string   line;
			while (std::getline(in, line)) {
				std::erase_if(line, [](unsigned char a_c) { return std::isspace(a_c) != 0; });
				if (SameText(line, "OneShadow=1")) {
					return true;
				}
			}
			return false;
		}

		// the file itself: written beside itself and moved over, on the thread that calls it (the main thread - SaveSettings)
		void WriteSettingsFile()
		{
			std::ostringstream text;
			text << "; RELight - Spell Lighting NG - written by its menu (SKSE Menu Framework)\n[Settings]\n";
			for (const auto& k : kKeys) {
				text << k.name << "=" << k.get() << "\n";
			}
			text << "[Switches]\n";
			std::unordered_set<std::string> written;  // a pack's files share one switch, so one line
			for (const auto& o : Options()) {
				if (o.switchable && written.insert(o.id).second) {
					text << o.id << "=" << (o.on ? 1 : 0) << "\n";
				}
			}
			for (const auto& [id, v] : gOtherSwitches) {
				text << id << "=" << v << "\n";
			}
			const std::string tmp = std::string(kPath) + ".tmp";
			std::error_code   ec;
			std::ofstream     out(tmp, std::ios::trunc);
			if (!out) {
				SKSE::log::warn("settings: {} could not be written", kPath);
				return;
			}
			out << text.str();
			out.close();
			if (!out) {
				SKSE::log::warn("settings: {} could not be written", kPath);
			} else if (std::filesystem::rename(tmp, kPath, ec); ec) {
				SKSE::log::warn("settings: {} could not be replaced ({})", kPath, ec.message());
			}
			std::filesystem::remove(tmp, ec);  // nothing left behind when the move failed
		}
	}

	void LoadSettings(bool a_log)
	{
		gOtherSwitches.clear();
		std::ifstream            in(Fade::SettingsText::ReadFrom(kPath, kOldPath));
		std::string              line;
		std::string              section;
		std::size_t              read = 0;
		std::vector<std::string> problems;
		std::vector<std::string> unknown;  // each unknown section named once
		bool                     picksRead = false, pulseRead = false, dynamicRead = false, handShadowsRead = false;
		for (bool first = true; in && std::getline(in, line); first = false) {
			if (first && line.starts_with("\xEF\xBB\xBF")) {
				line.erase(0, 3);  // a file saved "UTF-8 with BOM" would lose its first section
			}
			const auto l = Trim(line);
			if (l.empty() || l.front() == ';' || l.front() == '#') {
				continue;
			}
			if (l.front() == '[' && l.back() == ']') {
				section = Trim(l.substr(1, l.size() - 2));
				continue;
			}
			const auto eq = l.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto key = Trim(l.substr(0, eq));
			auto       rest = l.substr(eq + 1);
			if (const auto c = rest.find_first_of(";#"); c != std::string_view::npos) {
				rest = rest.substr(0, c);  // a comment after the value, as the fading module's file allows
			}
			const auto val = Trim(rest);
			const bool settings = SameText(section, "Settings"), switches = SameText(section, "Switches");
			if (!settings && !switches) {
				if (!SameText(section, "Brightness") && std::ranges::find(unknown, section) == unknown.end()) {
					unknown.push_back(section);  // the retired [Brightness] section is passed over quietly
					problems.push_back(std::format("[{}]: not a section this mod reads; its lines are passed over", section));
				}
				continue;
			}
			const auto v = WholeNumber(val);
			if (!v) {
				problems.push_back(std::format("{}={}: not a whole number; the default stays", key, val));
				continue;
			}
			++read;
			if (settings) {
				picksRead = picksRead || IsPickKey(key);
				pulseRead = pulseRead || SameText(key, "PulseShapes");
				handShadowsRead = handShadowsRead || SameText(key, "HandShadows");
				dynamicRead = dynamicRead || SameText(key, "DynamicLighting");
				// the glow limit's switch and count and Dim in daylight left the menu in 3.1: an older file's keys pass quietly
				if (!SameText(key, "GlowLimit") && !SameText(key, "GlowLimitCount") && !SameText(key, "DimInDaylight") && !ApplySetting(key, *v)) {
					problems.push_back(std::format("{}: not a setting", key));
				}
			} else {
				bool installed = false;
				for (auto& o : Options()) {
					if (o.switchable && SameText(o.id, key)) {
						o.on = *v != 0;
						installed = true;
					}
				}
				if (!installed) {
					gOtherSwitches.emplace_back(std::string(key), *v);
				}
			}
		}
		if (!picksRead) {
			CarryOldPicks(a_log);
		}
		if (!handShadowsRead) {
			gHandShadows = !ShadowInstallerPicksOne();  // never saved: the shadow installer's pick
		}
		if (!pulseRead) {
			gPulse = !dynamicRead;  // a new player starts on Realistic; one who saved Dynamic Lighting keeps it (On = Regular)
		}
		if (!a_log) {
			return;  // the early read before the data (main.cpp): the read after it says what it found
		}
		for (const auto& p : problems) {
			SKSE::log::warn("settings: {}", p);
		}
		std::size_t off = 0;
		for (const auto& o : Options()) {
			off += (o.switchable && !o.on) ? 1 : 0;
		}
		SKSE::log::info(
			"settings: brightness {}%, reach {}%, hand lights {}, weapon lights {}, {} "
			"switch(es) off ({} line(s) read, {} problem(s))",
			gBrightness.load(), gReach.load(), gHands ? "on" : "off",
			gWeapons ? "on" : "off", off, read, problems.size());
	}

	// The menu calls this on the RENDER thread, often many times while a slider is dragged: the file is written once, by an
	// SKSE task on the main thread (the re-score: no file work on the render thread). The values are atomics, so the task
	// writes whatever they hold when it runs.
	void SaveSettings()
	{
		static std::atomic<bool> queued{ false };
		if (queued.exchange(true)) {
			return;
		}
		const auto write = [] {
			queued = false;
			LightKit::Guarded("settings save", []() {
				WriteSettingsFile();
				SyncBurnEmGlobal();
				RetuneBurningBanners();  // the burning banners' fire lights under the values just saved (BurningBanners.cpp)
			});
		};
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(write);
		} else {
			write();
		}
	}

	int   BrightnessPercent() { return gBrightness; }
	float Brightness() { return static_cast<float>(gBrightness) / 100.0f; }

	void SetBrightnessPercent(int a_percent)
	{
		a_percent = std::clamp(a_percent, kMin, kMax);
		if (gBrightness.exchange(a_percent) != a_percent) {
			BumpSettings();
			SKSE::log::info("brightness set to {}%", a_percent);
		}
	}

	int   ReachPercent() { return gReach; }
	float Reach() { return static_cast<float>(gReach) / 100.0f; }

	void SetReachPercent(int a_percent)
	{
		a_percent = std::clamp(a_percent, kReachMin, kReachMax);
		if (gReach.exchange(a_percent) != a_percent) {
			BumpSettings();
			SKSE::log::info("reach set to {}%", a_percent);
		}
	}

	int StrengthPercent(Strength a_kind)
	{
		return a_kind == Strength::kNone ? 100 : gStrength[static_cast<std::size_t>(a_kind) - 1].load(std::memory_order_relaxed);
	}

	float StrengthOf(Strength a_kind) { return static_cast<float>(StrengthPercent(a_kind)) / 100.0f; }

	void SetStrengthPercent(Strength a_kind, int a_percent)
	{
		if (a_kind == Strength::kNone) {
			return;
		}
		a_percent = std::clamp(a_percent, kStrengthMin, kStrengthMax);
		if (gStrength[static_cast<std::size_t>(a_kind) - 1].exchange(a_percent) != a_percent) {
			BumpSettings();
		}
	}

	bool PulseShapes() { return gPulse; }

	void SetPulseShapes(bool a_on)
	{
		if (gPulse.exchange(a_on) != a_on) {
			NoteSliderLook();
		}
	}

	bool PulseRealistic() { return gPulse && DynamicLightingThisSession(); }

	bool FecFrozenFadesOut() { return gFecFrozenFades; }

	bool FecBurningFadesOut() { return gFecBurningFades; }

	void SetFecBurningFadesOut(bool a_on)
	{
		if (gFecBurningFades.exchange(a_on) != a_on) {
			BumpSettings();
			SKSE::log::info("burning bodies' glow {}", a_on ? "fades out" : "stays while burning");
		}
	}

	void SetFecFrozenFadesOut(bool a_on)
	{
		if (gFecFrozenFades.exchange(a_on) != a_on) {
			BumpSettings();
			SKSE::log::info("frozen bodies' glow {}", a_on ? "fades out" : "stays while frozen");
		}
	}

	std::size_t GlowLimit()
	{
		// off Community Shaders the game lights a surface with only a few lights at once and switches the rest off (they flicker
		// as the camera moves): 5 there, as the rows; Light Limit Fix takes far more, and 16 keeps a big fight smooth
		return LightingFound() == Lighting::kShaders ? 16 : 5;
	}

	bool HandLightsOn() { return gHands; }

	void SetHandLightsOn(bool a_on)
	{
		if (gHands.exchange(a_on) != a_on) {
			SKSE::log::info("hand lights turned {}", a_on ? "on" : "off");
		}
	}

	bool WeaponLightsOn() { return gWeapons; }

	void SetWeaponLightsOn(bool a_on)
	{
		if (gWeapons.exchange(a_on) != a_on) {
			BumpSettings();
			SKSE::log::info("weapon lights turned {}", a_on ? "on" : "off");
		}
	}

	bool EnchantedWeaponsOn() { return gEnchantedWeapons; }

	void SetEnchantedWeaponsOn(bool a_on)
	{
		if (gEnchantedWeapons.exchange(a_on) != a_on) {
			BumpSettings();
			SKSE::log::info("enchanted weapons' lights turned {}", a_on ? "on" : "off");
		}
	}

	bool StrangeRunesLightOn() { return gStrangeRunes; }

	void SetStrangeRunesLightOn(bool a_on)
	{
		if (gStrangeRunes.exchange(a_on) != a_on) {
			SKSE::log::info("Strange Runes lights the hands: {}", a_on ? "on" : "off");
		}
	}

	bool StrangeRunesLoaded() { return StrangeRunes::Loaded(); }

	bool NatureLightsOn(int a_index) { return a_index < 0 || a_index >= static_cast<int>(gNature.size()) || gNature[static_cast<std::size_t>(a_index)]; }

	void SetNatureLightsOn(int a_index, bool a_on)
	{
		if (a_index >= 0 && a_index < static_cast<int>(gNature.size()) && gNature[static_cast<std::size_t>(a_index)].exchange(a_on) != a_on) {
			BumpSettings();  // the option walk, the rows and the body glows look again
			SKSE::log::info("{} turned {}", NatureKind::kSettingKeys[static_cast<std::size_t>(a_index)], a_on ? "on" : "off");
		}
	}

	bool AnyNatureOff()
	{
		return std::ranges::any_of(gNature, [](const auto& a_on) { return !a_on.load(); });
	}

	int NatureIndexOf(const RE::TESForm* a_form)
	{
		const auto* model = a_form ? a_form->As<RE::TESModel>() : nullptr;
		const char* path = model ? model->GetModel() : nullptr;
		return path ? NatureKind::IndexOf(NatureKind::KindOf(path)) : -1;
	}

	bool NatureKindOff(std::string_view a_path) { return !NatureLightsOn(NatureKind::IndexOf(NatureKind::KindOf(a_path))); }

	bool StrangeRunesHands() { return gStrangeRunes && StrangeRunes::Loaded(); }

	// it starts at the Advanced Settings file's VAERebornEffects (read before the settings file) until the settings are first saved;
	// from then on the saved slider wins
	int VaerEffectsPercent() { return static_cast<int>(std::lround(gVaeEffects.load(std::memory_order_relaxed) * 100.0f)); }

	void SetVaerEffectsPercent(int a_percent)
	{
		a_percent = std::clamp(a_percent, 0, 400);
		if (VaerEffectsPercent() != a_percent) {
			gVaeEffects.store(static_cast<float>(a_percent) / 100.0f, std::memory_order_relaxed);
			SKSE::log::info("VAER Reborn's enchantment effects set to {}%", a_percent);
		}
	}

	bool ShadowFromEachHand() { return gHandShadows; }

	void SetShadowFromEachHand(bool a_on)
	{
		if (gHandShadows.exchange(a_on) != a_on) {
			SKSE::log::info("a shadow from each hand: {}", a_on ? "on" : "off");
		}
	}

	bool BurnEmBannersLoaded()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		return dh && (dh->LookupLoadedModByName("BurnEmBanners.esp") || dh->LookupLoadedLightModByName("BurnEmBanners.esp"));
	}

	bool BurnEmBannersLightsItself() { return gBurnEm; }

	void SetBurnEmBannersLightsItself(bool a_on)
	{
		if (gBurnEm.exchange(a_on) != a_on) {
			SKSE::log::info("Burn'em Banners lights its banners: {}", a_on ? "on" : "off");
		}
	}

	// Illuminated's way (its Settings.cpp MakeGlobal): a global made by the game's own factory, its editor ID put into the game's
	// table so Light Placer's condition finds it by name, read back by that name
	void MakeBurnEmGlobal()
	{
		constexpr const char* kId = "RELightBurnEmBannersLight";
		auto*                 g = LightKit::NewForm<RE::TESGlobal>();
		if (!g) {
			SKSE::log::warn("{}: the game made no global", kId);
			return;
		}
		g->SetFormEditorID(kId);
		const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
		if (map) {
			RE::BSWriteLockGuard guard{ lock };
			map->insert({ RE::BSFixedString(kId), g });
		}
		gBurnEmGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kId) == g ? g : nullptr;
		SyncBurnEmGlobal();
		SKSE::log::info("{}: {}", kId, gBurnEmGlobal ? "made" : "not found by its name");
	}

	void SyncBurnEmGlobal()
	{
		if (gBurnEmGlobal) {
			gBurnEmGlobal->value = gBurnEm ? 1.0f : 0.0f;
		}
	}

	bool EnbShaderLightsOn() { return gEnbShaderLights; }

	void SetEnbShaderLightsOn(bool a_on)
	{
		if (gEnbShaderLights.exchange(a_on) != a_on) {
			BumpSettings();  // the body lights look again: ours on its shaders off, or back
			SKSE::log::info("ENB Lights For Effect Shaders lights FEC's bodies: {}", a_on ? "on" : "off");
		}
	}

	bool EnbShaderLightsLoaded()
	{
		static const bool here = EnbShaderLights::Installed();  // asked after every SKSE plugin has loaded
		return here;
	}

	bool EnbShadersGivenBack() { return gEnbShaderLights && EnbShaderLightsLoaded(); }

	bool EnbShaderActor(const RE::Actor* a_actor)
	{
		// its own ini: 0 the player, 1 and followers, 2 everyone (its default)
		static const int valid = EnbShaderLights::ValidActors();
		return a_actor && (valid >= 2 || a_actor->IsPlayerRef() || (valid == 1 && a_actor->IsPlayerTeammate()));
	}

	void ApplyEnbShaderLights()
	{
		const bool giveBack = EnbShadersGivenBack() || AllLightsOff();
		for (auto* shader : gEnbOurs) {
			if (giveBack) {
				EnbShaderLights::GiveBack(shader);
			} else {
				EnbShaderLights::TakeOff(shader);
			}
		}
	}

	void TakeEnbShaderLights()
	{
		static bool done = false;
		if (done || !EnbShaderLightsLoaded()) {
			return;
		}
		done = true;
		for (auto* shader : BodyShaders()) {
			if (EnbShaderLights::Lit(shader)) {
				gEnbOurs.push_back(shader);
			}
		}
		ApplyEnbShaderLights();
		SKSE::log::info("ENB Lights For Effect Shaders lights {} body shader(s) RELight lights too: ours is the default there", gEnbOurs.size());
	}

	int WardColor() { return gWard; }

	void SetWardColor(int a_color)
	{
		a_color = std::clamp(a_color, 0, 1);
		if (gWard.exchange(a_color) != a_color) {
			SKSE::log::info("ward color set to {}", a_color == 1 ? "white" : "vanilla blue");
		}
	}

	// a preset (Menu.cpp, his word: presets kept per lighting): every [Settings] key, and every switch as "switch:<id>"
	std::vector<std::pair<std::string, int>> SettingsSnapshot()
	{
		std::vector<std::pair<std::string, int>> out;
		for (const auto& k : kKeys) {
			out.emplace_back(k.name, k.get());
		}
		std::unordered_set<std::string> seen;
		for (const auto& o : Options()) {
			if (o.switchable && seen.insert(o.id).second) {
				out.emplace_back("switch:" + o.id, o.on ? 1 : 0);
			}
		}
		return out;
	}

	void ApplySettingsSnapshot(const std::vector<std::pair<std::string, int>>& a_values)
	{
		for (const auto& [key, v] : a_values) {
			if (key.starts_with("switch:")) {
				const auto id = std::string_view(key).substr(7);
				for (std::size_t i = 0; i < Options().size(); ++i) {
					if (Options()[i].switchable && SameText(Options()[i].id, id)) {
						SetOptionOn(i, v != 0);
					}
				}
			} else if (!ApplySetting(key, v)) {
				SKSE::log::info("a preset names the setting {}, which this version does not have - left out", key);
			}
		}
	}

	int  HandLightsFor() { return gHandsFor; }
	void SetHandLightsFor(int a_v) { gHandsFor = std::clamp(a_v, 0, 3); }
	bool AutoLightsOn() { return gAuto; }
	void SetAutoLightsOn(bool a_on) { gAuto = a_on; }
	bool AllLightsOff() { return gAllOff; }
	void SetAllLightsOff(bool a_off)
	{
		gAllOff = a_off;
		BumpSettings();
	}

	namespace
	{
		std::atomic<std::uint32_t> gSettingsGen{ 0 };
	}
	void BumpSettings()
	{
		gSettingsGen.fetch_add(1, std::memory_order_relaxed);
		NoteSliderLook();
	}
	std::uint32_t SettingsGeneration() { return gSettingsGen.load(std::memory_order_relaxed); }
	int           ArtPick(int a_group) { return a_group >= 0 && a_group < 4 ? gArt[static_cast<std::size_t>(a_group)].load() : 0; }

	// read before the data at a start; once the data is read, a change recolors at once: the tints and the lights already lit
	// re-read their colors, and the hand lights and the sprays' copies are dressed again on the main thread
	void SetArtPick(int a_group, int a_v)
	{
		if (a_group < 0 || a_group >= 4) {
			return;
		}
		const int v = ClampPick(a_group, a_v);
		if (gArt[static_cast<std::size_t>(a_group)].exchange(v) == v || DataFiles() == 0) {
			return;
		}
		ArtPickChanged();
		SKSE::log::info("art pick {} set to {}: its colors change now", a_group, v);
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([]() {
				LightKit::Guarded("art pick", []() {
					RecolorStreamLights();
					ApplyHandLights(false);
				});
			});
		}
	}

	bool DynamicLighting() { return gDynamic; }
	bool DynamicLightingThisSession() { return gDynamicSession; }
	void FixDynamicLighting() { gDynamicSession = gDynamic.load(); }
	void SetDynamicLighting(bool a_on) { gDynamic = a_on; }
	bool PerformanceMode() { return gPerformance; }
	void SetPerformanceMode(bool a_on)
	{
		if (gPerformance.exchange(a_on) != a_on) {
			BumpSettings();
		}
	}

	// his call: a pack the build split across the downloads is ONE switch - its files share a `file` line, so
	// every option with that id turns together
	void SetOptionOn(std::size_t a_index, bool a_on)
	{
		auto& opts = Options();
		if (a_index >= opts.size() || !opts[a_index].switchable) {
			return;
		}
		const auto  id = opts[a_index].id;
		std::size_t n = 0;
		for (auto& o : opts) {
			if (o.switchable && o.id == id && o.on != a_on) {
				o.on = a_on;
				++n;
			}
		}
		if (n) {
			BumpSettings();
			SKSE::log::info("switch: {} turned {} ({} file(s))", id, a_on ? "on" : "off", n);
		}
	}
}
