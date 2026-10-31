// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// LIGHTS BY ITEM - HIS ORDER: "completely remove own light for unlit weapons option. put that as an entirely new main
// menu and make it work for spells and weapons. have it be to where the user can search for the weapon/spell and change it's
// lights. so basically dynamic wards feature." His picks the same morning: a search over every spell, staff and weapon, an on /
// off switch per item, a color per item (Auto = the light's own, or one the player picks), "in my hands now", reset one / reset
// all - and "no slider per item, they all work with the main sliders".
//
// An item's choice is applied where its light is: once a frame on the main thread, at the end of the fading module's pass (so
// after the sliders and the fading, before a ground twin copies the hand light), on every light at the hand that holds it -
// a spell's: under the hand's magic nodes and the game's casting light; a weapon's (staves and bound weapons too): under its
// 3D in the hand, and for a staff also its hand's magic nodes and casting light, and for an enchanted weapon the roots its
// enchantment effects hang lights on (Fading on or off). And again right after Light Placer animates those lights - an
// effect's, a caster's, a cell's (Dynamic Lighting's color and fade keys rewrite them every update, so a choice written once
// a frame lost to them). Off puts the light out (fade 0), a color
// recolors it; the fade and color it had are kept, and given back the moment the item is set to Auto again (unless something
// else wrote the light since). Every lighting mod's light at that hand is reached - Light Placer's, RE::Light's, the game's
// own, ours - since the choice is about the item, not the light's maker.
// The choices live in Mod::kItemsPath, one line per item that is not Auto, keyed "Plugin.esp|0x00ABCD" (the item's own file
// and local id, so they survive a load order change); only an item of the list (the catalog) can be given a choice.

#include "Fade.h"
#include "FadeFormText.h"
#include "LightKit.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
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

#include <sstream>

namespace Fade::Items
{
	namespace
	{
		using Translation::T;

		struct Choice
		{
			bool        off{ false };
			bool        colored{ false };
			RE::NiColor color{ 1.0f, 1.0f, 1.0f };

			[[nodiscard]] bool Auto() const noexcept { return !off && !colored; }
		};

		// one item in the menu's list (built once at data load, main thread; read-only after)
		struct Item
		{
			RE::FormID  id{ 0 };
			std::string key;    // "Plugin.esp|0x00ABCD"
			std::string name;   // as the game shows it
			std::string lower;  // for the search
			std::string group;  // the school, or the weapon's kind
			bool        spell{ false };
		};

		std::vector<Item>                      gCatalog;
		std::unordered_set<RE::FormID>         gCatalogIds;  // the same items, to refuse a choice for anything else (read-only after load)
		std::mutex                             gLock;        // the choices: the menu (render thread) changes them, the main thread applies
		std::unordered_map<RE::FormID, Choice> gChoices;
		std::atomic<std::uint32_t>             gGeneration{ 0 };  // bumped on every change; the main thread copies on a change
		std::array<std::atomic<RE::FormID>, 2> gInHands{};        // the player's left / right item now, for "in my hands now"
		// the file is written by the main thread once the changes settle, never by the menu while it holds gLock (CodeRabbit,
		// Illuminated #6 / RELight #8: a color drag changes the choice every frame)
		std::atomic<bool>         gDirty{ false };
		std::atomic<std::int64_t> gChangedAt{ 0 };  // steady clock, milliseconds
		constexpr std::int64_t    kSaveQuietMs = 500;
		std::atomic<int>          gSaveFailures{ 0 };  // failed writes since the last change or the last good one
		constexpr int             kSaveRetries = 3;    // then the file waits for the next change: a folder that refuses writes is not hammered

		[[nodiscard]] std::int64_t NowMs() noexcept
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// main thread only: this frame's copy of the choices, and every light a choice is on now
		std::unordered_map<RE::FormID, Choice> gApplied;
		std::uint32_t                          gAppliedGen = static_cast<std::uint32_t>(-1);
		struct Held
		{
			RE::NiPointer<RE::NiPointLight> light;
			float                           ownFade{ 0.0f }, wroteFade{ -1.0f };
			RE::NiColor                     ownColor{}, wroteColor{ -1.0f, -1.0f, -1.0f };
			bool                            faded{ false }, colored{ false };
			std::uint32_t                   frame{ 0 };
			Choice                          choice{};  // what this frame put on it, for Reapply
		};
		std::unordered_map<RE::NiPointLight*, Held> gHeld;
		std::uint32_t                               gFrame = 0;
		std::atomic<bool>                           gHolding{ false };  // gHeld is not empty: the hooks' cheap test

		[[nodiscard]] std::string KeyOf(const RE::TESForm* a_form)
		{
			const auto* file = a_form ? a_form->GetFile(0) : nullptr;
			if (!file) {
				return {};
			}
			const auto local = a_form->GetLocalFormID();
			return std::format("{}|0x{:06X}", file->GetFilename(), local);
		}

		[[nodiscard]] RE::FormID FormOfKey(std::string_view a_key)
		{
			// FormText::Parse: the plugin and the id both trimmed, an empty plugin refused, the whole id text the number
			// ("0x12junk" names nothing) - the rule files' own reader
			const auto spec = FormText::Parse(a_key);
			if (spec.kind != FormText::Spec::Kind::kPluginAndID || a_key.find('|') == std::string_view::npos) {
				return 0;
			}
			auto*       dh = RE::TESDataHandler::GetSingleton();
			const auto* file = dh ? dh->LookupModByName(spec.plugin) : nullptr;
			// the plugin's own part of the id: a load-order prefix copied from a record editor named another form (L11)
			const auto* form = file ? dh->LookupForm(FormText::LocalID(spec.id, file->IsLight()), spec.plugin) : nullptr;
			return form ? form->GetFormID() : 0;
		}

		// one choice as the file and DevBench write it: "auto", "off", "r,g,b" or "off,r,g,b" (off, keeping the color picked
		// for when it is switched back on). nullopt: not a choice
		[[nodiscard]] std::optional<Choice> ParseChoice(std::string_view a_text)
		{
			Choice c;
			auto   rest = SettingsText::Trim(a_text);
			if (SettingsText::SameText(rest, "auto")) {
				return c;
			}
			if (SettingsText::SameText(rest, "off")) {
				c.off = true;
				return c;
			}
			if (const auto comma = rest.find(','); comma != std::string_view::npos && SettingsText::SameText(SettingsText::Trim(rest.substr(0, comma)), "off")) {
				c.off = true;
				rest = rest.substr(comma + 1);
			}
			int rgb[3]{};
			for (int& v : rgb) {
				const auto comma = rest.find(',');
				if (!SettingsText::ReadInt(SettingsText::Trim(rest.substr(0, comma)), v)) {
					return std::nullopt;
				}
				rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
			}
			c.colored = true;
			c.color = { std::clamp(rgb[0], 0, 255) / 255.0f, std::clamp(rgb[1], 0, 255) / 255.0f, std::clamp(rgb[2], 0, 255) / 255.0f };
			return c;
		}

		// a copy of the choices, written outside every lock (main thread, an SKSE task); false when the file was not written
		[[nodiscard]] bool Save(const std::unordered_map<RE::FormID, Choice>& a_choices)
		{
			std::ostringstream text;
			text << "; " << Mod::kName << " - Lights by Item, written by its menu. One line per item that is not Auto:\n"
				 << ";   Plugin.esp|0x00ABCD=off           its light is off\n"
				 << ";   Plugin.esp|0x00ABCD=255,128,0     its light in this color\n"
				 << ";   Plugin.esp|0x00ABCD=off,255,128,0 off, and in this color when it is switched on again\n[Items]\n";
			for (const auto& item : gCatalog) {
				const auto it = a_choices.find(item.id);
				if (it == a_choices.end() || it->second.Auto()) {
					continue;
				}
				const auto& c = it->second;
				const auto  rgb = std::format("{},{},{}", std::lround(c.color.red * 255.0f), std::lround(c.color.green * 255.0f), std::lround(c.color.blue * 255.0f));
				std::string value = rgb;
				if (c.off) {
					value = c.colored ? "off," + rgb : std::string("off");  // off, and the color it takes when switched on again
				}
				text << item.key << "=" << value << "\n";
			}
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(Mod::kItemsPath).parent_path(), ec);
			const std::string tmp = std::string(Mod::kItemsPath) + ".tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				out << text.str();
				out.close();  // a close that fails is a write that failed
				if (out.fail()) {
					SKSE::log::warn("lights by item: {} could not be written", Mod::kItemsPath);
					std::filesystem::remove(tmp, ec);
					return false;
				}
			}
			if (std::filesystem::rename(tmp, Mod::kItemsPath, ec); ec) {
				SKSE::log::warn("lights by item: {} could not be replaced ({})", Mod::kItemsPath, ec.message());
				std::filesystem::remove(tmp, ec);
				return false;
			}
			return true;
		}

		void MarkChanged()
		{
			gGeneration.fetch_add(1, std::memory_order_relaxed);
			gSaveFailures.store(0, std::memory_order_relaxed);
			gChangedAt.store(NowMs(), std::memory_order_relaxed);
			gDirty.store(true, std::memory_order_release);
		}

		void Set(RE::FormID a_id, const Choice& a_choice)
		{
			{
				std::lock_guard lock(gLock);
				if (a_choice.Auto()) {
					gChoices.erase(a_id);
				} else {
					gChoices[a_id] = a_choice;
				}
			}
			MarkChanged();
		}

		// main thread: the file, once the choices have not moved for kSaveQuietMs (or at once, a_now) - written by an SKSE task
		// from a copy, never under the fading pass's per-frame lock (this is called from inside it, UpdateHands)
		void SaveIfSettled(bool a_now)
		{
			if (!gDirty.load(std::memory_order_acquire) || (!a_now && NowMs() - gChangedAt.load(std::memory_order_relaxed) < kSaveQuietMs)) {
				return;
			}
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;  // no task to write it with: kept dirty for the next try, never written under the lock
			}
			gDirty.store(false, std::memory_order_relaxed);
			const auto                             generation = gGeneration.load(std::memory_order_relaxed);
			std::unordered_map<RE::FormID, Choice> copy;
			{
				std::lock_guard lock(gLock);
				copy = gChoices;
			}
			// a failed write is tried again after kSaveQuietMs, up to kSaveRetries times, then at the next change. A copy that a
			// newer change or a read of the file has passed is not tried again: the newer choices are saved with their own tries.
			tasks->AddTask([c = std::move(copy), generation]() {
				bool saved = false;
				LightKit::Guarded("lights by item save", [&c, &saved]() { saved = Save(c); });  // a throw is a failed write, tried again below
				LightKit::Guarded("lights by item save retry", [saved, generation]() {
					if (saved) {
						gSaveFailures.store(0, std::memory_order_relaxed);
						return;
					}
					if (generation != gGeneration.load(std::memory_order_relaxed)) {
						return;
					}
					if (gSaveFailures.fetch_add(1, std::memory_order_relaxed) < kSaveRetries) {
						gChangedAt.store(NowMs(), std::memory_order_relaxed);
						gDirty.store(true, std::memory_order_release);
					} else {
						SKSE::log::warn("lights by item: {} still not written - the next change tries again", Mod::kItemsPath);
					}
				});
			});
		}

		// ------------------------------------------------------------------ applying, main thread
		void Touch(RE::NiPointLight* a_light, const Choice& a_choice)
		{
			if (!a_light) {
				return;
			}
			const auto turn = LightKit::Turn();  // the choice and its handshake in one turn (LightKit.h)
			auto [it, added] = gHeld.try_emplace(a_light);
			auto& h = it->second;
			if (added) {
				h.light.reset(a_light);
			}
			h.frame = gFrame;
			h.choice = a_choice;
			auto&             d = a_light->GetLightRuntimeData();
			const float       fadeBefore = d.fade;
			const RE::NiColor colorBefore = d.diffuse;
			if (a_choice.off) {
				if (!h.faded || d.fade != h.wroteFade) {
					h.ownFade = d.fade;  // what its owner gave it: given back when the item is Auto again
				}
				d.fade = 0.0f;
				h.wroteFade = 0.0f;
				h.faded = true;
			} else if (h.faded) {
				if (d.fade == h.wroteFade) {
					d.fade = h.ownFade;
				}
				h.faded = false;
			}
			if (a_choice.colored) {
				const bool same = d.diffuse.red == h.wroteColor.red && d.diffuse.green == h.wroteColor.green && d.diffuse.blue == h.wroteColor.blue;
				if (!h.colored || !same) {
					h.ownColor = d.diffuse;
				}
				d.diffuse = a_choice.color;
				h.wroteColor = a_choice.color;
				h.colored = true;
			} else if (h.colored) {
				const bool same = d.diffuse.red == h.wroteColor.red && d.diffuse.green == h.wroteColor.green && d.diffuse.blue == h.wroteColor.blue;
				if (same) {
					d.diffuse = h.ownColor;
				}
				h.colored = false;
			}
			// what was written here is reported to the fading pass and to Brightness, so neither takes an item's 0 or its color
			// for the light's own next frame (CodeRabbit, RELight #8: an item off, then back to Auto, compounded the fade)
			NoteItemWrite(a_light, fadeBefore, d.fade, colorBefore, d.diffuse);
		}

		void TouchUnder(RE::NiAVObject* a_root, const Choice& a_choice)
		{
			if (!a_root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphLights(a_root, [&](RE::NiPointLight* a_light) {
				Touch(a_light, a_choice);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void VisitActor(RE::Actor* a_actor)
		{
			if (!a_actor || !a_actor->Is3DLoaded()) {
				return;
			}
			const bool player = a_actor->IsPlayerRef();
			for (const bool left : { false, true }) {
				auto* form = a_actor->GetEquippedObject(left);
				if (player) {
					gInHands[left ? 0 : 1].store(form ? form->GetFormID() : 0, std::memory_order_relaxed);
				}
				const auto it = form ? gApplied.find(form->GetFormID()) : gApplied.end();
				if (it == gApplied.end()) {
					continue;
				}
				const auto& choice = it->second;
				// the hand's magic nodes and casting light: a spell's lights - and a staff's in use (FadeSpells.cpp)
				const auto magicHand = [&]() {
					RE::NiAVObject*   nodes[2]{};
					RE::NiPointLight* caster = nullptr;
					SpellHandLights(a_actor, left, nodes, caster);
					for (auto* node : nodes) {
						TouchUnder(node, choice);
					}
					Touch(caster, choice);
					for (auto* light : NotedLightsOf(a_actor, left)) {
						TouchUnder(light, choice);
					}
				};
				if (form->As<RE::SpellItem>()) {
					magicHand();
				} else if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
					TouchUnder(WeaponPart(a_actor, false, left, form), choice);
					if (player) {
						TouchUnder(WeaponPart(a_actor, true, left, form), choice);
					}
					if (weapon->IsStaff()) {
						magicHand();
					}
					// its enchantment's lights: the roots the fading pass keeps, or - Fading off, a weapon the rules exempt - the
					// game's own list of that hand's enchantment effects (FadeLights.cpp EffectRootsOf; CodeRabbit on RELight #16)
					for (auto* root : EffectRootsOf(a_actor, left)) {
						TouchUnder(root, choice);
					}
					for (auto* light : NotedLightsOf(a_actor, left)) {  // a plugin's light for this hand off its 3D (NoteHandLight)
						TouchUnder(light, choice);
					}
				}
			}
		}
	}

	void Load()
	{
		std::lock_guard lock(gLock);
		gChoices.clear();
		const auto    from = SettingsText::ReadFrom(Mod::kItemsPath, Mod::kOldItemsPath);
		std::ifstream in(from);
		std::string   line;
		std::size_t   read = 0, missing = 0;
		while (std::getline(in, line)) {
			const auto t = SettingsText::Trim(line);
			if (t.empty() || t.front() == ';' || t.front() == '[') {
				continue;
			}
			const auto eq = t.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto id = FormOfKey(SettingsText::Trim(t.substr(0, eq)));
			if (!id || !gCatalogIds.contains(id)) {
				++missing;  // not in this load order, or not an item the list holds
				continue;
			}
			const auto c = ParseChoice(t.substr(eq + 1));
			if (!c || c->Auto()) {
				continue;
			}
			gChoices[id] = *c;
			++read;
		}
		gGeneration.fetch_add(1, std::memory_order_relaxed);
		SKSE::log::info("lights by item: {} choice(s) read from {} ({} name an item not in this load order's list)", read, from, missing);
	}

	void BuildCatalog()
	{
		static constexpr const char* kSchools[]{ "Destruction", "Restoration", "Conjuration", "Alteration", "Illusion" };
		gCatalog.clear();
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}
		for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
			if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell || spell->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
				continue;
			}
			const char* name = spell->GetFullName();
			if (!name || !*name) {
				continue;
			}
			const auto kind = KindOf(spell);
			const auto key = KeyOf(spell);
			if (key.empty()) {
				continue;
			}
			gCatalog.push_back({ spell->GetFormID(), key, name, Lower(name), kind.school >= 0 ? kSchools[kind.school] : "Other spells", true });
		}
		for (auto* weapon : dh->GetFormArray<RE::TESObjectWEAP>()) {
			if (!weapon || !weapon->GetPlayable()) {
				continue;
			}
			const char* name = weapon->GetFullName();
			const bool  staff = weapon->IsStaff();
			const bool  bound = weapon->IsBound();
			if (!name || !*name || !(weapon->formEnchanting || staff || bound)) {
				continue;  // a weapon with no magic carries no light of ours to change
			}
			const auto key = KeyOf(weapon);
			if (key.empty()) {
				continue;
			}
			gCatalog.push_back({ weapon->GetFormID(), key, name, Lower(name), staff ? "Staves" : bound ? "Bound weapons" :
																										 "Enchanted weapons",
				false });
		}
		std::ranges::sort(gCatalog, [](const Item& a, const Item& b) { return std::make_tuple(!a.spell, a.group, a.lower) < std::make_tuple(!b.spell, b.group, b.lower); });
		gCatalogIds.clear();
		for (const auto& item : gCatalog) {
			gCatalogIds.insert(item.id);
		}
		SKSE::log::info("lights by item: {} spells and weapons in the list", gCatalog.size());
	}

	void Apply()
	{
		++gFrame;
		SaveIfSettled(false);
		if (const auto gen = gGeneration.load(std::memory_order_relaxed); gen != gAppliedGen) {
			std::lock_guard lock(gLock);
			gApplied = gChoices;
			gAppliedGen = gen;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!gApplied.empty()) {
			VisitActor(player);
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (auto actor = handle.get(); actor && actor.get() != player) {
						VisitActor(actor.get());
					}
				}
			}
		} else if (player) {
			for (const bool left : { false, true }) {
				auto* form = player->GetEquippedObject(left);
				gInHands[left ? 0 : 1].store(form ? form->GetFormID() : 0, std::memory_order_relaxed);
			}
		}
		// a light no choice reached this frame goes back as its owner left it (an item set to Auto, put away, a light gone)
		for (auto it = gHeld.begin(); it != gHeld.end();) {
			auto&      h = it->second;
			const bool gone = !h.light || h.light->GetRefCount() <= 1;
			if (gone || h.frame != gFrame) {
				if (!gone) {
					Touch(h.light.get(), Choice{});  // Auto: what we wrote goes back to its own
				}
				it = gHeld.erase(it);
			} else {
				++it;
			}
		}
		gHolding.store(!gHeld.empty(), std::memory_order_relaxed);
	}

	bool SetFromText(RE::FormID a_id, std::string_view a_choice)
	{
		const auto c = ParseChoice(a_choice);
		if (!c || !gCatalogIds.contains(a_id)) {
			return false;  // not a choice, or an item the page could not show (the list is the catalog)
		}
		Set(a_id, *c);
		return true;
	}

	void Reapply()
	{
		for (auto& [light, h] : gHeld) {
			if (h.light && h.frame == gFrame && h.light->GetRefCount() > 1) {
				Touch(h.light.get(), h.choice);
			}
		}
	}

	void ReapplyUnder(RE::NiAVObject* a_root)
	{
		if (!a_root || gHeld.empty()) {
			return;
		}
		RE::BSVisit::TraverseScenegraphLights(a_root, [](RE::NiPointLight* a_light) {
			if (const auto it = gHeld.find(a_light); it != gHeld.end() && it->second.frame == gFrame) {
				Touch(a_light, it->second.choice);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	bool Holding() noexcept
	{
		return gHolding.load(std::memory_order_relaxed);
	}

	std::optional<RE::NiColor> ChoiceColor(RE::FormID a_id)
	{
		const auto it = gApplied.find(a_id);
		if (it == gApplied.end() || it->second.Auto()) {
			return std::nullopt;
		}
		return it->second.off ? RE::NiColor{ 0.0f, 0.0f, 0.0f } : it->second.color;
	}

	void Release()
	{
		SaveIfSettled(true);
		for (auto& [light, h] : gHeld) {
			if (h.light && h.light->GetRefCount() > 1) {
				Touch(h.light.get(), Choice{});
			}
		}
		gHeld.clear();
		gHolding.store(false, std::memory_order_relaxed);
	}

	// ------------------------------------------------------------------ the page
	// spells, staves and weapons on one page, Choose Your Own Color
	void Render()
	{
		using namespace ImGuiMCP;
		const MenuStyle::Page page;
		static char           search[64]{};
		static bool           handsOnly = false;
		MenuStyle::Header(MenuStyle::Icon::kBulb, T("Choose Your Own Color"));
		TextWrapped("%s", T("Find a spell, staff or weapon and change its light: switch it off, or give it a color of its own. Everything "
							"else keeps its light as it is (Auto). The main Brightness and Reach sliders still reach them all."));
		// his list: "explain the buttons in light by item and make them one above the other" - each control on its own line,
		// what it does beside it
		SetNextItemWidth(260.0f);
		if (InputTextWithHint("##itemSearch", T("Search a spell, staff or weapon"), search, sizeof(search))) {
			handsOnly = false;
		}
		SameLine();
		TextDisabled("%s", T("Type part of a name to find it."));
		if (Button(T("In my hands now"))) {
			handsOnly = true;
			search[0] = '\0';
		}
		SameLine();
		TextDisabled("%s", T("Lists only what you hold right now."));
		if (Button(T("Reset all"))) {
			{
				std::lock_guard lock(gLock);
				gChoices.clear();
			}
			MarkChanged();
		}
		SameLine();
		TextDisabled("%s", T("Everything back to Auto."));
		TextDisabled("%s", T("Each item: first box = its light on or off, second box = a color of its own (pick it in the square), "
							 "Reset = back to Auto."));
		std::unordered_map<RE::FormID, Choice> choices;
		{
			std::lock_guard lock(gLock);
			choices = gChoices;
		}
		const std::string     needle = Lower(search);
		const RE::FormID      hands[2]{ gInHands[0].load(std::memory_order_relaxed), gInHands[1].load(std::memory_order_relaxed) };
		std::string           group;
		std::size_t           shown = 0;
		constexpr std::size_t kMaxShown = 240;  // a search narrows it; the whole load order is thousands
		for (const auto& item : gCatalog) {
			const bool inHands = item.id && (item.id == hands[0] || item.id == hands[1]);
			const bool changed = choices.contains(item.id);
			if (handsOnly ? !inHands : (needle.empty() ? !changed : item.lower.find(needle) == std::string::npos)) {
				continue;  // with no search, the list shows the items you changed
			}
			if (++shown > kMaxShown) {
				TextDisabled("%s", T("... and more - type more of the name to narrow the list"));
				break;
			}
			if (item.group != group) {
				group = item.group;
				Spacing();
				TextColored(ImVec4{ 1.0f, 0.86f, 0.55f, 1.0f }, "%s", T(group.c_str()));
			}
			PushID(static_cast<int>(item.id));
			auto c = choices.contains(item.id) ? choices[item.id] : Choice{};
			bool on = !c.off;
			if (Checkbox("##on", &on)) {
				c.off = !on;
				Set(item.id, c);
			}
			MenuStyle::Tip(T("Its light on or off."));
			SameLine();
			bool own = c.colored;
			if (Checkbox("##own", &own)) {
				c.colored = own;
				Set(item.id, c);
			}
			MenuStyle::Tip(T("Its own color (off: Auto, the light's own color)."));
			SameLine();
			BeginDisabled(!c.colored);
			float rgb[3]{ c.color.red, c.color.green, c.color.blue };
			if (ColorEdit3("##color", rgb, ImGuiColorEditFlags_NoInputs)) {
				c.color = { rgb[0], rgb[1], rgb[2] };
				Set(item.id, c);
			}
			EndDisabled();
			SameLine();
			Text("%s", item.name.c_str());
			if (changed) {
				SameLine();
				if (SmallButton(T("Reset"))) {
					Set(item.id, Choice{});
				}
			}
			PopID();
		}
		if (shown == 0) {
			TextDisabled("%s", handsOnly      ? T("Nothing with a light in your hands right now.") :
							   needle.empty() ? T("Nothing changed yet - search above.") :
												T("Nothing by that name."));
		}
	}
}
