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
// stays frozen, so on "Frozen bodies" fades out its frozen shader's light goes out a few seconds in (FrozenFade).
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
		constexpr std::size_t kMaxLive = 16;
		constexpr const char* kLightName = "RSABody";
		constexpr float       kFrozenHold = 4.0f;  // seconds at full strength, then out over kFrozenOut
		constexpr float       kFrozenOut = 2.0f;
		constexpr RE::FormID  kFrozenLocal = 0x80D;  // FEC_FrostFXS in FEC.esp (an ESL - its id moves with the load order)

		[[nodiscard]] const RE::TESEffectShader* FrozenShader()
		{
			static const auto* shader = [] {
				auto* data = RE::TESDataHandler::GetSingleton();
				return data ? data->LookupForm<RE::TESEffectShader>(kFrozenLocal, "FEC.esp") : nullptr;
			}();
			return shader;
		}

		[[nodiscard]] float FrozenFade(const RE::ReferenceEffect* a_effect)
		{
			const auto* shader = netimmerse_cast<const RE::ShaderReferenceEffect*>(a_effect);
			if (!shader || !FecFrozenFadesOut() || !FrozenShader() || shader->effectData != FrozenShader()) {
				return 1.0f;
			}
			return std::clamp(1.0f - (shader->age - kFrozenHold) / kFrozenOut, 0.0f, 1.0f);
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
			bool                               hers{ false };  // on the player: it fades with her spell lights while she sneaks
		};

		std::vector<Live> gLive;  // main thread only
		std::size_t       gMade = 0;
		bool              gCapLogged = false;

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

		// the art a spell plays as its casting art (HandLights.cpp lights the hands) or on a weapon (its enchantment art): never
		// a body light. Worked out once, before the first walk of the effects (never inside it - that walk holds the game's lock)
		std::unordered_set<const RE::BGSArtObject*> gHandOrWeaponArt;
		bool                                        gArtKnown = false;

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

		// Truman's enchantment light: the effect's attach root, or the third-person node of the same name (a light under the
		// first-person skeleton lights nothing on ENB). An unnamed root is used as it is - a search for "" finds any unnamed node
		[[nodiscard]] RE::NiNode* NodeFor(RE::ReferenceEffect* a_art, RE::TESObjectREFR* a_ref)
		{
			auto* root = a_art->GetAttachRoot();
			if (!root) {
				return nullptr;
			}
			auto*       third = a_ref->Get3D(false);
			const char* name = root->name.c_str();
			if (third && third != root && name && *name) {
				if (auto* same = third->GetObjectByName(root->name)) {
					root = same;
				}
			}
			return root->AsNode();
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
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!scene || !player || !lists) {
			return;
		}
		if (!gArtKnown) {
			LearnHandOrWeaponArt();
		}
		++gUpdate;
		for (auto& v : gLive) {
			v.seen = false;
		}
		for (auto& f : gFailed) {
			f.seen = false;
		}
		// "All RELight - Spell Lighting NG lights off" puts every light of the mod out (2.5), and while she sneaks her own body glow
		// fades with her spell lights (her held lights the same way - Held.cpp)
		const bool  hidden = AllLightsOff();
		const float sneak = SneakLevel();  // Hooks.cpp: her body glow comes off once it has faded out
		const auto [scale, reachScale] = OwnLightScale();
		struct Todo
		{
			RE::NiPointer<RE::ReferenceEffect> art;
			const Stream*                      body{};
			RE::NiPointer<RE::NiNode>          node;
			bool                               hers{};
			RE::FormID                         formId{};
		};
		std::vector<Todo> todo;
		if (!hidden) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto*                model = a_effect ? netimmerse_cast<RE::ModelReferenceEffect*>(a_effect) : nullptr;
				auto*                shader = model || !a_effect ? nullptr : netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect);
				RE::ReferenceEffect* art = model ? static_cast<RE::ReferenceEffect*>(model) : shader;
				const Stream*        body = nullptr;
				if (model && !model->finished && model->artObject && model->artObject3D && !gHandOrWeaponArt.contains(model->artObject)) {
					body = BodyOf(model->artObject);
				} else if (shader && !shader->finished) {
					body = BodyOf(shader->effectData);
				}
				if (!body || !OptionLit(body->option)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto ref = art->target.get();
				auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor || !actor->Is3DLoaded() || (sneak <= 0.0f && actor == player) || LeftOutByBudget(actor)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* node = NodeFor(art, actor);
				if (!node) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// already lit: kept, unless its node was rebuilt (a view switch, a new 3D) - then it is made again below
				const auto have = std::ranges::find_if(gLive, [art](const Live& v) { return v.art.get() == art; });
				if (have != gLive.end() && have->node.get() == node && have->body == body) {
					have->seen = true;
					return RE::BSContainer::ForEachResult::kContinue;
				}
				todo.push_back({ RE::NiPointer<RE::ReferenceEffect>(art), body, RE::NiPointer<RE::NiNode>(node), actor == player, actor->GetFormID() });
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		// made and put in the scene after the walk: the game holds its effect list's lock through it
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
			// her own art is never left out by the cap: a fight's cloaks and heals on others wait instead
			if (gLive.size() >= kMaxLive && !hers) {
				if (!gCapLogged) {
					gCapLogged = true;
					SKSE::log::info("[BODY] {} body lights are lit at once, the most there may be: the rest wait for one to go out", kMaxLive);
				}
				continue;
			}
			const float       fade = body->fade * scale * (hers ? sneak : 1.0f) * FrozenFade(art);
			const float       reach = body->radius * reachScale;
			RE::NiPointLight* light = nullptr;
			auto*             bs = MakeOwnLight(*body, fade, reach, node, scene, kLightName, light);
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
		std::erase_if(gLive, [scene](Live& v) {
			if (!v.seen) {
				TakeOff(v, scene);
			}
			return !v.seen;
		});
		std::erase_if(gFailed, [](const Failed& f) { return !f.seen; });  // its art is gone: a new art there warns again
		// the sliders and the daylight reach a light already hung
		for (auto& v : gLive) {
			const float fade = v.body->fade * scale * (v.hers ? sneak : 1.0f) * FrozenFade(v.art.get());
			const float reach = v.body->radius * reachScale;
			if (fade != v.written || reach != v.wroteReach) {
				RetuneOwnLight(v.light.get(), *v.body, fade, reach, reach != v.wroteReach);
				v.written = fade;
				v.wroteReach = reach;
			}
		}
	}

	void DropBodyLights()
	{
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		for (auto& v : gLive) {
			TakeOff(v, scene);
		}
		gLive.clear();
		gFailed.clear();
		ForgetBodyArt();  // a save may bring other casting art (RefindHandLights)
	}

	void ForgetBodyArt()
	{
		gHandOrWeaponArt.clear();  // learned again on the next update, outside the walk of the effects
		gArtKnown = false;
	}

	std::size_t LiveBodyLights() { return gLive.size(); }
	std::size_t BodyLightsMade() { return gMade; }
}
