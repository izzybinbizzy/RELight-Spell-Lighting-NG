// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The switches: every tick-box option the installer put down gets one, and a switched-off option's lights are
// put out while the game runs.
//
// How a light is matched to an option:
//   - RE::Light names every light it makes from a config "RL" + the node it hung it on (LightManager.cpp),
//     so the two letters leave every other light in the game alone;
//   - the light hangs under the object's own 3D, so walking up its parents to the reference gives the object;
//   - the reference's base form is what the data files name, by form or by mesh key (Data.cpp);
//   - an ENCHANTMENT light is the exception: RE::Light hangs it on the weapon node of the ACTOR holding the weapon
//     (ShaderReferenceEffect::Init, LightAttachmentHooks.cpp) and marks it `fadeAmount = 5`, so the walk up finds the
// actor, whose base no data file names - his report: "vibrant weapons and vaer are saying 0 lit". Each
//     frame every live effect shader of ours is walked the way RE::Light attached to it, and the marked lights under
//     its root belong to the option whose data names that shader's editor ID.
//
// It never edits a config, a record or a file. A light it puts out is held in a list and given back the
// moment the switch is turned on again.
//
// THREADS: the lists are the main thread's only. A 3D-load hook (a loader thread) puts a switched-off option's lights out
// at once and hands them over (CullOptionLightsUnder -> AdoptOptionLights, an SKSE task). The per-frame walk remembers what
// it worked out about each light (gKnown) and asks again only when that light is new to it, or is a different light at
// the same address (its name or parent changed) - the walk up to the reference and the data lookup are not done for every
// light every frame.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		struct Held
		{
			RE::NiLight* light{ nullptr };  // held in Keep.cpp; ForgetOptionLights drops it here before it is freed
			std::size_t  option{ 0 };
			bool         enchant{ false };
		};
		std::vector<Held>                      gHeldOut;
		std::unordered_set<const RE::NiLight*> gHeldSet;  // the same lights, to answer "is it held out" without a walk
		// this frame's lights, sorted by light once the walk is done (FindEnchantLights) and searched by halves; the storage is
		// kept from frame to frame
		std::vector<std::pair<const RE::NiLight*, std::size_t>> gEnchant;  // enchantment lights -> option
		// this frame's flesh-spell lights under FleshFX -> the hue they are drawn in (LightKit.h FleshFx; Brightness.cpp)
		std::vector<std::pair<const RE::NiLight*, const RE::NiColor*>> gFlesh;
		std::unordered_set<const RE::TESEffectShader*>                 gFleshMissLogged;  // flesh shaders whose light was not found, said once each
		std::vector<RE::NiLight*>                                      gSheathedOut;      // held in Keep.cpp; put out by the sheathed net below

		// what the per-frame walk worked out about a light, with what identifies it (a freed light's address can be reused)
		struct Known
		{
			const void*   name{ nullptr };    // the light's name text (RE::Light's "RL..." name is set once, at creation)
			const void*   parent{ nullptr };  // the node it hangs on
			std::size_t   option{ kNone };    // kNone: not one of ours (or not RE::Light's)
			std::uint32_t frame{ 0 };         // last seen; a light unseen for a while is forgotten
		};
		std::unordered_map<const RE::NiLight*, Known> gKnown;
		std::uint32_t                                 gFrame = 0;

		// where RE::Light attached for this effect (GetReferenceAttachRoot, then the third-person node of the same name)
		RE::NiAVObject* EnchantRootOf(RE::ShaderReferenceEffect* a_effect)
		{
			if (const auto* weap = skyrim_cast<RE::WeaponEnchantmentController*>(a_effect->controller); weap && !weap->shader) {
				return nullptr;
			}
			auto*      root = a_effect->GetAttachRoot();
			const auto ref = a_effect->target.get();
			if (!root || !ref) {
				return nullptr;
			}
			if (auto* third = ref->Get3D(false)) {
				if (auto* same = third->GetObjectByName(root->name)) {
					root = same;
				}
			}
			return root;
		}

		// one entry per light, found by halves: a big fight has hundreds
		template <class T>
		[[nodiscard]] const T* Find(const std::vector<std::pair<const RE::NiLight*, T>>& a_list, const RE::NiLight* a_light)
		{
			const auto it = std::ranges::lower_bound(a_list, a_light, {}, &std::pair<const RE::NiLight*, T>::first);
			return it != a_list.end() && it->first == a_light ? &it->second : nullptr;
		}

		// this frame's enchantment lights of ours, sorted by light so each pass finds one by halves (a big fight has hundreds).
		// A flesh spell's shader (FleshFX's look winning on it) hangs on the caster's body: RE::Light puts a config with no
		// attach path straight under the effect's root (found the way RE::Light finds it, EnchantRootOf), so only a light
		// hanging DIRECTLY on that root is its light - an enchanted weapon's light deeper under the same actor keeps its color.
		void FindEnchantLights()
		{
			gEnchant.clear();
			gFlesh.clear();
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			lists->ForEachShaderEffect([](RE::ShaderReferenceEffect* a_effect) {
				const auto opt = a_effect ? OptionOfShader(a_effect->effectData) : kNone;
				auto*      root = opt != kNone ? EnchantRootOf(a_effect) : nullptr;
				if (root) {
					const auto* hue = LightKit::FleshFx::HueOf(a_effect->effectData);
					std::size_t direct = 0;
					std::size_t deeper = 0;
					RE::BSVisit::TraverseScenegraphLights(root, [opt, hue, root, &direct, &deeper](RE::NiPointLight* a_light) {
						if (hue && IsEnchantLight(a_light)) {
							if (a_light->parent == root) {
								gFlesh.emplace_back(a_light, hue);
								++direct;
							} else {
								++deeper;
							}
						}
						if (IsEnchantLight(a_light)) {
							gEnchant.emplace_back(a_light, opt);
						}
						return RE::BSVisit::BSVisitControl::kContinue;
					});
					// RE::Light's lights under the body but none straight on it: the flesh light is not where it was expected, so
					// it keeps its old color - said once per shader, never silent
					if (hue && !direct && deeper && gFleshMissLogged.insert(a_effect->effectData).second) {
						SKSE::log::warn(
							"FleshFX: a flesh spell's RE::Light light was not found directly on its body ({} lights deeper) - "
							"it keeps its old color",
							deeper);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			// a light named twice: the last effect's option holds (reversed, so a stable sort puts it first), the first effect's hue
			std::ranges::reverse(gEnchant);
			std::ranges::stable_sort(gEnchant, {}, &std::pair<const RE::NiLight*, std::size_t>::first);
			const auto enchantRepeats = std::ranges::unique(gEnchant, {}, &std::pair<const RE::NiLight*, std::size_t>::first);
			gEnchant.erase(enchantRepeats.begin(), enchantRepeats.end());
			std::ranges::stable_sort(gFlesh, {}, &std::pair<const RE::NiLight*, const RE::NiColor*>::first);
			const auto fleshRepeats = std::ranges::unique(gFlesh, {}, &std::pair<const RE::NiLight*, const RE::NiColor*>::first);
			gFlesh.erase(fleshRepeats.begin(), fleshRepeats.end());
		}

		std::size_t LookUpOption(RE::NiLight* a_light)
		{
			if (!IsReLightLight(a_light)) {
				return kNone;
			}
			if (IsEnchantLight(a_light)) {
				return EnchantOptionOf(a_light);
			}
			auto* ref = ReferenceOf(a_light);
			return ref ? OptionOf(ref->GetBaseObject()) : kNone;
		}

		// an enchantment light's option follows its effect each frame (an effect can start and stop); any other light's is
		// worked out once and remembered while it is the same light
		std::size_t OptionOfLight(RE::NiLight* a_light)
		{
			if (IsEnchantLight(a_light)) {
				return EnchantOptionOf(a_light);
			}
			auto&       k = gKnown[a_light];
			const void* name = a_light->name.c_str();
			const void* parent = a_light->parent;
			if (k.frame == 0 || k.name != name || k.parent != parent) {
				k = { name, parent, LookUpOption(a_light), gFrame };
			}
			k.frame = gFrame;
			return k.option;
		}

		// all lights off holds every option's lights out here, where lights are culled - never in OptionLit, which the hand lights'
		// discovery reads at load (CodeRabbit, #24: a game started with the switch on found no automatic hand light)
		bool Off(std::size_t a_option) { return a_option != kNone && (AllLightsOff() || !OptionLit(a_option)); }

		void HoldOut(RE::NiLight* a_light, std::size_t a_option, bool a_enchant)
		{
			if (gHeldSet.insert(a_light).second) {
				KeepLight(a_light);
				gHeldOut.push_back({ a_light, a_option, a_enchant });
			}
		}

		// The sheathed net (1.5, three user reports "my enchanted weapons keep glowing even when sheathed"):
		// RE::Light puts its enchantment light out on the sheathe animation's end and lights it again whenever the
		// enchantment effect starts, so a weapon put away some other way, or an effect that restarts on a put-away
		// weapon, could keep it lit. Here an enchantment light on an actor whose weapon is fully SHEATHED is put out every
		// frame, and given back the moment that actor starts to draw. To be removed if RE::Light covers it itself.
		bool FullySheathed(RE::NiLight* a_light)
		{
			auto* ref = ReferenceOf(a_light);
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			return actor && actor->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kSheathed;
		}

		void SheathedNet(RE::NiLight* a_light)
		{
			if (!IsEnchantLight(a_light) || a_light->GetAppCulled() || !FullySheathed(a_light)) {
				return;
			}
			a_light->SetAppCulled(true);
			if (std::ranges::find(gSheathedOut, a_light) == gSheathedOut.end()) {
				KeepLight(a_light);
				gSheathedOut.push_back(a_light);
			}
		}

		void GiveBackDrawn()
		{
			std::erase_if(gSheathedOut, [](RE::NiLight* a_light) {
				if (FullySheathed(a_light)) {
					return false;
				}
				// a switched-off option keeps it out: it moves to that option's list, which gives it back with the switch
				if (const auto opt = OptionOfLight(a_light); Off(opt)) {
					HoldOut(a_light, opt, true);
				} else if (!HeldOutForOption(a_light)) {
					a_light->SetAppCulled(false);
				}
				return true;
			});
		}

		// RE::Light's Load3D hook for hazards attaches its config's light on every load of the hazard's 3D with no check for one
		// already there, so a burning patch can wear two identical lights at one spot. The later twin is put out; the first stays.
		std::vector<const RE::NiLight*> gTwinSeen;  // this frame's hazard lights of RE::Light's, the first of each

		bool SameTwin(const RE::NiLight* a_a, const RE::NiLight* a_b)
		{
			return a_a->parent == a_b->parent && a_a->name == a_b->name &&
			       a_a->GetLightRuntimeData().diffuse == a_b->GetLightRuntimeData().diffuse &&
			       a_a->local.translate.GetDistance(a_b->local.translate) < 0.5f;
		}

		void TwinNet(RE::NiLight* a_light)
		{
			if (!IsReLightLight(a_light) || IsEnchantLight(a_light) || a_light->GetAppCulled() || !a_light->parent) {
				return;
			}
			const auto* ref = ReferenceOf(a_light);
			if (!ref || !ref->Is(RE::FormType::PlacedHazard)) {
				return;
			}
			if (std::ranges::any_of(gTwinSeen, [a_light](const RE::NiLight* a_seen) { return SameTwin(a_seen, a_light); })) {
				a_light->SetAppCulled(true);
				return;
			}
			gTwinSeen.push_back(a_light);
		}

		// RE::Light culls an enchantment light on sheathe: a switch turned back on must not light a sheathed weapon
		bool MayGiveBack(const Held& a_held)
		{
			if (!a_held.enchant) {
				return true;
			}
			auto* ref = ReferenceOf(a_held.light);
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			return actor && actor->AsActorState()->IsWeaponDrawn();
		}
	}

	std::size_t EnchantOptionOf(const RE::NiLight* a_light)
	{
		const auto* option = Find(gEnchant, a_light);
		return option ? *option : kNone;
	}

	const RE::NiColor* FleshHueOf(const RE::NiLight* a_light)
	{
		const auto* hue = Find(gFlesh, a_light);
		return hue ? *hue : nullptr;
	}

	RE::TESObjectREFR* ReferenceOf(RE::NiAVObject* a_obj)
	{
		for (auto* o = a_obj; o; o = o->parent) {
			if (auto* ref = o->GetUserData()) {
				return ref;
			}
		}
		return nullptr;
	}

	bool HeldOutForOption(const RE::NiLight* a_light) { return gHeldSet.contains(a_light); }

	void ForgetOptionLights(const GoneLights& a_gone)
	{
		std::erase_if(gHeldOut, [&](const Held& h) { return a_gone.contains(h.light); });
		std::erase_if(gHeldSet, [&](const RE::NiLight* l) { return a_gone.contains(l); });
		std::erase_if(gSheathedOut, [&](RE::NiLight* l) { return a_gone.contains(l); });
		std::erase_if(gKnown, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
	}

	void UpdateOptionLights()
	{
		++gFrame;
		auto& opts = Options();
		for (auto& o : opts) {
			o.lit = 0;
			o.heldOut = 0;
		}
		FindEnchantLights();
		// give back whatever belongs to a switch that is on again (what has unloaded is already forgotten: Keep.cpp)
		std::erase_if(gHeldOut, [](Held& h) {
			if (!Off(h.option)) {
				if (MayGiveBack(h)) {
					h.light->SetAppCulled(false);
				} else if (std::ranges::find(gSheathedOut, h.light) == gSheathedOut.end()) {
					// the switch came back on while the weapon is put away: the light joins the sheathed net, which lights it
					// the moment the weapon is drawn (the audit, 2026-10-14: it fell out of every list and stayed dark until
					// the weapon was equipped again). Keep.cpp already holds it (HoldOut)
					gSheathedOut.push_back(h.light);
				}
				gHeldSet.erase(h.light);
				return true;
			}
			return false;
		});
		GiveBackDrawn();
		auto* ssn = LightKit::Scene();
		if (!ssn) {
			return;
		}
		gTwinSeen.clear();
		for (auto& bsLight : ssn->GetRuntimeData().activeLights) {
			if (!bsLight || !bsLight->light) {
				continue;
			}
			auto* niLight = bsLight->light.get();
			TwinNet(niLight);
			SheathedNet(niLight);  // every RE::Light enchantment light, ours or not
			const auto opt = OptionOfLight(niLight);
			if (opt == kNone) {
				continue;
			}
			if (Off(opt)) {
				// RE::Light unculls its enchantment light on each new equip, so one already held can come round again
				if (!niLight->GetAppCulled()) {
					niLight->SetAppCulled(true);
					HoldOut(niLight, opt, IsEnchantLight(niLight));
				}
			} else if (!niLight->GetAppCulled()) {
				++opts[opt].lit;
			}
		}
		for (const auto& h : gHeldOut) {
			if (h.option < opts.size()) {
				++opts[h.option].heldOut;
			}
		}
		// a light not seen for a few seconds has left the scene: what was worked out about it is let go
		constexpr std::uint32_t kForget = 300;
		if (gFrame % kForget == 0) {
			std::erase_if(gKnown, [](const auto& a_kv) { return gFrame - a_kv.second.frame > kForget; });
		}
	}

	LightList CullOptionLightsUnder(RE::NiAVObject* a_root, std::size_t& a_option)
	{
		LightList out;
		a_option = kNone;
		if (!a_root) {
			return out;
		}
		// done at load so a switched-off light never shows for the frame before the next player update (any thread: only
		// the lights are touched here)
		auto*      ref = ReferenceOf(a_root);
		const auto opt = ref ? OptionOf(ref->GetBaseObject()) : kNone;
		if (!Off(opt)) {
			return out;
		}
		a_option = opt;
		RE::BSVisit::TraverseScenegraphLights(a_root, [&out](RE::NiPointLight* a_light) {
			if (a_light && !a_light->GetAppCulled() && IsReLightLight(a_light)) {
				a_light->SetAppCulled(true);
				out.emplace_back(a_light);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return out;
	}

	void AdoptOptionLights(std::size_t a_option, const LightList& a_lights)
	{
		for (const auto& light : a_lights) {
			if (light->GetRefCount() <= 1) {
				continue;  // already gone from the game
			}
			if (Off(a_option)) {
				HoldOut(light.get(), a_option, false);
			} else {
				light->SetAppCulled(false);  // the switch went back on before the main thread came round
			}
		}
	}
}
