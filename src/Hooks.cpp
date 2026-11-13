// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The hooks (main.cpp holds the order of work and the SKSE messages):
//   the spell-light call (up to four call sites, each before RE::Light's hook there): our hand lights made, the light
//                 budget ("Hand lights for");
//   3D loads      of projectiles, explosions and hazards: a switched-off option's light out before its first frame;
//   the player update: the per-frame passes in their order - kept lights swept, the switches, held lights,
//                 body lights, Brightness, the fading module, the ground twins;
//   effect updates and cell animations: the fading module's other two places.
//
// THREADS: every list of game lights here (and in Options.cpp, HandLights.cpp, Held.cpp) is filled and emptied on the
// main thread only. The hooks that may run on the game's loader and job threads (the spell-light call, a 3D load) read
// the settings (atomics) and the caster's plain state (position, follower), touch only the light itself (put it
// out, so it never shows a frame it should not) and hand the bookkeeping to the main thread as an SKSE task that holds the
// light until then - so no lock is held across a frame, and none is needed. The spell-light call also reads the names up
// the light's node chain (which hand it is); that leans on the actor's 3D not being reparented or freed while the game makes
// a light on it.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX  // the build defines it too (xmake.lua)
#endif
#include "Fade.h"
#include "Plugin.h"

#include <Windows.h>  // VirtualQuery, GetModuleHandleExW: whose code a VR call site's call leads into

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	// every spell light the game made through the hooked call sites, so the passes below can find it again. Main thread only. Plain
	// pointers: Keep.cpp holds each light, and ForgetSpellLights drops it here before it is freed
	std::unordered_set<RE::NiLight*> gMagicLights;

	// "Hand lights for - Everyone nearby": every other caster's casting light, its caster, and whether it is kept out now - out
	// while the caster is far, back when near, out again when far (Illuminated's ApplyHandBudget). Main thread only; plain
	// pointers, dropped by ForgetSpellLights
	struct BudgetLight
	{
		RE::FormID caster{ 0 };
		bool       out{ false };
	};
	std::unordered_map<RE::NiLight*, BudgetLight> gBudgetLights;
	constexpr int                                 kEveryoneNearby = 1;  // the Hand lights for choice (Menu.cpp kWho)
	// a far caster's light made on any thread, kept out until its task writes gBudgetLights (a twin must not light it before)
	std::mutex                       gBudgetPendingLock;
	std::unordered_set<RE::NiLight*> gBudgetPending;

	// main thread: the caster now, by the id it had when its light was made (the light goes when its caster's 3D does)
	[[nodiscard]] const RE::Actor* CasterOf(const BudgetLight& a_b) { return RE::TESForm::LookupByID<RE::Actor>(a_b.caster); }

	// on the main thread only (an SKSE task from the hook below): a new spell light goes into our lists. The last
	// reference to a light is only ever dropped on the main thread (here, or in Keep.cpp's sweep)
	void NoteMagicLight(const RE::NiPointer<RE::NiLight>& a_light, const std::string& a_handKey, bool a_ours, Pulse::Kind a_kind,
		Plugin::Dressed a_dressed)
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
			Plugin::RememberLight(a_light.get(), a_handKey.empty() ? nullptr : Plugin::HandFxOf(a_handKey), a_kind, !a_handKey.empty(),
				a_dressed);
		}
	}

	// "Hand lights for" (Illuminated's light budget for big fights, ported): this is the call that makes a casting light, ours
	// or RE::Light's - a caster the setting leaves out gets none, except under "Everyone nearby" (made, kept out until near)
	[[nodiscard]] bool LeftOutCaster(const RE::TESObjectREFR* a_ref)
	{
		const auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
		return actor && Plugin::LeftOutByBudget(actor);  // the one budget rule, for held lights too (Held.cpp)
	}

	// "Everyone nearby" only: every other caster's light is made and followed, kept out while the caster is far
	[[nodiscard]] bool NearbyBudget() { return !Plugin::PerformanceMode() && Plugin::HandLightsFor() == kEveryoneNearby; }

	// main thread, before the switches: each caster asked once a frame; a light whose caster came near is lit again here (a
	// switched-off one goes out again in UpdateOptionLights), one whose caster is far goes out
	void UpdateBudgetLights()
	{
		if (gBudgetLights.empty()) {
			return;
		}
		const LightKit::Cost::Pass                                 pass{ 4, "budget lights" };
		static std::unordered_map<RE::FormID, std::optional<bool>> leftOut;  // main thread; its storage kept between frames
		leftOut.clear();
		for (auto& [light, b] : gBudgetLights) {
			const auto [it, added] = leftOut.try_emplace(b.caster);
			if (added) {
				if (const auto* actor = CasterOf(b)) {
					it->second = Plugin::LeftOutByBudget(actor);
				}
			}
			if (!it->second) {
				continue;  // its caster not found: left as it is
			}
			if (*it->second) {
				light->SetAppCulled(true);
			} else if (b.out && !Plugin::HeldOutForOption(light) && !Plugin::HandLightHeldOut(light)) {
				light->SetAppCulled(false);
			}
			b.out = *it->second;
		}
	}

	// main thread, after the switches: a light a switch gave back stays out while its caster is far
	void HoldBudgetLightsOut()
	{
		if (gBudgetLights.empty()) {
			return;
		}
		const LightKit::Cost::Pass pass{ 6, "budget held out" };
		for (const auto& [light, b] : gBudgetLights) {
			if (b.out) {
				light->SetAppCulled(true);
			}
		}
	}

	using MagicLightFn = RE::NiPointLight*(RE::TESObjectLIGH*, RE::TESObjectREFR*, RE::NiNode*, bool, bool, bool);

	// On Community Shaders a hand light sits out from the palm, where Illuminated's does. Only the fresh light's own fields are
	// written (this runs on loader and job threads). Under a billboard, which turns with the view, it stays put; VR's hand frame is unmeasured.
	void OutOfThePalm(RE::NiPointLight* a_light, const RE::NiNode* a_node)
	{
		if (!a_light || !a_node || !Plugin::InverseSquare() || REL::Module::IsVR()) {
			return;
		}
		RE::NiTransform chain;
		for (const RE::NiAVObject* at = a_node; at; at = at->parent) {
			const std::string_view name(at->name.c_str());
			if (name.contains("L MagicNode") || name.contains("R MagicNode")) {
				a_light->local.translate = chain.Invert() * LightKit::kHandPoint;
				a_light->world.translate = a_node->world * a_light->local.translate;
				return;
			}
			if (name.contains("Billboard")) {
				return;
			}
			chain = at->local * chain;
		}
	}

	// Her hand lights that cast a shadow (the Bottled shadow download): every hand light of hers; with both hands lit they sit
	// at one point between them, so she throws one shadow, not two crossed ones. Filled on any thread, pruned on the main thread;
	// Keep.cpp holds each light, ForgetSpellLights drops it here first
	std::mutex                       gHerShadowLock;
	std::atomic<bool>                gHerShadowsAny{ false };  // set when one joins; the pass clears it when none is left
	std::unordered_set<RE::NiLight*> gHerShadows;
	std::unordered_set<RE::NiLight*> gShadowsBetween;  // the casters moved between her hands; main thread only

	// a hand light that shows; faded to nothing (the off choice) or put out by a switch, its hand counts as unlit
	[[nodiscard]] bool Shows(RE::NiLight* a_light) { return a_light->GetLightRuntimeData().fade > 0.0f && !Plugin::WantedDark(a_light); }

	[[nodiscard]] bool HasHandLight(const RE::NiAVObject* a_magic)
	{
		return std::ranges::any_of(gMagicLights, [a_magic](RE::NiLight* a_light) {
			if (!Shows(a_light)) {
				return false;
			}
			for (const RE::NiAVObject* up = a_light->parent; up; up = up->parent) {
				if (up == a_magic) {
					return true;
				}
			}
			return false;
		});
	}

	// the hand magic node a light hangs under, or null under a Billboard (it turns with the view: OutOfThePalm left it in place)
	[[nodiscard]] RE::NiAVObject* MagicNodeOf(const RE::NiLight* a_light)
	{
		for (RE::NiAVObject* up = a_light->parent; up; up = up->parent) {
			const std::string_view name(up->name.c_str());
			if (name.contains("Billboard")) {
				return nullptr;
			}
			if (name.contains("L MagicNode") || name.contains("R MagicNode")) {
				return up;
			}
		}
		return nullptr;
	}

	void MoveTo(RE::NiLight* a_light, const RE::NiPoint3& a_point)
	{
		a_light->local.translate = a_light->parent->world.Invert() * a_point;
		a_light->world.translate = a_point;
	}

	// main thread, each frame: with both her hands lit their shadow casters sit at one point between them, so she throws one
	// shadow; a hand lit alone casts from out of its own palm (OutOfThePalm's place)
	void PlaceHerShadow()
	{
		if (!gHerShadowsAny.load(std::memory_order_relaxed)) {
			return;
		}
		static std::vector<RE::NiLight*> lights;
		lights.clear();
		{
			std::lock_guard lock(gHerShadowLock);
			std::erase_if(gHerShadows, [](const RE::NiLight* a_l) { return !a_l->parent; });
			lights.assign(gHerShadows.begin(), gHerShadows.end());
			std::erase_if(gShadowsBetween, [](RE::NiLight* a_l) { return !gHerShadows.contains(a_l); });
			gHerShadowsAny.store(!gHerShadows.empty(), std::memory_order_relaxed);
		}
		if (lights.empty() || !Plugin::InverseSquare() || REL::Module::IsVR()) {
			return;
		}
		const LightKit::Cost::Pass pass{ 10, "her shadow, promoted hands" };
		// her two hands are paired per skeleton: a first-person hand never pairs with a third-person one
		const auto rootOf = [](RE::NiAVObject* a_node) {
			while (a_node->parent) {
				a_node = a_node->parent;
			}
			return a_node;
		};
		for (auto* light : lights) {
			auto* node = MagicNodeOf(light);
			if (!node) {
				continue;
			}
			const bool  left = std::string_view(node->name.c_str()).contains("L MagicNode");
			const auto* strings = RE::FixedStrings::GetSingleton();
			auto*       other = strings ? rootOf(node)->GetObjectByName(left ? strings->npcRMagicNode : strings->npcLMagicNode) : nullptr;
			const bool  both = other && std::ranges::any_of(lights, [other](RE::NiLight* a_l) {
				return MagicNodeOf(a_l) == other && Shows(a_l);
			});
			if (both) {
				MoveTo(light, (node->world * LightKit::kHandPoint + other->world * LightKit::kHandPoint) * 0.5f);
				gShadowsBetween.insert(light);
			} else if (gShadowsBetween.erase(light) > 0) {
				OutOfThePalm(netimmerse_cast<RE::NiPointLight*>(light), light->parent);
			}
		}
	}

	// the setting in whichever section holds it; parsed, so a repeated key keeps its last value as Open Shaders reads it
	[[nodiscard]] bool PromoteIn(const nlohmann::json& a_node)
	{
		if (a_node.is_object()) {
			if (const auto it = a_node.find("PromoteNormalToShadow"); it != a_node.end() && it->is_boolean()) {
				return it->get<bool>();
			}
		}
		if (a_node.is_structured()) {
			for (const auto& child : a_node) {
				if (PromoteIn(child)) {
					return true;
				}
			}
		}
		return false;
	}

	// Open Shaders' "Promote Normal Lights to Shadow Casters" makes every light a caster, so both her hand lights cast; it reads
	// the setting at start
	[[nodiscard]] bool PromoteOn()
	{
		std::ifstream in("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", std::ios::binary);
		const auto    json = nlohmann::json::parse(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(), nullptr, false);
		return !json.is_discarded() && PromoteIn(json);
	}

	std::vector<RE::NiPointLight*> gPromotedBetween;  // main thread only; a light is read only while gMagicLights holds it

	// main thread, each frame, with Promote on and no Bottled shadow download: while both her hands are lit their lights sit at
	// one point between them, so she throws one shadow (the same lights, the same brightness)
	void PlacePromotedHands()
	{
		static const bool on = Plugin::InverseSquare() && !REL::Module::IsVR() && !Plugin::ShadowPatch() && PromoteOn();
		auto*             player = RE::PlayerCharacter::GetSingleton();
		const auto*       strings = RE::FixedStrings::GetSingleton();
		if (!on || !player || !strings || (gMagicLights.empty() && gPromotedBetween.empty())) {
			return;
		}
		if (gPromotedBetween.empty() && player->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kSheathed) {
			return;  // her hands down: nothing to place and nothing to put back (a draw's first frames already light her hands)
		}
		const LightKit::Cost::Pass            pass{ 10, "her shadow, promoted hands" };
		static std::vector<RE::NiPointLight*> between;
		between.clear();
		for (const bool first : { false, true }) {
			auto* root = player->Get3D(first);
			auto* left = root ? root->GetObjectByName(strings->npcLMagicNode) : nullptr;
			auto* right = root ? root->GetObjectByName(strings->npcRMagicNode) : nullptr;
			if (!left || !right || !HasHandLight(left) || !HasHandLight(right)) {
				continue;
			}
			const auto point = (left->world * LightKit::kHandPoint + right->world * LightKit::kHandPoint) * 0.5f;
			for (auto* any : gMagicLights) {
				auto*       light = netimmerse_cast<RE::NiPointLight*>(any);
				const auto* node = light ? MagicNodeOf(light) : nullptr;
				if (node && (node == left || node == right)) {
					MoveTo(light, point);
					between.push_back(light);
				}
			}
		}
		for (auto* light : gPromotedBetween) {
			if (std::ranges::find(between, light) == between.end() && gMagicLights.contains(light) && light->parent) {
				OutOfThePalm(light, light->parent);
			}
		}
		gPromotedBetween.assign(between.begin(), between.end());
	}

	// the spell-light call, at every hooked site; a_next is what that site called before us (the game's function, or another
	// plugin's hook - RE::Light's)
	RE::NiPointLight* MakeMagicLight(const REL::Relocation<MagicLightFn>& a_next, RE::TESObjectLIGH* a_light, RE::TESObjectREFR* a_ref,
		RE::NiNode* a_node, bool a_forceDynamic, bool a_useLightRadius, bool a_affectRequesterOnly)
	{
		const bool distant = LeftOutCaster(a_ref);
		const bool nearby = NearbyBudget();
		if (distant && !nearby) {
			return nullptr;
		}
		const RE::FormID budgetCaster = nearby && a_ref && a_ref->As<RE::Actor>() && !a_ref->IsPlayerRef() ? a_ref->GetFormID() : 0;
		// our hand lights are made with the game's own function and never reach RE::Light, which would switch off
		// a casting light it has no config for (an in-memory record can have none)
		const Plugin::Hand* hand = Plugin::HandOfLight(a_light);
		RE::NiPointLight*   made = nullptr;
		Plugin::Dressed     dressed{};  // what a hand light's dress wrote: its Brightness bases (the task carries them)
		if (hand) {
			// her own hand lights cast shadows with the optional Bottled Shaders shadow download; with both hands lit
			// PlaceHerShadow puts them at one point, so she throws one shadow
			const bool hers = a_ref && a_ref->IsPlayerRef() && Plugin::ShadowPatch();
			auto*      use = hers ? Plugin::ShadowTwinOf(a_light) : a_light;
			made = netimmerse_cast<RE::NiPointLight*>(use->GenDynamic(a_ref, a_node, a_forceDynamic, a_useLightRadius, a_affectRequesterOnly));
			if (hers && use != a_light && made) {
				LightKit::Guarded("her shadow light", [made]() {
					std::lock_guard lock(gHerShadowLock);
					gHerShadows.insert(made);
					gHerShadowsAny.store(true, std::memory_order_relaxed);
				});
			}
			dressed = Plugin::DressHandLight(made, *hand);  // only writes the fresh light's own fields
			OutOfThePalm(made, a_node);
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
		if (made) {
			LightKit::Guarded("spell light", [&]() {
				const bool ours = hand || Plugin::IsSprayLight(a_light) || Plugin::IsStreamObject(a_ref);
				if (distant) {
					made->SetAppCulled(true);
				}
				if (auto* tasks = SKSE::GetTaskInterface()) {
					// the spell in the hand now, when its light is made - a spell swapped before the task runs is not this light's
					// the light may hang on a node inside the hand art (its AttachLight), so the hand is read off the first magic node
					// or hand above it (the review's hostile M3)
					bool left = false;
					for (const RE::NiAVObject* up = a_node; up; up = up->parent) {
						const std::string_view name = up->name.c_str() ? up->name.c_str() : "";
						if (name.contains("L MagicNode") || name.contains("L Hand")) {
							left = true;
							break;
						}
						if (name.contains("R MagicNode") || name.contains("R Hand")) {
							break;
						}
					}
					const auto kind = hand && a_ref ? LightKit::PulseKindOfHand(a_ref->As<RE::Actor>(), left) : Pulse::Kind::kNone;
					const bool pending = budgetCaster != 0 && distant;
					if (pending) {
						std::lock_guard lock(gBudgetPendingLock);
						gBudgetPending.insert(made);
					}
					tasks->AddTask([light = RE::NiPointer<RE::NiLight>(made), key = hand ? hand->key : std::string{}, ours, budgetCaster, distant, kind, dressed, pending]() {
						LightKit::Guarded("spell light task", [&]() {
							NoteMagicLight(light, key, ours, key.empty() ? Pulse::Kind::kNone : kind, dressed);
							if (budgetCaster != 0 && gMagicLights.contains(light.get())) {
								gBudgetLights[light.get()] = { budgetCaster, distant };
							}
						});
						LightKit::Guarded("spell light pending", [&]() {
							if (pending) {
								std::lock_guard lock(gBudgetPendingLock);
								gBudgetPending.erase(light.get());
							}
							if (!gMagicLights.contains(light.get())) {  // let go before the main thread came round: never kept
								std::lock_guard lock(gHerShadowLock);
								gHerShadows.erase(light.get());
							}
						});
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
			LightKit::Guarded("3D load", [&]() {
				// may be a loader thread: the lights are put out now (never a frame lit) and our lists take them on the main thread.
				// RE::Light has already hung its light here; a switched-off option's goes out before its first frame
				std::size_t option = Plugin::kNone;
				if (auto culled = Plugin::CullOptionLightsUnder(root, option); !culled.empty()) {
					tasks->AddTask([lights = std::move(culled), option]() { LightKit::Guarded("option lights task", [&]() { Plugin::AdoptOptionLights(option, lights); }); });
				}
				Plugin::NoteSweep(Plugin::SweepWhy::kMagicObject);
				if constexpr (std::is_same_v<T, RE::Hazard>) {
					Plugin::NoteOptionsLook();  // RE::Light may have hung a twin light on it (Options.cpp TwinNet)
				}
				if constexpr (std::is_base_of_v<RE::Projectile, T>) {
					Plugin::QueueRow(a_this);  // a bolt / spray / cone our data lights: its row of lights, hung on the main thread (Rows.cpp)
				}
			});
			return root;
		}
		// and out: a light kept for it stays in the scene until the sweep lets go, and no other moment announces its end. Its own vtable's
		// slot, which RefSet3D's write on the plain reference's never reaches. Flags only, any thread
		static void SetThunk(T* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
		{
			const bool had = a_this->Is3DLoaded();
			set(a_this, a_object, a_queue3DTasks);
			if (!a_object && had) {
				Plugin::NoteSweep(Plugin::SweepWhy::kMagicDrop);
			}
		}
		static inline REL::Relocation<decltype(thunk)>    func;
		static inline REL::Relocation<decltype(SetThunk)> set;
		static void                                       Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
			func = vtbl.write_vfunc(0x6A, thunk);
			set = vtbl.write_vfunc(0x6C, SetThunk);
		}
	};

	struct PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			func(a_this, a_delta);
			LightKit::Guarded("player update", [a_delta]() {
				// each pass times itself after its own early return (LightKit::Cost::Pass)
				// first: every list forgets the lights that left the game since the last frame, and they are freed
				Plugin::SweepKeptLights();
				UpdateBudgetLights();
				Plugin::UpdateOptionLights();
				HoldBudgetLightsOut();
				Plugin::UpdateHeldLights();
				Plugin::UpdateBodyLights();  // the body glow while casting (BodyLights.cpp)
				Plugin::UpdateRows();        // the rows of lights along bolts, sprays and cones (Rows.cpp)
				PlaceHerShadow();
				PlacePromotedHands();
				// RE::Light has already written this frame's fades (its update runs inside `func` above): the sliders next, then
				// the fading module as the final pass, on what the sliders wrote (each fade it writes is reported back -
				// Brightness.cpp's NoteFadeWrite)
				Plugin::UpdateBrightness(a_delta);
				Fade::UpdateHands(a_delta);
				Plugin::TickGroundLights();  // last: off Community Shaders the hand lights' twins copy this frame's numbers (GroundLights.cpp)
				LightKit::Cost::FrameEnd();
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
			// every effect in the loaded world comes through here: nothing of the fading module's to do is the common case
			if (!Fade::EffectsWanted()) {
				return;
			}
			LightKit::Guarded("effect update", [a_this]() {
				Fade::AfterReferenceEffect(a_this, std::is_same_v<T, RE::ModelReferenceEffect>);  // an art model, not a shader's actor
			});
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x3B, thunk);  // ReferenceEffect::UpdatePosition
		}
	};

	// an art starting on anything (Light Placer's Init way - powerof3, GPL-3.0, used with his permission): the body lights hear of
	// it (BodyLights.cpp), and a shader's start asks for the option walk - RE::Light hangs an enchantment light there. Flags only
	template <class T>
	struct ArtInit
	{
		// the game acts on what Init returns, though CommonLib declares it void: a thunk that loses it drops the effect
		static bool thunk(T* a_this)
		{
			const bool started = func(a_this);
			Plugin::NoteSweep(Plugin::SweepWhy::kArtInit);
			Plugin::NoteArtStarted();
			if constexpr (std::is_same_v<T, RE::ShaderReferenceEffect>) {
				Plugin::NoteOptionsLook();
			}
			return started;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x36, thunk);  // ReferenceEffect::Init
		}
	};

	// a shader effect taken off the game's list by this call is never marked finished (a temporary-effects clear): the body lights look again. Flags only
	struct ArtDetach
	{
		static void thunk(RE::ShaderReferenceEffect* a_this)
		{
			Plugin::NoteSweep(Plugin::SweepWhy::kArtDetach);
			Plugin::NoteArtStarted();
			func(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(RE::ShaderReferenceEffect::VTABLE[0]).write_vfunc(0x27, thunk);  // BSTempEffect::Detach
		}
	};

	// only a reference RE::Light lights for us (its base one of our options) or a weapon (its enchantment light) brings or ends a light of
	// ours: the scenery streaming in and out at rest must not wake the sliders' walk
	[[nodiscard]] bool MayCarryOurLight(RE::TESObjectREFR* a_ref) noexcept
	{
		// a game hook: nothing may leave it, and a lookup that fails wakes the walk as before
		try {
			const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			return base && (base->IsWeapon() || Plugin::OptionOf(base) != Plugin::kNone);
		} catch (...) {
			return true;
		}
	}

	// a plain reference's 3D in, where RE::Light hangs its light: the game's load event names no reference placed by a script. Flags only, any thread
	struct RefLoad3D
	{
		static RE::NiAVObject* thunk(RE::TESObjectREFR* a_this, bool a_backgroundLoading)
		{
			auto* root = func(a_this, a_backgroundLoading);
			if (MayCarryOurLight(a_this)) {
				Plugin::NoteSweep(Plugin::SweepWhy::kRefLoad);
			}
			return root;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(RE::TESObjectREFR::VTABLE[0]).write_vfunc(0x6A, thunk);
		}
	};

	// and out: lights hung on it leave with it. Flags only, any thread
	struct RefSet3D
	{
		static void thunk(RE::TESObjectREFR* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
		{
			// most "no body" sets land on a reference that had none: the one cheap read comes before the base lookup
			const bool had = a_this->Is3DLoaded();
			func(a_this, a_object, a_queue3DTasks);
			if (!a_object && had && MayCarryOurLight(a_this)) {
				Plugin::NoteSweep(Plugin::SweepWhy::kRefDrop);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			func = REL::Relocation<std::uintptr_t>(RE::TESObjectREFR::VTABLE[0]).write_vfunc(0x6C, thunk);
		}
	};

	struct CellAnimations
	{
		static void thunk(RE::TESObjectCELL* a_cell)
		{
			func(a_cell);
			// every loaded cell comes through here: nothing to write again is the common case
			if (!Fade::ReapplyWanted()) {
				return;
			}
			// this frame's fading numbers again for every hand's lights, without advancing any timer
			LightKit::Guarded("cell animations", []() {
				const LightKit::Cost::Scope c{ 16, "cell animations" };
				Fade::ReapplyAll();
			});
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
			// trampoline sites: 4 (the spell-light call sites; the fourth exists on AE only)
			const auto old = trampoline.write_call<5>(target.address(), thunks[i]);
			*nexts[i] = old;
			++hooked;
			toGame += old == game ? 1 : 0;
		}
		// a site that still leads to the game's own function has no RE::Light hook there
		if (hooked == 0) {
			SKSE::log::warn("spell lights: no call site hooked - Hand lights for does not reach spell lights");
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
		RefLoad3D::Install();
		RefSet3D::Install();
		REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
		PlayerUpdate::func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), PlayerUpdate::thunk);  // Actor::Update (CommonLib's Actor.cpp)
		EffectUpdate<RE::ShaderReferenceEffect>::Install();
		EffectUpdate<RE::ModelReferenceEffect>::Install();
		ArtInit<RE::ShaderReferenceEffect>::Install();
		ArtInit<RE::ModelReferenceEffect>::Install();
		ArtDetach::Install();
		CellAnimations::Install();
		SKSE::log::info(
			"projectile, explosion and hazard loads, the player update, effect updates and cell animations hooked after every "
			"plugin loaded");
	}
}

namespace Plugin
{
	void InstallSpellLightHooks() { InstallCallSites(); }
	void InstallLateHooks() { InstallLoadHooks(); }
}

bool Plugin::WantedDark(RE::NiLight* a_light)
{
	// the main thread's lists (GroundLights.cpp asks from the player update)
	return HeldOutForOption(a_light) || HandLightHeldOut(a_light) || KeptOutByBudget(a_light);
}

bool Plugin::KeptOutByBudget(RE::NiLight* a_light)
{
	// asked live: a task between two player updates must not read the last frame's flag for a caster who has moved since
	const auto it = gBudgetLights.find(a_light);
	if (it == gBudgetLights.end()) {
		std::lock_guard lock(gBudgetPendingLock);
		return gBudgetPending.contains(a_light);
	}
	const auto* actor = CasterOf(it->second);
	return actor ? LeftOutByBudget(actor) : it->second.out;
}

bool Plugin::BudgetByDistance() { return NearbyBudget(); }

bool Plugin::LeftOutByBudget(const RE::Actor* a_actor)
{
	// Performance: the player and followers at most
	const int who = PerformanceMode() ? (std::max)(HandLightsFor(), 2) : HandLightsFor();
	if (!who || !a_actor || a_actor->IsPlayerRef()) {
		return false;
	}
	if (who == kEveryoneNearby) {
		const float nearby = gNearby.load(std::memory_order_relaxed);  // "about forty paces" - [Lights] NearbyDistance
		const auto* player = RE::PlayerCharacter::GetSingleton();
		return !player || a_actor->GetPosition().GetSquaredDistance(player->GetPosition()) > nearby * nearby;
	}
	return who == 3 || !a_actor->IsPlayerTeammate();
}

void Plugin::ForgetSpellLights(const GoneLights& a_gone)
{
	std::erase_if(gMagicLights, [&](const RE::NiLight* a_l) { return a_gone.contains(a_l); });
	std::erase_if(gBudgetLights, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
	{
		std::lock_guard lock(gHerShadowLock);
		std::erase_if(gHerShadows, [&](const RE::NiLight* a_l) { return a_gone.contains(a_l); });
	}
	std::erase_if(gShadowsBetween, [&](const RE::NiLight* a_l) { return a_gone.contains(a_l); });
	std::erase_if(gPromotedBetween, [&](const RE::NiPointLight* a_l) { return a_gone.contains(a_l); });
	ForgetOptionLights(a_gone);
}
