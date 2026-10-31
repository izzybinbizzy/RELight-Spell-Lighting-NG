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

		// the model loader resolves on VR too (its id is in the VR Address Library), as Dynamic Wards' VR build uses it
		StrangeWards ReadStrangeWards()
		{
			StrangeWards              s;
			RE::NiPointer<RE::NiNode> root;
			if (RE::BSModelDB::Demand("magic\\wardbodyfx.nif", root, RE::BSModelDB::DBTraits::ArgsType{}) != RE::BSResource::ErrorCode::kNone || !root) {
				return s;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* a_geometry) {
				auto* shader = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(shader);
				auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->material) : nullptr;
				if (!material || (!LightKit::Lower(material->sourceTexturePath.c_str()).contains("po3_strangerunes") &&
									 !LightKit::Lower(material->greyscaleTexturePath.c_str()).contains("po3_strangerunes"))) {
					return RE::BSVisit::BSVisitControl::kContinue;  // only Strange Wards' own blocks tell its pick
				}
				s.installed = true;
				const RE::NiColor base{ material->baseColor.red, material->baseColor.green, material->baseColor.blue };
				if (!s.pick) {
					const auto found = std::ranges::find_if(kStrangePicks, [&](const StrangePick& a_pick) { return Near(base, a_pick.base); });
					s.pick = found != kStrangePicks.end() ? &*found : nullptr;  // the first block that names a pick decides
				}
				return RE::BSVisit::BSVisitControl::kContinue;
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
			// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
			const bool  has360 = dh->LookupLoadedModByName(k360Plugin) || dh->LookupLoadedLightModByName(k360Plugin);
			const bool  white = WardColor() == 1;
			auto*       dome = dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm");
			auto*       hand = dh->LookupForm<RE::BGSArtObject>(kHandArt, "Skyrim.esm");
			auto*       flash = has360 ? dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin) : nullptr;
			const Look& look = white ? kWhite : kVanillaBlue;
			if (has360) {
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
			Point(hand, has360 ? look.sphereHand : !StrangeWardsLook().installed ? look.hand :
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
				gWornHand->SetModel(has360 ? look.sphereWornHand : look.wornHand);
				std::size_t worn = 0;
				for (const auto& r : kWorn) {
					if (auto* e = dh->LookupForm<RE::EffectSetting>(r.id, r.plugin)) {
						e->data.castingArt = gWornHand;
						e->data.hitEffectArt = has360 ? dome : gWornDome;
						++worn;
					}
				}
				SKSE::log::info("wards: {} worn ward(s) (Spellbreaker, Crusader, Reman) wear the looping copies", worn);
			}
			// the game's own ward hand light, where no installed layer lights the ward hand (HandLights.cpp does where one does)
			if (auto* light = dh->LookupForm<RE::TESObjectLIGH>(kWardLight, "Skyrim.esm")) {
				if (!gWardLightColor) {
					gWardLightColor = light->data.color;
				}
				if (const auto& strange = StrangeWardsLook(); strange.installed && strange.color && !has360) {
					const auto byte = [](float a_value) { return static_cast<std::uint8_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f)); };
					light->data.color = RE::Color{ byte(strange.color->red), byte(strange.color->green), byte(strange.color->blue), 0 };
				} else {
					// Strange Wards' Blue pick keeps its own blue dome, so its light stays the game's blue whatever the look
					light->data.color = white && !(strange.installed && !has360) ? RE::Color{ 255, 255, 255, 0 } : *gWardLightColor;
				}
			}
			SKSE::log::info("wards: {} {} ({}), hand {}", has360 ? "360 sphere" : "dome", white ? "white" : "vanilla blue",
				dome && dome->GetModel() ? dome->GetModel() : "no art form", hand && hand->GetModel() ? hand->GetModel() : "no art form");
		}
	}

	bool WardsSteppedDown() { return DynamicWardsLoaded(); }

	bool StrangeWardsHandShows()
	{
		static const bool shows = [] {
			auto*      dh = RE::TESDataHandler::GetSingleton();
			const bool has360 = dh && (dh->LookupLoadedModByName(k360Plugin) || dh->LookupLoadedLightModByName(k360Plugin));
			return !has360 && StrangeWardsLook().installed;
		}();
		return shows;
	}

	std::optional<RE::NiColor> StrangeWardsPick() { return StrangeWardsLook().color; }

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
}
