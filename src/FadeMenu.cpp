// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Pages in SKSE Menu Framework's Mod Control Panel, under the mod's own section, in his order: HUD (the charge bar and the
// charge gems) right below the mod's Configs page, then Fading - his answer 2026-10-13: "HUD right below Configs, Lights by
// Item after Fading" (the mod's own Menu.cpp adds Lights by Item, FadeItems.cpp's Items::Render, after these). Settings ("Fading"): every other setting of the
// module (his rule: settings live in the SKSE menu only), live on the next frame and saved on the game's main thread (never
// the render thread); the rule files pick weapons one by one. Debug (ONLY in the testing DLL of the PDB and Source download - his call: "remove fading
// debug entirely only keep it with source and pdb as an in game testing dll"; built with FADE_DEBUG_PAGE): the debug log,
// each tracked hand's weapon, charge, the numbers on its lights and how many lights were found, plus the rule files and
// their problems - what a bug report needs.
// The look is the shared MenuStyle.h with these pages' warm glowing headings; an optional HUD element shows one charge gem
// per hand of the player beside the health bar (HudGems), drawn from Hud() (FadeLights.cpp), a small copy the main thread
// publishes once a frame, so the render thread never waits on the per-frame pass. There is no reticle of ours (his order
// 2026-10-19: "only have reticle available through reticle arcs mod. no settings in our mod") - FadeArcs.cpp offers the
// module's bars to Reticle Arcs.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include "Fade.h"
#include "LightKit.h"

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

namespace Fade
{
	namespace
	{
		constexpr ImGuiMCP::ImVec4 kGold{ 1.0f, 0.86f, 0.55f, 1.0f };
#ifdef FADE_DEBUG_PAGE
		constexpr ImGuiMCP::ImVec4 kEmber{ 0.93f, 0.72f, 0.45f, 0.85f };
		constexpr ImGuiMCP::ImVec4 kDim{ 0.62f, 0.58f, 0.52f, 1.0f };
#endif
		using Translation::T;
#define TR_MARK(x) x  // a line Translation.json carries, though it is not written inside T( ) here

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

		void GlowHeading(unsigned a_icon, const char* a_text)
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
			FontAwesome::PushSolid();
			TextColored(kGold, "%s", FontAwesome::UnicodeToUtf8(a_icon).c_str());
			FontAwesome::Pop();
			SameLine();
			TextColored(kGold, "%s", T(a_text));
			Dummy(ImVec2{ 0.0f, 4.0f });
		}

		void Tip(const char* a_text) { MenuStyle::Tip(T(a_text)); }

		// The pages edit a copy of the settings (Config); only the keys that changed go back (Commit -> ApplySetting), so a
		// DevBench change made meanwhile is kept, and the shared copy is never written half-way. A switch or a choice saves
		// the file on the next game frame; a slider when it is let go.
		void Toggle(const char* a_label, bool& a_value, const char* a_tip, bool& a_save)
		{
			a_save |= ImGuiMCP::Checkbox(T(a_label), &a_value);
			Tip(a_tip);
		}

		// a percent slider over a 0..1 (or more) float
		void Percent(const char* a_label, float& a_value, int a_lo, int a_hi, const char* a_tip, bool& a_save)
		{
			int v = static_cast<int>(std::lround(a_value * 100.0f));
			if (ImGuiMCP::SliderInt(T(a_label), &v, a_lo, a_hi, "%d%%")) {
				a_value = static_cast<float>(std::clamp(v, a_lo, a_hi)) / 100.0f;
			}
			a_save |= ImGuiMCP::IsItemDeactivatedAfterEdit();
			Tip(a_tip);
		}

		template <class E>
		void Choice(const char* a_label, E& a_value, const char* const* a_items, int a_count, const char* a_tip, bool& a_save)
		{
			int                      v = static_cast<int>(a_value);
			std::vector<const char*> items;
			for (int i = 0; i < a_count; ++i) {
				items.push_back(T(a_items[i]));
			}
			if (ImGuiMCP::Combo(T(a_label), &v, items.data(), a_count)) {
				a_value = static_cast<E>(std::clamp(v, 0, a_count - 1));
				a_save = true;
			}
			Tip(a_tip);
		}

		// the file is written on the game's main thread, never on the render thread these pages run on (his rule): one task,
		// however many changes come before it runs
		void QueueSave()
		{
			static std::atomic<bool> queued{ false };
			if (queued.exchange(true)) {
				return;
			}
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([]() {
					queued = false;
					LightKit::Guarded("fading settings save", []() { SaveSettings(); });
				});
			} else {
				queued = false;
			}
		}

		// only what changed on a page goes back, key by key: a change DevBench made meanwhile is kept
		void Commit(const Settings& a_before, const Settings& a_now, bool a_save)
		{
			for (const auto& k : SettingsText::kKeys) {
				if (k.get(a_now) != k.get(a_before)) {
					ApplySetting(k.name, k.get(a_now));
				}
			}
			if (a_save) {
				QueueSave();
			}
		}

		void __stdcall RenderSettings()
		{
			const GlowStyle style;
			const Settings  before = Config();
			Settings        s = before;
			auto&           t = s.tuning;
			bool            save = false;
			const float     column = ImGuiMCP::GetContentRegionAvail().x * 0.5f;  // where a paired switch starts
			ImGuiMCP::PushItemWidth(column * 0.9f);

			Toggle("Enabled", s.enabled, "Off: every weapon light and glow is put back exactly as the other mods set it.", save);
			ImGuiMCP::BeginDisabled(!s.enabled);

			GlowHeading(Icon::kSun, "Fading");
			Percent("Brightness when empty", t.floor, 0, 50,
				"What is left of the light at 0% charge. 0% puts it out; 10% (the default) keeps a faint glow so you can "
				"tell the weapon is enchanted but spent.",
				save);
			{
				static const char* const kCurves[] = { TR_MARK("Linear"), TR_MARK("Gentle - stays bright longer") };
				Choice("Curve", t.curve, kCurves, 2, "How the light falls as the charge falls. Gentle holds most of its brightness until the charge is low.",
					save);
			}
			Percent("Reach follows", t.reachFollows, 0, 100,
				"How much of the fade the light's reach follows too. 0%: only the brightness fades; 100%: the reach shrinks just as much.",
				save);
			Toggle("Dim the enchantment glow too", s.dimShader,
				"The glow on the blade (the enchantment's shader and its art, VAER's swirls among them) fades with the charge, "
				"like the lights.",
				save);

			GlowHeading(Icon::kMoon, "Nearly empty");
			Toggle("Sputter", t.sputter, "Below the level set here, the light sputters: short, irregular dips, deeper toward empty.", save);
			ImGuiMCP::BeginDisabled(!t.sputter);
			Percent("Sputter below", t.sputterBelow, 1, 50, "The charge level where the sputter starts.", save);
			Percent("Sputter strength", t.sputterStrength, 0, 100, "How deep the deepest dip goes, at empty.", save);
			ImGuiMCP::EndDisabled();
			Toggle("Steady at empty", t.emptySteady, "At exactly 0% the light stops sputtering and holds still.", save);
			// HIS WORD: "remove cools toward option in color cooling, the only option will be grey, ember cooling only
			// for fire spell types that are orange" (Glow::TintFor)
			Toggle("Color cooling", t.cool,
				"Below the sputter level the light's color drains toward grey. Fire magic with an orange light turns a dull ember "
				"instead, like a dying fire.",
				save);
			ImGuiMCP::BeginDisabled(!t.cool);
			Percent("Cooling amount", t.coolAmount, 0, 100, "How far the color goes toward grey (or ember) at empty.", save);
			ImGuiMCP::EndDisabled();

			GlowHeading(Icon::kBolt, "Moments");
			Toggle("Hit pulse", t.pulse, "A quick flash when a hit spends charge.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Recharge flare", t.flare, "When a soul gem refills the weapon, the light swells past full and settles.", save);
			ImGuiMCP::BeginDisabled(!t.pulse);
			Percent("Pulse strength", t.pulseStrength, 0, 200, "How bright the hit pulse flashes.", save);
			ImGuiMCP::EndDisabled();
			ImGuiMCP::BeginDisabled(!t.flare);
			Percent("Flare strength", t.flareStrength, 0, 200, "How far past full the recharge flare swells.", save);
			ImGuiMCP::EndDisabled();

			// HIS WORD: "put bound weapons above own light for unlit weapons in the what fades menu. completely remove
			// own light for unlit weapons option" - gone with its setting (an OwnLight line in an older Fading.ini is read past
			// and left out at the next save)
			GlowHeading(Icon::kWand, "What fades");
			Toggle("Weapon enchantments", s.weapons, "An enchanted weapon's lights and glow follow its charge.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Staves", s.staves, "A staff's lights and glow follow its charge.", save);
			Toggle("Bound weapons", s.bound,
				"A bound weapon has no charge: its light stays full, then fades over the last seconds of its spell.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Spells", s.spells,
				"A spell in hand: the lights and glow on the casting hand follow your magicka, dimming as it runs low and "
				"coming back as it refills.",
				save);
			ImGuiMCP::BeginDisabled(!s.bound);
			{
				int secs = static_cast<int>(s.boundFadeSeconds);
				if (ImGuiMCP::SliderInt(T("Bound fade"), &secs, 1, 60, T("last %d s"))) {
					s.boundFadeSeconds = static_cast<float>(std::clamp(secs, 1, 60));
				}
				save |= ImGuiMCP::IsItemDeactivatedAfterEdit();
				Tip("Over how many of the spell's last seconds a bound weapon's light fades.");
			}
			ImGuiMCP::EndDisabled();
			{
				static const char* const kWho[] = { TR_MARK("Player"), TR_MARK("Player and followers"), TR_MARK("Everyone") };
				Choice("Whose", s.who, kWho, 3,
					"Everyone: every caster near you, enemies too - their spells follow their magicka. Other characters' weapons "
					"usually never lose charge in the base game, so theirs stay full unless another mod makes them spend it.",
					save);
			}

			GlowHeading(Icon::kBook, "By Magicka Type");  // his name for it (was "By kind of magic")
			ImGuiMCP::TextDisabled("%s", T("By the strongest effect of an enchantment or a spell. Off: that kind keeps its full light."));
			{
				static const char* const kElementLabels[] = { TR_MARK("Fire"), TR_MARK("Frost"), TR_MARK("Shock"), TR_MARK("Absorb"),
					TR_MARK("Soul trap"), TR_MARK("Paralyze"), TR_MARK("Fear, turn undead, banish"), TR_MARK("Other effects") };
				static const char* const kSchoolLabels[] = { TR_MARK("Destruction"), TR_MARK("Restoration"), TR_MARK("Conjuration"),
					TR_MARK("Alteration"), TR_MARK("Illusion") };
				for (std::size_t i = 0; i < s.elements.size(); ++i) {
					if (i % 2) {
						ImGuiMCP::SameLine(column);
					}
					Toggle(kElementLabels[i], s.elements[i], "Effects of this kind fade.", save);
				}
				for (std::size_t i = 0; i < s.schools.size(); ++i) {
					if (i % 2) {
						ImGuiMCP::SameLine(column);
					}
					Toggle(kSchoolLabels[i], s.schools[i], "Magic of this school fades (an effect of no school is never held back here).", save);
				}
			}

			ImGuiMCP::EndDisabled();
			ImGuiMCP::PopItemWidth();
			ImGuiMCP::Spacing();
			ImGuiMCP::TextDisabled(T("Weapons can also be picked one by one in the rule files, %s."), Mod::kRulesDirText);
			Commit(before, s, save);
		}

		// HIS MENU: "make a HUD submenu placed right below configs. add logical and simple options for
		// toggling and messing with our hud stuff" - its own page (registered first of the module's pages, so it sits right
		// below the mod's Configs page; his answer 2026-10-13):
		// the charge bar and the charge gems (size, opacity, the percent). The gems show what the fading module tracks, so they
		// need Fading's Enabled on.
		void __stdcall RenderHudSettings()
		{
			const GlowStyle style;
			const Settings  before = Config();
			Settings        s = before;
			bool            save = false;
			ImGuiMCP::PushItemWidth(ImGuiMCP::GetContentRegionAvail().x * 0.45f);

			GlowHeading(Icon::kGauge, "Charge bar");
			// his answer 2026-10-13: with Fading off nothing else shows a weapon's charge, so the bar comes back and this
			// switch greys out (FadeSettings.cpp hides the bar only while both are on)
			ImGuiMCP::BeginDisabled(!s.enabled);
			Toggle("Hide the charge bar", s.hideChargeBar,
				"The HUD's enchantment charge bar is hidden: the weapon's light shows the charge instead. Works with the "
				"vanilla HUD, SkyHUD and TrueHUD. Needs Fading on - with Fading off the bar shows.",
				save);
			ImGuiMCP::EndDisabled();

			GlowHeading(Icon::kGem, "Charge gems");
			ImGuiMCP::BeginDisabled(!s.enabled);
			Toggle("Charge gems on the HUD", s.hudGems,
				"A soul gem for each enchanted weapon hand beside the health bar - the left hand's on the left, the right hand's on "
				"the right - as full as that weapon's charge (a bound weapon's: the time its spell has left). It shows once the charge "
				"starts dropping, while your weapons are out - full ones too with Show at full charge. Needs Fading on.",
				save);
			ImGuiMCP::BeginDisabled(!s.hudGems);
			Percent("Gem size", s.hudGemSize, 50, 200, "How big the gems are.", save);
			Percent("Gem opacity", s.hudGemOpacity, 10, 100, "How strongly the gems show over the game.", save);
			Toggle("Show the percent", s.hudGemPercent, "The charge in percent under each gem.", save);
			// his ask 2026-10-16 ("it was fully charged, give toggle to change that setting"): off keeps his 2026-10-10 rule - a gem
			// only once the charge drops
			Toggle("Show at full charge", s.hudGemFull,
				"On: a gem for every enchanted or bound weapon in your hands whose light fades (What fades), full ones too, while they "
				"are out. Off: a gem shows once the charge starts dropping.",
				save);
			ImGuiMCP::EndDisabled();
			ImGuiMCP::EndDisabled();
			ImGuiMCP::PopItemWidth();
			Commit(before, s, save);
		}

#ifdef FADE_DEBUG_PAGE
		void __stdcall RenderDebug()
		{
			const GlowStyle style;
			const auto      hands = Snapshot();
			{
				Settings s = Config();
				bool     save = false;
				Toggle("Debug log", s.debugLog, "Writes each tracked weapon, rule match, pulse and flare to the mod's log file.", save);
				if (save) {
					ApplySetting("DebugLog", s.debugLog ? 1 : 0);
					QueueSave();
				}
			}

			GlowHeading(Icon::kEye, "Preview");
			auto& preview = PreviewState();
			bool  on = preview.on;
			if (ImGuiMCP::Checkbox(T("Pretend the charge is"), &on)) {
				preview.on = on;
			}
			Tip("Every tracked weapon shows this charge instead of its own, to see the fade, sputter and cooling without "
				"fighting. Not saved: it switches off when the game restarts.");
			ImGuiMCP::SameLine();
			{
				int v = static_cast<int>(std::lround(preview.fraction.load() * 100.0f));
				ImGuiMCP::SetNextItemWidth(180.0f);
				if (ImGuiMCP::SliderInt("##previewCharge", &v, 0, 100, "%d%%")) {
					preview.fraction = static_cast<float>(std::clamp(v, 0, 100)) / 100.0f;
				}
			}
			if (ImGuiMCP::Button(T("Pulse"))) {
				preview.pulse = true;
			}
			Tip("The flash a spending hit makes, now.");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(T("Flare"))) {
				preview.flare = true;
			}
			Tip("The swell a recharge makes, now.");

			GlowHeading(Icon::kHand, "Hands");
			if (hands.empty()) {
				ImGuiMCP::TextDisabled("%s", T("No enchanted or bound weapon, and no spell, in hand right now."));
			}
			for (std::size_t i = 0; i < hands.size(); ++i) {
				const auto& h = hands[i];
				ImGuiMCP::PushID(static_cast<int>(i));
				ImGuiMCP::TextColored(kEmber, T("%s - %s hand"), h.actor.c_str(), h.left ? T("left") : T("right"));
				ImGuiMCP::Indent();
				ImGuiMCP::Text("%s", h.weapon.c_str());
				ImGuiMCP::TextDisabled("%s", h.enchantment.c_str());
				if (h.exempt) {
					ImGuiMCP::Text(T("left alone - %s"), h.why.c_str());
				} else {
					if (h.spell) {
						ImGuiMCP::Text(T("magicka %.0f / %.0f - %.0f%%"), h.current, h.max, h.fraction * 100.0f);
					} else if (h.bound) {
						ImGuiMCP::Text(T("%.1f s of %.0f s left - %.0f%%"), h.current, h.max, h.fraction * 100.0f);
					} else {
						ImGuiMCP::Text(T("charge %.0f / %.0f - %.0f%%"), h.current, h.max, h.fraction * 100.0f);
					}
					ImGuiMCP::ProgressBar(h.fraction, ImGuiMCP::ImVec2{ -1.0f, 0.0f }, "");
					ImGuiMCP::Text(T("brightness x%.2f   reach x%.2f   cooling %.0f%%"), h.brightness, h.reach, h.cool * 100.0f);
					ImGuiMCP::TextColored(h.lights ? kDim : kGold, T("%zu light(s) under %zu root(s)%s"), h.lights, h.roots,
						h.lights ? "" : T(" - nothing to dim: no lighting mod lights this weapon?"));
					ImGuiMCP::TextDisabled(T("decided by: %s"), h.why.c_str());
				}
				if (h.chargeAV >= 0.0f && !h.bound && !h.spell) {
					ImGuiMCP::TextDisabled(T("the game's own %s actor value: %.1f"), h.left ? "LeftItemCharge" : "RightItemCharge", h.chargeAV);
				}
				ImGuiMCP::Unindent();
				ImGuiMCP::PopID();
			}

			GlowHeading(Icon::kBulb, "Lights");
			ImGuiMCP::Text(T("%zu light(s) being scaled; %zu glow(s) dimmed"), ScaledLightCount(), DimmedGlowCount());
			if (const auto frozen = FrozenLightCount()) {
				ImGuiMCP::TextColored(kGold, T("%zu light(s) held steady: another plugin scales them the same way"), frozen);
				Tip("Two plugins each scaling the other's output would drive the light to black or white. This mod noticed "
					"and holds its base value; the light still fades, but check which other mod touches weapon lights.");
			}

			GlowHeading(Icon::kBook, "Rule files");
			ImGuiMCP::Text(T("%zu rule(s) from %zu file(s) in %s"), RuleCount(), RuleFileCount(), Mod::kRulesDirText);
			if (ImGuiMCP::Button(T("Reload rule files"))) {
				// on the game's main thread, between frames: the per-frame pass reads the rules there
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([]() { LightKit::Guarded("fading rule files", []() { LoadRules(); }); });
				}
			}
			Tip("Reads the .json files again, so a rule can be edited without restarting the game.");
			for (const auto& p : RuleProblems()) {
				ImGuiMCP::TextColored(kGold, "%s", p.c_str());
			}
		}
#endif
	}

	namespace
	{
		// a gem's outline (RenderHud, below)
		using GemPoints = std::array<ImGuiMCP::ImVec2, 6>;

		// the part of a convex gem outline below the height `a_y` (the filled part), as a convex polygon. A straight cut keeps
		// some corners and adds two crossings, so the part has at most one point more than the outline - a fixed array (with
		// room to spare), no allocation per frame.
		struct GemPart
		{
			std::array<ImGuiMCP::ImVec2, std::tuple_size_v<GemPoints> + 2> points{};
			int                                                            count{ 0 };

			void Add(ImGuiMCP::ImVec2 a_p) noexcept
			{
				if (count < static_cast<int>(points.size())) {
					points[static_cast<std::size_t>(count++)] = a_p;
				}
			}
		};

		[[nodiscard]] GemPart Below(const GemPoints& a_gem, float a_y) noexcept
		{
			GemPart out;
			for (std::size_t i = 0; i < a_gem.size(); ++i) {
				const auto& p = a_gem[i];
				const auto& q = a_gem[(i + 1) % a_gem.size()];
				const bool  pIn = p.y >= a_y, qIn = q.y >= a_y;
				if (pIn) {
					out.Add(p);
				}
				if (pIn != qIn) {  // p.y != q.y here, so the division is safe
					const float t = (a_y - p.y) / (q.y - p.y);
					out.Add(ImGuiMCP::ImVec2{ p.x + (q.x - p.x) * t, a_y });
				}
			}
			return out;
		}

		// THE MENUS (his report: "the widgets also do not turn off when opening the menus like all other widgets
		// do"). The HUD state is published from the player's update, which stops while a menu pauses the game - so the last
		// frame's state stood on screen over the inventory. A menu open / close watcher keeps the set of open game menus
		// that hide the HUD; the render thread reads one flag.
		class MenuWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatch& Get()
			{
				static MenuWatch watch;
				return watch;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view name = a_event->menuName.c_str();
				if (std::ranges::find(kHiding, name) != std::end(kHiding)) {
					LightKit::Guarded("menu watch", [&]() {
						std::lock_guard lock(gate);
						if (a_event->opening) {
							open.insert(std::string(name));
						} else {
							open.erase(std::string(name));
						}
						hide.store(!open.empty(), std::memory_order_relaxed);
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
			std::atomic<bool> hide{ false };

		private:
			// the game's own menus over which its HUD steps aside (a mod's always-open menu - TrueHUD's - is not one of them)
			static constexpr std::string_view kHiding[]{ "BarterMenu", "Book Menu", "Console", "Console Native UI Menu", "ContainerMenu",
				"Crafting Menu", "Creation Club Menu", "Creations Menu", "Dialogue Menu", "FavoritesMenu", "GiftMenu", "InventoryMenu",
				"Journal Menu", "Kinect Menu", "LevelUp Menu", "Loading Menu", "Lockpicking Menu", "MagicMenu", "Main Menu", "MapMenu",
				"MessageBoxMenu", "Mist Menu", "Mod Manager Menu", "Quantity Menu", "RaceSex Menu", "Sleep/Wait Menu", "StatsMenu",
				"Training Menu", "Tutorial Menu", "TweenMenu" };
			std::mutex                        gate;
			std::set<std::string>             open;
		};

		// the HUD steps aside: a hiding menu is open, or the game is paused (SKSE Menu Framework's own window pauses it)
		bool HideHud()
		{
			if (MenusHideHud()) {
				return true;
			}
			// the game's own HUD hidden (the tm toggle and the HUD-hiding keys that use it) hides our gems too
			auto* ui = RE::UI::GetSingleton();
			return ui && (ui->GameIsPaused() || !ui->IsShowingMenus());
		}

		// THE 3D SOUL GEM (his order: "I want a blender made 3d soul gem in super high detail (but optimized well so as to not
		// cause performance issues while retaining clarity) for the hud"). Pre-rendered in Blender by the mod's own render and
		// sheet tools (not part of this repository): a cut crystal turning about its long axis,
		// kFrames pictures over one symmetry step (45 degrees) so the loop never jumps, in a kCols x kRows sheet; two looks - the
		// dark empty crystal and the same crystal lit from inside in white, tinted here and shown from the bottom up to the
		// charge; three sizes, because the loader reads only the top mip (no mip maps), so a sheet squeezed far below its own
		// size shimmers as the gem turns - the one nearest the drawn size is used (the gem's cell is drawn ~45 px tall at
		// 1080p, ~60 at 1440p, ~90 at 4K at Gem size 100 %, up to ~225). BC7, ~0.9 MB in all, loaded on the render thread (the
		// HUD callback, as Arcane Overdrive's bar sheets) the first time a gem shows - a sheet that did not load is tried again
		// a few times. Missing or wrong-sized sheets: the drawn gem below. The constants below are measured off the renders; the
		// sheet tool refuses renders that no longer match kBody.
		struct GemArt
		{
			static constexpr int    kFrames = 12, kCols = 4, kRows = 3;
			static constexpr float  kFps = 8.0f;  // one 45-degree step in 1.5 s: a slow turn (neighbouring frames cross-faded)
			static constexpr int    kTries = 5;   // loads tried while no sheet size is ready
			static constexpr double kRetrySeconds = 10.0;
			static constexpr float  kBody = 490.0f / 560.0f;  // the gem's height in its cell, measured off the renders (the round cut)
			static constexpr float  kAspect = 4.0f / 7.0f;    // a cell's width over its height
			static constexpr float  kStretch = 1.15f;         // a sheet may be drawn up to 15 % above its own size before the next one is used

			struct Size
			{
				std::string_view empty, charge;
				float            cellW, cellH;  // in texels; the sheet is kCols x kRows cells
			};
			static constexpr std::array<Size, 3> kSizes{ { { "Soul Gem Empty Small.dds", "Soul Gem Charge Small.dds", 32.0f, 56.0f },
				{ "Soul Gem Empty Medium.dds", "Soul Gem Charge Medium.dds", 64.0f, 112.0f },
				{ "Soul Gem Empty Large.dds", "Soul Gem Charge Large.dds", 128.0f, 224.0f } } };  // smallest first

			struct Sheets
			{
				ImGuiMCP::ImTextureID empty{ nullptr }, charge{ nullptr };
				bool                  refused{ false };  // a sheet came back at another size: a wrong install, never tried again
				[[nodiscard]] bool    Ready() const noexcept { return empty && charge; }
				[[nodiscard]] bool    Missing() const noexcept { return !Ready() && !refused; }
			};
			std::array<Sheets, kSizes.size()> sheets{};

			[[nodiscard]] int ReadySizes() const noexcept
			{
				return static_cast<int>(std::count_if(sheets.begin(), sheets.end(), [](const Sheets& a_s) { return a_s.Ready(); }));
			}

			// the render thread only (the HUD callback): loads every sheet on the first call; while a size is missing (a loader
			// not up yet, a file briefly locked) its missing sheets are tried again every kRetrySeconds, kTries times in all -
			// never a size whose sheet was refused for its size
			[[nodiscard]] static const GemArt& Get(double a_now)
			{
				static GemArt art;
				static int    attempts = 0;
				static double nextTry = 0.0;
				const bool    missing = std::any_of(art.sheets.begin(), art.sheets.end(), [](const Sheets& a_s) { return a_s.Missing(); });
				if (missing && attempts < kTries && (attempts == 0 || a_now >= nextTry)) {
					art.Load(++attempts);
					nextTry = a_now + kRetrySeconds;
				}
				return art;
			}

		private:
			// SKSE Menu Framework's LoadTexture, asked for the size it read as well (the header's wrapper drops it). A size it
			// reports that is not the sheet's own means another layout - not drawn (a_refused set), so no cell can come out
			// misaligned.
			[[nodiscard]] static ImGuiMCP::ImTextureID LoadSheet(const std::string& a_path, float a_w, float a_h, bool& a_refused)
			{
				static const auto load = ::GetMenuFrameworkFunction<SKSEMenuFramework::Model::LoadTextureFuction>("LoadTexture");
				if (!load) {
					return nullptr;
				}
				ImGuiMCP::ImVec2 size{ 0.0f, 0.0f };
				auto* const      tex = load(a_path.c_str(), &size);
				if (tex && size.x > 0.0f && (std::lround(size.x) != std::lround(a_w) || std::lround(size.y) != std::lround(a_h))) {
					SKSE::log::warn("HUD soul gem: {} is {}x{}, not {}x{} - not drawn", a_path, size.x, size.y, a_w, a_h);
					SKSEMenuFramework::DisposeTexture(a_path);  // a refused sheet holds no video memory
					a_refused = true;
					return nullptr;
				}
				return tex;
			}

			// loads only what is still missing (a sheet that loaded on an earlier try is kept, never loaded twice); a size with a
			// refused sheet lets its other sheet go too, as it can never be drawn
			void Load(int a_attempt)
			{
				const std::string dir = Mod::kHudArtDir;
				for (std::size_t i = 0; i < kSizes.size(); ++i) {
					auto& sh = sheets[i];
					if (!sh.Missing()) {
						continue;
					}
					const auto& s = kSizes[i];
					const float w = s.cellW * kCols, h = s.cellH * kRows;
					const auto  empty = dir + std::string(s.empty), charge = dir + std::string(s.charge);
					if (!sh.empty) {
						sh.empty = LoadSheet(empty, w, h, sh.refused);
					}
					if (!sh.charge) {
						sh.charge = LoadSheet(charge, w, h, sh.refused);
					}
					if (sh.refused) {
						if (sh.empty) {
							SKSEMenuFramework::DisposeTexture(empty);
						}
						if (sh.charge) {
							SKSEMenuFramework::DisposeTexture(charge);
						}
						sh.empty = sh.charge = nullptr;
					}
				}
				const int ready = ReadySizes();
				SKSE::log::info("HUD soul gem: {} of {} sheet sizes loaded from {} (try {} of {}){}", ready, kSizes.size(), dir, a_attempt, kTries,
					ready ? "" : " - the drawn gem is used");
			}
		};

		// draws one gem from the sheets: centre a_c, the gem's half height a_hh (as the drawn gem's), charge a_fill 0..1 in
		// a_hue, opacity a_alpha, the clock a_now (seconds). False = no sheets, draw the old gem. The empty crystal above the
		// charge level and the tinted lit crystal below it are two quads that meet at the level - never one over the other,
		// so the charged part keeps the gem's opacity when the HUD's Gem opacity is below 100 %.
		bool DrawGemArt(ImGuiMCP::ImDrawList* a_dl, ImGuiMCP::ImVec2 a_c, float a_hh, float a_fill, ImGuiMCP::ImVec4 a_hue, float a_alpha, double a_now)
		{
			using namespace ImGuiMCP;
			const auto& art = GemArt::Get(a_now);
			const float h = 2.0f * a_hh / GemArt::kBody;  // the cell's drawn height
			if (!std::isfinite(h) || h <= 0.0f) {
				return false;
			}
			// the smallest loaded size that is not stretched more than kStretch, else the largest loaded one
			std::size_t pick = GemArt::kSizes.size();
			for (std::size_t i = 0; i < GemArt::kSizes.size(); ++i) {
				if (art.sheets[i].Ready()) {
					pick = i;
					if (GemArt::kSizes[i].cellH * GemArt::kStretch >= h) {
						break;
					}
				}
			}
			if (pick == GemArt::kSizes.size()) {
				return false;
			}
			const auto&  sheet = art.sheets[pick];
			const auto&  size = GemArt::kSizes[pick];
			const double turns = std::isfinite(a_now) ? std::fmod(std::max(a_now, 0.0) * GemArt::kFps, static_cast<double>(GemArt::kFrames)) : 0.0;
			const int    frame = std::clamp(static_cast<int>(turns), 0, GemArt::kFrames - 1);
			const float  blend = std::clamp(static_cast<float>(turns - static_cast<double>(frame)), 0.0f, 1.0f);  // toward the next frame
			const float  tw = size.cellW * GemArt::kCols, th = size.cellH * GemArt::kRows;                        // the sheet in texels
			const float  w = h * GemArt::kAspect;
			const ImVec2 lo{ a_c.x - w * 0.5f, a_c.y - h * 0.5f }, hi{ a_c.x + w * 0.5f, a_c.y + h * 0.5f };
			// the charge level: the gem itself spans a_c.y +- a_hh, full at its top tip, empty at its bottom tip
			const float fill = std::isfinite(a_fill) ? std::clamp(a_fill, 0.0f, 1.0f) : 0.0f;
			const float level = std::clamp(a_c.y + a_hh - 2.0f * a_hh * fill, lo.y, hi.y);
			const float cut = (level - lo.y) / h;  // the level's place in the cell, 0..1 from the top
			// one frame: the empty crystal above the level, the tinted lit one below it (a half-texel inset, so the bilinear
			// filter never reaches into the next cell)
			const auto drawFrame = [&](int a_frame, float a_opacity) {
				const float col = static_cast<float>(a_frame % GemArt::kCols), row = static_cast<float>(a_frame / GemArt::kCols);
				const float u0 = (col * size.cellW + 0.5f) / tw, u1 = ((col + 1.0f) * size.cellW - 0.5f) / tw;
				const float v0 = (row * size.cellH + 0.5f) / th, v1 = ((row + 1.0f) * size.cellH - 0.5f) / th;
				const float vCut = v0 + (v1 - v0) * cut;
				if (level > lo.y) {
					ImDrawListManager::AddImage(a_dl, sheet.empty, lo, ImVec2{ hi.x, level }, ImVec2{ u0, v0 }, ImVec2{ u1, vCut },
						ColorConvertFloat4ToU32(ImVec4{ 1.0f, 1.0f, 1.0f, a_opacity }));
				}
				if (level < hi.y) {
					ImDrawListManager::AddImage(a_dl, sheet.charge, ImVec2{ lo.x, level }, hi, ImVec2{ u0, vCut }, ImVec2{ u1, v1 },
						ColorConvertFloat4ToU32(ImVec4{ a_hue.x, a_hue.y, a_hue.z, a_opacity }));
				}
			};
			// the turn cross-faded: this frame, then the next one over it as far as the clock has moved toward it - a smooth
			// turn instead of a jump kFps times a second. Two layers blended one over the other would cover more than a_alpha
			// (an 8 Hz flicker below 100 % opacity), so this frame is drawn at a(1-b)/(1-ab) under the next at ab: together
			// exactly a_alpha over the gem's solid body (93 % of its texels), a little more only on its anti-aliased rim.
			if (blend <= 0.01f) {
				drawFrame(frame, a_alpha);
				return true;
			}
			const float over = a_alpha * blend;
			drawFrame(frame, over < 0.9999f ? a_alpha * (1.0f - blend) / (1.0f - over) : a_alpha);
			drawFrame((frame + 1) % GemArt::kFrames, over);
			return true;
		}

		// THE HUD GEMS (his word: each hand has its own cut soul gem on its own side of the health bar). 🔁 HIS WORD
		// "the enchantment gems should not work for spells and only show when the weapon enchantment is dropping"
		// and "remove the circle surrounding the soul gem widgets" - a gem is a WEAPON hand's (soul-gem violet for an
		// enchantment, pale blue for a bound weapon, ember under the sputter point), shown once its charge is below full - or
		// always with Show at full charge (his ask 2026-10-16) - with no halo. Render thread: it reads only Hud().
		void __stdcall RenderHud()
		{
			using namespace ImGuiMCP;
			static std::array<float, 2>  shown{ 0.0f, 0.0f };  // each side's fade, 0..1
			static std::array<float, 2>  fill{ 1.0f, 1.0f };
			static std::array<ImVec4, 2> tint{ ImVec4{}, ImVec4{} };
			const auto                   hud = Hud();
			auto*                        io = GetIO();
			if (!io) {
				return;
			}
			std::array<bool, 2> here{ false, false };
			if (hud.gems && hud.drawn && !HideHud()) {
				for (std::size_t i = 0; i < 2; ++i) {
					const auto& h = hud.hands[i];
					if (!h.shown || h.spell || (h.fraction >= 0.995f && !hud.gemFull)) {
						continue;  // a spell's magicka is not a gem's; a full weapon shows nothing unless Show at full charge is on
					}
					here[i] = true;
					fill[i] = std::isfinite(h.fraction) ? std::clamp(h.fraction, 0.0f, 1.0f) : 0.0f;
					tint[i] = h.bound ? ImVec4{ 0.62f, 0.84f, 1.0f, 1.0f } : ImVec4{ 0.72f, 0.46f, 1.0f, 1.0f };
				}
			}
			const float dt = std::isfinite(io->DeltaTime) ? std::clamp(io->DeltaTime, 0.0f, 0.25f) : 0.0f;
			for (std::size_t i = 0; i < 2; ++i) {
				shown[i] = std::clamp(shown[i] + (here[i] ? dt : -dt) * 4.0f, 0.0f, 1.0f);
			}
			if (shown[0] <= 0.0f && shown[1] <= 0.0f) {
				return;
			}
			auto* dl = GetForegroundDrawList();
			if (!dl) {
				return;
			}
			// sized to the screen (1080 lines = 1), each gem beside one end of the vanilla health bar
			const float screen = Glow::ScreenScale(io->DisplaySize.y);
			const float k = screen * std::clamp(hud.gemSize, 0.5f, 2.0f);  // the HUD page's Gem size
			const float cx = io->DisplaySize.x * 0.5f, y = io->DisplaySize.y - Tuning::Get(Tuning::gGemHeight) * screen;
			const float hw = 11.0f * k, hh = 19.0f * k, sh = 7.0f * k;  // half width, half height, shoulder height
			for (std::size_t i = 0; i < 2; ++i) {
				if (shown[i] <= 0.0f) {
					continue;
				}
				const float  a = shown[i] * std::clamp(hud.gemOpacity, 0.1f, 1.0f);
				const auto   col = [a](float r, float g, float b, float o) { return ColorConvertFloat4ToU32(ImVec4{ r, g, b, o * a }); };
				const float  f = fill[i];
				const auto   hue = f < hud.low ? ImVec4{ 1.0f, 0.42f, 0.18f, 1.0f } : tint[i];
				const ImVec2 c{ cx + (i == 0 ? -1.0f : 1.0f) * Tuning::Get(Tuning::gGemDistance) * screen, y };
				if (!DrawGemArt(dl, c, hh, f, hue, a, GetTime())) {
					const GemPoints gem{ ImVec2{ c.x, c.y - hh }, ImVec2{ c.x + hw, c.y - sh }, ImVec2{ c.x + hw, c.y + sh },
						ImVec2{ c.x, c.y + hh }, ImVec2{ c.x - hw, c.y + sh }, ImVec2{ c.x - hw, c.y - sh } };
					// no sheets: the dark empty gem, then what is left of the charge, filled up from the bottom
					ImDrawListManager::AddConvexPolyFilled(dl, gem.data(), static_cast<int>(gem.size()), col(0.05f, 0.04f, 0.06f, 0.85f));
					const float level = c.y + hh - 2.0f * hh * f;
					if (f > 0.0f) {
						const auto part = Below(gem, level);
						if (part.count >= 3) {
							ImDrawListManager::AddConvexPolyFilled(dl, part.points.data(), part.count, col(hue.x, hue.y, hue.z, 0.85f));
						}
					}
					// the facets: the girdle and the lines from the tips, faint; the outline in bone
					const ImU32 facet = col(1.0f, 1.0f, 1.0f, 0.22f);
					ImDrawListManager::AddLine(dl, gem[5], gem[1], facet, 1.0f);
					ImDrawListManager::AddLine(dl, gem[4], gem[2], facet, 1.0f);
					ImDrawListManager::AddLine(dl, gem[0], ImVec2{ c.x, c.y - sh }, facet, 1.0f);
					ImDrawListManager::AddLine(dl, gem[3], ImVec2{ c.x, c.y + sh }, facet, 1.0f);
					ImDrawListManager::AddPolyline(dl, gem.data(), static_cast<int>(gem.size()), col(0.90f, 0.85f, 0.74f, 0.95f), ImDrawFlags_Closed,
						std::max(1.0f, 1.5f * k));
				}
				if (!hud.gemPercent) {
					continue;
				}
				// the percent under the gem, centred
				char text[8];
				std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(std::lround(f * 100.0f)));
				const float tw = 7.0f * static_cast<float>(std::strlen(text));
				ImDrawListManager::AddText(dl, ImVec2{ c.x - tw * 0.5f, c.y + hh + 3.0f * k }, col(0.93f, 0.90f, 0.82f, 0.95f), text);
			}
		}
	}

	bool MenusHideHud() noexcept { return MenuWatch::Get().hide.load(std::memory_order_relaxed); }

	void RegisterMenu()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&MenuWatch::Get());  // the HUD elements hide over the game's menus
		}
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed, so there is no settings page; the settings file still applies");
			return;
		}
		// under the mod's own section; the colour scheme is the plugin's (its Menu.cpp sets it first): one DLL, one theme
		SKSEMenuFramework::SetSection(T(Mod::kName));
		// his answer 2026-10-13: "HUD right below Configs, Lights by Item after Fading" - the mod's own pages (up to Configs)
		// are added before these, its later pages (Lights by Item / Spell - Items::Render under the mod's own settings for that
		// page -, then the rest) after them (its RegisterMenuTail)
		SKSEMenuFramework::AddSectionItem(T("HUD"), RenderHudSettings);
		SKSEMenuFramework::AddSectionItem(T("Fading"), RenderSettings);
#ifdef FADE_DEBUG_PAGE
		SKSEMenuFramework::AddSectionItem(T("Fading - debug"), RenderDebug);
#endif
		SKSEMenuFramework::AddHudElement(RenderHud);
		SKSE::log::info("settings pages added to SKSE Menu Framework {}", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
