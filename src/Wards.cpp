// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The wards: one dome per ward, and the color pick. Everything here steps down when DynamicWards.dll is loaded, which
// dresses every ward itself (his call: "have the whole ward system step down if dynamic wards is present").
//
// ONE DOME. A ward spell's rank is the effect that accumulates Ward Power; every OTHER effect on the same spell that wears
// the vanilla ward art draws a second dome and a second hand light - vanilla's ShieldConcSelf, Mysticism's Dummy rows,
// Odin's Ward Regeneration (his capture: an orange 360 dome with a second dome inside, measured in his game:
// Lesser Ward fires Ward - Lesser and Odin's Ward Regeneration, both on WardBodyFX). Those lose their art and their light.
// An effect on a mod's own ward art (Better Vampires' blood ward) is never touched.
//
// THE COLOR (his call, "relight through skse menu"): Vanilla blue or White. The vanilla ward art forms are
// pointed at our meshes (`meshes\magic\Glow Wards\`): the dome (with 360 Ward the 360 sphere, in place of its orange), the
// hand art and 360 Ward's hit flash. Vanilla keeps the vanilla dome and hand, and gives the 360 sphere and flash the TRUE
// vanilla blue - `Glow Wards\Vanilla\`, the same files Illuminated uses (his answer 2026-10-13; the old `Blue\` sphere was
// Dynamic Wards' deeper Adept Blue) and a hand in the sphere's blue beside it. With Strange Wards the hand keeps the game's own
// art, which reads the gradient its pick ships. The hand light follows in HandLights.cpp. Nothing is saved.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t kShieldConcSelf = 0x0FCC62;  // Skyrim.esm
		constexpr std::uint32_t kDomeArt = 0x018124;         // Skyrim.esm WardHitEffect: magic\wardbodyfx.nif
		constexpr std::uint32_t kHandArt = 0x0253F1;         // Skyrim.esm: magic\wardinhandfx.nif
		constexpr std::uint32_t kFlashArt = 0x000802;        // 360 Ward.esp: magic\wardShieldHitFX.nif
		constexpr std::uint32_t kWardLight = 0x02F3EF;       // Skyrim.esm MagicLightWardHand01: every ward's own hand light
		constexpr const char*   k360Plugin = "360 Ward.esp";
		constexpr const char*   kEmptyModel = "Effects\\FXEmptyObject.nif";
		// our meshes, by look: White, or Vanilla blue - with 360 Ward its sphere and flash the TRUE vanilla blue (`Vanilla\`,
		// Illuminated's files), without it our vanilla-blue dome (`Blue\`, the vanilla look on our own texture copies)
		struct Look
		{
			const char* sphere;
			const char* flash;
			const char* dome;
			const char* hand;
			const char* wornDome;
			const char* wornHand;
			const char* sphereHand;  // the hands beside 360 Ward's sphere
			const char* sphereWornHand;
		};
		constexpr Look kWhite{ "Magic\\Glow Wards\\White\\wardbodyfx360.nif", "Magic\\Glow Wards\\White\\wardshieldhitfx.nif",
			"Magic\\Glow Wards\\White\\wardbodyfx.nif", "Magic\\Glow Wards\\White\\wardinhandfx.nif",
			"Magic\\Glow Wards\\White\\wardbodyfxworn.nif", "Magic\\Glow Wards\\White\\wardinhandfxworn.nif",
			"Magic\\Glow Wards\\White\\wardinhandfx.nif", "Magic\\Glow Wards\\White\\wardinhandfxworn.nif" };
		constexpr Look kVanillaBlue{ "Magic\\Glow Wards\\Vanilla\\wardbodyfx360.nif", "Magic\\Glow Wards\\Vanilla\\wardshieldhitfx.nif",
			"Magic\\Glow Wards\\Blue\\wardbodyfx.nif", "Magic\\Glow Wards\\Blue\\wardinhandfx.nif",
			"Magic\\Glow Wards\\Blue\\wardbodyfxworn.nif", "Magic\\Glow Wards\\Blue\\wardinhandfxworn.nif",
			"Magic\\Glow Wards\\Vanilla\\wardinhandfx.nif", "Magic\\Glow Wards\\Vanilla\\wardinhandfxworn.nif" };

		RE::BGSArtObject*                                  gEmpty = nullptr;
		std::vector<RE::EffectSetting*>                    gSilenced;
		std::unordered_map<RE::BGSArtObject*, std::string> gOriginal;  // an art form's model before we pointed it at ours
		bool                                               gSteppedDownLogged = false;
		std::optional<RE::Color>                           gWardLightColor;  // MagicLightWardHand01's own, before White

		bool DynamicWardsLoaded() { return REX::W32::GetModuleHandleA("DynamicWards.dll") != nullptr; }

		// HIS REPORT (Spellbreaker - the old fix, lost in a handoff purge): a WORN ward - Spellbreaker, the Divine
		// Crusader shield, the Shield of Reman - never drives a cast ward's transitions, so the vanilla meshes sit idle: only
		// the swirl, and the dome flashes as the ward ends. They wear copies of our dome and hand on Magic\\BeginLoopEnd.hkx
		// (`wardgen.py warddomes`), in art forms of their own, so the cast wards keep theirs.
		struct WornRow
		{
			const char* plugin{ nullptr };
			RE::FormID  id{ 0 };
		};
		constexpr WornRow kWorn[] = {
			{ "Skyrim.esm", 0x07DCDB },                        // Spellbreaker's Ward - Deflection
			{ "ccmtysse001-knightsofthenine.esl", 0x00081D },  // the Divine Crusader shield's
			{ "LegacyoftheDragonborn.esm", 0x124E5B },         // Reman's Ward
		};
		RE::BGSArtObject* gWornDome = nullptr;
		RE::BGSArtObject* gWornHand = nullptr;
		// the worn wards' models, loaded once and held for the session: read off disk when a worn ward came up, its dome showed late
		std::vector<RE::NiPointer<RE::NiNode>> gWornModels;

		// the vanilla ward art, by the bare mesh name - also once we have pointed it at our own copies, which keep the names
		bool IsVanillaWardArt(const RE::BGSArtObject* a_art)
		{
			const char* model = a_art ? a_art->GetModel() : nullptr;
			if (!model || !*model) {
				return false;
			}
			const auto key = MeshKey(model);
			return key == "wardinhandfx" || key == "wardbodyfx" || key == "wardbodyfx360";
		}

		bool WearsWardArt(const RE::EffectSetting* a_e)
		{
			return a_e && (IsVanillaWardArt(a_e->data.castingArt) || IsVanillaWardArt(a_e->data.hitEffectArt));
		}

		bool IsRank(const RE::EffectSetting* a_e)
		{
			return a_e->data.archetype == RE::EffectArchetypes::ArchetypeID::kAccumulateMagnitude &&
			       a_e->data.primaryAV == RE::ActorValue::kWardPower;
		}

		RE::BGSArtObject* Empty()
		{
			if (!gEmpty) {
				gEmpty = LightKit::NewForm<RE::BGSArtObject>();
				if (gEmpty) {
					gEmpty->SetModel(kEmptyModel);
				}
			}
			return gEmpty;
		}

		// every effect that shares a spell with a ward rank and wears the vanilla ward art without being a rank itself
		void FindExtras()
		{
			std::unordered_set<RE::EffectSetting*> extras(gSilenced.begin(), gSilenced.end());
			auto*                                  dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			if (auto* shield = dh->LookupForm<RE::EffectSetting>(kShieldConcSelf, "Skyrim.esm"); shield && WearsWardArt(shield)) {
				extras.insert(shield);
			}
			for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
				if (!spell) {
					continue;
				}
				bool                            rank = false;
				std::vector<RE::EffectSetting*> others;
				for (auto* effect : spell->effects) {
					auto* base = effect ? effect->baseEffect : nullptr;
					if (!WearsWardArt(base)) {
						continue;
					}
					if (IsRank(base)) {
						rank = true;
					} else {
						others.push_back(base);
					}
				}
				if (rank) {
					extras.insert(others.begin(), others.end());
				}
			}
			// one dome on a shield too: Artificer's Spellbreaker enchantment carries the worn ward beside two effects of its own in the vanilla ward art (Ward
			// Power, Stamina), which drew two idle domes and two more hand lights. An effect a castable spell also carries keeps its art
			std::unordered_set<const RE::EffectSetting*> worn;
			for (const auto& r : kWorn) {
				if (const auto* e = dh->LookupForm<RE::EffectSetting>(r.id, r.plugin)) {
					worn.insert(e);
				}
			}
			std::unordered_set<const RE::EffectSetting*> castable;
			for (const auto* spell : dh->GetFormArray<RE::SpellItem>()) {
				if (spell && spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
					for (const auto* effect : spell->effects) {
						if (effect && effect->baseEffect) {
							castable.insert(effect->baseEffect);
						}
					}
				}
			}
			for (auto* ench : dh->GetFormArray<RE::EnchantmentItem>()) {
				if (!ench || std::ranges::none_of(ench->effects, [&worn](const RE::Effect* a_e) { return a_e && worn.contains(a_e->baseEffect); })) {
					continue;
				}
				for (auto* effect : ench->effects) {
					auto* base = effect ? effect->baseEffect : nullptr;
					if (base && !worn.contains(base) && !castable.contains(base) && WearsWardArt(base)) {
						extras.insert(base);
					}
				}
			}
			gSilenced.assign(extras.begin(), extras.end());
		}

		std::size_t Silence()
		{
			auto*       empty = Empty();
			std::size_t changed = 0;
			if (!empty) {
				return changed;
			}
			for (auto* e : gSilenced) {
				auto& d = e->data;
				if (d.castingArt != empty || d.hitEffectArt != empty || d.enchantEffectArt || d.light) {
					++changed;
				}
				d.castingArt = empty;
				d.hitEffectArt = empty;
				d.enchantEffectArt = nullptr;
				d.light = nullptr;  // a second hand light (vanilla's white MagicLightWardHand01) goes with the second dome
			}
			return changed;
		}

		// main thread, data load; not on VR (kept SE / AE only, untested there)
		void HoldWornModels()
		{
			if (REL::Module::IsVR() || !gWornModels.empty()) {
				return;
			}
			for (const auto* model : { kVanillaBlue.wornHand, kVanillaBlue.sphereWornHand, kVanillaBlue.wornDome, kWhite.wornHand, kWhite.wornDome }) {
				RE::NiPointer<RE::NiNode> root;
				if (RE::BSModelDB::Demand(model, root, RE::BSModelDB::DBTraits::ArgsType{}) == RE::BSResource::ErrorCode::kNone && root) {
					gWornModels.push_back(std::move(root));
				} else {
					SKSE::log::warn("wards: {} could not be loaded ahead - a worn ward reads it when it comes up", model);
				}
			}
			SKSE::log::info("wards: {} worn ward model(s) held loaded for the session", gWornModels.size());
		}

		// Strange Wards (a vanilla ward replacer, Nexus 139921) puts its own dome at the vanilla mesh path; its textures sit
		// under effects\po3_strangerunes\. Its picks differ only by the dome's base color (read off its three meshes).
		struct StrangePick
		{
			RE::NiColor base;
			const char* name{ nullptr };
			bool        tinted{ false };  // our ward light takes its color; the Blue pick keeps the vanilla blue-white
			const char* hand{ nullptr };  // our hand art in its color: the game's keeps blue particles no gradient reaches
		};
		constexpr std::array kStrangePicks{ StrangePick{ { 0.098f, 0.592f, 0.902f }, "the Blue pick", false },
			StrangePick{ { 0.886f, 0.345f, 0.133f }, "the Amber pick", true, "Magic\\Glow Wards\\Strange Amber\\wardinhandfx.nif" },
			StrangePick{ { 0.935f, 0.869f, 0.843f }, "the Sswaye Special pick", true, "Magic\\Glow Wards\\Strange Sswaye\\wardinhandfx.nif" } };

		struct StrangeWards
		{
			bool                       installed{ false };
			const StrangePick*         pick{ nullptr };  // none = a pick not known
			std::optional<RE::NiColor> color;            // none = the vanilla blue-white
		};

		[[nodiscard]] bool Near(const RE::NiColor& a_a, const RE::NiColor& a_b)
		{
			return std::abs(a_a.red - a_b.red) < 0.02f && std::abs(a_a.green - a_b.green) < 0.02f && std::abs(a_a.blue - a_b.blue) < 0.02f;
		}

		// every effect-shader material of a model; false when it does not load. The model loader resolves on VR too (its id is in
		// the VR Address Library), as Dynamic Wards' VR build uses it
		bool ForEachEffectMaterial(const char* a_model, const std::function<void(RE::BSEffectShaderMaterial&)>& a_fn)
		{
			RE::NiPointer<RE::NiNode> root;
			if (!a_model || !*a_model || RE::BSModelDB::Demand(a_model, root, RE::BSModelDB::DBTraits::ArgsType{}) != RE::BSResource::ErrorCode::kNone || !root) {
				return false;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* a_geometry) {
				auto* shader = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(shader);
				if (auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->material) : nullptr) {
					a_fn(*material);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return true;
		}

		StrangeWards ReadStrangeWards()
		{
			StrangeWards s;
			ForEachEffectMaterial("magic\\wardbodyfx.nif", [&](RE::BSEffectShaderMaterial& a_material) {
				if (!LightKit::Lower(a_material.sourceTexturePath.c_str()).contains("po3_strangerunes") &&
					!LightKit::Lower(a_material.greyscaleTexturePath.c_str()).contains("po3_strangerunes")) {
					return;  // only Strange Wards' own blocks tell its pick
				}
				s.installed = true;
				const RE::NiColor base{ a_material.baseColor.red, a_material.baseColor.green, a_material.baseColor.blue };
				if (!s.pick) {
					const auto found = std::ranges::find_if(kStrangePicks, [&](const StrangePick& a_pick) { return Near(base, a_pick.base); });
					s.pick = found != kStrangePicks.end() ? &*found : nullptr;  // the first block that names a pick decides
				}
			});
			if (s.pick && s.pick->tinted) {
				s.color = s.pick->base;
			}
			return s;
		}

		const StrangeWards& StrangeWardsLook()
		{
			static const StrangeWards look = [] {
				auto s = ReadStrangeWards();
				SKSE::log::info("wards: Strange Wards is {}{}", s.installed ? "installed - its dome keeps its own look, " : "not installed",
					!s.installed ? "" : s.pick ? s.pick->name :
												 "a pick not known (the vanilla blue-white light kept)");
				return s;
			}();
			return look;
		}

		// 360 Ward's own orange, the base color of its sphere's and flash's coloured blocks (read off its meshes 2026-11-02)
		constexpr RE::NiColor kStockOrange{ 1.0f, 0.6f, 0.3f };

		[[nodiscard]] bool NearStock(const RE::NiColor& a_color)
		{
			return std::abs(a_color.red - kStockOrange.red) < 0.05f && std::abs(a_color.green - kStockOrange.green) < 0.05f &&
			       std::abs(a_color.blue - kStockOrange.blue) < 0.05f;
		}

		// a block that carries a hue (a pale tint too), not a white or grey one
		[[nodiscard]] bool Coloured(const RE::NiColor& a_color)
		{
			const float hi = (std::max)({ a_color.red, a_color.green, a_color.blue });
			const float lo = (std::min)({ a_color.red, a_color.green, a_color.blue });
			return hi > 0.0f && (hi - lo) / hi > 0.05f;
		}

		// 360 Ward - Other colors (Nexus 156803) recolors 360 Ward's sphere in place (Illuminated's way): the sphere is left as it is
		// and the ward's light takes its color; its hit flash too when that file is recolored, else the flash is our vanilla blue
		struct Other360
		{
			std::optional<RE::NiColor> color;           // none = 360 Ward's own orange, or no 360 Ward
			bool                       flash{ false };  // its hit flash recolored as well
		};

		// a model that is 360 Ward's, recolored: no block keeps its orange. Its color = its coloured blocks' (a White recolor has
		// none: all its blocks). None for our own copies, Strange Wards' dome (the same path, handled apart) or a stock model
		[[nodiscard]] std::optional<RE::NiColor> RecoloredAt(const char* a_model)
		{
			if (!a_model || LightKit::Lower(a_model).contains("glow wards")) {
				return std::nullopt;
			}
			RE::NiColor all{}, hue{};
			int         blocks = 0, hued = 0;
			bool        stock = false, strange = false;
			const bool  loaded = ForEachEffectMaterial(a_model, [&](RE::BSEffectShaderMaterial& a_material) {
				const RE::NiColor c{ a_material.baseColor.red, a_material.baseColor.green, a_material.baseColor.blue };
				strange = strange || LightKit::Lower(a_material.sourceTexturePath.c_str()).contains("po3_strangerunes") ||
				          LightKit::Lower(a_material.greyscaleTexturePath.c_str()).contains("po3_strangerunes");
				stock = stock || NearStock(c);
				all.red += c.red;
				all.green += c.green;
				all.blue += c.blue;
				++blocks;
				if (Coloured(c)) {
					hue.red += c.red;
					hue.green += c.green;
					hue.blue += c.blue;
					++hued;
				}
			});
			if (!loaded || strange || stock || blocks == 0) {
				return std::nullopt;
			}
			const auto& sum = hued ? hue : all;
			const float n = static_cast<float>(hued ? hued : blocks);
			return RE::NiColor{ sum.red / n, sum.green / n, sum.blue / n };
		}

		// the model the load order gave an art form, also once Point has pointed it at ours
		[[nodiscard]] const char* OriginalModel(RE::BGSArtObject* a_art)
		{
			if (!a_art) {
				return nullptr;
			}
			const auto it = gOriginal.find(a_art);
			return it != gOriginal.end() ? it->second.c_str() : a_art->GetModel();
		}

		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		[[nodiscard]] bool Has360()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName(k360Plugin) || dh->LookupLoadedLightModByName(k360Plugin));
		}

		// read once the data has loaded (the dome's form resolves), off the models the load order gives the dome and flash, whoever
		// asks first (Paint or a hand light); asked earlier it answers "no recolor" without keeping that answer
		Other360 Other360Look()
		{
			// a hand light can ask from a loader thread while Paint asks on the main thread
			static std::mutex              lock;
			static std::optional<Other360> look;
			const std::scoped_lock         guard(lock);
			if (look) {
				return *look;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			auto* dome = dh ? dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm") : nullptr;
			if (!dome) {
				return {};
			}
			Other360 o;
			if (Has360()) {
				auto* flash = dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin);
				o.color = RecoloredAt(OriginalModel(dome));
				o.flash = o.color && flash && RecoloredAt(OriginalModel(flash));
				SKSE::log::info("wards: 360 Ward's sphere {}", o.color ? "is recolored by another mod - left as it is, the ward's light in its color" : "is its own");
			}
			look = o;
			return o;
		}

		// point an art form at our copy, or back at the model it had
		void Point(RE::BGSArtObject* a_art, const char* a_ours)
		{
			if (!a_art) {
				return;
			}
			auto it = gOriginal.find(a_art);
			if (it == gOriginal.end()) {
				const char* now = a_art->GetModel();
				it = gOriginal.emplace(a_art, now ? now : "").first;
			}
			const std::string want = a_ours ? std::string(a_ours) : it->second;
			const char*       now = a_art->GetModel();
			if (!now || want != now) {
				a_art->SetModel(want.c_str());
			}
		}

		void Paint()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			const bool     has360 = Has360();
			const bool     white = WardColor() == 1;
			auto*          dome = dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm");
			auto*          hand = dh->LookupForm<RE::BGSArtObject>(kHandArt, "Skyrim.esm");
			auto*          flash = has360 ? dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin) : nullptr;
			const Look&    look = white ? kWhite : kVanillaBlue;
			const Other360 other = Other360Look();  // empty without 360 Ward
			if (has360 && other.color) {
				Point(dome, nullptr);
				Point(flash, other.flash ? nullptr : kVanillaBlue.flash);  // the stock orange flash would not match it; the pick is hidden here
			} else if (has360) {
				Point(dome, look.sphere);
				Point(flash, look.flash);
			} else if (StrangeWardsLook().installed) {
				Point(dome, nullptr);  // Strange Wards' own dome, whatever the color pick
			} else {
				// his call: Vanilla blue wears OUR dome too - the vanilla look, reading our own copies of its
				// textures, so no other mod's loose textures reach it
				Point(dome, look.dome);
			}
			const auto* strangePick = StrangeWardsLook().pick;
			Point(hand, has360 ? (other.color ? nullptr : look.sphereHand) : !StrangeWardsLook().installed ? look.hand :
																		 strangePick                       ? strangePick->hand :
																											 nullptr);
			// the worn wards: their own hand always (its graph has the same fault on 360), their own dome unless 360 Ward's
			// sphere - already on BeginLoopEnd - is the dome
			if (!gWornDome) {
				gWornDome = LightKit::NewForm<RE::BGSArtObject>();
			}
			if (!gWornHand) {
				gWornHand = LightKit::NewForm<RE::BGSArtObject>();
			}
			if (gWornDome && gWornHand) {
				gWornDome->SetModel(look.wornDome);
				gWornHand->SetModel(has360 && !other.color ? look.sphereWornHand : look.wornHand);
				std::size_t worn = 0;
				for (const auto& r : kWorn) {
					if (auto* e = dh->LookupForm<RE::EffectSetting>(r.id, r.plugin)) {
						e->data.castingArt = gWornHand;
						e->data.hitEffectArt = has360 ? dome : gWornDome;
						++worn;
					}
				}
				HoldWornModels();
				SKSE::log::info("wards: {} worn ward(s) (Spellbreaker, Crusader, Reman) wear the looping copies", worn);
			}
			// the game's own ward hand light, where no installed layer lights the ward hand (HandLights.cpp does where one does)
			if (auto* light = dh->LookupForm<RE::TESObjectLIGH>(kWardLight, "Skyrim.esm")) {
				if (!gWardLightColor) {
					gWardLightColor = light->data.color;
				}
				const auto& strange = StrangeWardsLook();
				// a Strange Wards color exists only while it is installed
				if (const auto tint = has360 ? other.color : strange.color; tint) {
					const auto byte = [](float a_value) { return static_cast<std::uint8_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f)); };
					light->data.color = RE::Color{ byte(tint->red), byte(tint->green), byte(tint->blue), 0 };
				} else {
					// Strange Wards' Blue pick keeps its own blue dome, so its light stays the game's blue whatever the look
					light->data.color = white && !(strange.installed && !has360) ? RE::Color{ 255, 255, 255, 0 } : *gWardLightColor;
				}
			}
			SKSE::log::info("wards: {} {} ({}), hand {}", has360 ? "360 sphere" : "dome",
				other.color ? "in its own recolor" : white ? "white" :
															 "vanilla blue",
				dome && dome->GetModel() ? dome->GetModel() : "no art form", hand && hand->GetModel() ? hand->GetModel() : "no art form");
		}
	}

	bool WardsSteppedDown() { return DynamicWardsLoaded(); }

	bool StrangeWardsHandShows()
	{
		static const bool shows = [] {
			return !Has360() && StrangeWardsLook().installed;
		}();
		return shows;
	}

	std::optional<RE::NiColor> StrangeWardsPick() { return StrangeWardsLook().color; }

	std::optional<RE::NiColor> Other360Color()
	{
		return DynamicWardsLoaded() ? std::nullopt : Other360Look().color;
	}

	void ApplyWards(const char* a_why)
	{
		if (DynamicWardsLoaded()) {
			if (!gSteppedDownLogged) {
				gSteppedDownLogged = true;
				SKSE::log::info("wards: Dynamic Wards is loaded - it dresses the wards, so this plugin leaves them alone");
			}
			return;
		}
		FindExtras();
		const auto changed = Silence();
		SKSE::log::info("wards: {} effect(s) wear the ward art beside a ward's rank and show no second dome ({} changed; {})",
			gSilenced.size(), changed, a_why);
		Paint();
	}

	// ------------------------------------------------------------------ the shield's ward at once
	// A worn ward comes up through its enchantment, whose effects carry the condition IsBlocking == 1; the game re-checks an active effect's conditions
	// only on its own timer (fActiveEffectConditionUpdateInterval, measured on Illuminated: the dome showed 0.1-1.0 s after the block). So when her block starts,
	// each of her active effects from an ENCHANTMENT whose conditions ask IsBlocking is checked at once (ActiveEffect::EvaluateConditions, the effect's own
	// virtual), and once more on the next frame in case the game has not yet marked her blocking. The wait is the game's, not the dome's, so this runs with or
	// without Dynamic Wards. Illuminated's Wards.cpp, the same way.
	namespace
	{
		// the animation event her graph sends as a block begins ("blockStart" is the input event's name and never reaches the graph)
		constexpr std::string_view kBlockTag = "blockStartOut";

		[[nodiscard]] bool WaitsForBlock(const RE::ActiveEffect* a_effect)
		{
			using Flag = RE::ActiveEffect::Flag;
			if (!a_effect || !a_effect->effect || !a_effect->spell || a_effect->flags.none(Flag::kHasConditions) ||
				a_effect->flags.any(Flag::kDispelled) || a_effect->spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
				return false;
			}
			for (const auto* item = a_effect->effect->conditions.head; item; item = item->next) {
				if (item->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kIsBlocking) {
					return true;
				}
			}
			return false;
		}

		class ShieldWards : public RE::MagicTarget::ForEachActiveEffectVisitor
		{
		public:
			RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
			{
				if (WaitsForBlock(a_effect)) {
					found.push_back(a_effect);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			std::vector<RE::ActiveEffect*> found;
		};

		// main thread: collected first, then checked, so the list is never walked while an effect changes state
		void RecheckShieldWards()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* target = player ? player->AsMagicTarget() : nullptr;
			if (!target) {
				return;
			}
			ShieldWards wards;
			target->VisitEffects(wards);
			for (auto* effect : wards.found) {
				effect->EvaluateConditions(0.0f, true);
			}
		}

		class BlockWatch : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				LightKit::Guarded("block watch", [a_event]() {
					const std::string_view tag = a_event ? a_event->tag.c_str() : "";
					if (!LightKit::SameText(tag, kBlockTag)) {
						return;
					}
					// the graph's events may come off the main thread: the effects are read on it
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([]() {
							LightKit::Guarded("shield ward check", []() {
								RecheckShieldWards();
								// once more a frame later: IsBlocking may only read true after this frame's update
								if (auto* again = SKSE::GetTaskInterface()) {
									again->AddTask([]() { LightKit::Guarded("shield ward check (next frame)", RecheckShieldWards); });
								}
							});
						});
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		BlockWatch gBlockWatch;
		// the graph the watch is on (main thread only): a transformation or a 3D reset builds her a new one. Held, so a new graph can never take its address
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> gWatchedGraph;

		void CheckBlockGraph()
		{
			auto*                                            player = RE::PlayerCharacter::GetSingleton();
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
			if (player && player->GetAnimationGraphManager(graph) && graph && graph.get() != gWatchedGraph.get()) {
				WatchBlocking();
			}
		}

		void NoteGraphLook()
		{
			static std::atomic<bool> queued{ false };
			auto*                    tasks = SKSE::GetTaskInterface();
			if (tasks && !queued.exchange(true)) {
				tasks->AddTask([]() {
					queued = false;
					LightKit::Guarded("block watch", []() { CheckBlockGraph(); });
				});
			}
		}

		// her graph is built with her 3D: SKSE's 3D-update event, her load event, a race change. She cannot block before she draws, so her own draw looks too
		class GraphWatch final :
			public RE::BSTEventSink<SKSE::NiNodeUpdateEvent>,
			public RE::BSTEventSink<SKSE::ActionEvent>,
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::NiNodeUpdateEvent* a_event, RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*) override
			{
				if (a_event && a_event->reference && a_event->reference->IsPlayerRef()) {
					NoteGraphLook();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				using T = SKSE::ActionEvent::Type;
				if (a_event && a_event->actor && a_event->actor->IsPlayerRef() && (a_event->type.get() == T::kBeginDraw || a_event->type.get() == T::kEndDraw)) {
					NoteGraphLook();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_event && a_event->loaded && a_event->formID == 0x14) {
					NoteGraphLook();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSwitchRaceCompleteEvent* a_event, RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override
			{
				if (a_event && a_event->subject && a_event->subject->IsPlayerRef()) {
					NoteGraphLook();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	// a game loaded or a new one started (main.cpp), or her graph was rebuilt (GraphWatch): the watch goes on the graph she has, taken off first so it is never on twice
	void WatchBlocking()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		static GraphWatch watch;
		static const bool watching = [] {
			if (auto* updates = SKSE::GetNiNodeUpdateEventSource()) {
				updates->AddEventSink(&watch);
			}
			if (auto* actions = SKSE::GetActionEventSource()) {
				actions->AddEventSink(&watch);
			}
			if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
				events->AddEventSink<RE::TESObjectLoadedEvent>(&watch);
				events->AddEventSink<RE::TESSwitchRaceCompleteEvent>(&watch);
			}
			return true;
		}();
		static_cast<void>(watching);
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
		player->GetAnimationGraphManager(graph);
		player->RemoveAnimationGraphEventSink(&gBlockWatch);
		const bool added = graph && player->AddAnimationGraphEventSink(&gBlockWatch);
		gWatchedGraph = added ? graph : nullptr;
		SKSE::log::info("wards: the block watch is {} the player's animation graph", added ? "on" : "NOT on (no graph yet)");
	}
}
