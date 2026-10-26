// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Pages in SKSE Menu Framework's Mod Control Panel, under their own section so nothing of RE::Light's own menu is touched, laid out
// as Illuminated's: Lights (the lighting found, presets, Brightness, Reach, daylight, sneaking, Flicker, hand and weapon lights,
// the ward color), Spell & Enchantment Art (the weapon enchantment mods, then the Shock, Fire, Frost and Healing art picks),
// Configs (the spells' own switches, Creation Club, the single-mod patches, then one group per author) and Praedy's Staves. The
// fading module adds HUD and Fading after them, then this file the Lights by Item folder (Spell: automatic lights above the spell
// list; Weapons and Staves) and Performance, the last page. Every change is saved at once (Settings.cpp) and reaches lights
// already lit. The look is the shared MenuStyle.h in warm spell-light gold, with this mod's glowing headings.

#define WIN32_LEAN_AND_MEAN  // NOMINMAX is set for every file in xmake.lua (the fading module needs it too)
#include "Fade.h"
#include "FadeSettingsText.h"
#include "Plugin.h"
#include "DataText.h"

// SKSE Menu Framework's own header (theirs, MIT): its warnings are not ours, and ours are errors (xmake.lua) - under
// MSVC and under clang-cl, which checks every file too
#pragma warning(push, 0)
#if defined(__clang__)
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Weverything"
#endif
#include "SKSEMenuFramework.h"
#if defined(__clang__)
#	pragma clang diagnostic pop
#endif
#pragma warning(pop)
#include "Translation.h"
#include "MenuStyle.h"

namespace Plugin
{
	namespace
	{

		// the menu draws off the game's main thread; which light a magic effect wears is changed on it
		void RehandSoon()
		{
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([]() {
					ApplyHandLights(true);
				});
			}
		}

		// a preset may change the ward color too: the wards and the hand lights take the new settings together (CodeRabbit,
		// RELight #8)
		void RepresetSoon()
		{
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([]() {
					ApplyWards("a preset");
					RefindHandLights();
					ApplyHandLights(true);
				});
			}
		}

		constexpr std::string_view kEnchantments = "Weapon Enchantments";  // the menugroup of VAER Reborn and Vibrant Weapons EAE
		constexpr std::string_view kPraedys = "Praedy's Staves";           // the menugroup of Praedy's seven staves options
		constexpr std::string_view kWeaponsGroup = "Weapons";              // the menugroup of Artifacts and Bound Weapons

		// where an option's switch is drawn, as Illuminated's pages are: the weapon enchantment mods on Spell & Enchantment Art (with
		// the four art picks), Praedy's staves on a page of their own, everything else on Configs
		enum class Page
		{
			kArt,
			kConfigs,
			kPraedys
		};

		[[nodiscard]] Page PageOf(const Option& a_option)
		{
			return a_option.category == kEnchantments ? Page::kArt : a_option.category == kPraedys ? Page::kPraedys :
			                                                                                         Page::kConfigs;
		}

		// Illuminated's Praedy's Staves page: each staff under the mod it needs, in the sections and words of its settings file
		struct PraedySection
		{
			std::string_view option, group, label, needs;
		};
		constexpr PraedySection kPraedySections[]{
			{ "Praedy's Staves", "Setup", "Praedy's Staves", "" },
			{ "Praedy's Staves - Legacy of the Dragonborn", "Setup", "Legacy of the Dragonborn", "Needs Legacy of the Dragonborn (LegacyoftheDragonborn.esm)." },
			{ "Praedy's Staves - Spider control rod", "Forgotten Seasons", "Spider control rod", "Needs the Forgotten Seasons Creation (ccTWBSSE001-PuzzleDungeon.esm)." },
			{ "Praedy's Staves - Ayleid staff", "The Cause", "Ayleid staff", "Needs The Cause Creation (ccBGSSSE067-DaedInv.esm)." },
			{ "Praedy's Staves - Olivias staff", "3DNPC", "Olivias staff", "Needs Interesting NPCs (3DNPC.esp)." },
			{ "Praedy's Staves - Staff of Worms", "Undeath", "Staff of Worms", "Needs Undeath (Undeath.esp)." },
			{ "Praedy's Staves - Staff of Meteor Storms", "Extras", "Staff of Meteor Storms", "Needs Staff of Meteor Storms (RiverStaff.esp)." },
		};

		// its section's place on the page (a staff the table does not know goes last, under its own name)
		[[nodiscard]] std::size_t PraedyIndexOf(const Option& a_option)
		{
			const auto it = std::ranges::find(kPraedySections, std::string_view{ a_option.name }, &PraedySection::option);
			return static_cast<std::size_t>(it - std::ranges::begin(kPraedySections));
		}

		// The look (his ask: "a cool ui design ... not too crazy, just a subtle glowy vibe"): warm spell-light
		// amber on the menu's own dark - headings on a soft glow that fades to the right with a thin lit underline, gold
		// check marks and slider grips, frames a touch warmer when hovered. Scoped to our pages, so no other mod's changes.
		constexpr ImGuiMCP::ImVec4 kGold{ 1.0f, 0.86f, 0.55f, 1.0f };
		// every shown line goes through T(): Data\SKSE\Plugins\RELightSpellLightingNG\Translation.json (relightgen.py writes the English one)
		using Translation::T;

#ifdef FADE_DEBUG_PAGE
		// a printf line with three counts, for a widget that takes plain text (the testing DLL's status card)
		[[nodiscard]] std::string Fill3(const char* a_format, std::size_t a_a, std::size_t a_b, std::size_t a_c)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), a_format, a_a, a_b, a_c);
			return buf;
		}
#endif

		// his call: as Illuminated does - when CS Light is loaded, say which of its options light the same
		// things a second time (the option names are CS Light's own installer's, spelling included).
		// Looked up ONCE on the main thread when the pages are added (RegisterMenu), as Illuminated does - the menu draws on
		// its own thread and never touches the data handler (CodeRabbit, RELight #11)
		const char* gCSLight = nullptr;

		void FindCSLight()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (const auto* name : { "CS Light.esp", "CS Light.esl" }) {
				if (dh && (dh->LookupLoadedModByName(name) || dh->LookupLoadedLightModByName(name))) {
					gCSLight = name;
				}
			}
		}

		[[nodiscard]] const char* CSLightLoaded() { return gCSLight; }

		class GlowStyle
		{
		public:
			GlowStyle()
			{
				using namespace ImGuiMCP;
				PushStyleColor(ImGuiCol_FrameBg, ImVec4{ 0.12f, 0.10f, 0.08f, 0.75f });
				PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{ 0.30f, 0.21f, 0.10f, 0.75f });
				PushStyleColor(ImGuiCol_Separator, ImVec4{ 1.0f, 0.78f, 0.45f, 0.22f });
			}
			~GlowStyle() { ImGuiMCP::PopStyleColor(3); }
			GlowStyle(const GlowStyle&) = delete;
			GlowStyle& operator=(const GlowStyle&) = delete;

		private:
			MenuStyle::Page page;  // the shared accent, rounding and hovers
		};
		namespace Icon = MenuStyle::Icon;

		// a heading on a soft glow; a_icon 0 for none (the pages' category headings)
		void GlowHeading(const char* a_text, unsigned a_icon = 0)
		{
			using namespace ImGuiMCP;
			Spacing();
			auto*        dl = GetWindowDrawList();
			const ImVec2 at = GetCursorScreenPos();
			const float  w = GetContentRegionAvail().x;
			const float  h = GetTextLineHeight() + 6.0f;
			ImDrawListManager::AddRectFilledMultiColor(dl, at, ImVec2{ at.x + w, at.y + h }, IM_COL32(255, 186, 90, 46),
				IM_COL32(255, 186, 90, 0), IM_COL32(255, 186, 90, 0), IM_COL32(255, 186, 90, 46));
			ImDrawListManager::AddLine(dl, ImVec2{ at.x, at.y + h }, ImVec2{ at.x + w * 0.55f, at.y + h }, IM_COL32(255, 205, 120, 110), 1.0f);
			Dummy(ImVec2{ 0.0f, 3.0f });
			Text(" ");
			SameLine(0.0f, 0.0f);
			if (a_icon) {
				FontAwesome::PushSolid();
				TextColored(kGold, "%s", FontAwesome::UnicodeToUtf8(a_icon).c_str());
				FontAwesome::Pop();
				SameLine();
			}
			TextColored(kGold, "%s", T(a_text));
			Dummy(ImVec2{ 0.0f, 4.0f });
		}

		// the headings of Configs, in Illuminated's order: the spells' own switches, Creation Club, the single-mod patches (Other Mods -
		// his rule: author patches go last), then one group per author
		struct Heading
		{
			int         rank;
			std::string text;
		};

		[[nodiscard]] Heading ConfigsHeading(const Option& a_option)
		{
			if (!a_option.author.empty()) {
				return { 3, a_option.author };
			}
			if (a_option.name == "Creation Club") {
				return { 1, T("Creation Club") };
			}
			if (a_option.download == "Spells" || a_option.category == kWeaponsGroup) {
				return { 0, T("Spells and Effects") };
			}
			return { 2, T("Other Mods") };
		}

		// the switches of one page: every option with a switch that belongs there, once per id (a pack split across the downloads is
		// several options with one id: one row, one switch), under the headings above; Praedy's staves in the data's own order
		void DrawSwitches(Page a_page)
		{
			auto& opts = Options();
			struct Row
			{
				Heading     heading;
				std::size_t index;
			};
			std::vector<Row>                rows;
			std::unordered_set<std::string> seen;
			for (const auto i : OptionsInMenuOrder()) {
				const auto& o = opts[i];
				if (!o.switchable || PageOf(o) != a_page || !seen.insert(o.id).second) {
					continue;
				}
				Heading heading = a_page == Page::kConfigs ? ConfigsHeading(o) : Heading{ 0, T("Weapon Enchantments") };
				if (a_page == Page::kPraedys) {
					const auto at = PraedyIndexOf(o);
					heading = { static_cast<int>(at), at < std::size(kPraedySections) ? std::string{ kPraedySections[at].group } : o.name };
				}
				rows.push_back({ heading, i });
			}
			if (a_page == Page::kPraedys) {
				// a section's staves together, the sections in Illuminated's order, each staff where the data puts it
				std::ranges::stable_sort(rows, [](const Row& a_x, const Row& a_y) { return a_x.heading.rank < a_y.heading.rank; });
			} else {
				std::ranges::stable_sort(rows, [&opts](const Row& a_x, const Row& a_y) {
					return std::tie(a_x.heading.rank, a_x.heading.text, opts[a_x.index].name) < std::tie(a_y.heading.rank, a_y.heading.text, opts[a_y.index].name);
				});
			}
			// every file of a pack, counted once per draw (CodeRabbit deep scan, CWE-407: a scan of every option
			// for every row was quadratic in the number of installed data files)
			std::unordered_map<std::string_view, std::pair<std::size_t, std::size_t>> counts;
			for (const auto& m : opts) {
				if (m.switchable) {
					auto& c = counts[m.id];
					c.first += m.lit;
					c.second += m.heldOut;
				}
			}
			std::string shown;
			for (const auto& row : rows) {
				auto& o = opts[row.index];
				if (row.heading.text != shown) {
					shown = row.heading.text;
					GlowHeading(shown.c_str());  // GlowHeading translates
				}
				ImGuiMCP::PushID(static_cast<int>(row.index));
				const auto           at = a_page == Page::kPraedys ? PraedyIndexOf(o) : std::size(kPraedySections);
				const PraedySection* staff = at < std::size(kPraedySections) ? &kPraedySections[at] : nullptr;
				const std::string    label{ staff ? staff->label : std::string_view{ o.name } };
				bool                 on = o.on;
				if (ImGuiMCP::Checkbox(T(label.c_str()), &on)) {
					SetOptionOn(row.index, on);
					SaveSettings();
					RehandSoon();
				}
				if (staff && !staff->needs.empty()) {
					const std::string tip = std::string{ T(o.desc.c_str()) } + "\n" + T(std::string{ staff->needs }.c_str());
					MenuStyle::Tip(tip.c_str());
				} else if (!o.desc.empty()) {  // his call: a broad word on what each switch lights
					MenuStyle::Tip(T(o.desc.c_str()));
				}
				const auto [lit, heldOut] = counts[o.id];  // every file of a pack
				ImGuiMCP::SameLine();
				if (!o.on) {
					ImGuiMCP::TextDisabled(T("off - %zu held out"), heldOut);
				} else if (o.weapons && !WeaponLightsOn()) {
					ImGuiMCP::TextDisabled(T("Weapon lights off - %zu held out"), heldOut);
				} else {
					ImGuiMCP::TextDisabled(T("%zu lit"), lit);
				}
				ImGuiMCP::PopID();
			}
		}

		// one of the four art picks (Illuminated's Shock, Fire, Frost and Healing groups): the pick, and what the chosen one does. The
		// choices follow DataText::kPickNames (the game's own art first); a pick applies the next time the game starts
		void DrawArtPick(int a_group, const char* a_label, const std::vector<const char*>& a_items, const std::vector<const char*>& a_tips)
		{
			int pick = std::clamp(ArtPick(a_group), 0, static_cast<int>(a_items.size()) - 1);
			if (ImGuiMCP::Combo(a_label, &pick, a_items.data(), static_cast<int>(a_items.size()))) {
				SetArtPick(a_group, pick);
				SaveSettings();
			}
			MenuStyle::Tip(a_tips[static_cast<std::size_t>(pick)]);
		}

		// ------------------------------------------------------------------ the player's own presets, one set per lighting
		// HIS WORD (Illuminated's list, synced): "any presets users make need to be version dependant so they can have
		// presets per version if they switch to say, enb to community shaders mid save." Presets.ini beside the data files,
		// a section per preset "[<lighting>|<name>]"; the menu shows the presets of the lighting found now (HandLights.cpp).
		constexpr const char* kPresetsPath = "Data/SKSE/Plugins/RELightSpellLightingNG/Presets.ini";
		// up to 2.5 in the RelightSpellAddon folder: read while the new one is not there yet, never written (HIS ORDER 2026-10-20)
		constexpr const char* kOldPresetsPath = "Data/SKSE/Plugins/RelightSpellAddon/Presets.ini";
		struct UserPreset
		{
			std::string                              lighting, name;
			std::vector<std::pair<std::string, int>> values;
		};
		std::mutex              gPresetLock;
		std::vector<UserPreset> gPresets;  // read once when the pages are added (main thread), then under gPresetLock

		void ReadPresetsLocked()
		{
			gPresets.clear();
			std::ifstream in(Fade::SettingsText::ReadFrom(kPresetsPath, kOldPresetsPath));
			std::string   line;
			while (std::getline(in, line)) {
				std::string_view t = line;
				while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
					t.remove_suffix(1);
				}
				while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) {
					t.remove_prefix(1);
				}
				if (t.empty() || t.front() == ';') {
					continue;
				}
				if (t.front() == '[' && t.back() == ']') {
					const auto section = t.substr(1, t.size() - 2);
					const auto bar = section.find('|');
					if (bar != std::string_view::npos && bar + 1 < section.size()) {
						gPresets.push_back({ std::string(section.substr(0, bar)), std::string(section.substr(bar + 1)), {} });
					}
					continue;
				}
				const auto eq = t.find('=');
				int        v = 0;
				if (eq != std::string_view::npos && !gPresets.empty()) {
					const auto val = t.substr(eq + 1);
					if (const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), v); ec == std::errc{}) {
						gPresets.back().values.emplace_back(std::string(t.substr(0, eq)), v);
					}
				}
			}
		}

		// the caller holds gPresetLock; the file is written by an SKSE task on the main thread, from the copy it is handed
		// (Settings.cpp's rule: no file work on the render thread)
		void WritePresetsSoon()
		{
			std::ostringstream text;
			text << "; RELight - Spell Lighting NG - the presets you saved in the menu, one set per lighting. Written by the menu.\n";
			for (const auto& p : gPresets) {
				text << "\n[" << p.lighting << "|" << p.name << "]\n";
				for (const auto& [k, v] : p.values) {
					text << k << "=" << v << "\n";
				}
			}
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddTask([all = std::move(text).str()]() {
				std::error_code ec;
				std::filesystem::create_directories(std::filesystem::path(kPresetsPath).parent_path(), ec);
				const std::string tmp = std::string(kPresetsPath) + ".tmp";
				{
					std::ofstream out(tmp, std::ios::trunc);
					out << all;
					if (!out.flush()) {
						SKSE::log::warn("presets: {} could not be written", kPresetsPath);
						return;
					}
				}
				if (std::filesystem::rename(tmp, kPresetsPath, ec); ec) {
					SKSE::log::warn("presets: {} could not be replaced ({})", kPresetsPath, ec.message());
					std::filesystem::remove(tmp, ec);
				}
			});
		}

		// the Version heading's one line (Illuminated's DrawVersion): the lighting found, never picked (his word)
		void DrawVersion()
		{
			const std::string lighting = LightingName(LightingFound());
			ImGuiMCP::Text("%s", T("Lighting"));
			ImGuiMCP::SameLine();
			ImGuiMCP::TextColored(kGold, T("Automatic - %s"), T(lighting.c_str()));
			MenuStyle::Tip(T(
				"Found by itself when the game starts. Off Community Shaders the hand lights are drawn for the "
				"game's own lighting and light the ground. Your presets are kept apart for each lighting."));
		}

		void DrawUserPresets()
		{
			const std::string lighting = LightingName(LightingFound());
			static char       name[48]{};
			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::InputTextWithHint("##presetName", T("Name a preset"), name, sizeof(name));
			ImGuiMCP::SameLine();
			std::string typed = name;
			while (!typed.empty() && std::isspace(static_cast<unsigned char>(typed.back()))) {
				typed.pop_back();
			}
			ImGuiMCP::BeginDisabled(typed.empty() || typed.find_first_of("[]|=") != std::string::npos);
			if (ImGuiMCP::Button(T("Save as preset"))) {
				std::lock_guard lock(gPresetLock);
				std::erase_if(gPresets, [&](const UserPreset& p) { return p.lighting == lighting && p.name == typed; });
				gPresets.push_back({ lighting, typed, SettingsSnapshot() });
				WritePresetsSoon();
				name[0] = '\0';
			}
			ImGuiMCP::EndDisabled();
			const char* const saveFmt = T("Saves every setting and switch as it is now under this name, for %s.");
			const char* const lightingName = T(lighting.c_str());
			std::string       saveTip(static_cast<std::size_t>((std::max)(std::snprintf(nullptr, 0, saveFmt, lightingName), 0)) + 1, '\0');
			std::snprintf(saveTip.data(), saveTip.size(), saveFmt, lightingName);
			MenuStyle::Tip(saveTip.c_str());
			std::lock_guard lock(gPresetLock);
			for (std::size_t i = 0; i < gPresets.size(); ++i) {
				if (gPresets[i].lighting != lighting) {
					continue;
				}
				ImGuiMCP::PushID(static_cast<int>(2000 + i));
				if (ImGuiMCP::Button(gPresets[i].name.c_str())) {
					ApplySettingsSnapshot(gPresets[i].values);
					SaveSettings();
					RepresetSoon();
				}
				MenuStyle::Tip(T("Load this preset."));
				ImGuiMCP::SameLine();
				const bool gone = ImGuiMCP::SmallButton(T("Delete"));
				ImGuiMCP::PopID();
				if (gone) {
					gPresets.erase(gPresets.begin() + static_cast<std::ptrdiff_t>(i));
					WritePresetsSoon();
					break;
				}
			}
		}

		// Illuminated's Lights page; the lighting is found, never picked, and the art picks are on the Spell & Enchantment Art page
		void __stdcall RenderLights()
		{
			const GlowStyle style;
			GlowHeading("Version", Icon::kBulb);
			DrawVersion();
#ifdef FADE_DEBUG_PAGE
			// the status card: only in the TESTING DLL (his word: "only in testing dll")
			MenuStyle::Status(true, Fill3(T("%zu data file(s), %zu held weapon light(s) right now, %zu spell(s) with a hand light"), DataFiles(),
										LiveHeldLights(), HandEffects())
										.c_str());
#endif
			GlowHeading("Presets", Icon::kBulb);
			// Illuminated's presets, the same three (his word: "remove performance preset, only subtle default and cinematic"):
			// one click sets the two sliders, the light budget and Dim in daylight (-1 leaves one as it is); Default puts back all
			// four as the mod ships them, as its tip says. The player's own presets are kept per lighting
			struct Preset
			{
				const char* name;
				const char* tip;
				int         brightness, reach, hands, daylight;
			};
			const Preset presets[] = {
				{ T("Subtle"), T("Softer lights that stay close to the spell."), 75, 80, -1, -1 },
				{ T("Default"), T("The lights as the mod was made."), 100, 100, 0, 1 },  // Dim in daylight too, as its tip says (the audit)
				{ T("Cinematic"), T("Brighter lights that reach further."), 150, 120, -1, -1 },
			};
			for (std::size_t i = 0; i < std::size(presets); ++i) {
				const auto& p = presets[i];
				if (i) {
					ImGuiMCP::SameLine();
				}
				if (ImGuiMCP::Button(p.name)) {
					SetBrightnessPercent(p.brightness);
					SetReachPercent(p.reach);
					if (p.hands >= 0) {
						SetHandLightsFor(p.hands);
					}
					if (p.daylight >= 0) {
						SetDimInDaylight(p.daylight);
					}
					SaveSettings();
				}
				MenuStyle::Tip(p.tip);
			}
			DrawUserPresets();

			// 🚫🚫 no Light colors pick (Vanilla / Paler / Deeper): HIS WORDS 2026-10-21 "pale and deep also need to be removed" - the
			// second time he took it out (CLAUDE.md)

			GlowHeading("Light Strength", Icon::kBulb);
			int b = BrightnessPercent();
			if (ImGuiMCP::SliderInt(T("Brightness"), &b, 10, 200, "%d%%")) {
				SetBrightnessPercent(b);
			}
			if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"How bright this mod's lights are, and only this mod's. 100% is the measured brightness; about 30% "
				"is what the old Reduced download was. How far they carry does not change - that is the slider below."));

			int r = ReachPercent();
			if (ImGuiMCP::SliderInt(T("Reach"), &r, 50, 150, "%d%%")) {
				SetReachPercent(r);
			}
			if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"How far this mod's lights carry. 100% is the measured reach. How bright they are does not change - "
				"that is the slider above."));

			// Illuminated's light settings, ported (his "add everything from illuminated into relight ... sister
			// mods"); RE::Light lights the objects, these are what it has no setting for
			const char* const kDaylight[] = { T("Off"), T("A little"), T("More") };
			int               day = DimInDaylight();
			if (ImGuiMCP::Combo(T("Dim in daylight"), &day, kDaylight, 3)) {
				SetDimInDaylight(day);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"This mod's lights are dimmer outdoors by day and in brightly lit rooms, where a bright light looks out of place, and at "
				"full strength at night and in the dark. Follows the time of day as it passes."));

			GlowHeading("While Sneaking", Icon::kBulb);
			const char* const kSneaking[] = { T("Lights fade while sneaking"), T("Lights stay on while sneaking") };
			int               sneak = SneakOn() ? 0 : 1;
			if (ImGuiMCP::Combo(T("Sneaking"), &sneak, kSneaking, 2)) {
				SetSneakOn(sneak == 0);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"When you crouch, your spell lights fade down over half a second - hand lights, projectiles, runes, explosions, "
				"hazards, your weapon lights and the glow on your body - and fade back up when you stand."));
			if (SneakOn()) {
				int dim = SneakBrightness();
				if (ImGuiMCP::SliderInt(T("Sneaking brightness"), &dim, 0, 100, "%d%%")) {
					SetSneakBrightness(dim);
				}
				if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
					SaveSettings();
				}
				MenuStyle::Tip(T("How bright your lights stay while you sneak. At 0% they fade out completely."));
			}
			const char* const kInvisible[] = { T("Lights fade out"), T("Lights stay on") };
			int               hidden = InvisibleFadeOn() ? 0 : 1;
			if (ImGuiMCP::Combo(T("While invisible or in shadow"), &hidden, kInvisible, 2)) {
				SetInvisibleFadeOn(hidden == 0);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"Lights fade out: while you are invisible (the Shadow Stone, Invisibility, a potion or scroll), or while you have a sign "
				"or perk that only works in the dark (a Shadow birthsign), your lights fade out so they never give you away or cancel it, "
				"and come back after. Lights stay on: neither changes your lights."));

			// the old installer's Dynamic Lighting question, in Illuminated's place for it (its Flicker group)
			GlowHeading("Flicker", Icon::kBulb);
			const char* const kDynamic[] = { T("Off"), T("On") };
			int               dynamic = DynamicLighting() ? 1 : 0;
			if (ImGuiMCP::Combo(T("Dynamic Lighting"), &dynamic, kDynamic, 2)) {
				SetDynamicLighting(dynamic == 1);
				SaveSettings();
			}
			MenuStyle::Tip(T("Off: static lights. On: lights breathe and flicker like fire and lightning, and so do the rows of light along a spray or bolt. Applies the next time the game starts."));

			GlowHeading("Hands and Weapons", Icon::kBulb);
			bool hands = HandLightsOn();
			if (ImGuiMCP::Checkbox(T("Hand lights"), &hands)) {
				SetHandLightsOn(hands);
				SaveSettings();
				RehandSoon();
			}
			MenuStyle::Tip(T("A light on your hands while you cast, in the color of the spell. Changes reach a spell you are already holding."));

			// his call: "a toggle for weapon lights just like hand lights and right under it"
			bool weapons = WeaponLightsOn();
			if (ImGuiMCP::Checkbox(T("Weapon lights"), &weapons)) {
				SetWeaponLightsOn(weapons);
				SaveSettings();
				RehandSoon();
			}
			MenuStyle::Tip(T(
				"Every weapon light: enchanted weapons, bound weapons, artifacts and staves. Off puts them all out at "
				"once; the switches on the Spell & Enchantment Art page choose among them."));

			// Illuminated's "Sprays Bolts and Wards" heading: RELight's sprays and bolts are RE::Light's, its wards' color is ours
			GlowHeading("Sprays, Bolts and Wards", Icon::kShield);
			if (WardsSteppedDown()) {
				ImGuiMCP::TextDisabled("%s", T("Dynamic Wards is installed - it colors the wards, so this setting stands aside."));
			} else {
				const char* const kColors[] = { T("Vanilla blue"), T("White") };
				int               c = WardColor();
				if (ImGuiMCP::Combo(T("Ward color"), &c, kColors, 2)) {
					SetWardColor(c);
					SaveSettings();
					// the art forms and lights are changed on the game's main thread; the next cast shows it
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([]() {
							ApplyWards("the menu");
							RefindHandLights();
						});
					}
				}
				MenuStyle::Tip(T(
					"The ward's dome, the 360 Ward sphere and its flash, the art on your hand and the hand light, in one color. "
					"Vanilla blue keeps the vanilla dome and gives 360 Ward's sphere the vanilla blue. Shows on the next cast."));
			}

			// Illuminated's "Other Light Mods" heading: CS Light loaded is one warning line, its how-to in the hover text
			if (const auto* cs = CSLightLoaded()) {
				GlowHeading("Other Light Mods", Icon::kBulb);
				ImGuiMCP::TextColored(kGold, T("%s is loaded."), cs);
				MenuStyle::Tip(T("RELight - Spell Lighting NG does not need CS Light. Keeping it for its world lights? Untick its Magic FX, Mysticsm, Bound Weapons, Praedy Staves, Spiders, Misc Effects and Dwarven Spiders options, or those lights glow twice."));
			}
		}

		// 🔁 HIS ORDER 2026-10-20: "spells without a patch moved to lights by item/spell" - above the fading module's item list
		void __stdcall RenderItems()
		{
			{
				const GlowStyle style;
				GlowHeading("Spells Without a Patch", Icon::kBulb);
				bool autoOn = AutoLightsOn();
				if (ImGuiMCP::Checkbox(T("Light spells this mod has no patch for"), &autoOn)) {
					SetAutoLightsOn(autoOn);
					SaveSettings();
					RehandSoon();
				}
				MenuStyle::Tip(T(
					"RE::Light switches off a casting light it has no config for, so another mod's spell would be dark in the hand. On: it "
					"gets a hand light in the color of its own light, or of its element, at the strength of this mod's hand lights."));
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled(T("%zu lit"), AutoHandEffects());
			}
			Fade::Items::RenderSpells();
		}

		// 🔁 HIS ORDER 2026-10-20: "I want a quality/performance setting for relight and illuminated. quality has all features turned
		// on. performance has features that you decide have a lot of lights turned off. i want this added to performance, and
		// performance added to its own main menu as the last menu" (was: "big fights can be called performance and be at the
		// bottom of lights menu")
		void __stdcall RenderPerformance()
		{
			const GlowStyle style;
			GlowHeading("Performance", Icon::kBulb);
			const char* const kQuality[] = { T("Quality"), T("Performance") };
			int               quality = PerformanceMode() ? 1 : 0;
			if (ImGuiMCP::Combo(T("Quality"), &quality, kQuality, 2)) {
				SetPerformanceMode(quality == 1);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"Quality: every light this mod has. Performance: only you and your followers carry hand, weapon and body "
				"lights, and sprays and bolts keep only their own light (no row along them). For big fights and slower PCs."));
			const char* const kWho[] = { T("Everyone"), T("Everyone nearby"), T("Player and followers"), T("Player only") };
			int               who = HandLightsFor();
			if (ImGuiMCP::Combo(T("Hand lights for"), &who, kWho, 4)) {
				SetHandLightsFor(who);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"Which casters' hands carry a spell light. In a big fight fewer lights keep the game smooth. Everyone nearby leaves "
				"out casters farther than about forty paces from you. Reaches the next spell they ready."));
			// his list: "under performance add a toggle to turn off lights completely for the entire mod"
			bool allOff = AllLightsOff();
			if (ImGuiMCP::Checkbox(T("All RELight - Spell Lighting NG lights off"), &allOff)) {
				SetAllLightsOff(allOff);
				SaveSettings();
				RehandSoon();
			}
			MenuStyle::Tip(T(
				"This mod's lights go out - every switch's lights and the hand, weapon and ground lights (sprays and bolts keep their own "
				"light) - to compare, or when every frame counts. A spell already in your hand changes the next time you ready it. Off "
				"again brings them all back."));
		}

		// Illuminated's Configs page: the spells' own switches under "Spells and Effects" (Artifacts and Bound Weapons with them), Creation
		// Club, the single-mod patches (Other Mods), then each author's mods under the author's name
		void __stdcall RenderConfigs()
		{
			const GlowStyle style;
			DrawSwitches(Page::kConfigs);
		}

		// Illuminated puts Praedy's Staves on a page of their own, right below Configs
		void __stdcall RenderPraedys()
		{
			const GlowStyle style;
			if (!WeaponLightsOn()) {
				MenuStyle::Status(false, T("Weapon lights is off on the Lights page, so every light here is out."));
			}
			DrawSwitches(Page::kPraedys);
		}

		// Illuminated's Spell & Enchantment Art page: the weapon enchantment mods first, then the art mod you run for each element
		void __stdcall RenderArt()
		{
			const GlowStyle style;
			if (!WeaponLightsOn()) {
				MenuStyle::Status(false, T("Weapon lights is off on the Lights page, so every light here is out."));
			}
			DrawSwitches(Page::kArt);
			GlowHeading("Shock");
			DrawArtPick(2, T("Shock Art"), { T("Vanilla shock"), T("Kittytail Lightning"), T("Voltage") },
				{ T("Skyrim's own blue-violet. Applies the next time the game starts."),
					T("A deeper, more saturated blue.\nInstall first: Kittytail's Lightning VFX Edit.\nWrong pick and shock lights the wrong color. Nothing else changes. Applies the next time the game starts."),
					T("A lighter, cyan-shifted blue.\nInstall first: Voltage.\nWrong pick and shock lights the wrong color. Nothing else changes. Applies the next time the game starts.") });
			GlowHeading("Fire");
			DrawArtPick(0, T("Fire Art"), { T("Vanilla fire"), T("Inferno"), T("Embers XD"), T("Kittytail Flame") },
				{ T("Skyrim's orange. Applies the next time the game starts."),
					T("A hotter, more yellow fire light.\nInstall first: Inferno - Fire Effects Redux.\nWrong pick and fire lights the wrong color. Nothing else changes. Applies the next time the game starts."),
					T("A deeper amber fire light.\nInstall first: Embers XD.\nWrong pick and fire lights the wrong color. Nothing else changes. Applies the next time the game starts."),
					T("A deeper, redder fire light.\nInstall first: Kittytail's Flame VFX Edit.\nWrong pick and fire lights the wrong color. Nothing else changes. Applies the next time the game starts.") });
			GlowHeading("Frost");
			DrawArtPick(1, T("Frost Art"), { T("Vanilla / Arctic"), T("Kittytail") },
				{ T("Skyrim's pale blue. Applies the next time the game starts."),
					T("A deeper, more saturated blue.\nInstall first: Kittytail's Frost VFX Edit.\nWrong pick and frost lights the wrong color. Nothing else changes. Applies the next time the game starts.") });
			GlowHeading("Healing");
			DrawArtPick(3, T("Healing Art"), { T("Vanilla healing"), T("Kittytail healing") },
				{ T("Healing hands keep the light Skyrim gives them. Applies the next time the game starts."),
					T("The cooler blue healing light from Kittytail's pack.\nInstall first: Kittytail's healing VFX edit. Applies the next time the game starts.") });
		}

#ifdef FADE_DEBUG_PAGE
		// THE DIAGNOSTICS PAGE - only in the TESTING DLL (his word: "only in testing dll"; built with FADE_DEBUG_PAGE, shipped in
		// the PDB and Source download): what the plugin found and made, for a bug report
		void __stdcall RenderDiagnostics()
		{
			const GlowStyle style;
			GlowHeading("Found", Icon::kBulb);
			ImGuiMCP::Text(T("Lighting: %s (the inverse square shader: %s)"), T(LightingName(LightingFound())), InverseSquare() ? T("yes") : T("no"));
			const auto* cs = CSLightLoaded();
			ImGuiMCP::Text(T("CS Light: %s"), cs ? cs : T("not loaded"));
			ImGuiMCP::Text(T("Data files: %zu, options: %zu, hand keys: %zu, spray light records: %zu"), DataFiles(), Options().size(), Hands().size(),
				SprayLightRecords().size());
			GlowHeading("Made", Icon::kBulb);
			ImGuiMCP::Text(T("Hand lights made: %zu, for %zu spell(s); %zu of them by an automatic light"), HandLightsMade(), HandEffects(), AutoHandEffects());
			ImGuiMCP::Text(T("Held weapon lights now: %zu; ground lights now: %zu; game lights kept: %zu"), LiveHeldLights(), GroundLightCount(), KeptLights());
			GlowHeading("Hands and the log", Icon::kBulb);
			ImGuiMCP::TextWrapped("%s", T("Each hand's lights right now and the debug log switch are on the Fading - debug page."));
		}
#endif
	}

	void RegisterMenu()
	{
		FindCSLight();
		// the old folder's file read only while the new one is not there (CodeRabbit on #29; never written)
		SKSE::log::info("{}", Translation::Load(Fade::SettingsText::ReadFrom("Data/SKSE/Plugins/RELightSpellLightingNG/Translation.json",
								  "Data/SKSE/Plugins/RelightSpellAddon/Translation.json")));
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed, so there is no settings page; the settings file still applies");
			return;
		}
		{
			std::lock_guard lock(gPresetLock);
			ReadPresetsLocked();  // here on the main thread, so the menu never reads a file on its own thread
		}
		MenuStyle::gTheme = MenuStyle::MakeTheme(0xFFD58A);  // warm spell-light gold
		SKSEMenuFramework::SetSection(T("RELight - Spell Lighting NG"));
		// Illuminated's order (Praedy's Staves right below Configs); the fading module adds HUD and Fading after these, then
		// RegisterMenuTail adds Lights by Item and Performance
		SKSEMenuFramework::AddSectionItem(T("Lights"), RenderLights);
		SKSEMenuFramework::AddSectionItem(T("Spell & Enchantment Art"), RenderArt);
		SKSEMenuFramework::AddSectionItem(T("Configs"), RenderConfigs);
		SKSEMenuFramework::AddSectionItem(T("Praedy's Staves"), RenderPraedys);
		SKSE::log::info("settings pages added to SKSE Menu Framework {}", SKSEMenuFramework::GetMenuFrameworkVersion());
	}

	void RegisterMenuTail()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			return;
		}
		SKSEMenuFramework::SetSection(T("RELight - Spell Lighting NG"));
		// after the fading module's HUD and Fading: Lights by Item (FadeItems.cpp). A '/' in a name makes a submenu: it holds
		// Spell (Spells Without a Patch above the spell list) and the weapons' page
		static const std::string kSpellPage = std::string(T("Lights by Item")) + "/" + T("Spell");
		static const std::string kWeaponPage = std::string(T("Lights by Item")) + "/" + T("Weapons and Staves");
		SKSEMenuFramework::AddSectionItem(kSpellPage.c_str(), RenderItems);
		SKSEMenuFramework::AddSectionItem(kWeaponPage.c_str(), Fade::Items::RenderWeapons);
		SKSEMenuFramework::AddSectionItem(T("Performance"), RenderPerformance);  // the last page (his order 2026-10-20)
#ifdef FADE_DEBUG_PAGE
		SKSEMenuFramework::AddSectionItem(T("Diagnostics"), RenderDiagnostics);
#endif
	}
}
