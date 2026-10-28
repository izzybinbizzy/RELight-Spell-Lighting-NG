// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// The lights on a weapon, and scaling them. This module makes no light: it dims the ones other mods hang on a weapon - Light Placer's, RE::Light's, lights inside Enchantment Art Extender art or a weapon's
// own mesh, anything that is a NiPointLight in the scene graph under the weapon.
//
// Where a hand's lights hang, all of which are searched every frame:
//   - the weapon's 3D in the actor's biped: the right hand's in its weapon-type slot, the left hand's in the shield slot
//     (the game puts a left-hand weapon there); the player's first-person biped too;
//   - the attach root of every enchantment effect on the weapon (WeaponEnchantmentController::attachRoot), handed over
//     by the reference-effect hooks (the plugin's one wrapper per hook). Light Placer hangs an enchantment art's lights
//     under that root - and for
//     the player in first person it moves them to the third-person weapon, which the biped search also covers.
//
// When: Light Placer rewrites a flickering or animated light's fade in ReferenceEffect::UpdatePosition and in the cell's
// animation pass; a steady light it sets once. So the scaling runs in three places a frame - right after each enchantment
// effect's update (AfterReferenceEffect), in the player's update (UpdateHands), and after each loaded cell's light animation
// (ReapplyAll, this frame's numbers again - every hand, as a light may be animated in a cell its actor has since left; RELight
// 2.2's code review) - and each light keeps Glow::Scaled's
// "what we wrote / what it was" pair, so running twice, or on a light nobody rewrites, never compounds. The Brightness
// slider scales the same lights just before (the plugin's Brightness pass): each fade and each reach written here is
// reported to it (Plugin::NoteFadeWrite, Plugin::NoteReachWrite), and it reports a slider change on a steady light back
// (Rebase), so neither takes the other's write for a new value.
//
// When a light stops being under a tracked hand (weapon put away into the inventory, swapped, charge refilled on an
// exempt weapon) it is put back as we found it, unless someone else has written it since.
//
// "Dim the enchantment glow" does the same to the glow itself: the enchantment shader's fill and rim alpha
// (AfterShaderEffect) and the emissive scale of an enchantment art's glowing meshes (DimArt: VAER's swirls, EAE's art).

#include "Fade.h"
#include "Plugin.h"

namespace Fade
{
	namespace
	{
		constexpr std::uint32_t kKeepFrames = 2;    // a light, root or shader not seen for this many frames is let go
		constexpr std::uint32_t kForgetHand = 600;  // a hand not seen for this many frames (~10 s) is forgotten

		struct ColorKeep
		{
			RE::NiColor base{}, written{};  // written: what we wrote last (meaningful once `touched`)
			bool        touched{ false };

			[[nodiscard]] static bool Same(const RE::NiColor& a, const RE::NiColor& b) noexcept
			{
				return a.red == b.red && a.green == b.green && a.blue == b.blue;
			}
			// a_fire: the hand's magic is fire - an orange light cools to ember, every other to grey (Glow::TintFor)
			RE::NiColor Apply(const RE::NiColor& a_current, bool a_fire, float a_amount)
			{
				if (!touched || !Same(a_current, written)) {
					base = a_current;
				}
				const Glow::Rgb own{ base.red, base.green, base.blue };
				const auto      c = Glow::Cool(own, Glow::TintFor(a_fire, own), a_amount);
				written = { c.r, c.g, c.b };
				touched = true;
				return written;
			}
			[[nodiscard]] RE::NiColor Restore(const RE::NiColor& a_current) const
			{
				return touched && Same(a_current, written) ? base : a_current;
			}
		};

		// the hand's numbers a light's last write was made with (ApplyLight)
		struct LightInputs
		{
			float brightness{ 0.0f }, reach{ 0.0f }, cool{ 0.0f };
			bool  fire{ false }, reachFollows{ false };

			[[nodiscard]] bool operator==(const LightInputs&) const = default;
		};

		struct LightSeen
		{
			RE::NiPointer<RE::NiPointLight> light;
			Glow::Scaled                    fade, radius, cutoff;  // cutoff: an inverse-square light's, on Community Shaders
			ColorKeep                       color;
			std::uint32_t                   frame{ 0 };
			std::uint64_t                   hand{ 0 };                               // the hand whose numbers it last took, for ReapplyAll
			float                           shownFade{ 0.0f }, shownRadius{ 0.0f };  // what it held after our last write
			LightInputs                     from{};                                  // the hand's numbers that write was made with
			bool                            fromKnown{ false };                      // cleared by a write not ours (an item, a re-base)
		};

		struct ShaderSeen
		{
			RE::NiPointer<RE::ShaderReferenceEffect> effect;
			Glow::Scaled                             fill, rim;
			std::uint32_t                            frame{ 0 };
		};

		// an enchantment art's glowing mesh (VAER's swirls, EAE's art): its effect material's emissive scale, keyed by the
		// material, since two meshes may share one (two keys on one material would each take the other's write as a base)
		struct ArtSeen
		{
			RE::NiPointer<RE::BSShaderProperty> property;  // keeps the material alive while we hold its base
			Glow::Scaled                        scale;
			std::uint32_t                       frame{ 0 };
		};

		struct Root
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::uint32_t                 frame{ 0 };
		};

		struct HandTrack
		{
			RE::ActorHandle  actor;
			std::uint64_t    key{ 0 };              // its own place in gHands (Key, or SpellKey for a spell hand)
			const RE::Actor* actorSeen{ nullptr };  // the actor as last read on the main thread: only compared, never used
			bool             left{ false };
			bool             spell{ false };  // a spell in hand (FadeSpells.cpp): follows magicka, keyed apart (SpellKey)
			bool             fire{ false };   // its strongest effect is fire: an orange light cools to ember (Glow::TintFor)
			bool             active{ false };
			std::uint32_t    frame{ 0 };
			// a spell hand's kind (KindOf), worked out again only when the spell in hand changes
			const RE::SpellItem* kindSpell{ nullptr };
			RE::FormID           kindID{ 0 };
			Kind                 kind{};
			const void*          weapon{ nullptr };
			const void*          instance{ nullptr };
			Glow::Hand           glow{};
			Glow::Output         out{};
			Verdict              verdict{};
			Reading              reading{};
			// what the verdict was worked out for (it is kept until one of these changes)
			bool          verdictKnown{ false };
			const void*   verdictWeapon{ nullptr };
			const void*   verdictEnch{ nullptr };
			RE::FormID    verdictEnchID{ 0 };  // with the pointer: an enchantment freed and made again is another
			std::uint32_t verdictRules{ 0 };
			Settings      verdictSettings{};
			float         fraction{ 1.0f };
			// what the HUD shows: the charge, magicka, the preview - or a bound weapon's time left over its WHOLE spell, while its
			// light (fraction) holds full until the last seconds (Glow::BoundFraction)
			float             hudFraction{ 1.0f };
			std::vector<Root> effectRoots;  // enchantment effects' attach roots, from the reference-effect hooks
			std::size_t       lights{ 0 }, roots{ 0 };
			std::string       actorName;
			std::string       weaponLabel, enchLabel;  // made on the main thread, for the Debug page and DevBench
			float             chargeAV{ -1.0f };       // the game's own item-charge value, read on the main thread
		};

		std::mutex                                       gLock;  // the player update and the effect hooks may be on different threads
		std::uint32_t                                    gFrame = 1;
		std::unordered_map<std::uint64_t, HandTrack>     gHands;
		std::unordered_map<RE::NiPointLight*, LightSeen> gLights;
		std::unordered_map<const void*, ShaderSeen>      gShaders;
		std::unordered_map<const void*, ArtSeen>         gArt;
		// how many WEAPON hands are active (the only ones the effect hooks can use - ActiveHand), read without the lock by the
		// effect hooks: every art and shader effect in the world calls them each frame, and with no enchanted weapon out they need
		// not look up (RTTI) whose effect it is
		std::atomic<std::size_t> gActiveHands{ 0 };
		// the same hands, in gHands' order, for ActiveHand: rebuilt with gActiveHands under gLock (a map's nodes never move, so
		// the pointers hold until the next rebuild or RestoreAll)
		std::vector<HandTrack*> gActiveList;
		// this frame's switches, for the effect hooks, which run for every effect in the world and must not lock and copy
		// the settings each time
		std::atomic<bool> gEnabled{ false }, gDimShader{ false };
		// Lights by Item's own record of each weapon hand's enchantment-effect roots, for a hand the fading pass does not follow
		// (Fading off, the weapon exempt): filled by the effect hooks, under gLock, only while Items asked for it the frame before
		// (gItemRootsWanted, set by EffectRootsOf, cleared by UpdateHands). Never filled by walking the game's own list of magic
		// effects from inside the pass - that takes the game's lock while gLock is held, the effect hooks take them the other way
		// round (RELight 2.2's code review: a freeze)
		std::unordered_map<std::uint64_t, std::vector<Root>> gItemRoots;
		std::atomic<bool>                                    gItemRootsWanted{ false };
		// the lights a plugin hung for a hand away from that hand's 3D (NoteHandLight), by ItemKey, under gLock
		std::unordered_map<std::uint64_t, std::vector<Root>> gNotedLights;
		// the HUD's copy (Fade.h HudState): written at the end of each UpdateHands, read by the render thread. Its own small
		// lock, held only while a few dozen bytes are copied - the render thread never waits on the per-frame pass (gLock)
		std::mutex gHudLock;
		HudState   gHud;

		[[nodiscard]] std::uint64_t Key(RE::ActorHandle a_actor, bool a_left)
		{
			return (static_cast<std::uint64_t>(a_actor.native_handle()) << 1) | (a_left ? 1u : 0u);
		}

		// a hand holding a spell: never the same key as the weapon hand, which the effect hooks look up by Key
		[[nodiscard]] std::uint64_t SpellKey(RE::ActorHandle a_actor, bool a_left) { return Key(a_actor, a_left) | (1ull << 63); }

		// gItemRoots' key: the actor by its address (the effect hooks may run off the main thread, where no handle is made)
		[[nodiscard]] std::uint64_t ItemKey(const RE::Actor* a_actor, bool a_left)
		{
			return (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_actor)) << 1) | (a_left ? 1u : 0u);
		}

		void NoteRoot(std::vector<Root>& a_roots, RE::NiAVObject* a_node)
		{
			auto kept = std::ranges::find_if(a_roots, [a_node](const Root& r) { return r.node.get() == a_node; });
			if (kept == a_roots.end()) {
				a_roots.push_back({ RE::NiPointer<RE::NiAVObject>(a_node), gFrame });
			} else {
				kept->frame = gFrame;
			}
		}

		[[nodiscard]] LightInputs InputsOf(const HandTrack& a_hand)
		{
			return { a_hand.out.brightness, a_hand.out.reach, a_hand.out.cool, a_hand.fire, a_hand.verdict.tuning.reachFollows > 0.0f };
		}

		// the light still holds every value ApplyLight last wrote, with the hand's numbers it has now: ApplyLight would write the
		// same values again (Glow::Scaled / ColorKeep write base x factor when the value read is their own). Read under the turn
		[[nodiscard]] bool HoldsOurWrite(const LightSeen& a_seen, const HandTrack& a_hand)
		{
			if (!a_seen.fromKnown || !(a_seen.from == InputsOf(a_hand))) {
				return false;
			}
			auto*       light = a_seen.light.get();
			const auto& d = light->GetLightRuntimeData();
			if (!a_seen.fade.Written() || d.fade != a_seen.fade.written || d.radius.x != a_seen.shownRadius) {
				return false;
			}
			if (a_seen.radius.Written() && (d.radius.x != a_seen.radius.written || d.radius.y != a_seen.radius.written)) {
				return false;
			}
			if (a_seen.color.touched && !ColorKeep::Same(d.diffuse, a_seen.color.written)) {
				return false;
			}
			const float cutoff = LightKit::Isl::On(light) ? LightKit::Isl::Cutoff(light) : 0.0f;
			return !(cutoff > 0.0f && std::isfinite(cutoff)) || (a_seen.cutoff.Written() && cutoff == a_seen.cutoff.written);
		}

		// a_found: the light was found under the hand this frame (a re-apply of this frame's numbers does not count, or a
		// light that has left the weapon would never be let go)
		void ApplyLight(RE::NiPointLight* a_light, const HandTrack& a_hand, bool a_found = true)
		{
			auto [it, added] = gLights.try_emplace(a_light);
			auto& seen = it->second;
			if (added) {
				seen.light.reset(a_light);
			}
			if (a_found) {
				seen.frame = gFrame;
			}
			seen.hand = a_hand.key;               // a spell hand's own key: ReapplyAll must find the hand that scaled it, not the weapon hand
			const auto  turn = LightKit::Turn();  // its fields and the handshakes below in one turn (LightKit.h)
			auto&       data = a_light->GetLightRuntimeData();
			const auto& t = a_hand.verdict.tuning;
			const float before = data.fade;
			data.fade = seen.fade.Apply(before, a_hand.out.brightness);
			Plugin::NoteFadeWrite(a_light, before, data.fade);
			if (t.reachFollows > 0.0f || seen.radius.Written()) {
				const float reachBefore = data.radius.x;
				const float r = seen.radius.Apply(reachBefore, a_hand.out.reach);
				data.radius.x = r;  // x and y are the reach, z is the size
				data.radius.y = r;
				Plugin::NoteReachWrite(a_light, reachBefore, r);  // the reach handshake, as the fade's (a plugin's Reach slider)
			}
			// on Community Shaders the reach comes from the fade and the cutoff, never the radius above: the cutoff moves with the
			// fade (x brightness / reach^2) so the light reaches what "Reach follows" asks
			if (const float cutoffBefore = LightKit::Isl::On(a_light) ? LightKit::Isl::Cutoff(a_light) : 0.0f; cutoffBefore > 0.0f && std::isfinite(cutoffBefore)) {
				const float q = (std::max)(a_hand.out.reach, 0.05f);
				LightKit::Isl::SetCutoff(a_light, seen.cutoff.Apply(cutoffBefore, a_hand.out.brightness / (q * q)));
				seen.cutoff.written = LightKit::Isl::Cutoff(a_light);  // as clamped: our own write, never read back as another's
				Plugin::NoteCutoffWrite(a_light, cutoffBefore, seen.cutoff.written);
			}
			if (a_hand.out.cool > 0.0f || seen.color.touched) {
				data.diffuse = seen.color.Apply(data.diffuse, a_hand.fire, a_hand.out.cool);
			}
			seen.shownFade = data.fade;  // for LightsNow, which must not read the game's light off the main thread
			seen.shownRadius = data.radius.x;
			seen.from = InputsOf(a_hand);
			seen.fromKnown = true;
		}

		void RestoreLight(LightSeen& a_seen)
		{
			if (!a_seen.light) {
				return;
			}
			const auto  turn = LightKit::Turn();
			auto&       data = a_seen.light->GetLightRuntimeData();
			const float before = data.fade;
			data.fade = a_seen.fade.Restore(before);
			Plugin::NoteFadeWrite(a_seen.light.get(), before, data.fade);
			if (a_seen.radius.Written()) {
				const float reachBefore = data.radius.x;
				const float r = a_seen.radius.Restore(reachBefore);
				data.radius.x = r;
				data.radius.y = r;
				Plugin::NoteReachWrite(a_seen.light.get(), reachBefore, r);
			}
			if (a_seen.cutoff.Written() && LightKit::Isl::On(a_seen.light.get())) {
				const float cutoffBefore = LightKit::Isl::Cutoff(a_seen.light.get());
				LightKit::Isl::SetCutoff(a_seen.light.get(), a_seen.cutoff.Restore(cutoffBefore));
				Plugin::NoteCutoffWrite(a_seen.light.get(), cutoffBefore, LightKit::Isl::Cutoff(a_seen.light.get()));
			}
			data.diffuse = a_seen.color.Restore(data.diffuse);
		}

		void RestoreShader(ShaderSeen& a_seen)
		{
			// a finished effect's shader data may already be gone: nothing to put back on it
			auto* data = a_seen.effect && !a_seen.effect->finished ? a_seen.effect->effectShaderData : nullptr;
			if (data) {
				data->fillColor.alpha = a_seen.fill.Restore(data->fillColor.alpha);
				data->rimColor.alpha = a_seen.rim.Restore(data->rimColor.alpha);
			}
		}

		RE::BSEffectShaderMaterial* EffectMaterial(RE::BSShaderProperty* a_property)
		{
			auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(a_property);
			return effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->GetBaseMaterial()) : nullptr;
		}

		void RestoreArt(ArtSeen& a_seen)
		{
			if (auto* material = a_seen.property ? EffectMaterial(a_seen.property.get()) : nullptr) {
				material->baseColorScale = a_seen.scale.Restore(material->baseColorScale);
			}
		}

		// every glowing mesh of an enchantment art takes the hand's brightness (never past its own: a pulse or flare can
		// bring a dimmed glow back up, not over-brighten it)
		void DimArt(RE::NiAVObject* a_model, float a_brightness)
		{
			const float k = std::clamp(a_brightness, 0.0f, 1.0f);
			RE::BSVisit::TraverseScenegraphGeometries(a_model, [&](RE::BSGeometry* a_geometry) {
				auto* shader = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (auto* material = EffectMaterial(shader)) {
					auto [it, added] = gArt.try_emplace(material);
					if (added) {
						it->second.property.reset(shader);
					}
					it->second.frame = gFrame;
					material->baseColorScale = it->second.scale.Apply(material->baseColorScale, k);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// every NiPointLight under a root takes this hand's numbers; returns how many
		std::size_t ApplyUnder(RE::NiAVObject* a_root, const HandTrack& a_hand)
		{
			std::size_t n = 0;
			if (!a_root) {
				return n;
			}
			RE::BSVisit::TraverseScenegraphLights(a_root, [&](RE::NiPointLight* a_light) {
				if (a_light) {
					if (!gLights.contains(a_light) && DebugLogOn()) {
						std::string chain;
						for (auto* p = a_light->parent; p && chain.size() < 200; p = p->parent) {
							chain += std::format(" < {}", p->name.empty() ? "(unnamed)" : p->name.c_str());
						}
						SKSE::log::info("  new light under {}: {}{}", a_root->name.empty() ? "(unnamed)" : a_root->name.c_str(),
							a_light->name.empty() ? "(unnamed)" : a_light->name.c_str(), chain);
					}
					ApplyLight(a_light, a_hand);
					++n;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return n;
		}

		// an effect root that sits in a weapon part was searched with the part: searched again it would count its lights twice
		[[nodiscard]] bool UnderAny(const RE::NiAVObject* a_node, const std::array<RE::NiAVObject*, 2>& a_parts)
		{
			std::size_t depth = 0;
			for (const auto* n = a_node; n && depth < 64; n = n->parent, ++depth) {
				if (std::ranges::find(a_parts, n) != a_parts.end()) {
					return true;
				}
			}
			return false;
		}

		// the lights a plugin noted for this hand (NoteHandLight, before the pass): this frame's or last, still in the scene
		void ApplyNoted(const RE::Actor* a_actor, HandTrack& a_hand)
		{
			const auto it = gNotedLights.find(ItemKey(a_actor, a_hand.left));
			if (it == gNotedLights.end()) {
				return;
			}
			for (const auto& root : it->second) {
				if (root.node && root.node->parent && gFrame - root.frame <= 1) {
					++a_hand.roots;
					a_hand.lights += ApplyUnder(root.node.get(), a_hand);
				}
			}
		}

		void ApplyHand(RE::Actor* a_actor, HandTrack& a_hand)
		{
			a_hand.lights = a_hand.roots = 0;
			const auto*                    weapon = a_hand.reading.weapon;
			std::array<RE::NiAVObject*, 2> parts{ WeaponPart(a_actor, false, a_hand.left, weapon), nullptr };
			if (a_actor->IsPlayerRef()) {
				parts[1] = WeaponPart(a_actor, true, a_hand.left, weapon);
			}
			for (auto* part : parts) {
				if (part) {
					++a_hand.roots;
					a_hand.lights += ApplyUnder(part, a_hand);
				}
			}
			std::erase_if(a_hand.effectRoots, [](const Root& r) { return !r.node || gFrame - r.frame > kKeepFrames; });
			for (auto& root : a_hand.effectRoots) {
				if (!UnderAny(root.node.get(), parts)) {
					++a_hand.roots;
					a_hand.lights += ApplyUnder(root.node.get(), a_hand);
				}
			}
			ApplyNoted(a_actor, a_hand);
		}

		// the actors whose hands are read this frame, in a list kept from frame to frame (no allocation once it has grown)
		std::vector<RE::NiPointer<RE::Actor>> gActors;  // emptied by ReleaseAll, so no actor is held across a load

		std::vector<RE::NiPointer<RE::Actor>>& Actors(Who a_who)
		{
			auto& out = gActors;
			out.clear();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->Is3DLoaded()) {
				out.emplace_back(player);
			}
			if (a_who != Who::kPlayer) {
				if (auto* lists = RE::ProcessLists::GetSingleton()) {
					for (auto& handle : lists->highActorHandles) {
						auto actor = handle.get();
						if (actor && actor.get() != player && (a_who == Who::kEveryone || actor->IsPlayerTeammate()) && actor->Is3DLoaded() && !actor->IsDead()) {
							out.push_back(std::move(actor));
						}
					}
				}
			}
			return out;
		}

		void Sweep()
		{
			for (auto it = gLights.begin(); it != gLights.end();) {
				auto&      seen = it->second;
				const bool gone = !seen.light || seen.light->GetRefCount() <= 1;  // only we still hold it: it has left the game
				if (gone || gFrame - seen.frame > kKeepFrames) {
					if (!gone) {
						RestoreLight(seen);
					}
					it = gLights.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = gShaders.begin(); it != gShaders.end();) {
				if (gFrame - it->second.frame > kKeepFrames) {
					RestoreShader(it->second);
					it = gShaders.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = gArt.begin(); it != gArt.end();) {
				if (gFrame - it->second.frame > kKeepFrames) {
					RestoreArt(it->second);
					it = gArt.erase(it);
				} else {
					++it;
				}
			}
			std::erase_if(gHands, [](const auto& kv) { return gFrame - kv.second.frame > kForgetHand; });
		}

		// by the actor's address, not its handle: this runs on any thread (the effect hooks), and making or reading a handle is
		// the main thread's to do. A walk of this frame's active weapon hands only (gActiveList). Under gLock.
		HandTrack* ActiveHand(const RE::Actor* a_actor, bool a_left)
		{
			if (!a_actor) {
				return nullptr;
			}
			for (auto* h : gActiveList) {
				if (h->left == a_left && h->actorSeen == a_actor) {
					return h;
				}
			}
			return nullptr;
		}

		// the effect's weapon-enchantment controller by its vtable, no RTTI cast (the hooks ask this of every effect in the world);
		// no class of the game derives from it (its own type info), so it matches exactly what skyrim_cast matched
		[[nodiscard]] RE::WeaponEnchantmentController* WeaponEnchantmentOf(RE::ReferenceEffectController* a_ctrl)
		{
			if (!a_ctrl) {
				return nullptr;
			}
			static const REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_WeaponEnchantmentController[0] };
			std::uintptr_t                               first = 0;
			std::memcpy(&first, static_cast<const void*>(a_ctrl), sizeof(first));  // its vtable pointer, read as plain bytes
			return first == vtbl.address() ? static_cast<RE::WeaponEnchantmentController*>(a_ctrl) : nullptr;
		}

		// the hand an enchantment effect belongs to, and its actor; false when it is not a weapon enchantment's
		bool HandOfEffect(RE::ReferenceEffect* a_effect, RE::Actor*& a_actor, bool& a_left, RE::NiAVObject*& a_root)
		{
			auto* ctrl = a_effect ? WeaponEnchantmentOf(a_effect->controller) : nullptr;
			if (!ctrl || !ctrl->target || !ctrl->caster) {
				return false;
			}
			a_actor = ctrl->target;
			a_left = ctrl->caster->castingSource == RE::MagicSystem::CastingSource::kLeftHand;
			a_root = ctrl->attachRoot.get();
			return true;
		}

		// with the debug log on, when a weapon starts being tracked: every light hanging anywhere on the actor, with the nodes
		// above it - so a hand that finds 0 lights says where the lights ARE (a lighting mod hanging them somewhere new)
		void LogActorLights(RE::Actor* a_actor)
		{
			std::size_t n = 0;
			for (const bool first : { false, true }) {
				auto* root = a_actor->Get3D(first);
				if (!root || (first && !a_actor->IsPlayerRef())) {
					continue;
				}
				RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
					if (a_light && n < 16) {
						++n;
						std::string chain;
						for (auto* p = a_light->parent; p && chain.size() < 160; p = p->parent) {
							chain += std::format(" < {}", p->name.empty() ? "(unnamed)" : p->name.c_str());
						}
						const auto& d = a_light->GetLightRuntimeData();
						SKSE::log::info("  light {} ({} view): {}{} | fade {:.2f} radius {:.0f}", n, first ? "first" : "third",
							a_light->name.empty() ? "(unnamed)" : a_light->name.c_str(), chain, d.fade, d.radius.x);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			SKSE::log::info("  {} light(s) on {} in all", n, a_actor->GetName());
		}

		// every light, shader and art mesh back as we found it, and every hand forgotten (switched off, a load)
		void RestoreAll()
		{
			for (auto& [light, seen] : gLights) {
				RestoreLight(seen);
			}
			for (auto& [key, seen] : gShaders) {
				RestoreShader(seen);
			}
			for (auto& [key, seen] : gArt) {
				RestoreArt(seen);
			}
			gLights.clear();
			gShaders.clear();
			gArt.clear();
			gHands.clear();
			gActiveList.clear();
			gActiveHands = 0;
		}

		// one spell hand, one frame (from UpdateHands, under the lock)
		void UpdateSpellHand(RE::Actor* a_actor, bool a_left, const Settings& a_s, float a_delta, bool a_previewOn, bool a_previewToggled,
			float a_previewFraction)
		{
			auto& h = gHands[SpellKey(a_actor->GetHandle(), a_left)];
			h.actor = a_actor->GetHandle();
			h.key = SpellKey(h.actor, a_left);
			h.actorSeen = a_actor;
			h.left = a_left;
			h.spell = true;
			h.frame = gFrame;
			SpellHand sp;
			if (!ReadSpellHand(a_actor, a_left, sp)) {
				h.active = false;
				h.weapon = nullptr;
				h.reading = {};
				return;
			}
			h.reading = {};
			h.reading.tracked = true;
			h.reading.current = sp.current;
			h.reading.max = sp.max;
			h.reading.fraction = sp.fraction;
			if (h.kindSpell != sp.spell || h.kindID != sp.spell->GetFormID()) {  // the kind changes only with the spell
				h.kindSpell = sp.spell;
				h.kindID = sp.spell->GetFormID();
				h.kind = KindOf(sp.spell);
			}
			const Kind kind = h.kind;
			h.fire = kind.element == Element::kFire;
			if (const auto off = KindOff(a_s, kind); !off.empty()) {
				if (h.active || h.weapon != sp.spell) {
					h.weaponLabel = Label(sp.spell);
					h.verdict.why = off;
					if (a_s.debugLog) {
						SKSE::log::info("{} {} hand: spell left alone | {} - {}", a_actor->GetName(), a_left ? "left" : "right", h.weaponLabel, off);
					}
				}
				h.active = false;
				h.weapon = sp.spell;
				return;
			}
			h.fraction = a_previewOn ? Glow::Clamp01(a_previewFraction) : sp.fraction;
			h.hudFraction = h.fraction;
			if (a_previewToggled) {
				h.glow.lastFraction = h.fraction;
			}
			h.verdict.tuning = a_s.tuning;
			h.verdict.mode = Mode::kCharge;
			h.verdict.why = "spell - follows magicka";
			if (!h.active || h.weapon != sp.spell) {
				h.weapon = sp.spell;
				h.glow.Reset(h.fraction, static_cast<std::uint32_t>(SpellKey(h.actor, a_left) * 2654435761u));
				h.actorName = a_actor->GetName();
				h.weaponLabel = Label(sp.spell);
				h.enchLabel = "(spell - follows magicka)";
				if (a_s.debugLog) {
					SKSE::log::info("{} {} hand: spell now tracked | {} - magicka {:.0f}/{:.0f}", h.actorName, a_left ? "left" : "right",
						h.weaponLabel, sp.current, sp.max);
				}
			}
			h.active = true;
			// magicka spent on a cast is not a hit, nor magicka coming back a soul gem: no pulse, no flare
			h.out = Glow::Step(a_s.tuning, h.glow, h.fraction, a_delta, true);
			h.lights = h.roots = 0;
			for (auto* node : sp.nodes) {
				if (!node) {
					continue;
				}
				++h.roots;
				h.lights += ApplyUnder(node, h);
				if (a_s.dimShader) {
					DimArt(node, h.out.brightness);  // the casting art's glow on the hand
				}
			}
			if (sp.casterLight) {
				// the game's casting light: it may hang under the magic node (then this is the same light again, which
				// Glow::Scaled never compounds) or elsewhere
				if (!gLights.contains(sp.casterLight) || gLights[sp.casterLight].frame != gFrame) {
					++h.lights;
				}
				ApplyLight(sp.casterLight, h);
			}
			ApplyNoted(a_actor, h);
		}

		// a hand's color as its lights are made (before any cooling): the brightest light the pass scaled for that hand this
		// frame, brought to a top channel of 1 - what Reticle Arcs' bars wear (FadeArcs.cpp). None: no lit light of the hand
		// found this frame. Main thread, under gLock
		std::optional<Glow::Rgb> HandColor(std::uint64_t a_key)
		{
			const RE::NiColor* best = nullptr;
			float              most = 0.0f;
			for (const auto& [light, seen] : gLights) {
				if (!light || seen.hand != a_key || seen.frame != gFrame) {
					continue;
				}
				const auto& c = seen.color.touched ? seen.color.base : light->GetLightRuntimeData().diffuse;
				const float s = std::fabs(seen.shownFade) * (std::max)({ c.red, c.green, c.blue });
				if (std::isfinite(s) && s > most) {
					most = s;
					best = &c;
				}
			}
			const float top = best ? (std::max)({ best->red, best->green, best->blue }) : 0.0f;
			if (!(top > 0.02f) || !std::isfinite(top)) {
				return std::nullopt;
			}
			return Glow::Rgb{ best->red / top, best->green / top, best->blue / top };
		}

		// the shout the player has ready: the light its first word's spell wears - an effect's own light, else the light of
		// the projectile it fires (his yes 2026-10-20: Reticle Arcs' shout bar in our color too). None for a power or a shout
		// with no light; on Illuminated a light the plugin replaced reads as its own copy, so the bar wears the color we draw.
		std::optional<Glow::Rgb> ShoutColor(RE::PlayerCharacter* a_player)
		{
			const auto* power = a_player ? a_player->GetActorRuntimeData().selectedPower : nullptr;
			const auto* shout = power ? power->As<RE::TESShout>() : nullptr;
			const auto* spell = shout ? shout->variations[0].spell : nullptr;
			if (!spell) {
				return std::nullopt;
			}
			const auto colorOf = [](const RE::TESObjectLIGH* a_light) -> std::optional<Glow::Rgb> {
				if (!a_light) {
					return std::nullopt;
				}
				const auto& c = a_light->data.color;
				const float top = static_cast<float>((std::max)({ c.red, c.green, c.blue }));
				if (top <= 5.0f) {
					return std::nullopt;
				}
				return Glow::Rgb{ c.red / top, c.green / top, c.blue / top };
			};
			for (const auto* effect : spell->effects) {
				const auto* base = effect ? effect->baseEffect : nullptr;
				if (!base) {
					continue;
				}
				if (const auto own = colorOf(base->data.light)) {
					return own;
				}
				if (const auto* proj = base->data.projectileBase) {
					if (const auto fired = colorOf(proj->data.light)) {
						return fired;
					}
				}
			}
			return std::nullopt;
		}

		// what the HUD shows this frame: the player's hands that fade (not one left alone), the switches, and whether her
		// weapons or spells are out (the gems and Reticle Arcs' bars show only then). Main thread, under gLock.
		void PublishHud(const Settings& a_s)
		{
			HudState hud;
			hud.gems = a_s.enabled && a_s.hudGems;
			hud.gemSize = a_s.hudGemSize;
			hud.gemOpacity = a_s.hudGemOpacity;
			hud.gemPercent = a_s.hudGemPercent;
			hud.gemFull = a_s.hudGemFull;
			hud.low = a_s.tuning.sputterBelow;
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const auto* state = player ? player->AsActorState() : nullptr;
			hud.drawn = state && state->IsWeaponDrawn();
			if (a_s.enabled) {
				for (const auto& [key, h] : gHands) {
					if (h.active && h.frame == gFrame && player && h.actorSeen == player) {
						auto& side = hud.hands[h.left ? 0 : 1];
						// a hand holds a spell OR a weapon: should both read active on one side this frame, the spell wins
						if (side.shown && side.spell && !h.spell) {
							continue;
						}
						side.shown = true;
						side.spell = h.spell;
						side.bound = h.reading.bound;
						side.fraction = Glow::Clamp01(h.hudFraction);
						const auto color = HandColor(key);
						side.lit = color.has_value();
						side.color = color.value_or(Glow::Rgb{});
					}
				}
			}
			if (a_s.enabled) {
				const auto shout = ShoutColor(player);
				hud.shoutLit = shout.has_value();
				hud.shoutColor = shout.value_or(Glow::Rgb{});
			}
			std::lock_guard lock(gHudLock);
			gHud = hud;
		}

		void LogHand(const HandTrack& a_h, std::string_view a_what, bool a_on)
		{
			if (a_on) {
				SKSE::log::info("{} {} hand: {} | {} | {} - charge {:.0f}/{:.0f} ({:.0f}%) - {}", a_h.actorName, a_h.left ? "left" : "right",
					a_what, Label(a_h.reading.weapon), Label(a_h.reading.ench), a_h.reading.current, a_h.reading.max,
					a_h.fraction * 100.0f, a_h.verdict.why);
			}
		}
	}

	void UpdateHands(float a_delta)
	{
		// every weapon hand read BEFORE the lock: ReadHand walks a bound weapon's active effects, which the game locks, and the
		// effect hooks hold that lock when they ask for gLock - read under gLock it could freeze (the outside review's threads M3)
		const Settings                             s = Config();  // one copy for the frame
		static std::vector<std::array<Reading, 2>> readings;      // main thread; its storage kept between frames
		readings.clear();
		if (s.enabled) {
			for (auto& actor : Actors(s.who)) {
				readings.push_back({ ReadHand(actor.get(), false), ReadHand(actor.get(), true) });
			}
		}
		std::lock_guard lock(gLock);
		++gFrame;
		// Lights by Item asks again this frame if it still needs its own roots (EffectRootsOf); a hand's roots not seen for a
		// few frames are let go (they hold the effects' 3D)
		gItemRootsWanted.store(false, std::memory_order_relaxed);
		if (gFrame % 60 == 0) {
			for (auto it = gItemRoots.begin(); it != gItemRoots.end();) {
				std::erase_if(it->second, [](const Root& r) { return !r.node || gFrame - r.frame > kKeepFrames; });
				it = it->second.empty() ? gItemRoots.erase(it) : std::next(it);
			}
			std::erase_if(gNotedLights, [](auto& a_kv) {
				std::erase_if(a_kv.second, [](const Root& r) { return !r.node || gFrame - r.frame > kKeepFrames; });
				return a_kv.second.empty();
			});
		}
		gEnabled = s.enabled;
		gDimShader = s.dimShader;
		if (!s.enabled) {
			RestoreAll();
			PublishHud(s);
			Items::Apply();  // Lights by Item works with Fading off too
			return;
		}
		auto&       preview = PreviewState();
		const bool  pulseNow = preview.pulse.exchange(false);
		const bool  previewOn = preview.on;
		static bool previewWas = false;
		const bool  previewToggled = previewOn != previewWas;
		previewWas = previewOn;
		const bool flareNow = preview.flare.exchange(false);
		for (std::size_t i = 0; i < gActors.size() && i < readings.size(); ++i) {
			auto& actor = gActors[i];
			for (const bool left : { false, true }) {
				auto& h = gHands[Key(actor->GetHandle(), left)];
				h.actor = actor->GetHandle();
				h.key = Key(h.actor, left);
				h.actorSeen = actor.get();
				h.left = left;
				h.frame = gFrame;
				h.reading = readings[i][left ? 1 : 0];
				if (!h.reading.tracked) {
					h.active = false;
					h.weapon = nullptr;
					h.effectRoots.clear();  // nothing of ours here: let go of the old effects' 3D
					continue;
				}
				// the verdict changes only with the weapon, the settings or the rules: worked out again only then
				if (!h.verdictKnown || h.verdictWeapon != h.reading.weapon || h.verdictEnch != h.reading.ench ||
					h.verdictEnchID != (h.reading.ench ? h.reading.ench->GetFormID() : 0) || h.verdictRules != RulesGeneration() ||
					!(h.verdictSettings == s)) {
					h.verdict = Judge(h.reading.weapon, h.reading.ench, s);
					h.verdictKnown = true;
					h.verdictWeapon = h.reading.weapon;
					h.verdictEnch = h.reading.ench;
					h.verdictEnchID = h.reading.ench ? h.reading.ench->GetFormID() : 0;
					h.verdictRules = RulesGeneration();
					h.verdictSettings = s;
					h.weaponLabel = Label(h.reading.weapon);
					h.enchLabel = h.reading.ench ? Label(h.reading.ench) : std::string(h.reading.bound ? "(bound weapon)" : "(none)");
					h.fire = h.reading.ench && KindOf(h.reading.ench).element == Element::kFire;
				}
				if (auto* av = actor->AsActorValueOwner()) {
					h.chargeAV = av->GetActorValue(left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge);
				}
				if (h.verdict.mode == Mode::kExempt) {
					if (h.active || h.weapon != h.reading.weapon) {
						h.actorName = actor->GetName();
						LogHand(h, "left alone", s.debugLog);
					}
					h.active = false;
					h.weapon = h.reading.weapon;
					h.effectRoots.clear();
					continue;
				}
				const Glow::Follow follow = Glow::FollowOf(h.reading.bound, h.verdict.mode == Mode::kBound, h.reading.current,
					h.reading.max, h.verdict.boundFadeSeconds, h.reading.fraction);
				h.fraction = follow.fraction;
				if (previewOn) {  // before a new weapon's reset, so the preview's charge is not taken as a hit or a refill
					h.fraction = Glow::Clamp01(preview.fraction);
				}
				// the gem of a bound weapon counts its spell's whole time down (his report 2026-10-16: no gem on a bound sword
				// with half its time gone - it read the light's fraction, full until the last seconds)
				h.hudFraction = Glow::HudFraction(h.reading.bound, previewOn, h.reading.current, h.reading.max, h.fraction);
				if (previewToggled) {
					h.glow.lastFraction = h.fraction;  // the preview going on or off is not a hit or a refill either
				}
				if (!h.active || h.weapon != h.reading.weapon || h.instance != h.reading.instance) {
					h.weapon = h.reading.weapon;
					h.instance = h.reading.instance;
					h.glow.Reset(h.fraction, static_cast<std::uint32_t>(Key(h.actor, left) * 2654435761u));
					h.actorName = actor->GetName();
					LogHand(h, "now tracked", s.debugLog);
					if (s.debugLog) {
						LogActorLights(actor.get());
					}
				}
				if (pulseNow) {
					h.glow.TriggerPulse(h.verdict.tuning);
				}
				if (flareNow) {
					h.glow.TriggerFlare();
				}
				h.active = true;
				// a spell's time left is not a charge; nor is the preview's slider, which moves the charge by hand (its
				// buttons pulse and flare on purpose, above)
				h.out = Glow::Step(h.verdict.tuning, h.glow, h.fraction, a_delta, follow.timed || previewOn);
				if ((h.out.pulsed || h.out.flared) && s.debugLog) {
					LogHand(h, h.out.pulsed ? "spent charge (pulse)" : "recharged (flare)", true);
				}
				ApplyHand(actor.get(), h);
			}
		}
		// the spells in hand: their lights and glow follow the caster's magicka
		if (s.spells) {
			for (auto& actor : gActors) {
				for (const bool left : { false, true }) {
					UpdateSpellHand(actor.get(), left, s, a_delta, previewOn, previewToggled, preview.fraction);
				}
			}
		}
		// a hand not read this frame (a follower dismissed, an actor unloaded) is no longer active: the effect hooks must not
		// keep putting last frame's numbers on its lights
		for (auto& [key, hand] : gHands) {
			if (hand.frame != gFrame) {
				hand.active = false;
				hand.effectRoots.clear();  // its effects' 3D is not ours to keep
			}
		}
		Sweep();
		gActiveList.clear();
		for (auto& [key, hand] : gHands) {
			if (hand.active && !hand.spell) {
				gActiveList.push_back(&hand);
			}
		}
		gActiveHands = gActiveList.size();
		PublishHud(s);
		Items::Apply();  // last: an item's own choice wins over the fading (his order, FadeItems.cpp)
	}

	void AfterReferenceEffect(RE::ReferenceEffect* a_effect, bool a_own3D)
	{
		const bool items = gItemRootsWanted.load(std::memory_order_relaxed);
		if (gActiveHands.load(std::memory_order_relaxed) == 0 && !items && !Items::Holding()) {
			return;
		}
		RE::Actor*      actor = nullptr;
		bool            left = false;
		RE::NiAVObject* root = nullptr;
		if (!HandOfEffect(a_effect, actor, left, root)) {
			return;
		}
		std::lock_guard lock(gLock);
		// the root the effect hangs on (Light Placer hangs its lights there), and an art effect's own model. Not a shader
		// effect's own 3D: that is the whole actor it glows over, torch and armour lights and all
		std::array<RE::NiAVObject*, 2> nodes{ root, a_own3D ? a_effect->Get3D() : nullptr };
		if (items) {  // Lights by Item's own record, whatever the fading state (EffectRootsOf)
			auto& roots = gItemRoots[ItemKey(actor, left)];
			for (auto* node : nodes) {
				if (node) {
					NoteRoot(roots, node);
				}
			}
		}
		if (auto* hand = gEnabled ? ActiveHand(actor, left) : nullptr) {
			for (auto* node : nodes) {
				if (!node) {
					continue;
				}
				NoteRoot(hand->effectRoots, node);
				ApplyUnder(node, *hand);  // right after Light Placer wrote this frame's values for this effect
			}
			if (nodes[1] && gDimShader.load(std::memory_order_relaxed)) {
				DimArt(nodes[1], hand->out.brightness);  // the art's own glowing meshes: VAER's swirls, EAE's art
			}
		}
		for (auto* node : nodes) {
			Items::ReapplyUnder(node);  // last: an item's own choice over Light Placer's animation of this frame
		}
	}

	void AfterCaster(RE::ActorMagicCaster* a_caster)
	{
		using Source = RE::MagicSystem::CastingSource;
		if (!a_caster || !Items::Holding() || (a_caster->castingSource != Source::kLeftHand && a_caster->castingSource != Source::kRightHand)) {
			return;
		}
		// Light Placer animates this hand's casting lights in the caster's update: only its own lights, as Items::Apply finds them
		RE::NiAVObject*   nodes[2]{};
		RE::NiPointLight* light = nullptr;
		SpellHandLights(a_caster->GetCasterAsActor(), a_caster->castingSource == Source::kLeftHand, nodes, light);
		std::lock_guard lock(gLock);
		for (auto* node : nodes) {
			Items::ReapplyUnder(node);
		}
		Items::ReapplyUnder(light);
	}

	void AfterShaderEffect(RE::ShaderReferenceEffect* a_effect)
	{
		// the cheap test first: every effect shader in the loaded world comes through here every frame
		if (gActiveHands.load(std::memory_order_relaxed) == 0 || !gDimShader.load(std::memory_order_relaxed)) {
			return;
		}
		RE::Actor*      actor = nullptr;
		bool            left = false;
		RE::NiAVObject* root = nullptr;
		if (!a_effect || !a_effect->effectShaderData || !HandOfEffect(a_effect, actor, left, root)) {
			return;
		}
		std::lock_guard lock(gLock);
		const auto*     hand = ActiveHand(actor, left);
		if (!hand) {
			return;
		}
		auto [it, added] = gShaders.try_emplace(a_effect);
		auto& seen = it->second;
		if (added) {
			seen.effect.reset(a_effect);
		}
		seen.frame = gFrame;
		// a shader's alpha tops out at 1: a pulse or flare can brighten a dimmed glow back up, not past its own
		const float k = (std::min)(hand->out.brightness, 1.0f);
		auto*       data = a_effect->effectShaderData;
		data->fillColor.alpha = std::clamp(seen.fill.Apply(data->fillColor.alpha, k), 0.0f, 1.0f);
		data->rimColor.alpha = std::clamp(seen.rim.Apply(data->rimColor.alpha, k), 0.0f, 1.0f);
	}

	void ReapplyAll()
	{
		std::lock_guard lock(gLock);
		if (gEnabled) {
			std::optional<std::unique_lock<std::recursive_mutex>> turn;  // one turn for the walk, taken at its first light
			for (auto& [light, seen] : gLights) {
				if (!seen.light || gFrame - seen.frame > 1) {
					continue;
				}
				// every cell's pass re-applies every hand: a light may be animated in a cell its actor has since left. One that still
				// holds the write made with its hand's numbers now is left alone: the write would put back the same values
				const auto it = gHands.find(seen.hand);
				if (it == gHands.end() || !it->second.active) {
					continue;
				}
				if (!turn) {
					turn.emplace(LightKit::Turn());
				}
				if (!HoldsOurWrite(seen, it->second)) {
					ApplyLight(light, it->second, false);
				}
			}
		}
		Items::Reapply();  // an item's own choice still wins after the lights were written again (fading on or off)
	}

	void NoteItemWrite(RE::NiPointLight* a_light, float a_fadeBefore, float a_fadeAfter, const RE::NiColor& a_colorBefore,
		const RE::NiColor& a_colorAfter)
	{
		Plugin::NoteFadeWrite(a_light, a_fadeBefore, a_fadeAfter);
		const auto it = gLights.find(a_light);
		if (it == gLights.end()) {
			return;
		}
		auto& seen = it->second;
		seen.fromKnown = false;  // an item's write: the next pass writes this light again
		if (seen.fade.touched && seen.fade.written == a_fadeBefore) {
			seen.fade.written = a_fadeAfter;  // its base stays the light's own: the next pass scales that, not the item's write
			seen.shownFade = a_fadeAfter;
		}
		if (seen.color.touched && ColorKeep::Same(seen.color.written, a_colorBefore)) {
			seen.color.written = a_colorAfter;
		}
	}

	void Rebase(RE::NiPointLight* a_light, float a_ratio, float a_now)
	{
		std::lock_guard lock(gLock);
		const auto      it = gLights.find(a_light);
		if (it == gLights.end() || !it->second.fade.touched) {
			return;
		}
		auto& seen = it->second;
		seen.fromKnown = false;     // a new base: the next pass writes this light again
		seen.fade.base *= a_ratio;  // the Brightness slider moved the light's own value, not ours
		seen.fade.written = a_now;
		seen.shownFade = a_now;
	}

	void ReleaseAll()
	{
		std::lock_guard lock(gLock);
		RestoreAll();
		Items::Release();
		gActors.clear();
		gItemRoots.clear();  // the old game's effects' 3D
		gNotedLights.clear();
	}

	// a plugin's light for a hand that hangs away from the hand's 3D: the fading pass and Lights by Item take it as the hand's
	// (Illuminated's first-person weapon and art lights, ArtLights.cpp). Main thread, before the pass
	void NoteHandLight(const RE::Actor* a_actor, bool a_left, RE::NiPointLight* a_light)
	{
		if (!a_actor || !a_light) {
			return;
		}
		std::lock_guard lock(gLock);
		NoteRoot(gNotedLights[ItemKey(a_actor, a_left)], a_light);
	}

	std::vector<RE::NiAVObject*> NotedLightsOf(const RE::Actor* a_actor, bool a_left)
	{
		std::vector<RE::NiAVObject*> out;
		if (const auto it = gNotedLights.find(ItemKey(a_actor, a_left)); it != gNotedLights.end()) {
			for (const auto& root : it->second) {
				if (root.node && root.node->parent && gFrame - root.frame <= 1) {
					out.push_back(root.node.get());
				}
			}
		}
		return out;
	}

	RE::NiAVObject* WeaponPart(RE::Actor* a_actor, bool a_firstPerson, bool a_left, const RE::TESForm* a_weapon)
	{
		const auto& biped = a_actor ? a_actor->GetBiped(a_firstPerson) : nullptr;
		if (!biped || !a_weapon) {
			return nullptr;
		}
		using B = RE::BIPED_OBJECT;
		if (a_left) {
			const auto& shield = biped->objects[B::kShield];
			return shield.item == a_weapon && shield.partClone ? shield.partClone.get() : nullptr;
		}
		for (auto slot = static_cast<std::uint32_t>(B::kHandToHandMelee); slot <= static_cast<std::uint32_t>(B::kCrossbow); ++slot) {
			const auto& obj = biped->objects[slot];
			if (obj.item == a_weapon && obj.partClone) {
				return obj.partClone.get();
			}
		}
		return nullptr;
	}

	// an ACTIVE weapon hand's roots are the ones the fading pass keeps. While Fading is off or the weapon is exempt it keeps
	// none, so Lights by Item's own record is read instead (gItemRoots - the effect hooks fill it from the next frame on): an
	// item's choice reaches an enchantment's glow whatever the fading state (his answer 2026-10-14 "Fix it in 2.2", CodeRabbit
	// on RELight #16). Main thread (Items::Apply), under gLock.
	std::vector<RE::NiAVObject*> EffectRootsOf(const RE::Actor* a_actor, bool a_left)
	{
		std::vector<RE::NiAVObject*> out;
		if (const auto* hand = ActiveHand(a_actor, a_left); hand && !hand->effectRoots.empty()) {
			for (const auto& root : hand->effectRoots) {
				if (root.node) {
					out.push_back(root.node.get());
				}
			}
			return out;
		}
		gItemRootsWanted.store(true, std::memory_order_relaxed);
		if (const auto it = gItemRoots.find(ItemKey(a_actor, a_left)); it != gItemRoots.end()) {
			for (const auto& root : it->second) {
				if (root.node && gFrame - root.frame <= kKeepFrames) {
					out.push_back(root.node.get());
				}
			}
		}
		return out;
	}

	std::vector<HandView> Snapshot()
	{
		std::vector<HandView> out;
		std::lock_guard       lock(gLock);
		for (const auto& [key, h] : gHands) {
			if (!h.reading.tracked || gFrame - h.frame > kKeepFrames) {
				continue;
			}
			HandView v;
			v.actor = h.actorName;
			v.player = h.actorSeen && h.actorSeen == RE::PlayerCharacter::GetSingleton();  // a pointer compare, nothing read
			v.left = h.left;
			v.weapon = h.weaponLabel;
			v.enchantment = h.enchLabel;
			v.why = h.verdict.why;
			v.bound = h.reading.bound;
			v.spell = h.spell;
			v.exempt = !h.active;
			v.fraction = h.fraction;
			v.hudFraction = h.hudFraction;
			v.current = h.reading.current;
			v.max = h.reading.max;
			v.brightness = h.active ? h.out.brightness : 1.0f;
			v.reach = h.active ? h.out.reach : 1.0f;
			v.cool = h.active ? h.out.cool : 0.0f;
			v.lights = h.lights;
			v.roots = h.roots;
			v.chargeAV = h.chargeAV;  // nothing of the game is touched here: this runs on the menu's and DevBench's threads
			out.push_back(std::move(v));
		}
		std::ranges::sort(out, {}, [](const HandView& v) { return std::make_pair(v.actor, v.left); });
		return out;
	}

	HudState Hud()
	{
		std::lock_guard lock(gHudLock);
		return gHud;
	}

	std::vector<LightNow> LightsNow()
	{
		std::vector<LightNow> out;
		std::lock_guard       lock(gLock);
		out.reserve(gLights.size());
		for (const auto& [light, seen] : gLights) {
			if (seen.light) {
				out.push_back({ seen.shownFade, seen.fade.base, seen.shownRadius, seen.fade.frozen });
			}
		}
		return out;
	}

	std::size_t DimmedGlowCount()
	{
		std::lock_guard lock(gLock);
		return gShaders.size() + gArt.size();
	}

	std::vector<std::pair<float, float>> ArtNow()
	{
		std::vector<std::pair<float, float>> out;
		std::lock_guard                      lock(gLock);
		for (const auto& [material, seen] : gArt) {
			out.emplace_back(seen.scale.base, seen.scale.Written() ? seen.scale.written : seen.scale.base);
		}
		return out;
	}

	std::size_t ScaledLightCount()
	{
		std::lock_guard lock(gLock);
		return gLights.size();
	}

	std::size_t FrozenLightCount()
	{
		std::lock_guard lock(gLock);
		return static_cast<std::size_t>(std::ranges::count_if(gLights, [](const auto& kv) { return kv.second.fade.frozen; }));
	}

}
