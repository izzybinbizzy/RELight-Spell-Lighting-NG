// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Pages in SKSE Menu Framework's Mod Control Panel, under their own section so nothing of RE::Light's own menu is touched, laid out
// as Illuminated's: Lights (the lighting found, presets, Brightness, Reach, Flicker, hand and weapon lights,
// the ward color), Choose Your Own Color (the four art picks, automatic lights, the item list), Configs (the weapon enchantment
// mods, the spells' own switches, Creation Club, then the other mods' groups) and Praedy's Staves. The fading module adds HUD and
// Fading after them, then this file Performance, the last page. Every change is saved at once (Settings.cpp) and reaches lights
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
				tasks->AddTask([]() { LightKit::Guarded("menu hand lights", []() { ApplyHandLights(true); }); });
			}
		}

		// a preset may change the ward color too: the wards and the hand lights take the new settings together (CodeRabbit,
		// RELight #8)
		void RepresetSoon()
		{
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([]() {
					LightKit::Guarded("menu preset", []() {
						ApplyWards("a preset");
						RefindHandLights();
						ApplyHandLights(true);
					});
				});
			}
		}

		constexpr std::string_view kEnchantments = "Weapon Enchantments";  // the menugroup of VAER Reborn and Vibrant Weapons EAE
		constexpr std::string_view kPraedys = "Praedy's Staves";           // the menugroup of Praedy's seven staves options
		constexpr std::string_view kWeaponsGroup = "Weapons";              // the menugroup of Artifacts and Bound Weapons

		// where an option's switch is drawn, as Illuminated's pages are: the weapon enchantment mods first on Configs, Praedy's staves
		// on a page of their own, everything else after them on Configs
		enum class Page
		{
			kEnchantments,
			kConfigs,
			kPraedys
		};

		[[nodiscard]] Page PageOf(const Option& a_option)
		{
			return a_option.category == kEnchantments ? Page::kEnchantments : a_option.category == kPraedys ? Page::kPraedys :
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
			{ "Praedy's Staves - Olivias staff", "3DNPC", "Olivias staff", "Needs Interesting NPCs (3DNPC.esp)." },
			{ "Praedy's Staves - Staff of Meteor Storms", "Extras", "Staff of Meteor Storms", "Needs Staff of Meteor Storms (RiverStaff.esp)." },
			{ "Praedy's Staves - Spider control rod", "Forgotten Seasons", "Spider control rod", "Needs the Forgotten Seasons Creation (ccTWBSSE001-PuzzleDungeon.esm)." },
			{ "Praedy's Staves - Ayleid staff", "The Cause", "Ayleid staff", "Needs The Cause Creation (ccBGSSSE067-DaedInv.esm)." },
			{ "Praedy's Staves - Staff of Worms", "Undeath", "Staff of Worms", "Needs Undeath (Undeath.esp)." },
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

		// the headings of Configs, in Illuminated's order: the spells' own switches, Creation Club, one group per author A-Z, then the
		// single-mod patches (Other Mods)
		struct Heading
		{
			int         rank;
			std::string text;
		};

		[[nodiscard]] Heading ConfigsHeading(const Option& a_option)
		{
			if (!a_option.author.empty()) {
				return { 2, a_option.author };
			}
			if (a_option.name == "Creation Club") {
				return { 1, T("Creation Club") };
			}
			if (a_option.download == "Spells" || a_option.category == kWeaponsGroup) {
				return { 0, T("Spells and Effects") };
			}
			return { 3, T("Other Mods") };
		}

		bool gFecLoaded = false;  // read once at RegisterMenu (the main thread): the menu never asks the data handler
		void DrawFrozenPick();
		void DrawWardPick(bool a_beside);
		void DrawStrength(Strength a_kind, bool a_beside);

		// the switch each kind's strength sits beside (bd.5: a setting's own control on its line, to its right)
		[[nodiscard]] Strength StrengthBeside(std::string_view a_option)
		{
			if (a_option == "Explosions") {
				return Strength::kExplosions;
			}
			if (a_option == "Projectile Lights") {
				return Strength::kStreams;
			}
			return a_option == "Cloaks" ? Strength::kGlows : Strength::kNone;
		}

		// the switches of one page: every option with a switch that belongs there, once per id (a pack split across the downloads is
		// several options with one id: one row, one switch), under the headings above; Praedy's staves in the data's own order
		void DrawSwitches(Page a_page, std::unordered_set<std::string>& a_seen, const char* a_open = "")
		{
			auto& opts = Options();
			struct Row
			{
				Heading     heading;
				std::size_t index;
				std::string key;  // A-Z without regard to case
			};
			std::vector<Row> rows;
			for (const auto i : OptionsInMenuOrder()) {
				const auto& o = opts[i];
				if (!o.switchable || PageOf(o) != a_page || !a_seen.insert(o.id).second) {
					continue;
				}
				Heading heading = a_page == Page::kConfigs ? ConfigsHeading(o) : Heading{ 0, T("Weapon Enchantments") };
				if (a_page == Page::kPraedys) {
					const auto at = PraedyIndexOf(o);
					heading = { static_cast<int>(at), at < std::size(kPraedySections) ? std::string{ kPraedySections[at].group } : o.name };
				}
				rows.push_back({ heading, i, LightKit::Lower(heading.text) + "\n" + LightKit::Lower(o.name) });
			}
			if (a_page == Page::kPraedys) {
				// a section's staves together, the sections in Illuminated's order, each staff where the data puts it
				std::ranges::stable_sort(rows, [](const Row& a_x, const Row& a_y) { return a_x.heading.rank < a_y.heading.rank; });
			} else {
				std::ranges::stable_sort(rows, [](const Row& a_x, const Row& a_y) { return std::tie(a_x.heading.rank, a_x.key) < std::tie(a_y.heading.rank, a_y.key); });
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
			std::string shown{ a_open };  // a heading the page already drew
			bool        wardShown = false;
			std::array  strengthShown{ false, false, false };  // explosions, streams, glows
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
				if (a_page == Page::kConfigs && gFecLoaded && o.name == "Frozen Electrocuted Combustion") {
					DrawFrozenPick();
				}
				if (a_page == Page::kConfigs && o.name == "Wards") {
					DrawWardPick(true);
					wardShown = true;
				}
				if (const auto kind = a_page == Page::kConfigs ? StrengthBeside(o.name) : Strength::kNone; kind != Strength::kNone) {
					DrawStrength(kind, true);
					strengthShown[static_cast<std::size_t>(kind) - 1] = true;
				}
				ImGuiMCP::PopID();
			}
			// a kind whose switch is not there (its option not installed) still applies, so its strength keeps a heading of its own
			if (a_page == Page::kConfigs && std::ranges::find(strengthShown, false) != strengthShown.end()) {
				GlowHeading("Strength");
				for (std::size_t i = 0; i < strengthShown.size(); ++i) {
					if (!strengthShown[i]) {
						ImGuiMCP::PushID(static_cast<int>(1000 + i));
						DrawStrength(static_cast<Strength>(i + 1), false);
						ImGuiMCP::PopID();
					}
				}
			}
			// no Wards row to sit beside: the color still applies, so it keeps a heading of its own
			if (a_page == Page::kConfigs && !wardShown) {
				GlowHeading("Wards");
				DrawWardPick(false);
			}
		}

		// a dropdown whose every choice carries its own tip on its line of the open list (Illuminated's ChoiceCombo)
		bool ChoiceCombo(const char* a_label, int& a_value, const std::vector<const char*>& a_items, const std::vector<const char*>& a_tips)
		{
			bool       changed = false;
			const auto n = static_cast<int>(a_items.size());
			if (ImGuiMCP::BeginCombo(a_label, a_value >= 0 && a_value < n ? a_items[a_value] : "")) {
				for (int i = 0; i < n; ++i) {
					const bool selected = i == a_value;
					if (ImGuiMCP::Selectable(a_items[i], selected)) {
						a_value = i;
						changed = true;
					}
					if (i < static_cast<int>(a_tips.size()) && a_tips[i] && *a_tips[i]) {
						MenuStyle::Tip(a_tips[i]);
					}
					if (selected) {
						ImGuiMCP::SetItemDefaultFocus();
					}
				}
				ImGuiMCP::EndCombo();
			}
			return changed;
		}

		// the FEC picks, drawn beside FEC's own switch on the Configs page (Illuminated's "follows")
		void DrawFrozenPick()
		{
			ImGuiMCP::SameLine(0.0f, 24.0f);
			ImGuiMCP::SetNextItemWidth(220.0f);
			const std::vector<const char*> items{ T("Glow fades out"), T("Glow stays while frozen") };
			const float                    column = ImGuiMCP::GetCursorPosX();
			const std::vector<const char*> tips{ T("A body frozen solid glows blue for a few seconds, then the glow fades out."),
				T("The glow stays for as long as the body is frozen.") };
			int                            frozen = FecFrozenFadesOut() ? 0 : 1;
			if (ChoiceCombo(T("Frozen bodies"), frozen, items, tips)) {
				SetFecFrozenFadesOut(frozen == 0);
				SaveSettings();
			}
			MenuStyle::Tip(tips[static_cast<std::size_t>(frozen)]);
			// under Frozen bodies, in its column: a fourth item on the FEC row falls off a narrow window
			ImGuiMCP::SetCursorPosX(column);
			ImGuiMCP::SetNextItemWidth(220.0f);
			const std::vector<const char*> burnItems{ T("Glow fades out"), T("Glow stays while burning") };
			const std::vector<const char*> burnTips{ T("A burned body glows orange while it burns; the glow goes out with its flames, or a few seconds in if they never end."),
				T("The glow stays for as long as the body burns.") };
			int                            burning = FecBurningFadesOut() ? 0 : 1;
			if (ChoiceCombo(T("Burning bodies"), burning, burnItems, burnTips)) {
				SetFecBurningFadesOut(burning == 0);
				SaveSettings();
			}
			MenuStyle::Tip(burnTips[static_cast<std::size_t>(burning)]);
		}

		// the ward color, drawn beside the Wards switch on the Configs page (Illuminated's Ward look sits there)
		void DrawWardPick(bool a_beside)
		{
			if (a_beside) {
				ImGuiMCP::SameLine(0.0f, 24.0f);
			}
			if (WardsSteppedDown()) {
				ImGuiMCP::TextDisabled("%s", T("Dynamic Wards colors the wards"));
				MenuStyle::Tip(T("Dynamic Wards is installed - it colors the wards, so the ward color stands aside."));
				return;
			}
			if (Other360Color()) {
				ImGuiMCP::TextDisabled("%s", T("A recolored 360 Ward sphere colors the ward"));
				MenuStyle::Tip(T("Another mod recolors 360 Ward's sphere, so the ward's light takes that color."));
				return;
			}
			ImGuiMCP::SetNextItemWidth(220.0f);
			const std::vector<const char*> kColors{ T("Vanilla blue"), T("White") };
			const std::vector<const char*> kColorTips{ T("The ward, its art and your hand light in the game's own blue."),
				T("The ward, its art and your hand light in white.") };
			int                            c = WardColor();
			if (ChoiceCombo(T("Ward color"), c, kColors, kColorTips)) {
				SetWardColor(c);
				SaveSettings();
				// the art forms and lights are changed on the game's main thread; the next cast shows it
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([]() {
						LightKit::Guarded("menu ward color", []() {
							ApplyWards("the menu");
							RefindHandLights();
						});
					});
				}
			}
			MenuStyle::Tip(T(
				"One color for the ward's dome, 360 Ward's sphere and flash, your hand art and hand light. Shows on the next cast."));
		}

		// a kind's strength on top of Brightness, 25 to 200 % (Settings.cpp); beside its switch, or on a line of its own when there is none
		void DrawStrength(Strength a_kind, bool a_beside)
		{
			struct Look
			{
				Strength    kind;
				const char* alone;  // the label when no switch stands beside it
				const char* tip;
			};
			static constexpr Look kLooks[]{
				{ Strength::kExplosions, "Explosion strength", "How bright explosions are, on top of Brightness. 100% is as I set them." },
				{ Strength::kStreams, "Stream and spray strength", "How bright the lights along streams, sprays and bolts are, on top of Brightness. 100% is as I set them." },
				{ Strength::kGlows, "Glow strength", "How bright glows on people are (cloaks, flesh spells, FEC bodies), on top of Brightness." },
				{ Strength::kEnchanted, "Enchanted weapon strength", "How bright enchanted weapons' lights are, on top of Brightness. Bound weapons, staves and artifacts use Brightness only." },
			};
			const auto* look = std::ranges::find(kLooks, a_kind, &Look::kind);
			if (look == std::end(kLooks)) {
				return;
			}
			if (a_beside) {
				ImGuiMCP::SameLine(0.0f, 24.0f);
			}
			ImGuiMCP::SetNextItemWidth(180.0f);
			int v = StrengthPercent(a_kind);
			if (ImGuiMCP::SliderInt(T(a_beside ? "Strength" : look->alone), &v, 25, 200, "%d%%")) {
				SetStrengthPercent(a_kind, v);
				SaveSettings();
			}
			MenuStyle::Tip(T(look->tip));
		}

		// one of the four art picks (Illuminated's Shock, Fire, Frost and Healing groups): the pick, and what the chosen one does. The
		// choices follow DataText::kPickNames (the game's own art first); its colors change at once, the lights only that art
		// brings at the next start (Data.cpp)
		void DrawArtPick(int a_group, const char* a_label, const std::vector<const char*>& a_items, const std::vector<const char*>& a_tips)
		{
			int pick = std::clamp(ArtPick(a_group), 0, static_cast<int>(a_items.size()) - 1);
			if (ChoiceCombo(a_label, pick, a_items, a_tips)) {
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
			for (bool first = true; std::getline(in, line); first = false) {
				if (first && line.starts_with("\xEF\xBB\xBF")) {
					line.erase(0, 3);  // a file saved "UTF-8 with BOM" would lose its first section
				}
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

		// beside itself, then moved over (main thread)
		void WritePresetsFile(const std::string& a_all)
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(kPresetsPath).parent_path(), ec);
			const std::string tmp = std::string(kPresetsPath) + ".tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				out << a_all;
				if (!out.flush()) {
					SKSE::log::warn("presets: {} could not be written", kPresetsPath);
					return;
				}
			}
			if (std::filesystem::rename(tmp, kPresetsPath, ec); ec) {
				SKSE::log::warn("presets: {} could not be replaced ({})", kPresetsPath, ec.message());
				std::filesystem::remove(tmp, ec);
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
			tasks->AddTask([all = std::move(text).str()]() { LightKit::Guarded("presets save", [&all]() { WritePresetsFile(all); }); });
		}

		// the Version heading's one line (Illuminated's DrawVersion): the lighting found, never picked (his word)
		void DrawVersion()
		{
			const std::string lighting = LightingName(LightingFound());
			ImGuiMCP::Text("%s", T("Lighting"));
			ImGuiMCP::SameLine();
			ImGuiMCP::TextColored(kGold, T("Automatic - %s"), T(lighting.c_str()));
			MenuStyle::Tip(T(
				"Found by itself when the game starts. Your presets are kept apart for each lighting."));
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

		// Illuminated's Lights page; the lighting is found, never picked, and the art picks are on the Choose Your Own Color page
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
			// one click sets the two sliders and the light budget (-1 leaves it as it is); Default puts back all
			// three as the mod ships them, as its tip says. The player's own presets are kept per lighting
			struct Preset
			{
				const char* name;
				const char* tip;
				int         brightness, reach, hands;
			};
			const Preset presets[] = {
				{ T("Subtle"), T("Softer lights that stay close to the spell."), 75, 80, -1 },
				{ T("Default"), T("My lights as I made them."), 100, 100, 0 },
				{ T("Cinematic"), T("Brighter lights that reach further."), 150, 120, -1 },
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
					// Default is the mod as it ships: the four strengths back to 100 % too
					if (p.hands == 0) {
						for (const auto kind : { Strength::kExplosions, Strength::kStreams, Strength::kGlows, Strength::kEnchanted }) {
							SetStrengthPercent(kind, 100);
						}
					}
					SaveSettings();
				}
				MenuStyle::Tip(p.tip);
			}
			DrawUserPresets();

			// 🚫🚫 no Light colors pick (Vanilla / Paler / Deeper): HIS WORDS 2026-10-21 "pale and deep also need to be removed" - the
			// second time he took it out (CLAUDE.md)

			// the old installer's Dynamic Lighting question, in Illuminated's place for it (its Flicker group)
			GlowHeading("Flicker", Icon::kBulb);
			const std::vector<const char*> kDynamic{ T("Off"), T("Regular"), T("Cinematic") };
			const std::vector<const char*> kDynamicTips{ T("Steady lights, no pulse or flicker. Applies the next time the game starts."),
				T("Lights pulse as they always have. From Off, it applies at the next game start."),
				T("Each kind of spell pulses its own way: fire flickers, shock snaps, healing breathes. From Off, applies at the next start.") };
			int                            dynamic = !DynamicLighting() ? 0 : PulseShapes() ? 2 :
			                                                                                  1;
			if (ChoiceCombo(T("Dynamic Lighting"), dynamic, kDynamic, kDynamicTips)) {
				SetDynamicLighting(dynamic != 0);
				SetPulseShapes(dynamic == 2);
				SaveSettings();
			}
			MenuStyle::Tip(kDynamicTips[static_cast<std::size_t>(dynamic)]);

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
				"Every weapon light: enchanted and bound weapons, artifacts and staves. The Configs page switches choose among them."));

			GlowHeading("Light Strength", Icon::kBulb);
			int b = BrightnessPercent();
			if (ImGuiMCP::SliderInt(T("Brightness"), &b, 10, 200, "%d%%")) {
				SetBrightnessPercent(b);
			}
			if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"How bright my lights are. 100% is the measured brightness; Reach sets how far they carry."));

			int r = ReachPercent();
			if (ImGuiMCP::SliderInt(T("Reach"), &r, 50, 150, "%d%%")) {
				SetReachPercent(r);
			}
			if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"How far my lights carry. 100% is the measured reach; Brightness sets how bright."));

			// Illuminated's "Other Light Mods" heading: CS Light loaded is one warning line, its how-to in the hover text
			if (const auto* cs = CSLightLoaded()) {
				GlowHeading("Other Light Mods", Icon::kBulb);
				ImGuiMCP::TextColored(kGold, T("%s is loaded."), cs);
				MenuStyle::Tip(T("You don't need CS Light. If you keep it, untick its spell options or those lights glow twice."));
			}
		}

		void DrawArtPicks()
		{
			GlowHeading("Fire");
			DrawArtPick(0, T("Fire Art"), { T("Vanilla fire"), T("Inferno"), T("Embers XD"), T("Kittytail Flame") },
				{ T("Skyrim's orange."),
					T("A hotter, more yellow fire light. Needs Inferno - Fire Effects Redux."),
					T("A deeper amber fire light. Needs Embers XD."),
					T("A deeper, redder fire light. Needs Kittytail's Flame VFX Edit.") });
			GlowHeading("Frost");
			DrawArtPick(1, T("Frost Art"), { T("Vanilla / Arctic"), T("Kittytail") },
				{ T("Skyrim's pale blue."),
					T("A deeper, more saturated blue. Needs Kittytail's Frost VFX Edit.") });
			GlowHeading("Healing");
			DrawArtPick(3, T("Healing Art"), { T("Vanilla healing"), T("Kittytail healing") },
				{ T("Healing hands keep the light Skyrim gives them."),
					T("The cooler blue healing light from Kittytail's pack. Needs Kittytail's healing VFX edit.") });
			GlowHeading("Shock");
			DrawArtPick(2, T("Shock Art"), { T("Vanilla shock"), T("Kittytail Lightning"), T("Voltage"), T("Near Voltage") },
				{ T("Skyrim's own blue-violet."),
					T("A deeper, more saturated blue. Needs Kittytail's Lightning VFX Edit."),
					T("A lighter, cyan-shifted blue. Needs Voltage."),
					T("A violet purple. Needs Voltage and Near Voltage - Purple Edition.") });
		}

		// Spells Without a Patch and the art picks above the fading module's item list
		void __stdcall RenderItems()
		{
			{
				const GlowStyle style;
				DrawArtPicks();
				GlowHeading("Spells Without a Patch", Icon::kBulb);
				bool autoOn = AutoLightsOn();
				if (ImGuiMCP::Checkbox(T("Light spells I have no patch for"), &autoOn)) {
					SetAutoLightsOn(autoOn);
					SaveSettings();
					RehandSoon();
				}
				MenuStyle::Tip(T(
					"Gives spells from mods with no patch a hand light in their own color, so they are not dark in the hand."));
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled(T("%zu lit"), AutoHandEffects());
			}
			Fade::Items::Render();
		}

		// 🔁 HIS ORDER 2026-10-20: "I want a quality/performance setting for relight and illuminated. quality has all features turned
		// on. performance has features that you decide have a lot of lights turned off. i want this added to performance, and
		// performance added to its own main menu as the last menu" (was: "big fights can be called performance and be at the
		// bottom of lights menu")
		void __stdcall RenderPerformance()
		{
			const GlowStyle style;
			GlowHeading("Performance", Icon::kBulb);
			const std::vector<const char*> kQuality{ T("Quality"), T("Performance") };
			const std::vector<const char*> kQualityTips{ T("All my lights; Hand lights for still picks whose hands carry one."),
				T("Only you and your followers carry hand, weapon and body lights; sprays and bolts keep only their own light.") };
			int                            quality = PerformanceMode() ? 1 : 0;
			if (ChoiceCombo(T("Quality"), quality, kQuality, kQualityTips)) {
				SetPerformanceMode(quality == 1);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"Performance: lights for you and your followers only, and no rows along sprays and bolts. For big fights."));
			const std::vector<const char*> kWho{ T("Everyone"), T("Everyone nearby"), T("Player and followers"), T("Player only") };
			const std::vector<const char*> kWhoTips{ T("Every caster's hands carry a spell light (with Quality on Performance, you and your followers only)."),
				T("Casters carry one only while they are within about forty paces of you."), T("Only you and your followers carry one."),
				T("Only your own hands carry one.") };
			int                            who = HandLightsFor();
			if (ChoiceCombo(T("Hand lights for"), who, kWho, kWhoTips)) {
				SetHandLightsFor(who);
				SaveSettings();
			}
			MenuStyle::Tip(T(
				"Which casters' hands carry a spell light. Fewer lights keep big fights smooth."));
			// his list: "under performance add a toggle to turn off lights completely for the entire mod"
			bool allOff = AllLightsOff();
			if (ImGuiMCP::Checkbox(T("All my lights off"), &allOff)) {
				SetAllLightsOff(allOff);
				SaveSettings();
				RehandSoon();
			}
			MenuStyle::Tip(T(
				"Turns all my lights off, to compare or when every frame counts. Off again brings them back."));
		}

		// Illuminated's Configs page: the spells' own switches under "Spells and Effects" (Artifacts and Bound Weapons with them), Creation
		// Club, the single-mod patches (Other Mods), then each author's mods under the author's name
		void __stdcall RenderConfigs()
		{
			const GlowStyle style;
			if (!WeaponLightsOn()) {
				MenuStyle::Status(false, T("Weapon lights is off on the Lights page, so weapon lights here are out."));
			}
			GlowHeading("Weapon Enchantments");
			bool enchanted = EnchantedWeaponsOn();
			if (ImGuiMCP::Checkbox(T("Enchanted weapons"), &enchanted)) {
				SetEnchantedWeaponsOn(enchanted);
				SaveSettings();
			}
			MenuStyle::Tip(T("The light of enchanted weapons. Off puts only these out: bound weapons, staves and artifacts stay lit."));
			ImGuiMCP::PushID("enchanted strength");
			DrawStrength(Strength::kEnchanted, true);
			ImGuiMCP::PopID();
			// one row per id across both draws
			std::unordered_set<std::string> seen;
			DrawSwitches(Page::kEnchantments, seen, T("Weapon Enchantments"));
			DrawSwitches(Page::kConfigs, seen);
		}

		// Illuminated puts Praedy's Staves on a page of their own, right below Configs
		void __stdcall RenderPraedys()
		{
			const GlowStyle style;
			if (!WeaponLightsOn()) {
				MenuStyle::Status(false, T("Weapon lights is off on the Lights page, so every light here is out."));
			}
			std::unordered_set<std::string> seen;
			DrawSwitches(Page::kPraedys, seen);
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
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			gFecLoaded = data->LookupLoadedModByName("FEC.esp") || data->LookupLoadedLightModByName("FEC.esp");
		}
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
		// Illuminated's order (Choose Your Own Color right below Lights, Praedy's Staves right below Configs); the fading module adds
		// HUD and Fading after these, then RegisterMenuTail adds Performance
		SKSEMenuFramework::AddSectionItem(T("Lights"), LightKit::GuardedDraw<RenderLights>);
		SKSEMenuFramework::AddSectionItem(T("Choose Your Own Color"), LightKit::GuardedDraw<RenderItems>);
		SKSEMenuFramework::AddSectionItem(T("Configs"), LightKit::GuardedDraw<RenderConfigs>);
		SKSEMenuFramework::AddSectionItem(T("Praedy's Staves"), LightKit::GuardedDraw<RenderPraedys>);
		SKSE::log::info("settings pages added to SKSE Menu Framework {}", SKSEMenuFramework::GetMenuFrameworkVersion());
	}

	void RegisterMenuTail()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			return;
		}
		SKSEMenuFramework::SetSection(T("RELight - Spell Lighting NG"));
		// after the fading module's HUD and Fading
		SKSEMenuFramework::AddSectionItem(T("Performance"), LightKit::GuardedDraw<RenderPerformance>);  // the last page
#ifdef FADE_DEBUG_PAGE
		SKSEMenuFramework::AddSectionItem(T("Diagnostics"), LightKit::GuardedDraw<RenderDiagnostics>);
#endif
	}
}
