// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The body glow while casting. HIS ANSWER 2026-10-16 (checklist 16): "why did relight not get the body glow? and yes i want
// it for relight too", then "write the code based on how you think Truman would write it ... He likes a very simplistic
// approach." RE::Light hangs a config's light on a REFERENCE (a projectile, an explosion, a placed object) and on an
// enchantment shader. Art the game hangs on an actor - a cloak, a heal, CS Light's Magic FX rows - is neither, so those
// configs lit nothing. Here, once a frame on the main thread, every art model on an actor that a `body` line names gets that
// line's light, hung where RE::Light hangs its enchantment light (the third-person node of the art's attach root), and it
// comes off the frame the art is gone or finished, its option is switched off, her own glow has faded out while she sneaks
// (Hooks.cpp, as her held lights), "All RELight - Spell Lighting NG lights off" is on, or "Hand lights for" leaves
// its actor out. Any art a spell plays as its casting art (the hand lights; a master spell's casting body art too) and a
// weapon's enchantment art (held lights, Truman's enchantments) never get one. Body art that outlived its spell once kept a
// light on for a day (Illuminated's CLAUDE.md, the lighting data's rules): this light lives exactly as long as the art does.
// Frozen Electrocuted Combustion's lit shaders on a body (a `body` line naming the shader's form) the same way; a frozen body
// stays frozen and a burned one keeps smouldering, so on the "Frozen bodies" / "Burning bodies" fade-out picks their light
// goes out a few seconds in (FecBodyFade).
//
// CREDIT: the way ReLight by Truman (github.com/TrumanGIT/ReLight, GPL-3.0-or-later, used with his permission) lights an
// effect on an actor (LightAttachmentHooks.cpp ReferenceEffect::Init): the effect's attach root, swapped for the third-person
// node of the same name, a light cloned from one master light, marked, hung there and handed to the shadow scene node
// (Held.cpp MakeOwnLight, the same code our held lights use).

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kLightName = "RSABody";
		constexpr float       kFrozenHold = 4.0f;   // seconds at full strength, then out over kFecOut
		constexpr float       kBurningHold = 8.0f;  // a fire played with no end: the char darkening ends at 8 s
		constexpr float       kFecOut = 2.0f;
		constexpr float       kBurningOut = 1.0f;  // the flames die over this
		// FEC plays its charred / skeleton skin burn with no end: the skin is seen burning for its record's fill time (3.4 / 4.4 s)
		constexpr float kCharredHold = 3.0f;
		constexpr float kSkeletonHold = 4.0f;
		constexpr float kSkinOut = 0.5f;

		// FEC.esp is an ESL - its ids move with the load order: FEC_FrostFXS, FEC_FireFXS, FEC_FireHeavyFXS, then the charred and
		// the skeleton skin burns (FEC_FireDisintegrateSkinFXS, FEC_FireDisintegrateSkinLongFXS)
		[[nodiscard]] const std::array<const RE::TESEffectShader*, 5>& FecShaders()
		{
			static const auto shaders = [] {
				std::array<const RE::TESEffectShader*, 5> out{};
				if (auto* data = RE::TESDataHandler::GetSingleton()) {
					out = { data->LookupForm<RE::TESEffectShader>(0x80D, "FEC.esp"),
						data->LookupForm<RE::TESEffectShader>(0x806, "FEC.esp"), data->LookupForm<RE::TESEffectShader>(0x826, "FEC.esp"),
						data->LookupForm<RE::TESEffectShader>(0x887, "FEC.esp"), data->LookupForm<RE::TESEffectShader>(0x896, "FEC.esp") };
				}
				return out;
			}();
			return shaders;
		}

		// a frozen body stays frozen and a burned one keeps smouldering: on their fade-out picks the light goes out a few seconds in
		[[nodiscard]] float FecBodyFade(const RE::ReferenceEffect* a_effect)
		{
			const auto* shader = netimmerse_cast<const RE::ShaderReferenceEffect*>(a_effect);
			const auto& fec = FecShaders();
			if (!shader || !shader->effectData) {
				return 1.0f;
			}
			const bool frozen = shader->effectData == fec[0];
			const bool charred = shader->effectData == fec[3];
			const bool skeleton = shader->effectData == fec[4];
			const bool burning = shader->effectData == fec[1] || shader->effectData == fec[2] || charred || skeleton;
			if (!(frozen && FecFrozenFadesOut()) && !(burning && FecBurningFadesOut())) {
				return 1.0f;
			}
			if (frozen) {
				return std::clamp(1.0f - (shader->age - kFrozenHold) / kFecOut, 0.0f, 1.0f);
			}
			// FEC times its own fire (2-5 s): the light ends with the effect; only a burn played with no end fades
			if (shader->lifetime > 0.0f) {
				return 1.0f;
			}
			if (charred || skeleton) {
				return std::clamp(1.0f - (shader->age - (charred ? kCharredHold : kSkeletonHold)) / kSkinOut, 0.0f, 1.0f);
			}
			return std::clamp(1.0f - (shader->age - kBurningHold) / kBurningOut, 0.0f, 1.0f);
		}

		struct Live
		{
			RE::NiPointer<RE::ReferenceEffect> art;  // held, so its address is never another art's while we keep it
			const Stream*                      body{ nullptr };
			RE::NiPointer<RE::NiNode>          node;  // what it hangs on
			RE::NiPointer<RE::BSLight>         bs;
			RE::NiPointer<RE::NiPointLight>    light;
			float                              written{ -1.0f }, wroteReach{ -1.0f };
			bool                               seen{ false };
			bool                               hers{ false };   // on the player: it fades with her spell lights while she sneaks
			float                              distSq{ 0.0f };  // its actor's squared distance to her, this frame
		};

		// past the cap a nearer glow takes the farthest one's place only when it is clearly nearer (3/4 of the distance), so two
		// casters at about the same distance never trade places every frame
		constexpr float kNearerBy = 0.75f * 0.75f;

		std::vector<Live> gLive;  // main thread only

		// the cap counts the glows on others only: hers never take one of their places
		[[nodiscard]] std::size_t OthersLit()
		{
			return static_cast<std::size_t>(std::ranges::count_if(gLive, [](const Live& v) { return !v.hers; }));
		}
		std::size_t gMade = 0;
		bool        gCapLogged = false;

		// an art whose light could not be made or registered: warned about once, tried again every kRetryUpdates updates while
		// the art is still there (Held.cpp's retry - the scene may refuse a light for a while), forgotten when it is gone
		constexpr std::uint64_t kRetryUpdates = 60;
		std::uint64_t           gUpdate = 0;
		struct Failed
		{
			const RE::ReferenceEffect* art{ nullptr };
			std::uint64_t              retryAt{ 0 };
			bool                       seen{ false };
		};
		std::vector<Failed> gFailed;

		// new body art is looked for when an art starts anywhere (ReferenceEffect Init, Hooks.cpp - any thread; only a flag: an effect
		// kept by reference from its Init can be destroyed all the same), an actor's 3D or her cell comes in, and at a loaded game; between looks, under
		// the game's effect lock as a walk is, only the lights already lit are checked, each off its own art
		std::atomic<bool> gArtStarted{ false };
		std::atomic<bool> gLookNow{ true };
		// an art's model loads a moment after it starts: looked for again until its 3D is in, at most this many updates
		constexpr int kWaitUpdates = 600;
		int           gWaitUpdates = 0;
		// an actor whose body is not stored yet: its own count, or every art started in a fight would keep the walk going for one with none
		constexpr int    kBodyLooks = 300;
		std::atomic<int> gBodyLooks{ 0 };
		// a refused light, or a glow held out by the cap (the nearest must take its place, and how near she is has no event): looked at
		// again this often, only while one waits
		constexpr std::uint64_t kWaitingEvery = 60;
		bool                    gCapWaiting = false;

		// the art a spell plays as its casting art (HandLights.cpp lights the hands) or on a weapon (its enchantment art): never
		// a body light. Worked out once, before the first walk of the effects (never inside it - that walk holds the game's lock)
		std::unordered_set<const RE::BGSArtObject*> gHandOrWeaponArt;
		bool                                        gArtKnown = false;
		std::uint32_t                               gLastGen = 0;
		bool                                        gLastHerDark = false;

		void LearnHandOrWeaponArt()
		{
			for (const auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				if (effect) {
					gHandOrWeaponArt.insert(effect->data.castingArt);
					gHandOrWeaponArt.insert(effect->data.enchantEffectArt);
				}
			}
			gHandOrWeaponArt.erase(nullptr);
			gArtKnown = true;
		}

		// the hand a node sits under (itself when it is one), or nullptr - Illuminated's ArtLights.cpp
		[[nodiscard]] RE::NiAVObject* HandAbove(RE::NiAVObject* a_node)
		{
			for (auto* up = a_node; up; up = up->parent) {
				const char* name = up->name.c_str();
				if (name && std::string_view(name).find("Hand [") != std::string_view::npos) {
					return up;
				}
			}
			return nullptr;
		}

		// in first person the third-person arms are not posed; the spine's MagicEffectsNode lights the floor (Illuminated's ArtLights.cpp)
		[[nodiscard]] RE::NiNode* BodyNode(RE::NiAVObject* a_third)
		{
			static const RE::BSFixedString kEffectsNode{ "MagicEffectsNode" };
			auto*                          node = a_third->GetObjectByName(kEffectsNode);
			return (node ? node : a_third)->AsNode();
		}

		// Truman's enchantment light: the effect's attach root, or the third-person node of the same name (a light under the
		// first-person skeleton lights nothing on ENB). Her own art in first person hangs in the first-person skeleton, its model
		// its own root - a name the third-person body lacks - so there the nearest node above it the body has is used, the body
		// node for one on an arm. An unnamed root is used as it is - a search for "" finds any unnamed node
		[[nodiscard]] RE::NiNode* PickNode(RE::ReferenceEffect* a_art, RE::TESObjectREFR* a_ref)
		{
			auto* root = a_art->GetAttachRoot();
			if (!root) {
				return nullptr;
			}
			auto* const third = a_ref->Get3D(false);
			auto* const first = a_ref->Get3D(true);
			if (!third || third == root) {
				return root->AsNode();
			}
			bool underFirst = false;
			for (const RE::NiAVObject* up = root; first && first != third && up && !underFirst; up = up->parent) {
				underFirst = up == first;
			}
			if (!underFirst) {
				const char* name = root->name.c_str();
				if (name && *name) {
					if (auto* same = third->GetObjectByName(root->name)) {
						root = same;
					}
				}
				return root->AsNode();
			}
			for (auto* up = root; up; up = up == first ? nullptr : up->parent) {
				const char* name = up->name.c_str();
				auto*       same = name && *name ? third->GetObjectByName(up->name) : nullptr;
				if (same && same->AsNode()) {
					return HandAbove(same) ? BodyNode(third) : same->AsNode();
				}
			}
			return BodyNode(third);
		}

		// An art whose model carries attach techniques ("AttachT" strings, as MuffleHitFX does) has the game move its pieces between
		// the skeleton's nodes and track them; a light of ours put in that art is swept into the tracking and later read freed.
		// Such art lights from the body node, which the game never moves
		[[nodiscard]] RE::NiNode* NodeFor(RE::ReferenceEffect* a_art, RE::TESObjectREFR* a_ref)
		{
			auto* const node = PickNode(a_art, a_ref);
			auto* const third = node ? a_ref->Get3D(false) : nullptr;
			if (!third) {
				return node;
			}
			static const RE::BSFixedString kAttachTechniques{ "AttachT" };
			const auto*                    model = netimmerse_cast<RE::ModelReferenceEffect*>(a_art);
			bool                           moved = model && model->artObject3D && model->artObject3D->HasExtraData(kAttachTechniques);
			for (const RE::NiAVObject* up = node; up && up != third && !moved; up = up->parent) {
				moved = up->HasExtraData(kAttachTechniques);
			}
			return moved ? BodyNode(third) : node;
		}

		void TakeOff(Live& a_v, RE::ShadowSceneNode* a_scene)
		{
			if (a_scene && a_v.bs) {
				a_scene->RemoveLight(a_v.bs);
			}
			if (a_v.node && a_v.light && a_v.light->parent == a_v.node.get()) {
				a_v.node->DetachChild(a_v.light.get());
			}
		}
	}

	void UpdateBodyLights()
	{
		// nothing lit and nothing new: a setting, her sneak, an art's start or a look asked for is what changes that
		const std::uint32_t gen = SettingsGeneration();
		const bool          herDark = SneakLevel() <= 0.0f;
		if (gArtKnown && gLive.empty() && gFailed.empty() && !gCapWaiting && gen == gLastGen && herDark == gLastHerDark &&
			!gArtStarted.load(std::memory_order_relaxed) && !gLookNow.load(std::memory_order_relaxed)) {
			return;
		}
		const LightKit::Cost::Pass pass{ 8, "body lights" };
		auto*                      scene = LightKit::Scene();
		auto*                      player = RE::PlayerCharacter::GetSingleton();
		auto*                      lists = RE::ProcessLists::GetSingleton();
		if (!scene || !player || !lists) {
			return;
		}
		if (!gArtKnown) {
			LearnHandOrWeaponArt();
		}
		++gUpdate;
		// a setting, a switch or her sneak changed since the last frame: look again at once, so a glow let go comes straight back
		const bool changed = gen != gLastGen || herDark != gLastHerDark;
		gLastGen = gen;
		gLastHerDark = herDark;
		const bool started = gArtStarted.exchange(false);
		if (started) {
			gWaitUpdates = kWaitUpdates;
		}
		const bool look = changed || gLookNow.exchange(false) || started || ((gCapWaiting || !gFailed.empty()) && gUpdate % kWaitingEvery == 0);
		if (!look && gLive.empty()) {
			return;  // nothing lit and nothing new
		}
		bool waiting = false;
		bool noBody = false;
		if (look) {
			gCapWaiting = false;
		}
		for (auto& v : gLive) {
			v.seen = false;
		}
		if (look) {
			for (auto& f : gFailed) {
				f.seen = false;
			}
		}
		// "All RELight - Spell Lighting NG lights off" puts every light of the mod out (2.5), and while she sneaks her own body glow
		// fades with her spell lights (her held lights the same way - Held.cpp)
		const bool  hidden = AllLightsOff();
		const float sneak = SneakLevel();  // Hooks.cpp: her body glow comes off once it has faded out
		const auto [ownScale, reachScale] = OwnLightScale();
		const float scale = ownScale * StrengthOf(Strength::kGlows);
		struct Todo
		{
			RE::NiPointer<RE::ReferenceEffect> art;
			const Stream*                      body{};
			RE::NiPointer<RE::NiNode>          node;
			bool                               hers{};
			RE::FormID                         formId{};
			float                              distSq{};
		};
		static std::vector<Todo> todo;  // main thread; its storage kept between frames
		todo.clear();
		// one art judged: kept (seen), queued to be made, or left - the same rules on a look and between looks
		const auto consider = [&](RE::BSTempEffect* a_effect) {
			auto*                model = a_effect ? netimmerse_cast<RE::ModelReferenceEffect*>(a_effect) : nullptr;
			auto*                shader = model || !a_effect ? nullptr : netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect);
			RE::ReferenceEffect* art = model ? static_cast<RE::ReferenceEffect*>(model) : shader;
			const Stream*        body = nullptr;
			if (model && !model->finished && model->artObject && !gHandOrWeaponArt.contains(model->artObject)) {
				body = BodyOf(model->artObject);
				if (body && !model->artObject3D) {
					waiting = true;
					return;
				}
			} else if (shader && !shader->finished) {
				body = BodyOf(shader->effectData);
			}
			// a faded-out FEC body is not seen: its light comes off and leaves the glow cap
			if (!body || !OptionLit(body->option) || FecBodyFade(art) <= 0.0f) {
				return;
			}
			const auto ref = art->target.get();
			auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (actor && !actor->Is3DLoaded()) {
				noBody = true;  // the game stores its body a moment after its load is noted, and nothing says so
				return;
			}
			if (!actor || (sneak <= 0.0f && actor == player)) {
				return;
			}
			// no event says when a far actor comes near: looked at again while it waits, as a glow past the cap
			if (LeftOutByBudget(actor)) {
				gCapWaiting = true;
				return;
			}
			auto* node = NodeFor(art, actor);
			if (!node) {
				return;
			}
			const float distSq = actor == player ? 0.0f : player->GetPosition().GetSquaredDistance(actor->GetPosition());
			// already lit: kept, unless its node was rebuilt (a view switch, a new 3D) - then it is made again below
			const auto have = std::ranges::find_if(gLive, [art](const Live& v) { return v.art.get() == art; });
			if (have != gLive.end() && have->node.get() == node && have->body == body) {
				have->seen = true;
				have->distSq = distSq;
				return;
			}
			todo.push_back(
				{ RE::NiPointer<RE::ReferenceEffect>(art), body, RE::NiPointer<RE::NiNode>(node), actor == player, actor->GetFormID(), distSq });
		};
		if (!hidden && look) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				consider(a_effect);
				return RE::BSContainer::ForEachResult::kContinue;
			});
		} else if (!hidden) {
			RE::BSSpinLockGuard locker(lists->magicEffectsLock);
			for (std::size_t i = 0, n = gLive.size(); i < n; ++i) {
				consider(gLive[i].art.get());
			}
		}
		if (look && noBody && gBodyLooks.load(std::memory_order_relaxed) > 0) {
			gBodyLooks.fetch_sub(1, std::memory_order_relaxed);
			gLookNow.store(true, std::memory_order_relaxed);
		}
		if (look && waiting && gWaitUpdates > 0) {
			--gWaitUpdates;
			gLookNow.store(true, std::memory_order_relaxed);
		}
		// made and put in the scene after the walk: the game holds its effect list's lock through it; the nearest first
		std::ranges::sort(todo, {}, &Todo::distSq);
		for (const auto& t : todo) {
			auto*       art = t.art.get();
			auto*       node = t.node.get();
			const auto* body = t.body;
			const bool  hers = t.hers;
			if (const auto have = std::ranges::find_if(gLive, [art](const Live& v) { return v.art.get() == art; }); have != gLive.end()) {
				TakeOff(*have, scene);
				gLive.erase(have);
			}
			const auto failed = std::ranges::find_if(gFailed, [art](const Failed& f) { return f.art == art; });
			if (failed != gFailed.end()) {
				failed->seen = true;
				if (gUpdate < failed->retryAt) {
					continue;
				}
			}
			// her own art is never left out by the cap: at the cap a glow on someone clearly nearer takes the farthest one's place,
			// the rest wait for one to go out
			if (!hers && OthersLit() >= GlowLimit()) {
				const auto farthest = std::ranges::max_element(gLive, {}, [](const Live& v) { return v.hers ? -1.0f : v.distSq; });
				if (farthest == gLive.end() || farthest->hers || t.distSq >= farthest->distSq * kNearerBy) {
					gCapWaiting = true;
					if (!gCapLogged) {
						gCapLogged = true;
						SKSE::log::info("[BODY] {} body lights on others are lit at once, the most there may be: the nearest keep them", OthersLit());
					}
					continue;
				}
				TakeOff(*farthest, scene);
				gLive.erase(farthest);
				gCapWaiting = true;  // the one it lost to may move away
			}
			const float       fade = body->fade * scale * (hers ? sneak : 1.0f) * FecBodyFade(art);
			const float       reach = body->radius * reachScale;
			RE::NiPointLight* light = nullptr;
			// her Candlelight casts a shadow with the optional Bottled Shaders shadow download (his checklist 262, "b")
			const bool shadow = hers && ShadowPatch() && body->key == "lightspellactorfx";
			auto*      bs = MakeOwnLight(*body, fade, reach, node, scene, kLightName, light, shadow);
			if (!bs) {
				if (failed != gFailed.end()) {
					failed->retryAt = gUpdate + kRetryUpdates;
				} else {
					SKSE::log::warn("[BODY] {}: the light could not be made or registered; tried again every {} updates while the art is there",
						body->key, kRetryUpdates);
					gFailed.push_back({ art, gUpdate + kRetryUpdates, true });
				}
				continue;
			}
			if (failed != gFailed.end()) {
				gFailed.erase(failed);  // made at last: a later failure warns again
			}
			gLive.push_back({ t.art, body, t.node, RE::NiPointer<RE::BSLight>(bs), RE::NiPointer<RE::NiPointLight>(light), fade, reach, true, hers });
			if (++gMade <= 16) {
				SKSE::log::info("[BODY] {} | on {:08X} at {} | fade {:.2f} | radius {:.0f}", body->key, t.formId, node->name.c_str(), fade, reach);
			}
		}
		// what was not seen this frame - the art is gone, finished, switched off, or she sneaks - comes off
		const auto wereLit = gLive.size();
		std::erase_if(gLive, [scene](Live& v) {
			if (!v.seen) {
				TakeOff(v, scene);
			}
			return !v.seen;
		});
		if (gLive.size() < wereLit) {
			gLookNow.store(true, std::memory_order_relaxed);  // a glow waiting past the cap takes its place at once
		}
		if (look) {
			std::erase_if(gFailed, [](const Failed& f) { return !f.seen; });  // its art is gone: a new art there warns again
		}
		// never more than the cap on others, whatever came in this frame: the newest come off and wait like any other
		for (auto it = gLive.end(); OthersLit() > GlowLimit() && it != gLive.begin();) {
			--it;
			if (!it->hers) {
				TakeOff(*it, scene);
				it = gLive.erase(it);
			}
		}
		// the sliders reach a light already hung
		for (auto& v : gLive) {
			const float fade = v.body->fade * scale * (v.hers ? sneak : 1.0f) * FecBodyFade(v.art.get());
			const float reach = v.body->radius * reachScale;
			if (fade != v.written || reach != v.wroteReach) {
				RetuneOwnLight(v.light.get(), *v.body, fade, reach, reach != v.wroteReach);
				v.written = fade;
				v.wroteReach = reach;
			}
		}
	}

	void RenewBodyLights()
	{
		auto* scene = LightKit::Scene();
		for (auto& v : gLive) {
			TakeOff(v, scene);
		}
		gLive.clear();
		gFailed.clear();
		gLookNow = true;
		// the door may have left a body or a model still coming: their looks start over
		gWaitUpdates = kWaitUpdates;
		NoteBodyComing();
	}

	void DropBodyLights()
	{
		RenewBodyLights();
		ForgetBodyArt();  // a save may bring other casting art (RefindHandLights)
	}

	void NoteArtStarted() { gArtStarted.store(true, std::memory_order_relaxed); }

	void NoteBodyComing() { gBodyLooks.store(kBodyLooks, std::memory_order_relaxed); }

	void ForgetBodyArt()
	{
		gHandOrWeaponArt.clear();  // learned again on the next update, outside the walk of the effects
		gArtKnown = false;
	}

	std::size_t LiveBodyLights() { return gLive.size(); }
	std::size_t BodyLightsMade() { return gMade; }
}
