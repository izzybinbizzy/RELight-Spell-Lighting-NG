// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// A row of small lights along each bolt, flame spray and cone our data lights - Illuminated's row (its StreamLights.cpp), as
// HIS ANSWERS 2026-10-20 asked for both mods: checklist 140, "if you plain out cannot make part 1 of 140 to happen then revert
// to this as a backup plan" (one stretched light cannot be drawn inside our mods), then "Looks good. Do the flame sprays and
// cones in Illuminated first, then RELight", and 146 / 147 closed: "for now its the 12 or however many point lights you ended
// up using". The projectile wears its own light at its own size (Streams.cpp); the row lights its length - lights no farther
// apart than 80 units, laid again every frame to where its art ends, where it hit something, or its range (BeamRow.h, the
// same file as Illuminated's). Off on Performance (his Quality / Performance order of 2026-10-20), with All lights off, with
// its option switched off, and once the sneak fade has her lights out (Hooks.cpp). With Dynamic Lighting on the row
// flickers (BeamRow::Flicker), held under its cap so the peak can swing (BeamRow::Steady); with it off it holds still.
// Truman's call after the Lightning Bolt freeze still stands for the loader threads: the Load3D hook only QUEUES the
// projectile (QueueRow, any thread); every light is made, moved and taken off here on the main thread (UpdateRows, in the
// player update).
//
// CREDIT: a light cloned from one master light, marked as moving and handed to the shadow scene node - ReLight by Truman
// (github.com/TrumanGIT/ReLight, GPL-3.0-or-later, used with his permission), as Held.cpp MakeOwnLight.

#include "BeamRow.h"
#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::size_t kMostRowLights = 64;  // every row light in the scene together
		constexpr std::size_t kMostPerRow = 20;
		// off Community Shaders the game lets one piece of ground take only a few lights, so a row of 15 lit only the ground near
		// her hands; fewer, wider lights reach the whole stream (BeamRow::Lay makes each stronger to keep the row's light)
		constexpr std::size_t kMostPerRowPlain = 5;
		constexpr float       kRowPlain = 0.5f;     // every row off Community Shaders
		constexpr float       kRowHeight = 100.0f;  // a beam's usual height over the ground it lights
		constexpr const char* kRowName = "RSARow";

		std::unordered_map<RE::FormID, RowRecipe> gRecipes;  // projectile base -> its row; written once at data load, read-only after
		std::atomic<bool>                         gReady{ false };

		struct Live
		{
			RE::NiPointer<RE::NiAVObject>           root;
			RE::NiPointer<RE::TESObjectREFR>        ref;
			const RowRecipe*                        recipe{ nullptr };
			std::vector<RE::NiPointer<RE::BSLight>> lights;
			const RE::NiColor*                      tint{ nullptr };  // the art pick's color for this projectile, as its own light wears it (Brightness.cpp)
		};
		std::unordered_map<const RE::NiAVObject*, Live> gLive;  // main thread
		std::size_t                                     gLights = 0;
		std::size_t                                     gMade = 0;
		std::mutex                                      gQueueLock;
		std::vector<RE::NiPointer<RE::TESObjectREFR>>   gQueue;  // what a loader thread loaded, for the next frame (under gQueueLock)
		// a projectile whose row a switch took off while it still flies (Performance, All lights off, sneaking, its option, the light
		// cap): hung again when the rows come back, or a held stream stays bare until the next cast (main thread)
		std::vector<RE::NiPointer<RE::TESObjectREFR>> gParked;

		// Illuminated's row, the same numbers (its 1.4: the row came off its cap, so Vanilla and ENB were trimmed to Community
		// Shaders' lift)
		[[nodiscard]] float RowStrengthFor(Lighting a_lighting)
		{
			switch (a_lighting) {
			case Lighting::kVanilla:
				return 1.1f;
			case Lighting::kEnb:
				return 0.5f;  // the whole stream lit, not washed out
			default:
				return 1.0f;
			}
		}

		// Community Shaders draws a row light at 4x its fade, so twice the reach laid; drawn plain a row reaches only that
		[[nodiscard]] float RowReachFor(Lighting a_lighting)
		{
			return a_lighting == Lighting::kShaders ? 1.0f : 2.0f;
		}

		[[nodiscard]] bool RowsOn() { return !PerformanceMode() && !AllLightsOff(); }

		// while she sneaks the rows on HER spells fade with her spell lights (Hooks.cpp dims the lights on what she cast) and
		// come off once faded out; another caster's row keeps its light
		[[nodiscard]] bool RowOn(const RE::TESObjectREFR* a_ref) { return RowsOn() && (SneakLevel() > 0.0f || !CastByPlayer(a_ref)); }

		// Community Shaders draws an inverse-square point light at four times its fade and writes back the reach that gives, so the
		// cutoff is set for that (laid reach 197 read 389 otherwise)
		constexpr float kDrawnTimes = 4.0f;

		[[nodiscard]] float IslRoom(float a_radius)
		{
			return a_radius * a_radius + LightKit::kK * LightKit::kLightSize * LightKit::kLightSize * 0.5f;
		}

		// the strongest fade a row light of this reach can take on Community Shaders before its cutoff passes its top
		[[nodiscard]] float IslTop(float a_radius)
		{
			return Isl::kHighestCutoff * IslRoom(a_radius) / (kDrawnTimes * LightKit::kK);
		}

		// a light's strength, reach and color written whole; past its top the reach would grow instead of the light brightening
		void Dress(RE::NiPointLight* a_light, const RE::NiColor& a_color, float a_fade, float a_radius, bool a_isl)
		{
			auto& data = a_light->GetLightRuntimeData();
			data.diffuse = a_color;
			data.fade = a_fade;
			data.radius = { a_radius, a_radius, LightKit::kLightSize };
			a_light->SetLightAttenuation(a_radius);
			if (a_isl) {
				data.fade = (std::min)(a_fade, IslTop(a_radius));
				Isl::SetOwn(a_light);
				Isl::SetCutoff(a_light, kDrawnTimes * LightKit::kK * data.fade / IslRoom(a_radius));
			} else {
				data.ambient = { a_color.red * 0.1f, a_color.green * 0.1f, a_color.blue * 0.1f };  // RE::Light's rule (Truman)
			}
		}

		// where a row starts and which way it runs: a bolt along its model's +Y from its root; a flame spray or cone along the
		// projectile's own aim from where it stands (a spray's model points another way - measured 2026-10-11: its forward is -X)
		struct Axis
		{
			RE::NiPoint3 start;
			RE::NiPoint3 dir{ 0.0f, 1.0f, 0.0f };
		};

		[[nodiscard]] Axis AxisOf(const RE::TESObjectREFR* a_ref, const RE::NiAVObject* a_root, bool a_beam)
		{
			if (a_beam) {
				return { a_root->world.translate, a_root->world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f } };
			}
			const float pitch = a_ref->GetAngleX(), yaw = a_ref->GetAngleZ();
			return { a_ref->GetPosition(), { std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) } };
		}

		// how far along the axis the drawn art reaches (its geometry's bounds); 0 when nothing is drawn
		[[nodiscard]] float DrawnLength(RE::NiAVObject* a_root, const Axis& a_axis)
		{
			float reach = 0.0f;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
				const auto& bound = a_geometry->worldBound;
				if (bound.radius > 0.0f) {
					reach = (std::max)(reach, (bound.center - a_axis.start).Dot(a_axis.dir) + bound.radius);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return std::isfinite(reach) ? reach : 0.0f;
		}

		// where a row runs right now and how long it is: a bolt that hit something runs straight from its root to that impact (the
		// lightning is drawn to what it hit), one in the open along its model as far as its art is drawn; a flame spray or cone along
		// its aim, never past its newest impact. Never past the range
		struct Path
		{
			Axis  axis;
			float length{ 0.0f };
		};

		[[nodiscard]] Path PathOf(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root, bool a_beam)
		{
			Path  path{ AxisOf(a_ref, a_root, a_beam) };
			auto* proj = a_ref ? a_ref->As<RE::Projectile>() : nullptr;
			if (!proj) {
				return path;
			}
			auto& rt = proj->GetProjectileRuntimeData();
			path.length = rt.range;
			if (const float drawn = DrawnLength(a_root, path.axis); drawn > 1.0f && drawn < path.length) {
				path.length = drawn;
			}
			for (const auto* impact : rt.impacts) {  // the newest impact comes first
				if (impact) {
					const auto  hit = impact->desiredTargetLoc - path.axis.start;
					const float d = hit.Length();
					if (d > 1.0f && d < rt.range && (a_beam || d < path.length)) {
						path.length = d;
						if (a_beam) {
							path.axis.dir = hit / d;
						}
					}
					break;
				}
			}
			if (!std::isfinite(path.length)) {
				path.length = 0.0f;
			}
			return path;
		}

		void Forget(const RE::NiAVObject* a_root, RE::ShadowSceneNode* a_scene)
		{
			const auto it = gLive.find(a_root);
			if (it == gLive.end()) {
				return;
			}
			for (auto& bs : it->second.lights) {
				if (!bs) {
					continue;
				}
				if (a_scene) {
					a_scene->RemoveLight(bs);
				}
				if (auto* light = bs->light.get(); light && light->parent) {
					light->parent->DetachChild(light);  // no light node left on a projectile that lives on
				}
			}
			gLights -= (std::min)(gLights, it->second.lights.size());
			gLive.erase(it);
		}

		// true once the projectile wears its row
		bool Hang(RE::TESObjectREFR* a_ref, RE::ShadowSceneNode* a_scene)
		{
			auto*      root = a_ref ? a_ref->Get3D() : nullptr;
			auto*      node = root ? root->AsNode() : nullptr;
			auto*      base = a_ref ? a_ref->GetBaseObject() : nullptr;
			auto*      proj = a_ref ? a_ref->As<RE::Projectile>() : nullptr;
			const auto recipe = base ? gRecipes.find(base->GetFormID()) : gRecipes.end();
			if (node && gLive.contains(node)) {
				return true;
			}
			if (!node || !node->parent || !proj || recipe == gRecipes.end() || gLights >= kMostRowLights) {
				return false;
			}
			if (recipe->second.option != kNone && !OptionLit(recipe->second.option)) {
				return false;
			}
			const bool isl = InverseSquare();
			const auto most = isl ? kMostPerRow : kMostPerRowPlain;
			const auto want = BeamRow::Lay(proj->GetProjectileRuntimeData().range, static_cast<int>(most), recipe->second.reach, kRowHeight);
			const auto n = (std::min)(want.size(), kMostRowLights - gLights);
			Live       live{ RE::NiPointer<RE::NiAVObject>(node), RE::NiPointer<RE::TESObjectREFR>(a_ref), &recipe->second, {} };
			live.tint = TintOf(base);
			for (std::size_t i = 0; i < n; ++i) {
				auto* light = LightKit::CloneLight();
				if (!light) {
					break;
				}
				light->name = kRowName;
				light->fadeAmount = kMovingLightMark;                 // so RE::Light's flicker prevention lets a moving light reach every surface
				Dress(light, recipe->second.color, 0.0f, 1.0f, isl);  // dark until UpdateRows lays it out
				light->local.translate = {};
				light->local.scale = 1.0f;
				const RE::NiPointer<RE::NiPointLight> hold(light);  // freed here if it is never registered
				node->AttachChild(light, true);
				RE::NiUpdateData update{};
				light->Update(update);
				if (auto* bs = LightKit::AddToScene(a_scene, light, 2.0f)) {
					live.lights.emplace_back(bs);
				} else {
					node->DetachChild(light);
				}
			}
			if (live.lights.empty()) {
				return false;
			}
			if (gMade < 6) {
				SKSE::log::info("[ROW] {:08X} | {} light(s) along {:.0f} | reach target {:.0f} | fade {:.2f}", base->GetFormID(), live.lights.size(),
					proj->GetProjectileRuntimeData().range, recipe->second.reach, recipe->second.fade);
			}
			++gMade;
			gLights += live.lights.size();
			gLive.emplace(node, std::move(live));
			return true;
		}

		void Move()
		{
			const bool isl = InverseSquare();
			// the two sliders and Dim in daylight, then the per-lighting strength the rows were tuned with (no plain-drawing step:
			// RowStrengthFor already holds the Vanilla / ENB difference)
			const float brightNow = Brightness() * DaylightFactor();
			const float bright = brightNow * RowStrengthFor(LightingFound()), reachPct = Reach();
			// Dynamic Lighting (this session's, as the data was read with) makes the row flicker: a spray's own light is steady
			// on its own, so the row shimmers by itself while the switch is on
			static const auto start = std::chrono::steady_clock::now();
			const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			const bool        flickers = DynamicLightingThisSession();
			RE::NiUpdateData  update{};
			for (auto& [root, live] : gLive) {
				const auto& rec = *live.recipe;
				const auto  path = PathOf(live.ref.get(), live.root.get(), rec.beam);
				const auto& axis = path.axis;
				const float length = path.length;
				if (length <= 1.0f) {
					continue;
				}
				const auto  toLocal = live.root->world.Invert();
				const auto  row = BeamRow::Lay(length, static_cast<int>(live.lights.size()), rec.reach, kRowHeight);
				RE::NiColor color = live.tint ? *live.tint : rec.color;
				if (auto* fired = live.ref ? live.ref->As<RE::Projectile>() : nullptr; fired && fired->GetProjectileRuntimeData().spell) {
					if (const auto own = Fade::Items::ChoiceColor(fired->GetProjectileRuntimeData().spell->GetFormID())) {
						color = *own;  // the spell's Lights by Item color reaches its row too
					}
				}
				for (std::size_t i = 0; i < live.lights.size(); ++i) {
					auto* light = live.lights[i] ? netimmerse_cast<RE::NiPointLight*>(live.lights[i]->light.get()) : nullptr;
					if (!light) {
						continue;
					}
					if (i >= row.size()) {  // a row cut so short it needs fewer: the spare lights rest dark at the hand
						Dress(light, color, 0.0f, 1.0f, isl);
						light->local.translate = {};
						continue;
					}
					const auto& s = row[i];
					const float reach = s.reach * reachPct * RowReachFor(LightingFound());
					// at full strength five lights wash a stream out white off Community Shaders
					const float want = rec.fade * s.strength * bright * (isl ? 1.0f : kRowPlain);
					// a dimmer Brightness or daylight lowers the cap too, so the sliders still reach a capped row
					const float steady = BeamRow::Steady(want, isl ? IslTop(reach) * (std::min)(brightNow, 1.0f) : 0.0f);
					Dress(light, color, steady * (flickers ? BeamRow::Flicker(seconds, i) : 1.0f), reach, isl);
					light->local.translate = toLocal * (axis.start + axis.dir * (s.along * length));  // on the path, in the root's units
					if (static std::size_t told = 0; told < 6) {
						++told;
						SKSE::log::info("[ROW-LAID] {} light(s) over {:.0f} | reach {:.0f} | strength {:.2f} steady {:.2f} (set {:.2f}, top {:.2f}) | brightness {:.2f} | color {:.0f},{:.0f},{:.0f}{} | {}",
							row.size(), length, reach, want, steady, light->GetLightRuntimeData().fade, isl ? IslTop(reach) : 0.0f, bright, color.red * 255.0f,
							color.green * 255.0f, color.blue * 255.0f, live.tint ? " (the art pick's)" : "", flickers ? "flickers" : "steady");
					}
					light->Update(update);
				}
			}
		}
	}

	void SetRowRecipes(std::unordered_map<RE::FormID, RowRecipe>&& a_recipes)
	{
		if (gReady.load(std::memory_order_acquire)) {
			return;  // once a session: a loader thread may be reading them
		}
		gRecipes = std::move(a_recipes);
		gReady.store(true, std::memory_order_release);
		const auto beams = std::ranges::count_if(gRecipes, [](const auto& a_r) { return a_r.second.beam; });
		SKSE::log::info("rows: {} bolt(s) and {} spray(s) / cone(s) wear a row of lights along their length (at most {} per row, {} in the scene)",
			beams, gRecipes.size() - static_cast<std::size_t>(beams), kMostPerRow, kMostRowLights);
	}

	void QueueRow(RE::TESObjectREFR* a_ref)
	{
		auto* base = a_ref && gReady.load(std::memory_order_acquire) ? a_ref->GetBaseObject() : nullptr;
		if (!base || !gRecipes.contains(base->GetFormID())) {
			return;
		}
		std::lock_guard l{ gQueueLock };
		gQueue.emplace_back(a_ref);  // held by its refcount; no handle made on a loader thread
	}

	void UpdateRows()
	{
		auto*                                         scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		std::vector<RE::NiPointer<RE::TESObjectREFR>> queued;
		{
			std::lock_guard l{ gQueueLock };
			queued.swap(gQueue);
		}
		const bool on = RowsOn();
		// gone (its 3D out of the world or rebuilt), switched off, or every row off: taken off the scene now
		std::vector<const RE::NiAVObject*> drop;
		for (const auto& [root, live] : gLive) {
			const bool gone = !live.root->parent || !live.ref || live.ref->Get3D() != live.root.get();
			if (gone || !RowOn(live.ref.get()) || (live.recipe->option != kNone && !OptionLit(live.recipe->option))) {
				drop.push_back(root);
				if (!gone) {
					gParked.push_back(live.ref);
				}
			}
		}
		for (const auto* root : drop) {
			Forget(root, scene);
		}
		if (!on) {
			gParked.insert(gParked.end(), queued.begin(), queued.end());
		} else if (scene) {
			// kept while its option or the light cap still holds it back, so it is tried again
			std::erase_if(gParked, [scene](const auto& a_ref) { return RowOn(a_ref.get()) && Hang(a_ref.get(), scene); });
			for (const auto& ref : queued) {
				if (!RowOn(ref.get()) || !Hang(ref.get(), scene)) {
					gParked.push_back(ref);
				}
			}
		}
		// only what still flies stays parked: a long stretch on Performance would otherwise keep every spray projectile
		std::erase_if(gParked, [](const auto& a_ref) {
			const auto* root = a_ref ? a_ref->Get3D() : nullptr;
			return !root || !root->parent;
		});
		if (!gLive.empty()) {
			Move();
		}
	}

	void DropRows()
	{
		auto*                              scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		std::vector<const RE::NiAVObject*> all;
		for (const auto& [root, live] : gLive) {
			all.push_back(root);
		}
		for (const auto* root : all) {
			Forget(root, scene);
		}
		gParked.clear();
		std::lock_guard l{ gQueueLock };
		gQueue.clear();
	}

	bool IsRowLight(const RE::NiLight* a_light)
	{
		return a_light && std::string_view(a_light->name.c_str()) == kRowName;
	}

	std::size_t LiveRowLights() { return gLights; }
	std::size_t RowsMade() { return gMade; }
}
