// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The hooks, and spell lights fading while you sneak - out of main.cpp since 2.2 (his order "make sure relight is
// structurally up to par with illuminated": main.cpp holds the order of work and the SKSE messages, as Illuminated's does;
// Illuminated keeps its update hooks in LightCopies.cpp):
//   the spell-light call (up to four call sites, each before RE::Light's hook there): our hand lights made, the light
//                 budget ("Hand lights for"), sneaking;
//   3D loads      of projectiles, explosions and hazards: a switched-off option's light out before its first frame, sneaking;
//   the player update: the per-frame passes in their order - kept lights swept, sneaking, the switches, held lights,
//                 body lights, Brightness, the fading module, the ground twins;
//   effect updates and cell animations: the fading module's other two places.
//
// THREADS: every list of game lights here (and in Options.cpp, HandLights.cpp, Held.cpp) is filled and emptied on the
// main thread only. The hooks that may run on the game's loader and job threads (the spell-light call, a 3D load) read
// the settings (atomics) and the caster's plain state (sneaking, position, follower), touch only the light itself (put it
// out, so it never shows a frame it should not) and hand the bookkeeping to the main thread as an SKSE task that holds the
// light until then - so no lock is held across a frame, and none is needed.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX  // the build defines it too (xmake.lua)
#endif
#include "Fade.h"
#include "Plugin.h"

#include <Windows.h>  // VirtualQuery, GetModuleHandleExW: whose code a VR call site's call leads into

namespace
{
	// every spell light the game made through the hooked call sites, so sneaking can find it again. Main thread only. Plain
	// pointers: Keep.cpp holds each light, and ForgetSpellLights drops it here before it is freed
	std::unordered_set<RE::NiLight*> gMagicLights;

	// The sneak fade: while she sneaks with "Lights fade while sneaking", every light the sneaking pass finds fades to
	// "Sneaking brightness" over half a second, and back up when she stands. 1 = not dimmed. Written by the player update,
	// read on any thread
	std::atomic<float> gSneakLevel{ 1.0f };
	constexpr float    kSneakFadeSeconds = 0.5f;

	// Our write is the LAST word on a dimmed light's fade and never a base for anyone else's: each light remembers the fade
	// the other passes left it (`pre`) and what we wrote over it. Before any of them writes again - the player update, an
	// effect update, the cell's animations - the light gets its `pre` back if nobody wrote since, and after them it is dimmed
	// again; so Brightness, the fading module, RE::Light and the game only ever read their own numbers. Under gDimLock (an
	// effect update may not be on the main thread)
	struct Dimmed
	{
		float pre{ 0.0f };
		float wrote{ 0.0f };
		bool  down{ false };  // our write is on the light now
	};
	std::unordered_map<RE::NiLight*, Dimmed> gDim;
	std::mutex                               gDimLock;
	std::atomic<bool>                        gDimming{ false };  // gDim holds a light

	void DimOne(RE::NiLight* a_light, Dimmed& a_d, float a_level)
	{
		auto& fade = a_light->GetLightRuntimeData().fade;
		if (a_d.down && fade == a_d.wrote) {
			return;
		}
		a_d.pre = fade;
		a_d.wrote = fade * a_level;
		fade = a_d.wrote;
		a_d.down = true;
	}

	void DimAll(float a_level)
	{
		for (auto& [light, d] : gDim) {
			DimOne(light, d, a_level);
		}
	}

	void RestoreAll()
	{
		for (auto& [light, d] : gDim) {
			if (!d.down) {
				continue;
			}
			auto& fade = light->GetLightRuntimeData().fade;
			if (fade == d.wrote) {
				fade = d.pre;  // nobody wrote it since: back to what the others left it
			}
			d.down = false;
		}
	}

	// around a pass of ours that writes fades after the player update (an effect update, the cell pass)
	template <class F>
	void UndimmedFor(F&& a_pass)
	{
		if (!gDimming.load(std::memory_order_acquire)) {
			a_pass();
			return;
		}
		std::lock_guard lock(gDimLock);
		RestoreAll();
		a_pass();
		DimAll(gSneakLevel.load(std::memory_order_relaxed));
	}

	// the fading pass after an effect update writes only under a weapon enchantment's own actor, and only her lights are dimmed:
	// any other effect leaves the dimmed list alone
	[[nodiscard]] bool OnHerWeapon(const RE::ReferenceEffect* a_effect)
	{
		const auto* ctrl = a_effect ? skyrim_cast<const RE::WeaponEnchantmentController*>(a_effect->controller) : nullptr;
		return ctrl && ctrl->target && ctrl->target->IsPlayerRef();
	}

	// a perk or ability that only pays off in the dark (a Shadow birthsign: spells cheaper and quieter "in shadows") reads the
	// light level, which her lights raise - so while she carries one her lights fade out (his pick). Found once: every perk whose
	// conditions, or an entry point's, ask for a LOW light level, and every ability with such an effect
	[[nodiscard]] bool ReadsLightLevel(const RE::TESCondition& a_condition)
	{
		using Op = RE::CONDITION_ITEM_DATA::OpCode;
		for (const auto* item = a_condition.head; item; item = item->next) {
			const auto op = item->data.flags.opCode;
			if (item->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kGetLightLevel &&
				item->data.object.get() == RE::CONDITIONITEMOBJECT::kSelf && (op == Op::kLessThan || op == Op::kLessThanOrEqualTo)) {
				return true;
			}
		}
		return false;
	}

	struct ShadowSigns
	{
		std::vector<RE::BGSPerk*>   perks;
		std::vector<RE::SpellItem*> abilities;
	};

	[[nodiscard]] const ShadowSigns& Signs()
	{
		static const ShadowSigns signs = [] {
			ShadowSigns found;
			auto*       dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return found;
			}
			for (auto* perk : dh->GetFormArray<RE::BGSPerk>()) {
				const bool reads = perk && (ReadsLightLevel(perk->perkConditions) || std::ranges::any_of(perk->perkEntries, [](const auto* a_entry) {
					const auto* point = a_entry && a_entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint ?
					                        static_cast<const RE::BGSEntryPointPerkEntry*>(a_entry) :
					                        nullptr;
					return point && std::ranges::any_of(point->conditions, ReadsLightLevel);
				}));
				if (reads) {
					found.perks.push_back(perk);
				}
			}
			for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
				if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility &&
					std::ranges::any_of(spell->effects, [](const auto* a_effect) {
						return a_effect && (ReadsLightLevel(a_effect->conditions) || (a_effect->baseEffect && ReadsLightLevel(a_effect->baseEffect->conditions)));
					})) {
					found.abilities.push_back(spell);
				}
			}
			SKSE::log::info("sneak fade: {} perk(s) and {} abilit(y/ies) read the light level (her lights fade while she carries one)",
				found.perks.size(), found.abilities.size());
			return found;
		}();
		return signs;
	}

	std::atomic<bool> gHidden{ false };  // she is invisible or carries a shadow sign, with that setting on - the player update's look

	// her lights fade out: invisible (the Shadow Stone, Invisibility - every such effect raises the Invisibility value) or carrying
	// a shadow sign, with "While invisible or in shadow" on Lights fade out. Main thread
	[[nodiscard]] bool HiddenNow(const RE::PlayerCharacter& a_player)
	{
		if (!Plugin::InvisibleFadeOn()) {
			return false;
		}
		if (a_player.AsActorValueOwner()->GetActorValue(RE::ActorValue::kInvisibility) > 0.0f) {
			return true;
		}
		const auto& signs = Signs();
		return std::ranges::any_of(signs.perks, [&](auto* a_perk) { return a_player.HasPerk(a_perk); }) ||
		       std::ranges::any_of(signs.abilities, [&](auto* a_spell) { return a_player.HasSpell(a_spell); });
	}

	// a light made now is dimmed: she sneaks with the setting on, she is hidden, or her lights are still coming back up (any thread)
	bool SneakDimsNow()
	{
		if (gSneakLevel.load(std::memory_order_relaxed) < 1.0f || gHidden.load(std::memory_order_relaxed)) {
			return true;
		}
		if (!Plugin::SneakOn()) {
			return false;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && player->IsSneaking();
	}

	float StepSneakLevel(const RE::PlayerCharacter* a_player, float a_delta)
	{
		const bool sneaking = Plugin::SneakOn() && a_player && a_player->IsSneaking();
		const bool hidden = a_player && HiddenNow(*a_player);
		gHidden.store(hidden, std::memory_order_relaxed);
		const float target = hidden ? 0.0f : sneaking ? static_cast<float>(Plugin::SneakBrightness()) / 100.0f :
		                                                1.0f;
		const float step = std::clamp(a_delta, 0.0f, 0.25f) / kSneakFadeSeconds;
		float       level = gSneakLevel.load(std::memory_order_relaxed);
		level = level < target ? (std::min)(level + step, target) : (std::max)(level - step, target);
		gSneakLevel.store(level, std::memory_order_relaxed);
		return level;
	}

	// The projectiles, explosions and hazards that are MAGIC: what a magic effect fires or sets off (its projectile, its
	// explosion, its projectile's explosion, the hazard such an explosion leaves), worked out once at data load before
	// the hooks go in, read-only after (the 3D-load hooks ask from loader threads). An arrow, a trap's explosion or a
	// burning oil slick is none of these, so "Lights off while sneaking" leaves its light alone (the audit, 2026-10-14:
	// every arrow, explosion and hazard used to count as a spell)
	std::unordered_set<const void*> gSpellBases;
	std::atomic<bool>               gSpellBasesReady{ false };  // set once gSpellBases is built (release), read before it (acquire)

	void FindSpellBasesOnce()
	{
		gSpellBases.clear();
		std::unordered_set<const RE::BGSExplosion*> explosions;
		for (const auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
			if (!effect) {
				continue;
			}
			if (const auto* proj = effect->data.projectileBase) {
				gSpellBases.insert(proj);
				if (proj->data.explosionType) {
					explosions.insert(proj->data.explosionType);
				}
			}
			if (effect->data.explosion) {
				explosions.insert(effect->data.explosion);
			}
		}
		for (const auto* x : explosions) {
			gSpellBases.insert(x);
			if (x->data.spawnProjectile) {
				gSpellBases.insert(x->data.spawnProjectile);
			}
			// the hazard it leaves: CommonLib types this field as a reference, the game stores the hazard's base form there,
			// so it is only compared as an address, never read
			if (x->data.impactPlacedObject) {
				gSpellBases.insert(static_cast<const void*>(x->data.impactPlacedObject));
			}
		}
		gSpellBasesReady.store(true, std::memory_order_release);
		SKSE::log::info("lights off while sneaking: {} projectile, explosion and hazard form(s) are magic", gSpellBases.size());
	}

	[[nodiscard]] bool IsSpellObject(const RE::TESObjectREFR* a_ref)
	{
		using FT = RE::FormType;
		if (!a_ref || !a_ref->Is(FT::ProjectileMissile, FT::ProjectileArrow, FT::ProjectileGrenade, FT::ProjectileBeam, FT::ProjectileFlame,
						  FT::ProjectileCone, FT::ProjectileBarrier, FT::Explosion, FT::PlacedHazard)) {
			return false;
		}
		if (!gSpellBasesReady.load(std::memory_order_acquire)) {
			return false;  // before data load: no magic object exists yet
		}
		const auto* base = a_ref->GetBaseObject();
		// magic by what fires it, or an object this mod's data lights (a mod's scripted spell art)
		return base && (gSpellBases.contains(base) || Plugin::OptionOf(base) != Plugin::kNone);
	}

	// What the sneaking pass worked out about each light, so the walk up to its reference is not repeated for every light
	// every frame while she sneaks (the audit's speed finding). A freed light's address can come back as another light,
	// so each is remembered with its name text and its parent, as Options.cpp and Brightness.cpp do. Main thread only.
	struct SneakKnown
	{
		const void*   name{ nullptr };
		const void*   parent{ nullptr };
		bool          spell{ false };
		bool          rune{ false };  // the Runes setting itself is read every frame, so a change reaches a known light
		std::uint32_t frame{ 0 };
	};
	std::unordered_map<const RE::NiLight*, SneakKnown> gSneakKnown;
	std::uint32_t                                      gSneakFrame = 0;

	// the player's own: a light on her, or on what she cast (a projectile she shot, an explosion or hazard she set off). Only
	// hers fade while she sneaks - an enemy's hands and runes and a trap keep their light. No owner counts as hers, as before.
	// Main thread only: it resolves handles and the actor cause (the hooks below use MaybeSneakDarkens)
	[[nodiscard]] bool CastByHer(const RE::TESObjectREFR* a_ref)
	{
		const auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_ref || !player) {
			return !a_ref;
		}
		if (a_ref->As<RE::Actor>()) {
			return a_ref == player;
		}
		const auto hers = [player](const auto& a_handle) { return a_handle.get().get() == player; };
		if (const auto* projectile = a_ref->As<RE::Projectile>(); projectile && hers(projectile->GetProjectileRuntimeData().shooter)) {
			return true;
		}
		// As<> never matches a placed explosion: it shares its form type with BGSExplosion
		if (const auto* explosion = skyrim_cast<const RE::Explosion*>(a_ref); explosion && hers(explosion->GetExplosionRuntimeData().actorOwner)) {
			return true;
		}
		if (const auto* hazard = a_ref->As<RE::Hazard>(); hazard && hers(hazard->GetHazardRuntimeData().ownerActor)) {
			return true;
		}
		// a projectile an explosion spawned, or an explosion / hazard with no owner of its own: whoever set the chain off
		const auto* cause = a_ref->GetActorCause();
		return cause && hers(cause->actor);
	}

	// a rune: a lobbed projectile set off by someone coming near (its proximity trigger) - a timed burst or a bloom has none
	[[nodiscard]] bool IsRune(const RE::TESObjectREFR* a_ref)
	{
		const auto* base = a_ref && a_ref->Is(RE::FormType::ProjectileGrenade) ? a_ref->GetBaseObject() : nullptr;
		const auto* data = base ? base->As<RE::BGSProjectile>() : nullptr;
		return data && data->IsGrenade() && data->data.explosionProximity > 0.0f;
	}

	// a rune keeps its light while she sneaks when Runes says "Stay lit" (main thread)
	[[nodiscard]] bool RuneStaysLit(const RE::TESObjectREFR* a_ref) { return !Plugin::RunesFadeOn() && IsRune(a_ref); }

	// a spell light made through the hooked call goes dark while she sneaks when it hangs on her (a casting light) or on a magic
	// object she cast - never on a trap's explosion that happens to go through the same call (main thread: CastByHer)
	[[nodiscard]] bool SneakDarkens(const RE::TESObjectREFR* a_ref)
	{
		return CastByHer(a_ref) && !RuneStaysLit(a_ref) && (!a_ref || a_ref->As<RE::Actor>() || IsSpellObject(a_ref));
	}

	// any thread (a loader or job thread runs the hooks): no handle is resolved here - a light that MAY be hers goes out at
	// once, and AdoptSneakCulled asks on the main thread whose it is
	[[nodiscard]] bool MaybeSneakDarkens(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return true;
		}
		if (a_ref->As<RE::Actor>()) {
			return a_ref == RE::PlayerCharacter::GetSingleton();
		}
		return IsSpellObject(a_ref);
	}

	// a light the sneaking pass puts out: a spell light the hooked call made, on a caster or on a magic object (never on a
	// trap's explosion that happens to go through the same call), or any light on a magic projectile, explosion or hazard
	[[nodiscard]] bool SneakSpellLight(RE::NiLight* a_light)
	{
		auto&       k = gSneakKnown[a_light];
		const void* name = a_light->name.c_str();
		const void* parent = a_light->parent;
		if (k.frame == 0 || k.name != name || k.parent != parent) {
			const auto* ref = Plugin::ReferenceOf(a_light);
			const bool  hers = CastByHer(ref);
			const bool  made = gMagicLights.contains(a_light);
			// RE::Light's enchantment light on her weapon too: the setting's tip promises her weapon lights fade
			const bool spell = hers && (IsSpellObject(ref) || (made && SneakDarkens(ref)) || (Plugin::IsEnchantLight(a_light) && ref && ref->As<RE::Actor>()));
			if (!spell && hers && ref && ref->As<RE::Actor>()) {
				// a light on a caster not noted as a spell light yet (its SKSE task may come later this frame): asked again
				// next frame, never remembered as "not a spell light"
				gSneakKnown.erase(a_light);
				return false;
			}
			k = { name, parent, spell, IsRune(ref), gSneakFrame };
		}
		k.frame = gSneakFrame;
		return k.spell && !(k.rune && !Plugin::RunesFadeOn());
	}

	// the player update, at its start: every light dimmed last frame back to what the other passes left it, so this frame's
	// passes read their own numbers; DimSpellLights at the end finds this frame's lights again
	void UndimForFrame()
	{
		std::lock_guard lock(gDimLock);
		RestoreAll();
		gDim.clear();
		gDimming.store(false, std::memory_order_release);
	}

	// the player update, after every pass that writes a fade and before the ground twins copy them. A culled light is dimmed
	// too: off Community Shaders a hand light is hidden and its ground twin copies its numbers
	void DimSpellLights(float a_level)
	{
		auto* ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (a_level >= 1.0f || !ssn) {
			return;
		}
		++gSneakFrame;
		static std::vector<RE::NiLight*> found;  // main thread; its storage kept between frames
		found.clear();
		for (auto& bsLight : ssn->GetRuntimeData().activeLights) {
			if (bsLight && bsLight->light && SneakSpellLight(bsLight->light.get())) {
				found.push_back(bsLight->light.get());
			}
		}
		for (auto* light : found) {
			Plugin::KeepLight(light);  // outside gDimLock: Keep.cpp's sweep calls ForgetSpellLights
		}
		{
			std::lock_guard lock(gDimLock);
			for (auto* light : found) {
				DimOne(light, gDim[light], a_level);
			}
			gDimming.store(!gDim.empty(), std::memory_order_release);
		}
		// a light not seen for a few seconds has left the scene: what was worked out about it is let go
		constexpr std::uint32_t kForget = 300;
		if (gSneakFrame % kForget == 0) {
			std::erase_if(gSneakKnown, [](const auto& a_kv) { return gSneakFrame - a_kv.second.frame > kForget; });
		}
	}

	// on the main thread only (an SKSE task from the hook below): a new spell light goes into our lists. The last
	// reference to a light is only ever dropped on the main thread (here, or in Keep.cpp's sweep)
	void NoteMagicLight(const RE::NiPointer<RE::NiLight>& a_light, const std::string& a_handKey, bool a_ours, Pulse::Kind a_kind)
	{
		if (!a_light || a_light->GetRefCount() <= 1) {
			return;  // the game let go of it before the main thread came round: nothing to note (freed right here)
		}
		Plugin::KeepLight(a_light.get());
		gMagicLights.insert(a_light.get());
		if (!a_handKey.empty()) {
			Plugin::NoteHandLight(a_light.get(), a_handKey);
		}
		// a hand light of ours, a spray light RE::Light made from our config, and the light of a beam or breath
		// projectile our data names (RE::Light lights it; Streams.cpp) are ours for the sliders
		if (a_ours) {
			Plugin::RememberLight(a_light.get(), a_handKey.empty() ? nullptr : Plugin::HandFxOf(a_handKey), a_kind, !a_handKey.empty());
		}
		// a light made while her lights were dimmed was put out at once and comes here through AdoptSneakCulled; one made
		// just before she crouched is dimmed by the next player update's sneaking pass, which reads gMagicLights
	}

	// "Hand lights for" (Illuminated's light budget for big fights, ported): a caster the setting leaves out gets
	// no casting light at all - this is the call that would make it, ours or RE::Light's, so nothing is made and put out
	[[nodiscard]] bool LeftOutCaster(const RE::TESObjectREFR* a_ref)
	{
		const auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
		return actor && Plugin::LeftOutByBudget(actor);  // the one budget rule, for held lights too (Held.cpp)
	}

	void AdoptSneakCulled(const Plugin::LightList& a_lights);

	using MagicLightFn = RE::NiPointLight*(RE::TESObjectLIGH*, RE::TESObjectREFR*, RE::NiNode*, bool, bool, bool);

	// the spell-light call, at every hooked site; a_next is what that site called before us (the game's function, or another
	// plugin's hook - RE::Light's)
	RE::NiPointLight* MakeMagicLight(const REL::Relocation<MagicLightFn>& a_next, RE::TESObjectLIGH* a_light, RE::TESObjectREFR* a_ref,
		RE::NiNode* a_node, bool a_forceDynamic, bool a_useLightRadius, bool a_affectRequesterOnly)
	{
		if (LeftOutCaster(a_ref)) {
			return nullptr;
		}
		// our hand lights are made with the game's own function and never reach RE::Light, which would switch off
		// a casting light it has no config for (an in-memory record can have none)
		const Plugin::Hand* hand = Plugin::HandOfLight(a_light);
		RE::NiPointLight*   made = nullptr;
		if (hand) {
			// her own hand lights cast a shadow with the optional Bottled Shaders shadow download (his checklist 262, "b")
			auto* use = a_ref && a_ref->IsPlayerRef() && Plugin::ShadowPatch() ? Plugin::ShadowTwinOf(a_light) : a_light;
			made = netimmerse_cast<RE::NiPointLight*>(use->GenDynamic(a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly));
			Plugin::DressHandLight(made, *hand);  // only writes the fresh light's own fields
		} else if (Plugin::IsOurLight(a_light)) {
			// 2.3: a spray's / bolt's raised copy, or a beam record's other users' plain copy (Streams.cpp) - the game's own
			// function too, so RE::Light set to switch off game lights it has no config for never switches these off
			made = netimmerse_cast<RE::NiPointLight*>(
				a_light->GenDynamic(a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly));
			Plugin::DressStreamLight(made, a_light);
		} else {
			made = a_next(a_light, a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly);
		}
		// ⚠ the Lightning Bolt freeze (found with Truman, by switching hooks off one at a time): this hook
		// also runs on the game's loader and job threads. It used to keep the light in our lists right here and, every
		// 64th light, let go of lights nothing else held - so a light could be FREED on a loader thread while the game's
		// threads walked the scene. RE::Light's own hook keeps nothing (it marks the light and returns); ours now hands
		// the light to the main thread (an SKSE task, Truman's route for main-thread work), where every list of ours is
		// filled and emptied. The task holds the light until then, and lets go of it on the main thread too.
		// While her lights are dimmed the light is made and put out at once (as a 3D load's are), never a frame at full
		// strength; the main thread lights it again at the sneaking strength.
		if (made) {
			LightKit::Guarded("spell light", [&]() {
				const bool ours = hand || Plugin::IsSprayLight(a_light) || Plugin::IsStreamObject(a_ref);
				const bool sneaking = SneakDimsNow() && MaybeSneakDarkens(a_ref);
				if (sneaking) {
					made->SetAppCulled(true);
				}
				if (auto* tasks = SKSE::GetTaskInterface()) {
					// the spell in the hand now, when its light is made - a spell swapped before the task runs is not this light's
					const bool left = a_node && std::string_view(a_node->name.c_str()).contains("L MagicNode");
					const auto kind = hand && a_ref ? LightKit::PulseKindOfHand(a_ref->As<RE::Actor>(), left) : Pulse::Kind::kNone;
					tasks->AddTask([light = RE::NiPointer<RE::NiLight>(made), key = hand ? hand->key : std::string{}, ours, sneaking, kind]() {
						NoteMagicLight(light, key, ours, key.empty() ? Pulse::Kind::kNone : kind);
						if (sneaking) {
							AdoptSneakCulled({ light });
						}
					});
				}
			});
		}
		return made;
	}

	// one per call site, so each site goes on to the function IT called before (another plugin may have hooked one alone)
	template <std::size_t I>
	struct MagicLight
	{
		static RE::NiPointLight* thunk(RE::TESObjectLIGH* a_light, RE::TESObjectREFR* a_ref, RE::NiNode* a_node,
			bool a_forceDynamic, bool a_useLightRadius, bool a_affectRequesterOnly)
		{
			return MakeMagicLight(func, a_light, a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly);
		}
		static inline REL::Relocation<MagicLightFn> func;
	};

	// every light under a just-loaded 3D put out (any thread); returned, held, for the main thread to light again dimmed
	[[nodiscard]] Plugin::LightList CullTree(RE::NiAVObject* a_root)
	{
		Plugin::LightList out;
		RE::BSVisit::TraverseScenegraphLights(a_root, [&out](RE::NiPointLight* a_light) {
			if (a_light && !a_light->GetAppCulled()) {
				a_light->SetAppCulled(true);
				out.emplace_back(a_light);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return out;
	}

	// main thread: the lights put out at once while her lights were dimmed come back on at the strength of the moment
	// (unless a switch or the hand-light switch holds them out)
	void AdoptSneakCulled(const Plugin::LightList& a_lights)
	{
		const float level = gSneakLevel.load(std::memory_order_relaxed);
		for (const auto& light : a_lights) {
			if (light->GetRefCount() <= 1 || Plugin::HeldOutForOption(light.get()) || Plugin::HandLightHeldOut(light.get())) {
				continue;  // already gone from the game, or held out
			}
			if (!SneakDarkens(Plugin::ReferenceOf(light.get()))) {
				light->SetAppCulled(false);  // another caster's or a trap's, put out by the hook only to ask here: back at full strength
				continue;
			}
			if (level < 1.0f) {
				Plugin::KeepLight(light.get());
				std::lock_guard lock(gDimLock);
				DimOne(light.get(), gDim[light.get()], level);
				gDimming.store(true, std::memory_order_release);
			}
			light->SetAppCulled(false);
		}
	}

	template <class T>
	struct Load3D
	{
		static RE::NiAVObject* thunk(T* a_this, bool a_backgroundLoading)
		{
			auto* root = func(a_this, a_backgroundLoading);
			auto* tasks = root ? SKSE::GetTaskInterface() : nullptr;
			if (!tasks) {
				return root;
			}
			LightKit::Guarded("3D load", [a_this, root, tasks]() {
				// may be a loader thread: the lights are put out now (never a frame lit) and our lists take them on the main thread.
				// RE::Light has already hung its light here; a switched-off option's goes out before its first frame
				std::size_t option = Plugin::kNone;
				if (auto culled = Plugin::CullOptionLightsUnder(root, option); !culled.empty()) {
					tasks->AddTask([lights = std::move(culled), option]() { Plugin::AdoptOptionLights(option, lights); });
				}
				// while her lights are dimmed, the rest of a magic object's go out until the main thread dims them, or gives them
				// back when another caster made it (an arrow, a trap's explosion or an oil fire keeps its light - IsSpellObject)
				if (SneakDimsNow() && MaybeSneakDarkens(a_this)) {
					if (auto culled = CullTree(root); !culled.empty()) {
						tasks->AddTask([lights = std::move(culled)]() { AdoptSneakCulled(lights); });
					}
				}
				if constexpr (std::is_base_of_v<RE::Projectile, T>) {
					Plugin::QueueRow(a_this);  // a bolt / spray / cone our data lights: its row of lights, hung on the main thread (Rows.cpp)
				}
			});
			return root;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x6A, thunk);
		}
	};

	struct PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			// the dimmed lights get the others' numbers back BEFORE the update: RE::Light writes this frame's fades inside it
			LightKit::Guarded("player update undim", []() { UndimForFrame(); });
			func(a_this, a_delta);
			LightKit::Guarded("player update", [a_this, a_delta]() {
				// first: every list forgets the lights that left the game since the last frame, and they are freed
				Plugin::SweepKeptLights();
				const float sneak = StepSneakLevel(a_this, a_delta);
				Plugin::UpdateOptionLights();
				Plugin::UpdateHeldLights();
				Plugin::UpdateBodyLights();  // the body glow while casting (BodyLights.cpp)
				Plugin::UpdateRows();        // the rows of lights along bolts, sprays and cones (Rows.cpp)
				// RE::Light has already written this frame's fades (its update runs inside `func` above): the sliders next, then
				// the fading module as the final pass, on what the sliders wrote (each fade it writes is reported back -
				// Brightness.cpp's NoteFadeWrite)
				Plugin::UpdateBrightness(a_delta);
				Fade::UpdateHands(a_delta);
				DimSpellLights(sneak);       // over every fade written above
				Plugin::TickGroundLights();  // last: off Community Shaders the hand lights' twins copy this frame's numbers (GroundLights.cpp)
			});
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// the fading module's other two places: right after each enchantment effect's update (RE::Light rewrites a flickering
	// enchantment light's fade there) and after the cell's light animations - this frame's numbers again
	template <class T>
	struct EffectUpdate
	{
		static void thunk(T* a_this)
		{
			func(a_this);
			LightKit::Guarded("effect update", [a_this]() {
				const auto pass = [a_this]() {
					Fade::AfterReferenceEffect(a_this, std::is_same_v<T, RE::ModelReferenceEffect>);  // an art model, not a shader's actor
				};
				if (OnHerWeapon(a_this)) {
					UndimmedFor(pass);
				} else {
					pass();
				}
			});
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x3B, thunk);  // ReferenceEffect::UpdatePosition
		}
	};

	struct CellAnimations
	{
		static void thunk(RE::TESObjectCELL* a_cell)
		{
			func(a_cell);
			// this frame's fading numbers again for every hand's lights, without advancing any timer
			LightKit::Guarded("cell animations", []() { UndimmedFor([]() { Fade::ReapplyAll(); }); });
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			if (REL::Module::IsVR()) {
				// as Illuminated's VR build: this call's place in VR was never read, so nothing is written there
				SKSE::log::info("VR: cell animations not hooked; fading lights may show their full strength for part of a frame");
				return;
			}
			REL::Relocation<std::uintptr_t> site{ RELOCATION_ID(18458, 18889), 0x52 };
			if (!LightKit::IsCallAt(site.address())) {
				SKSE::log::warn(
					"cell animations: the expected call is not there (another plugin rewrote it?); fading lights may "
					"show their full strength for part of a frame");
				return;
			}
			func = SKSE::GetTrampoline().write_call<5>(site.address(), thunk);
		}
	};

	void InstallCallSites()
	{
		auto& trampoline = SKSE::GetTrampoline();
		// Skyrim VR's own addresses for the first three: the VR Address Library has no id for these functions (read off its
		// 0.282.0 file), so they are given as VR offsets - read off ReLight VR 4 (Nexus 193734, RE::Light built for VR), which
		// hooks these same calls in VR
		std::vector<REL::Relocation<std::uintptr_t>> sites{
			REL::Relocation<std::uintptr_t>{ REL::VariantID(33603, 34381, 0x54FAB0), REL::VariantOffset(0xAC, 0xE2, 0xAC) },
			REL::Relocation<std::uintptr_t>{ REL::VariantID(33391, 34151, 0x547A70), REL::VariantOffset(0x86, 0xCD, 0x86) },
			REL::Relocation<std::uintptr_t>{ REL::VariantID(42965, 44222, 0x7790E0), REL::VariantOffset(0x58, 0x36D, 0x58) }
		};
		if (REL::Module::IsAE()) {
			sites.emplace_back(RELOCATION_ID(33403, 34185), 0x407);
		}
		constexpr std::array thunks{ &MagicLight<0>::thunk, &MagicLight<1>::thunk, &MagicLight<2>::thunk, &MagicLight<3>::thunk };
		const std::array     nexts{ &MagicLight<0>::func, &MagicLight<1>::func, &MagicLight<2>::func, &MagicLight<3>::func };
		const auto           game = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(17208, 17610) }.address();
		// SE / AE: a site's place is the Address Library's, so the call there is the spell-light call (the game's, or another
		// plugin's hook of it). VR: the places were read off another mod, never off the game here - a site is written only when
		// its call leads to the game's own function or to RE::Light's hook of it (through the jump an SKSE trampoline slot
		// holds), so a wrong address is left alone instead of patched
		const auto known = [&](std::uintptr_t a_site) {
			if (!LightKit::IsCallAt(a_site)) {
				return false;
			}
			if (!REL::Module::IsVR()) {
				return true;
			}
			std::int32_t rel = 0;
			std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
			auto to = a_site + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
			if (to == game) {
				return true;
			}
			constexpr std::array<std::uint8_t, 6> kJump{ 0xFF, 0x25, 0, 0, 0, 0 };  // jmp [rip+0], then the hook's address
			MEMORY_BASIC_INFORMATION              mem{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(to), &mem, sizeof(mem)) == 0 || mem.State != MEM_COMMIT ||
				(mem.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0 || (mem.Protect & PAGE_GUARD) != 0 ||
				to + kJump.size() + sizeof(to) > reinterpret_cast<std::uintptr_t>(mem.BaseAddress) + mem.RegionSize ||
				std::memcmp(reinterpret_cast<const void*>(to), kJump.data(), kJump.size()) != 0) {
				return false;
			}
			std::memcpy(&to, reinterpret_cast<const void*>(to + kJump.size()), sizeof(to));
			HMODULE owner = nullptr;
			return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   reinterpret_cast<LPCWSTR>(to), &owner) != 0 &&
			       owner == GetModuleHandleW(L"ReLight.dll");
		};
		std::size_t hooked = 0, toGame = 0;
		for (std::size_t i = 0; i < sites.size() && i < thunks.size(); ++i) {
			const auto& target = sites[i];
			if (!known(target.address())) {
				SKSE::log::warn(
					"spell lights: call site {} of {} (game offset {:X}) is not the call it should be (another plugin rewrote it, or "
					"its address is off); left alone - those spells keep their own light handling",
					i + 1, sites.size(), target.address() - REL::Module::get().base());
				continue;
			}
			const auto old = trampoline.write_call<5>(target.address(), thunks[i]);
			*nexts[i] = old;
			++hooked;
			toGame += old == game ? 1 : 0;
		}
		// a site that still leads to the game's own function has no RE::Light hook there
		if (hooked == 0) {
			SKSE::log::warn("spell lights: no call site hooked - sneaking and Hand lights for do not reach spell lights");
		} else {
			SKSE::log::info(
				"spell lights: {} of {} call sites hooked, after every plugin loaded; {} led to the game's own function, {} to "
				"another plugin's hook (RE::Light's)",
				hooked, sites.size(), toGame, hooked - toGame);
		}
	}

	void InstallLoadHooks()
	{
		Load3D<RE::MissileProjectile>::Install();
		Load3D<RE::ArrowProjectile>::Install();
		Load3D<RE::GrenadeProjectile>::Install();
		Load3D<RE::BeamProjectile>::Install();
		Load3D<RE::FlameProjectile>::Install();
		Load3D<RE::ConeProjectile>::Install();
		Load3D<RE::BarrierProjectile>::Install();
		Load3D<RE::Explosion>::Install();
		Load3D<RE::Hazard>::Install();
		REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
		PlayerUpdate::func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), PlayerUpdate::thunk);  // Actor::Update (CommonLib's Actor.cpp)
		EffectUpdate<RE::ShaderReferenceEffect>::Install();
		EffectUpdate<RE::ModelReferenceEffect>::Install();
		CellAnimations::Install();
		SKSE::log::info(
			"projectile, explosion and hazard loads, the player update, effect updates and cell animations hooked after every "
			"plugin loaded");
	}
}

namespace Plugin
{
	void FindSpellBases() { FindSpellBasesOnce(); }
	void InstallSpellLightHooks() { InstallCallSites(); }
	void InstallLateHooks()
	{
		InstallLoadHooks();
		static_cast<void>(Signs());  // the perks and abilities that need the dark, found once here - never on the first frame of play
	}
}

bool Plugin::WantedDark(RE::NiLight* a_light)
{
	// the main thread's lists (GroundLights.cpp asks from the player update). Sneaking is not here: a dimmed light's twin
	// copies its dimmed fade
	return HeldOutForOption(a_light) || HandLightHeldOut(a_light);
}

float Plugin::SneakLevel() { return gSneakLevel.load(std::memory_order_relaxed); }

bool Plugin::CastByPlayer(const RE::TESObjectREFR* a_ref) { return CastByHer(a_ref); }

bool Plugin::LeftOutByBudget(const RE::Actor* a_actor)
{
	// Performance (his order 2026-10-20): the player and followers at most
	const int who = PerformanceMode() ? (std::max)(HandLightsFor(), 2) : HandLightsFor();
	if (!who || !a_actor || a_actor->IsPlayerRef()) {
		return false;
	}
	if (who == 1) {
		const float nearby = gNearby.load(std::memory_order_relaxed);  // "about forty paces" - [Lights] NearbyDistance
		const auto* player = RE::PlayerCharacter::GetSingleton();
		return !player || a_actor->GetPosition().GetSquaredDistance(player->GetPosition()) > nearby * nearby;
	}
	return who == 3 || !a_actor->IsPlayerTeammate();
}

void Plugin::ForgetSpellLights(const GoneLights& a_gone)
{
	std::erase_if(gMagicLights, [&](const RE::NiLight* a_l) { return a_gone.contains(a_l); });
	std::erase_if(gSneakKnown, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
	{
		std::lock_guard lock(gDimLock);
		std::erase_if(gDim, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
	}
	ForgetOptionLights(a_gone);
}
