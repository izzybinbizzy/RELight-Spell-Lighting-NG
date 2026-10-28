// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The light on the caster's hands - no plugin, no script.
//
// Casting art hangs off the actor, so no RE::Light mesh config reaches it. Instead each magic effect whose casting
// art is a mesh this mod lights is pointed, in memory, at its casting light: a light record made here in the color,
// strength and reach the installed layers give that mesh. RE::Light switches off casting lights it has no config for,
// and an in-memory record can have none, so main.cpp's hook makes these lights itself and never hands them on.
// Nothing is saved; take the mod out and every effect keeps its own light.
//
// AUTOMATIC HAND LIGHTS (Illuminated's, ported - his "add everything from illuminated into relight"): a spell
// whose casting art no layer lights (another mod's spell this mod has no patch for) would be dark - RE::Light switches its
// casting light off, having no config for it. Behind the setting "Light spells this mod has no patch for" it gets a hand
// light of ours instead, at the middle strength and reach of the tuned hand lights. Its color, in his order of 2026-10-18
// (Illuminated's CLAUDE.md §1 - the same order there): its own casting light's, else the light of the projectile it fires,
// else its art's (LightKit.h ArtColor), else the color most of the tuned spells of its resistance group wear (every group).
// A spell with none of them keeps what it has: no color is guessed.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		struct Target
		{
			RE::EffectSetting* effect{ nullptr };
			RE::TESObjectLIGH* own{ nullptr };  // the light its plugin gave it
			std::string        key;
			bool               automatic{ false };  // no layer lights its art: key names an automatic hand (gAutoHands)
		};

		constexpr std::string_view                kAutoPrefix = "auto ";
		constexpr std::array<std::string_view, 5> kSkipPrefixes{ "trap", "hazard", "voice", "ench", "test" };  // Illuminated's pass 1 list

		constexpr std::uint32_t kLighInverseSquare = LightKit::kRecordInverseSquare;  // on a LIGH record (LightKit.h)

		// a hand light already lit: a menu change reaches it at once instead of on the next cast (his report:
		// "i have to dispel the spell then re-cast")
		struct Live
		{
			RE::NiLight* light{ nullptr };  // held in Keep.cpp; ForgetHandLights drops it here before it is freed
			std::string  key;
			bool         heldOut{ false };  // put out because nothing lights its key any more
		};

		std::mutex                                                       gLock;
		std::vector<Target>                                              gTargets;
		std::vector<Live>                                                gLive;
		std::unordered_map<std::string, RE::TESObjectLIGH*>              gCopies;
		std::unordered_map<const RE::TESObjectLIGH*, const Hand*>        gInUse;
		std::size_t                                                      gLit = 0, gAutoLit = 0;
		std::map<std::string, Hand, std::less<>>                         gAutoHands;    // "auto r,g,b" -> its hand (stable: a map's nodes never move)
		std::unordered_map<std::string, RE::TESObjectLIGH*>              gAutoCopies;   // the same key -> its light record
		std::unordered_map<const RE::TESObjectLIGH*, RE::TESObjectLIGH*> gShadowTwins;  // a copy -> its shadow-casting twin (ShadowPatch)

		[[nodiscard]] bool AutoKey(std::string_view a_key) { return a_key.starts_with(kAutoPrefix); }

		[[nodiscard]] bool SkippedPrefix(const RE::EffectSetting* a_effect)
		{
			// the fading module keeps every magic effect's editor ID (the game drops them), so no other plugin is needed
			std::string low = Lower(Fade::EditorID(a_effect));
			if (low.size() > 4 && low.starts_with("dlc")) {
				low = low.substr(4);
			}
			return std::ranges::any_of(kSkipPrefixes, [&](std::string_view p) { return low.starts_with(p); });
		}

		// the layer that lights this hand now: the highest order whose option is not switched off
		const Hand* Winner(const std::string& a_key)
		{
			const auto& hands = Hands();
			auto        it = hands.find(a_key);
			if (it == hands.end()) {
				return nullptr;
			}
			for (const auto& h : it->second) {
				if (OptionLit(h.option)) {
					return &h;
				}
			}
			return nullptr;
		}

		// a copy of a hand in another color, made once per hand (under gLock, like every caller)
		const Hand* Tinted(std::unordered_map<const Hand*, Hand>& a_copies, const Hand* a_hand, const RE::NiColor& a_color)
		{
			auto [it, fresh] = a_copies.try_emplace(a_hand, *a_hand);
			if (fresh) {
				const auto byte = [](float a_v) { return static_cast<std::uint8_t>(std::lround(std::clamp(a_v, 0.0f, 1.0f) * 255.0f)); };
				it->second.color = a_color;
				it->second.rgb[0] = byte(a_color.red);
				it->second.rgb[1] = byte(a_color.green);
				it->second.rgb[2] = byte(a_color.blue);
			}
			return &it->second;
		}

		// the ward color pick: White gives the ward's hand light white, in the layer's own strength and reach - never while
		// Dynamic Wards colors the wards. Strange Wards' own hand (no 360 Ward) wears its pick's color whatever the White look
		// says; its Blue pick keeps the layer's blue
		const Hand* Picked(const Hand* a_hand)
		{
			if (!a_hand || WardsSteppedDown() || MeshKey(a_hand->key) != "wardinhandfx") {
				return a_hand;
			}
			if (StrangeWardsHandShows()) {
				static std::unordered_map<const Hand*, Hand> strange;
				const auto                                   pick = StrangeWardsPick();
				return pick ? Tinted(strange, a_hand, *pick) : a_hand;
			}
			if (WardColor() != 1) {
				return a_hand;
			}
			static std::unordered_map<const Hand*, Hand> white;
			return Tinted(white, a_hand, { 1.0f, 1.0f, 1.0f });
		}

		// vanilla MagicLightFrostHand01's shape (falloff 1, field of view 90, near clip 1) with the layer's color, reach and strength
		void Fill(RE::TESObjectLIGH* a_light, const Hand& a_hand)
		{
			auto& d = a_light->data;
			d.time = -1;
			d.radius = static_cast<std::uint32_t>(std::lround(std::clamp(a_hand.radius, 0.0f, 100000.0f)));
			d.color.red = a_hand.rgb[0];
			d.color.green = a_hand.rgb[1];
			d.color.blue = a_hand.rgb[2];
			d.color.alpha = 0;
			std::uint32_t flags = a_hand.portalStrict ? static_cast<std::uint32_t>(RE::TES_LIGHT_FLAGS::kPortalStrict) : 0u;
			if (a_hand.inverseSquare) {
				flags |= kLighInverseSquare;
			}
			d.flags = static_cast<RE::TES_LIGHT_FLAGS>(flags);
			d.fallofExponent = 1.0f;
			d.fov = 90.0f;
			d.nearDistance = 1.0f;
			d.flickerPeriodRecip = 1.0f;
			d.flickerIntensityAmplitude = 0.0f;
			d.flickerMovementAmplitude = 0.0f;
			a_light->fade = a_hand.fade;
		}
	}

	namespace
	{
		// Which magic effects wear a lit mesh. Found at data load and again when a save loads, because another plugin
		// (Dynamic Wards) can change casting art in memory too. An effect's own light is kept across a re-find. a_loadArt: an
		// art's color may be read off its model (LightKit::ArtColor) - only at data load: a re-find during play runs under
		// gLock, which the game threads' light hooks take, so it uses only the colors read then.
		// -> how many matched by full path
		std::size_t FindTargets(bool a_loadArt)
		{
			std::unordered_map<RE::EffectSetting*, RE::TESObjectLIGH*> own;
			for (auto& t : gTargets) {
				own.emplace(t.effect, t.own);
			}
			std::unordered_set<const RE::TESObjectLIGH*> ours;
			for (auto* c : gCopies | std::views::values) {
				ours.insert(c);
			}
			for (auto* c : gAutoCopies | std::views::values) {
				ours.insert(c);
			}
			gTargets.clear();
			std::size_t                     byPath = 0;
			std::vector<RE::EffectSetting*> uncovered;
			const auto                      ownOf = [&](RE::EffectSetting* a_effect) {
				RE::TESObjectLIGH* mine = a_effect->data.light;
				if (auto it = own.find(a_effect); it != own.end()) {
					mine = it->second;
				} else if (ours.contains(mine)) {
					mine = nullptr;
				}
				return mine;
			};
			for (auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				if (!effect || !effect->data.castingArt) {
					continue;
				}
				const char* model = effect->data.castingArt->GetModel();
				if (!model || !*model) {
					continue;
				}
				// the full path first (a mod's own art under a shared file name), then the bare name
				auto key = PathKey(model);
				if (gCopies.contains(key)) {
					++byPath;
				} else {
					key = MeshKey(model);
				}
				if (!gCopies.contains(key)) {
					uncovered.push_back(effect);
					continue;
				}
				gTargets.push_back({ effect, ownOf(effect), std::move(key) });
			}
			// the automatic hand lights: the middle of the tuned hand lights (by fade x radius²), and each resistance group's
			// color as most of the tuned spells of that group wear it (every group - his ruling 2026-10-18; a spell no
			// resistance applies to is in none)
			std::vector<const Hand*> tuned;
			for (const auto& [key, list] : Hands()) {
				if (!list.empty()) {
					tuned.push_back(&list.front());
				}
			}
			std::ranges::sort(tuned, {}, [](const Hand* h) { return h->fade * h->radius * h->radius; });
			std::map<RE::ActorValue, std::map<std::uint32_t, std::size_t>> byGroup;
			const auto                                                     pack = [](const std::uint8_t a_rgb[3]) { return (std::uint32_t{ a_rgb[0] } << 16) | (std::uint32_t{ a_rgb[1] } << 8) | a_rgb[2]; };
			const auto                                                     packColor = [](const RE::Color& c) { return (std::uint32_t{ c.red } << 16) | (std::uint32_t{ c.green } << 8) | c.blue; };
			// counted off the data's colors (each key's first layer), not off the switches that are on now: a menu switch then
			// changes no group's color, so nothing goes stale between re-finds (CodeRabbit, RELight #27)
			for (const auto& t : gTargets) {
				if (const auto group = t.effect->data.resistVariable; group != RE::ActorValue::kNone) {
					if (const auto it = Hands().find(t.key); it != Hands().end() && !it->second.empty()) {
						++byGroup[group][pack(it->second.front().rgb)];
					}
				}
			}
			for (auto* effect : uncovered) {
				// a ward (its effect raises Ward Power) and any spell on a ward's hand art (named wardinhandfx - the game's, Dynamic
				// Wards' per rank, ours) are the wards' own: Dynamic Wards dresses every ward when it is installed, Wards.cpp when it
				// is not (Illuminated measured 32 ward effects on Dynamic Wards' uncolored art taking an orange / red light 2026-10-19)
				if (tuned.empty() || effect->data.castingType == RE::MagicSystem::CastingType::kConstantEffect ||
					effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight || SkippedPrefix(effect) ||
					effect->data.primaryAV == RE::ActorValue::kWardPower || MeshKey(effect->data.castingArt->GetModel()).contains("wardinhandfx")) {
					continue;
				}
				std::uint32_t rgb = 0;
				bool          found = true;
				const auto*   shot = effect->data.projectileBase ? effect->data.projectileBase->data.light : nullptr;
				const auto    group = effect->data.resistVariable != RE::ActorValue::kNone ? byGroup.find(effect->data.resistVariable) : byGroup.end();
				if (const auto* l = ownOf(effect)) {
					rgb = packColor(l->data.color);
				} else if (shot) {
					rgb = packColor(shot->data.color);
				} else if (const auto art = LightKit::ArtColor(effect->data.castingArt->GetModel(), a_loadArt)) {
					rgb = packColor(*art);
				} else if (group != byGroup.end() && !group->second.empty()) {
					rgb = std::ranges::max_element(group->second, {}, [](const auto& kv) { return kv.second; })->first;
				} else {
					found = false;
				}
				if (!found) {
					continue;  // no color to read: it keeps what it has
				}
				// a black light is an author's dark on purpose (Colorful Bound Weapons' black summon): black stays dark
				if (static_cast<int>((std::max)({ (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF })) < LightKit::kDarkTop) {
					continue;
				}
				auto key = std::format("{}{},{},{}", kAutoPrefix, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
				if (!gAutoHands.contains(key)) {
					Hand h = *tuned[tuned.size() / 2];
					h.key = key;
					h.rgb[0] = static_cast<std::uint8_t>(rgb >> 16);
					h.rgb[1] = static_cast<std::uint8_t>(rgb >> 8);
					h.rgb[2] = static_cast<std::uint8_t>(rgb);
					h.color = { h.rgb[0] / 255.0f, h.rgb[1] / 255.0f, h.rgb[2] / 255.0f };
					h.option = kNone;
					auto* copy = LightKit::NewForm<RE::TESObjectLIGH>();
					if (!copy) {
						continue;
					}
					Fill(copy, h);
					gAutoCopies.emplace(key, copy);
					gAutoHands.emplace(key, std::move(h));
				}
				gTargets.push_back({ effect, ownOf(effect), std::move(key), true });
			}
			// an effect whose casting art moved to a mesh no layer lights is no target now: it gets its own light back,
			// or it would keep our light for the art it no longer wears
			for (const auto& [effect, light] : own) {
				if (ours.contains(effect->data.light) &&
					std::ranges::none_of(gTargets, [effect](const Target& a_t) { return a_t.effect == effect; })) {
					effect->data.light = light;
				}
			}
			return byPath;
		}
	}

	void RefindHandLights()
	{
		std::size_t n = 0, byPath = 0;
		{
			std::lock_guard l{ gLock };
			if (gCopies.empty()) {
				return;
			}
			byPath = FindTargets(false);
			n = gTargets.size();
		}
		SKSE::log::info("hand lights: re-found - {} magic effect(s) wear one of their meshes ({} by full path)", n, byPath);
		ApplyHandLights(true);
	}

	void MakeHandLights()
	{
		// the lighting was read just before (Lighting.cpp ReadLighting): off Community Shaders the hand lights are drawn plain
		// (DressHandLight) and light the ground through their twins (GroundLights.cpp)
		std::size_t made = 0, failed = 0, byPath = 0;
		{
			std::lock_guard l{ gLock };
			gCopies.clear();
			gTargets.clear();
			for (const auto& [key, list] : Hands()) {
				if (list.empty()) {
					continue;
				}
				auto* copy = LightKit::NewForm<RE::TESObjectLIGH>();
				if (!copy) {
					++failed;
					continue;
				}
				Fill(copy, list.front());
				gCopies.emplace(key, copy);
				++made;
			}
			byPath = FindTargets(true);  // data load: no game thread runs a light hook yet
			gShadowTwins.clear();
			if (ShadowPatch()) {
				for (const auto* copies : { &gCopies, &gAutoCopies }) {
					for (auto* copy : *copies | std::views::values) {
						if (auto* twin = LightKit::NewForm<RE::TESObjectLIGH>()) {
							gShadowTwins.emplace(copy, twin);
						}
					}
				}
				SKSE::log::info("hand lights: the Bottled Shaders shadow download is installed - {} shadow twin(s) for her hands", gShadowTwins.size());
			}
		}
		SKSE::log::info(
			"hand lights: {} light(s) made in memory, {} failed; {} magic effect(s) wear one of their meshes ({} by "
			"their full art path); inverse square lighting {}",
			made, failed, gTargets.size(), byPath, InverseSquare() ? "found" : "not found");
		{
			std::lock_guard l{ gLock };
			const auto      autos = std::ranges::count_if(gTargets, [](const Target& t) { return t.automatic; });
			SKSE::log::info("automatic hand lights: {} spell(s) no layer lights get one, in {} color(s); setting {}", autos, gAutoCopies.size(),
				AutoLightsOn() ? "on" : "off");
			for (const auto& t : gTargets) {
				if (t.automatic) {
					SKSE::log::info("[HAND-AUTO] {:08X} {} | {}", t.effect->GetFormID(), t.effect->GetFullName(), t.key);
				}
			}
		}
		ApplyHandLights(true);
	}

	void ApplyHandLights(bool a_log)
	{
		std::size_t lit = 0, given = 0;
		{
			std::lock_guard l{ gLock };
			gInUse.clear();
			const bool on = HandLightsOn() && !AllLightsOff();
			for (auto& [key, copy] : gCopies) {
				if (const Hand* h = on ? Picked(Winner(key)) : nullptr) {
					Fill(copy, *h);
					gInUse[copy] = h;
				}
			}
			const bool autoOn = on && AutoLightsOn();
			for (auto& [key, copy] : gAutoCopies) {
				if (autoOn) {
					gInUse[copy] = &gAutoHands.find(key)->second;
				}
			}
			std::size_t autoLit = 0;
			for (auto& t : gTargets) {
				const auto& copies = t.automatic ? gAutoCopies : gCopies;
				const auto  found = copies.find(t.key);
				auto*       copy = found != copies.end() ? found->second : nullptr;
				auto*       want = copy && gInUse.contains(copy) ? copy : t.own;
				if (t.effect->data.light != want) {
					t.effect->data.light = want;
				}
				if (copy && want == copy) {
					++lit;
					autoLit += t.automatic ? 1 : 0;
				} else {
					++given;
				}
			}
			gLit = lit;
			gAutoLit = autoLit;
			// the lights already in a caster's hands take the change now; one whose key nothing lights any more goes out
			// until the next cast gives the effect its own light back
			for (auto& live : gLive) {
				const auto  autoIt = AutoKey(live.key) ? gAutoHands.find(live.key) : gAutoHands.end();
				const Hand* h = autoIt != gAutoHands.end() ? (autoOn ? &autoIt->second : nullptr) : on ? Picked(Winner(live.key)) :
				                                                                                         nullptr;
				if (h) {
					if (live.heldOut) {
						live.light->SetAppCulled(false);
						live.heldOut = false;
					}
					DressHandLight(live.light, *h);
					RememberLight(live.light, HandFxOf(h->key), Pulse::Kind::kNone, true);
				} else if (!live.heldOut) {
					live.light->SetAppCulled(true);
					live.heldOut = true;
				}
			}
		}
		if (a_log) {
			SKSE::log::info("hand lights: {} magic effect(s) lit by this mod, {} left with their own light", lit, given);
		}
	}

	bool ShadowPatch()
	{
		static const bool on = [] {
			std::error_code ec;
			return std::filesystem::exists("Data/SKSE/Plugins/RELightSpellLightingNG/Bottled Shaders Shadows.ini", ec);
		}();
		return on;
	}

	RE::TESObjectLIGH* ShadowTwinOf(RE::TESObjectLIGH* a_light)
	{
		std::lock_guard l{ gLock };
		const auto      it = a_light ? gShadowTwins.find(a_light) : gShadowTwins.end();
		if (it == gShadowTwins.end()) {
			return a_light;
		}
		// the copy's numbers as the menu last set them, plus the engine's omni shadow flag
		auto* twin = it->second;
		twin->data = a_light->data;
		twin->data.flags.set(RE::TES_LIGHT_FLAGS::kOmniShadow);
		twin->fade = a_light->fade;
		return twin;
	}

	const Hand* HandOfLight(const RE::TESObjectLIGH* a_light)
	{
		if (!a_light) {
			return nullptr;
		}
		std::lock_guard l{ gLock };
		auto            it = gInUse.find(a_light);
		return it == gInUse.end() ? nullptr : it->second;
	}

	// dressed as RE::Light dresses a plugin light: fade, reach with the size in z, color, and under Community Shaders
	// the inverse square flag and cutoff
	void DressHandLight(RE::NiLight* a_light, const Hand& a_hand)
	{
		if (!a_light) {
			return;
		}
		a_light->fadeAmount = kMovingLightMark;
		auto& data = a_light->GetLightRuntimeData();
		data.diffuse = a_hand.color;
		if (InverseSquare()) {
			data.fade = a_hand.fade;
			data.radius = { a_hand.radius, a_hand.radius, a_hand.size };
			if (a_hand.inverseSquare) {
				Isl::SetOn(a_light);
			}
			Isl::SetCutoff(a_light, a_hand.cutoff);
			return;
		}
		// ENB and Vanilla (his report: "relight casting light is not working"): the layer's light is made for
		// inverse square lighting, which the game's own lighting does not draw - so it is drawn plain, as Illuminated draws
		// its lights there (LightKit::PlainOf), with an ambient of a tenth of its color (RE::Light's rule, Truman)
		const auto plain = a_hand.inverseSquare ? LightKit::PlainOf(a_hand.fade, a_hand.radius, a_hand.cutoff, a_hand.size) :
		                                          LightKit::Plain{ a_hand.fade, a_hand.radius };
		data.fade = plain.fade;
		data.radius = { plain.radius, plain.radius, a_hand.size };
		if (auto* point = netimmerse_cast<RE::NiPointLight*>(a_light)) {
			point->SetLightAttenuation(plain.radius);
		}
		data.ambient = { a_hand.color.red * 0.1f, a_hand.color.green * 0.1f, a_hand.color.blue * 0.1f };  // after the attenuation's words
	}

	// main thread (main.cpp NoteMagicLight)
	void NoteHandLight(RE::NiLight* a_light, const std::string& a_key)
	{
		if (!a_light) {
			return;
		}
		KeepLight(a_light);
		std::lock_guard l{ gLock };
		gLive.push_back({ a_light, a_key });
	}

	void ForgetHandLights(const GoneLights& a_gone)
	{
		std::lock_guard l{ gLock };
		std::erase_if(gLive, [&](const Live& a_l) { return a_gone.contains(a_l.light); });
	}

	bool HandLightHeldOut(const RE::NiLight* a_light)
	{
		std::lock_guard l{ gLock };
		return std::ranges::any_of(gLive, [a_light](const Live& a_l) { return a_l.heldOut && a_l.light == a_light; });
	}

	std::size_t HandLightsMade()
	{
		std::lock_guard l{ gLock };
		return gCopies.size();
	}

	std::size_t HandEffects()
	{
		std::lock_guard l{ gLock };
		return gLit;
	}

	std::size_t AutoHandEffects()
	{
		std::lock_guard l{ gLock };
		return gAutoLit;
	}

}
