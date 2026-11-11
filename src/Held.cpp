// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// A weapon's own light while it is drawn. RE::Light lights a weapon's mesh only where the weapon is a reference (on the
// ground, on a rack) and reaches a weapon in the hand only through its enchantment shader. The build writes the light each
// staff and weapon config of ours gives that mesh as a `held` line; this hangs it on the drawn weapon of whoever holds it
// (the player, in both views, and the actors near them) and takes it off when it is sheathed, unequipped or switched off.
// One object, one light: a weapon whose enchantment shader is one of ours may wear RE::Light's enchantment light in the
// hand, so the held light steps aside the frame that light is on its node (ReLightOn). "Hand lights for" (the light budget
// for big fights) leaves out the same actors here as it does for hand lights. Nothing on any record is edited.
//
// CREDIT: how a light is made and registered here (MakeOwnLight) follows ReLight by Truman (github.com/TrumanGIT/ReLight),
// GPL-3.0-or-later, with his permission and kept under the same license. From ReLight: one master NiPointLight made once
// and cloned for every use (a freshly made light attached straight away crashes), the light's size carried in the
// radius' z, the create parameters a non-shadow light needs (field of view 90, portal-strict, never fades), and handing
// the light to the shadow scene node, which is what renders it.

#include "Plugin.h"

#include <algorithm>

namespace Plugin
{
	namespace
	{
		constexpr std::size_t kMaxLive = 16;
		constexpr const char* kLightName = "RSAHeld";  // was "RSAStream", from the travelling lights this code came from
		bool                  gCapLogged = false;

		struct Live
		{
			RE::FormID                      actor{ 0 };
			bool                            left{ false }, firstPerson{ false };
			const Stream*                   held{ nullptr };
			RE::NiPointer<RE::NiNode>       node;  // what it hangs on - held, so taking the light off never reaches a freed node
			RE::NiPointer<RE::BSLight>      bs;
			RE::NiPointer<RE::NiPointLight> light;
			float                           written{ -1.0f }, wroteReach{ -1.0f };
		};

		std::vector<Live>        gLive;
		std::atomic<std::size_t> gLiveCount{ 0 };  // the testing DLL's pages read it from the render thread
		std::size_t              gTold = 0;

		struct Want
		{
			RE::Actor*    actor;
			bool          left, firstPerson;
			const Stream* held;
			RE::NiNode*   node;
		};

		// a held light that could not be made or registered: warned about once, and tried again every kRetryUpdates updates
		// while that hand still wants it - the scene may refuse a light for a while (the ground twins' retry, 2.2's CodeRabbit
		// round); forgotten when the hand stops wanting it (sheathed, unequipped, another weapon)
		constexpr std::uint64_t kRetryUpdates = 60;
		std::uint64_t           gUpdate = 0;  // UpdateHeldLights calls; 64 bits, so a retry deadline never wraps

		struct Failed
		{
			RE::FormID    actor{ 0 };
			bool          left{ false }, firstPerson{ false };
			const Stream* held{ nullptr };
			std::uint64_t retryAt{ 0 };

			[[nodiscard]] bool Is(const Want& a_w) const
			{
				return a_w.actor->GetFormID() == actor && a_w.left == left && a_w.firstPerson == firstPerson && a_w.held == held;
			}
		};
		std::vector<Failed> gFailed;

		// the actors whose weapon is out or coming out: SKSE's draw events note them (any thread, Events.cpp) and a look at an actor's
		// or a game's load adds any that sent none (one loaded with it drawn), so the pass walks these, not every actor near her
		std::mutex              gDrawnLock;
		std::vector<RE::FormID> gDrawnNoted;              // gDrawnLock
		std::atomic<bool>       gDrawnNotedAny{ false };  // a sink noted one since the last pass
		std::vector<RE::FormID> gDrawn;                   // the main thread's
		std::atomic<bool>       gLookNow{ true };         // a game loaded or an actor's 3D came in: look at once
		// the game stores a body a moment after its load is noted, and nothing says so: such an actor is looked at again for about 5 s
		constexpr int           kWaitLooksMost = 300;
		int                     gWaitLooks = 0;  // the main thread's
		bool                    gWaitingBody = false;
		std::vector<RE::FormID> gFar;  // listed but out of the high process: nothing announces one coming near

		// a far actor came near, or left: read only while one is listed
		[[nodiscard]] bool FarChanged()
		{
			return std::ranges::any_of(gFar, [](RE::FormID a_id) {
				const auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
				const auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
				return !actor || (process && process->InHighProcess());
			});
		}

		void AddDrawn(RE::FormID a_id)
		{
			if (std::ranges::find(gDrawn, a_id) == gDrawn.end()) {
				gDrawn.push_back(a_id);
			}
		}

		[[nodiscard]] bool WeaponOut(RE::Actor* a_actor) { return a_actor->AsActorState()->GetWeaponState() != RE::WEAPON_STATE::kSheathed; }

		void LookForDrawn(RE::PlayerCharacter* a_player)
		{
			if (WeaponOut(a_player)) {
				AddDrawn(a_player->GetFormID());
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (auto actor = handle.get(); actor && WeaponOut(actor.get())) {
						AddDrawn(actor->GetFormID());
					}
				}
			}
		}

		void TakeOff(Live& a_v, RE::ShadowSceneNode* a_scene)
		{
			TakeOffOwnLight(a_scene, a_v.bs);
			if (a_v.node && a_v.light && a_v.light->parent == a_v.node.get()) {
				a_v.node->DetachChild(a_v.light.get());
			}
		}

		[[nodiscard]] bool BothHands(const RE::TESObjectWEAP* a_weapon)
		{
			using T = RE::WEAPON_TYPE;
			const auto type = a_weapon->GetWeaponType();
			return type == T::kTwoHandSword || type == T::kTwoHandAxe || type == T::kBow || type == T::kCrossbow;
		}

		// RE::Light may hang its enchantment light on this weapon in the hand: an effect of its enchantment (the instance's own,
		// else the weapon's) carries an enchantment shader a config of ours names
		[[nodiscard]] bool EnchantmentLit(RE::Actor* a_actor, bool a_left, const RE::TESObjectWEAP* a_weapon)
		{
			const auto* entry = a_actor->GetEquippedEntryData(a_left);
			const auto* ench = entry ? entry->GetEnchantment() : nullptr;
			if (!ench) {
				ench = a_weapon->formEnchanting;
			}
			if (!ench) {
				return false;
			}
			return std::ranges::any_of(ench->effects, [](const RE::Effect* a_e) {
				return a_e && a_e->baseEffect && OptionOfShader(a_e->baseEffect->data.enchantShader) != kNone;
			});
		}

		// ...and has it? Measured on a Bound Sword: some games it does (its "RL" light, fadeAmount 5, on the weapon),
		// others none at all (after a load, in a long session) - so the held line lights the weapon whenever that light is not
		// there, in this view, and steps aside the frame it is
		[[nodiscard]] bool ReLightOn(RE::NiAVObject* a_node)
		{
			bool found = false;
			RE::BSVisit::TraverseScenegraphLights(a_node, [&found](RE::NiPointLight* a_light) {
				if (IsEnchantLight(a_light)) {  // RE::Light's enchantment light (Plugin.h)
					found = true;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return found;
		}

		// the drawn weapon's own 3D in this view, parented to the hand node - the config's position is relative to the mesh.
		// A bow is held in the left hand, so its 3D may sit under SHIELD though it is equipped in the right. Falls back to the
		// hand node itself when the biped has no clone of it.
		[[nodiscard]] RE::NiNode* NodeFor(RE::Actor* a_actor, bool a_left, bool a_firstPerson, const RE::TESObjectWEAP* a_weapon)
		{
			auto* root = a_actor->Get3D(a_firstPerson);
			if (!root) {
				return nullptr;
			}
			const std::string_view hand = a_left ? "SHIELD" : "WEAPON";
			if (const auto& biped = a_actor->GetBiped(a_firstPerson)) {
				RE::NiNode* other = nullptr;
				for (auto& obj : biped->objects) {
					auto*       clone = obj.item == a_weapon && obj.partClone ? obj.partClone->AsNode() : nullptr;
					const char* parent = clone && clone->parent ? clone->parent->name.c_str() : nullptr;
					if (!parent) {
						continue;
					}
					if (hand == parent) {
						return clone;
					}
					if (!a_left && a_weapon->IsBow() && std::string_view(parent) == "SHIELD") {
						other = clone;
					}
				}
				if (other) {
					return other;
				}
			}
			// pooled once: a string made from text is looked up in the game's string table each time
			static const RE::BSFixedString kShield{ "SHIELD" }, kWeapon{ "WEAPON" };
			auto*                          node = root->GetObjectByName(a_left ? kShield : kWeapon);
			return node ? node->AsNode() : nullptr;
		}

		// the weapons one actor has drawn, and the node each light hangs on, in each view the actor has
		void WantsOf(RE::Actor* a_actor, bool a_hidden, std::vector<Want>& a_out)
		{
			if (!a_actor || !a_actor->Is3DLoaded() || !a_actor->AsActorState()->IsWeaponDrawn()) {
				return;
			}
			const bool isPlayer = a_actor->IsPlayerRef();
			if ((isPlayer && a_hidden) || LeftOutByBudget(a_actor)) {
				return;  // sneaking, or an actor "Hand lights for" leaves out (the audit, 2026-10-14: held lights ignored it)
			}
			for (const bool left : { false, true }) {
				auto* form = a_actor->GetEquippedObject(left);
				auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
				// a weapon held in both hands is one weapon: it is lit once, from the right hand
				if (!weapon || (left && weapon == a_actor->GetEquippedObject(false) && BothHands(weapon))) {
					continue;
				}
				const bool enchanted = EnchantmentLit(a_actor, left, weapon);
				for (const bool fp : { false, true }) {
					if (fp && !isPlayer) {
						break;
					}
					const Stream* held = HeldOf(weapon, fp);
					if (!held || !OptionLit(held->option) || AllLightsOff()) {
						continue;
					}
					if (auto* node = NodeFor(a_actor, left, fp, weapon); node && !(enchanted && ReLightOn(node))) {
						a_out.push_back({ a_actor, left, fp, held, node });
					}
				}
			}
		}
	}

	namespace
	{
		// off Community Shaders the reach is set through the game's attenuation call, which writes the same two words as the
		// light's ambient color; the ambient is written after it, as when the light was made (the audit, 2026-10-14: the
		// Reach slider moved only the stated radius of a held light on Vanilla and ENB)
		void SetPlainReach(RE::NiPointLight* a_light, float a_reach, const RE::NiColor& a_color)
		{
			a_light->SetLightAttenuation(a_reach);
			// a Community Shaders that draws linear still reads these words as its flags: Initialised alone, never a color
			if (LightLimitFix()) {
				Isl::SetOwnLinear(a_light);
			} else {
				a_light->GetLightRuntimeData().ambient = LightKit::Tenth(a_color);
			}
		}
	}

	// the same Brightness the brightness pass gives every other light of ours; off Community Shaders a light made for inverse
	// square lighting is drawn plain - the
	// house light the hand lights take (LightMath.h PlainAt: fade x 1.14, the reach drawn at 178 / 133)
	OwnScale OwnLightScale()
	{
		const bool isl = InverseSquare();
		return { Brightness() * VanillaStrength() * (isl ? 1.0f : 1.14f), Reach() * (isl ? 1.0f : 178.0f / 133.0f) };
	}

	// one light made and registered the ReLight way, on the main thread; nullptr when it could not be. Held lights and body
	// lights (BodyLights.cpp) are both made here
	RE::BSLight* MakeOwnLight(const Stream& a_s, float a_fade, float a_reach, RE::NiNode* a_parent, RE::ShadowSceneNode* a_scene,
		const char* a_name, RE::NiPointLight*& a_made, bool a_shadow)
	{
		auto* light = LightKit::CloneLight();
		if (!light) {
			return nullptr;
		}
		light->name = a_name;
		light->fadeAmount = kMovingLightMark;
		auto&      data = light->GetLightRuntimeData();
		const auto color = VanillaLook(a_s.color);
		data.diffuse = color;
		data.fade = a_fade;
		data.radius = { a_reach, a_reach, a_s.size };  // x and y are the reach; z carries the light's size
		if (InverseSquare()) {
			light->SetLightAttenuation(a_reach);  // without it the light has no attenuation and lights nothing
			// after SetLightAttenuation, which writes the same two words; the cutoff is re-derived so Brightness moves the
			// peak with the reach held (at 100%/100% it equals the file's). A light of our own, never made by the game's
			// GenDynamic, so Community Shaders set none of its flags: the flag word is ours whole - the attenuation's float
			// left there could read as Disabled (2.2's code review, the hand-light fix)
			Isl::SetOwn(light);  // a light of our own: Community Shaders' words written whole (LightKit.h)
			Isl::SetCutoff(light, CutoffFor(a_fade, a_reach, a_s.size));
		} else {
			// off Community Shaders: an ambient of a tenth of its color, RE::Light's rule (Truman); the caller hands a plain
			// fade and reach (fade x 1.14, reach x 178 / 133)
			SetPlainReach(light, a_reach, color);
		}
		light->local.translate = a_s.at;
		light->local.scale = 1.0f;
		// not portal strict: a light on an actor lights the actor and those beside it indoors, not only the room's meshes
		auto* bs = LightKit::Hang(a_parent, light, a_scene, 1.0f, false, a_shadow);
		if (!bs) {
			return nullptr;
		}
		a_made = light;
		return bs;
	}

	// off the scene by its NiLight, the engine's own removal: a light Open Shaders' Promote made a shadow caster stays in the
	// scene when removed by its BSLight. One hung since the scene's last light pass still waits in the add queue, where the
	// lookup by NiLight does not search, so it goes by its BSLight. Main thread
	void TakeOffOwnLight(RE::ShadowSceneNode* a_scene, const RE::NiPointer<RE::BSLight>& a_bs)
	{
		if (!a_scene || !a_bs) {
			return;
		}
		bool queued = false;
		{
			auto&                     data = a_scene->GetRuntimeData();
			const RE::BSSpinLockGuard guard(data.lightQueueLock);
			queued = std::ranges::any_of(data.lightQueueAdd, [&](const RE::NiPointer<RE::BSLight>& a_one) { return a_one.get() == a_bs.get(); });
		}
		if (auto* light = a_bs->light.get(); light && !queued) {
			a_scene->RemoveLight(light);
		} else {
			a_scene->RemoveLight(a_bs);
		}
	}

	// the sliders onto a light MakeOwnLight made: fade and reach as the caller worked them out
	void RetuneOwnLight(RE::NiPointLight* a_light, const Stream& a_s, float a_fade, float a_reach, bool a_reachMoved)
	{
		// in a turn: the fading module's effect hooks write a held light too, from other threads (LightKit.h)
		const auto turn = LightKit::Turn();
		auto&      d = a_light->GetLightRuntimeData();
		d.fade = a_fade;
		d.radius.x = a_reach;
		d.radius.y = a_reach;
		if (InverseSquare()) {  // without it these words are ambient color (Plugin.h)
			Isl::SetCutoff(a_light, CutoffFor(a_fade, a_reach, a_s.size));
		} else if (a_reachMoved) {
			SetPlainReach(a_light, a_reach, VanillaLook(a_s.color));
		}
	}

	void NoteActorLoaded() noexcept { gLookNow.store(true, std::memory_order_relaxed); }

	const std::vector<RE::FormID>& DrawnActorIds() { return gDrawn; }

	void UpdateHeldLights()
	{
		// no weapon out, nothing lit, nothing noted and no look asked
		if (gDrawn.empty() && gLive.empty() && gFailed.empty() && !gDrawnNotedAny.load(std::memory_order_relaxed) && !gLookNow.load(std::memory_order_relaxed)) {
			return;
		}
		const LightKit::Cost::Pass pass{ 7, "held lights" };
		auto*                      scene = LightKit::Scene();
		auto*                      player = RE::PlayerCharacter::GetSingleton();
		if (!scene || !player) {
			return;
		}
		++gUpdate;
		// her own held lights fade with her spell lights while she sneaks (Hooks.cpp), and come off once faded out
		const float sneak = SneakLevel();
		const bool  hidden = sneak <= 0.0f;
		const auto  ownFade = [&](const Stream& a_held, RE::FormID a_actor) {
			return a_held.fade * (a_actor == player->GetFormID() ? sneak : 1.0f);
		};
		{
			std::lock_guard lock(gDrawnLock);
			for (const auto id : gDrawnNoted) {
				AddDrawn(id);
			}
			gDrawnNoted.clear();
			gDrawnNotedAny.store(false, std::memory_order_relaxed);
		}
		const bool looked = gLookNow.exchange(false, std::memory_order_relaxed);
		if (looked) {
			LookForDrawn(player);
			if (!gWaitingBody) {
				gWaitLooks = kWaitLooksMost;
			}
		}
		gWaitingBody = false;
		if (gDrawn.empty() && gLive.empty() && gFailed.empty()) {
			return;  // no weapon out and nothing lit
		}
		// what a weapon wants changes only with what NoteSweep hears (a draw, a sheathe, an equip, a death, a 3D, a view, her cell, an art's start), a
		// setting or her sneak level; a refused light is tried again, and neither a far actor coming near nor Everyone nearby has an event
		static std::uint32_t genWas = 0;
		static float         sneakWas = -1.0f;
		const auto           gen = SettingsGeneration();
		const bool           changed = gen != genWas || sneak != sneakWas;
		genWas = gen;
		sneakWas = sneak;
		if (!changed && !looked && !SweepWindowOpen() && gFailed.empty() && !BudgetByDistance() && !FarChanged()) {
			return;
		}
		static std::vector<Want> want;  // main thread; kept between frames so its storage is not made again each one
		want.clear();
		gFar.clear();
		// an actor leaves the list once its weapon is fully away or it is gone; a far one stays listed and is looked at from the moment it
		// comes near again, with no event to say so; the rest say what they want
		std::erase_if(gDrawn, [&](RE::FormID a_id) {
			auto*       actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			const auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
			if (!actor || !WeaponOut(actor)) {
				return true;
			}
			if (!actor->Is3DLoaded()) {
				gWaitingBody = gWaitLooks > 0;
				return !gWaitingBody;
			}
			if (process && process->InHighProcess()) {
				WantsOf(actor, actor == player && hidden, want);
			} else {
				gFar.push_back(a_id);
			}
			return false;
		});
		// what is no longer wanted, or hangs on a node the actor no longer has (a view switch rebuilds it), comes off
		std::erase_if(gLive, [&](Live& v) {
			const bool keep = std::ranges::any_of(want, [&v](const Want& w) {
				return w.actor->GetFormID() == v.actor && w.left == v.left && w.firstPerson == v.firstPerson && w.held == v.held &&
				       w.node == v.node.get();
			});
			if (!keep) {
				TakeOff(v, scene);
			}
			return !keep;
		});
		if (gWaitingBody) {
			--gWaitLooks;
			gLookNow.store(true, std::memory_order_relaxed);
		}
		std::erase_if(gFailed, [&](const Failed& f) { return std::ranges::none_of(want, [&f](const Want& w) { return f.Is(w); }); });
		// the sliders and (🔁 his "re add the enb and cs and vanilla versions ... auto detect") the plain drawing
		// off Community Shaders; the color is the light's own
		const auto [scale, reachScale] = OwnLightScale();
		for (const auto& w : want) {
			const bool have = std::ranges::any_of(gLive, [&w](const Live& v) {
				return w.actor->GetFormID() == v.actor && w.left == v.left && w.firstPerson == v.firstPerson;
			});
			if (have || std::ranges::any_of(gFailed, [&w](const Failed& f) { return f.Is(w) && gUpdate < f.retryAt; })) {
				continue;
			}
			if (gLive.size() >= kMaxLive && w.actor != player) {  // her own weapon is never the one left dark
				if (!gCapLogged) {
					gCapLogged = true;
					SKSE::log::info("[HELD] {} held weapon lights are lit at once, the most there may be: the rest wait for one to go out",
						kMaxLive);
				}
				continue;
			}
			const float       fade = ownFade(*w.held, w.actor->GetFormID()) * scale;
			const float       reach = w.held->radius * reachScale;
			RE::NiPointLight* light = nullptr;
			auto*             bs = MakeOwnLight(*w.held, fade, reach, w.node, scene, kLightName, light);
			if (!bs) {
				if (const auto failed = std::ranges::find_if(gFailed, [&w](const Failed& f) { return f.Is(w); }); failed != gFailed.end()) {
					failed->retryAt = gUpdate + kRetryUpdates;
				} else {
					SKSE::log::warn("[HELD] {}: the light could not be made or registered; tried again every {} updates while that hand wants it",
						w.held->key, kRetryUpdates);
					gFailed.push_back({ w.actor->GetFormID(), w.left, w.firstPerson, w.held, gUpdate + kRetryUpdates });
				}
				continue;
			}
			std::erase_if(gFailed, [&w](const Failed& f) { return f.Is(w); });  // made at last: a later failure warns again
			gLive.push_back({ w.actor->GetFormID(), w.left, w.firstPerson, w.held, RE::NiPointer<RE::NiNode>(w.node),
				RE::NiPointer<RE::BSLight>(bs), RE::NiPointer<RE::NiPointLight>(light), fade, reach });
			if (gTold < 16) {
				++gTold;
				SKSE::log::info("[HELD] {} | {:08X} {} hand, {} person | fade {:.2f} | radius {:.0f}", w.held->key, w.actor->GetFormID(),
					w.left ? "left" : "right", w.firstPerson ? "first" : "third", fade, reach);
			}
		}
		// the sliders reach a light already hung
		for (auto& v : gLive) {
			const float fade = ownFade(*v.held, v.actor) * scale;
			const float reach = v.held->radius * reachScale;
			if (fade != v.written || reach != v.wroteReach) {
				RetuneOwnLight(v.light.get(), *v.held, fade, reach, reach != v.wroteReach);
				v.written = fade;
				v.wroteReach = reach;
			}
		}
		gLiveCount = gLive.size();
	}

	void RenewHeldLights()
	{
		auto* scene = LightKit::Scene();
		for (auto& v : gLive) {
			TakeOff(v, scene);
		}
		gLive.clear();
		gLiveCount = 0;
		gFailed.clear();
		gLookNow = true;
	}

	void DropHeldLights()
	{
		auto* scene = LightKit::Scene();
		for (auto& v : gLive) {
			TakeOff(v, scene);
		}
		gLive.clear();
		gLiveCount = 0;
		gFailed.clear();
		{
			std::lock_guard lock(gDrawnLock);
			gDrawnNoted.clear();
		}
		gDrawn.clear();
		gFar.clear();
		gLookNow = true;
	}

	void NoteDrawing(RE::FormID a_actor)
	{
		std::lock_guard lock(gDrawnLock);
		if (std::ranges::find(gDrawnNoted, a_actor) == gDrawnNoted.end()) {
			gDrawnNoted.push_back(a_actor);
			gDrawnNotedAny.store(true, std::memory_order_relaxed);
		}
	}

	std::size_t LiveHeldLights() { return gLiveCount.load(); }

	bool HeldLightHolds(const RE::NiLight* a_light)
	{
		return std::ranges::any_of(gLive, [a_light](const Live& v) { return v.light.get() == a_light || (v.bs && v.bs->light.get() == a_light); });
	}
}
