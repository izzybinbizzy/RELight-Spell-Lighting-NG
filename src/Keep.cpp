// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The one place a light the GAME made is held. The spell lights, the hand lights,
// the lights the sliders scale and the lights a switch holds out are lists of plain pointers; the one reference that
// keeps such a light alive for them is here (the fading module, Lights by Item and the ground twins hold some too, so a light
// can stay a few frames past its last use). A light has left the game when this reference is its last, when the only other
// holder is its entry in the scene's lists, or when it has no node and no entry in any of those lists, though a scene light the
// scene let go of still holds it (KeptLights.h); then every list forgets it before it goes, on the main thread.
// (Each list used to hold a reference of its own and call a light gone at "one reference left". A hand light sat in three
// of them, so that was never true: it was never let go.)

#include "Plugin.h"

#include "KeptLights.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Plugin
{
	namespace
	{
		RE::BSSpinLock                                               gLock;
		std::unordered_map<RE::NiLight*, RE::NiPointer<RE::NiLight>> gKept;
		// looks only after something that can end a light (Events.cpp, the hooks), for the few frames what else held it needs to let go (the
		// fading's last passes); a light something else holds longer waits for the next such moment
		constexpr int                                                                      kSweepFrames = 8;
		std::atomic<int>                                                                   gSweepFrames{ kSweepFrames };
		std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(SweepWhy::kCount)> gSweepNotes{};
		KeptLights::Watch                                                                  gUnowned;  // main thread

		// main thread: the lights every loaded actor's magic casters hold through their scene lights
		[[nodiscard]] std::vector<const void*> CastingLights()
		{
			std::vector<const void*> out;
			const auto               add = [&out](RE::Actor* a_actor) {
				for (const auto* caster : a_actor->GetActorRuntimeData().magicCasters) {
					if (const auto* bs = caster ? caster->light.get() : nullptr; bs && bs->light) {
						out.push_back(bs->light.get());
					}
				}
			};
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				add(player);
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (auto actor = handle.get()) {
						add(actor.get());
					}
				}
			}
			return out;
		}
	}

	std::uint32_t SweepNotes(std::size_t a_why) noexcept
	{
		return gSweepNotes[a_why].load(std::memory_order_relaxed);
	}

	void NoteSweep(SweepWhy a_why) noexcept
	{
		gSweepNotes[static_cast<std::size_t>(a_why)].fetch_add(1, std::memory_order_relaxed);
		gSweepFrames.store(kSweepFrames, std::memory_order_relaxed);
		// what can only end a light brings none for the walk to find
		if (a_why != SweepWhy::kRefDrop && a_why != SweepWhy::kMagicDrop && a_why != SweepWhy::kDeath && a_why != SweepWhy::kArtDetach) {
			NoteSliderLook();
		}
	}

	bool SweepWindowOpen() noexcept { return gSweepFrames.load(std::memory_order_relaxed) > 0; }

	void KeepLight(RE::NiLight* a_light)
	{
		if (a_light) {
			RE::BSSpinLockGuard lock(gLock);
			gKept.try_emplace(a_light, a_light);
		}
	}

	void SweepKeptLights()
	{
		int left = gSweepFrames.load(std::memory_order_relaxed);
		if (left <= 0) {
			return;
		}
		const LightKit::Cost::Pass pass{ 2, "sweep kept lights" };
		gSweepFrames.compare_exchange_strong(left, left - 1, std::memory_order_relaxed);  // a note in between keeps its count
		std::vector<RE::NiPointer<RE::NiLight>> gone;                                     // let go when this returns, after every list has forgotten them
		static std::vector<KeptLights::Seen>    lost;                                     // main thread; its storage kept between looks
		lost.clear();
		{
			RE::BSSpinLockGuard lock(gLock);
			for (auto it = gKept.begin(); it != gKept.end();) {
				const auto refs = it->second->GetRefCount();
				if (refs <= 1) {
					gone.push_back(std::move(it->second));
					it = gKept.erase(it);
				} else {
					if (!it->second->parent) {
						lost.push_back({ it->first, refs, 0, 0 });
					}
					++it;
				}
			}
		}
		// its entry there holds it too, and the scene's own pass takes it out only when that is the last: we let go and that pass sees it.
		// One queued for removal holds it from the scene's queue, and may sit in an active list as well: each scene light counts once.
		// One in no list at all is drawn by nothing, though a scene light the scene let go of may still hold it
		auto* scene = lost.empty() ? nullptr : LightKit::Scene();
		if (!scene) {
			lost.clear();  // no scene to read: nothing is judged
		} else {
			static std::vector<const void*> counted;  // main thread; its storage kept between looks
			counted.clear();
			const auto count = [](const auto& a_list, std::uint32_t KeptLights::Seen::* a_field) {
				for (const auto& bsLight : a_list) {
					const auto* light = bsLight ? bsLight->light.get() : nullptr;
					const auto  it = light ? std::ranges::find(lost, static_cast<const void*>(light), &KeptLights::Seen::key) : lost.end();
					if (it != lost.end() && std::ranges::find(counted, static_cast<const void*>(bsLight.get())) == counted.end()) {
						counted.push_back(bsLight.get());
						++((*it).*a_field);
					}
				}
			};
			auto& lists = scene->GetRuntimeData();
			count(lists.activeLights, &KeptLights::Seen::entries);
			count(lists.activeShadowLights, &KeptLights::Seen::entries);
			RE::BSSpinLockGuard queue(lists.lightQueueLock);
			count(lists.lightQueueRemove, &KeptLights::Seen::entries);
			count(lists.lightQueueAdd, &KeptLights::Seen::elsewhere);
			count(lists.unk190, &KeptLights::Seen::elsewhere);
		}
		// a casting light the scene let go of is still its caster's, who hangs it again: never ours to let go of that way
		if (std::ranges::any_of(lost, KeptLights::OffScene)) {
			const auto casting = CastingLights();
			std::erase_if(lost, [&casting](const KeptLights::Seen& a_seen) {
				return KeptLights::OffScene(a_seen) && std::ranges::find(casting, a_seen.key) != casting.end();
			});
		}
		if (const auto unowned = gUnowned.Look(lost); !unowned.empty()) {
			RE::BSSpinLockGuard lock(gLock);
			for (auto it = gKept.begin(); it != gKept.end();) {
				if (std::ranges::find(unowned, static_cast<const void*>(it->first)) != unowned.end()) {
					gone.push_back(std::move(it->second));
					it = gKept.erase(it);
				} else {
					++it;
				}
			}
		}
		if (gUnowned.Waiting() && gSweepFrames.load(std::memory_order_relaxed) <= 0) {
			gSweepFrames.store(1, std::memory_order_relaxed);  // its next look; it ends once the watch has decided
		}
		if (gone.empty()) {
			return;
		}
		NoteOptionsLook();  // the Configs page counts what is lit
		GoneLights set;
		set.reserve(gone.size());
		for (const auto& light : gone) {
			set.insert(light.get());
		}
		ForgetSpellLights(set);  // Hooks.cpp's lists, then the switches' (Options.cpp) - all on the main thread
		ForgetHandLights(set);
		ForgetSliderLights(set);
	}

	std::size_t KeptLights()
	{
		RE::BSSpinLockGuard lock(gLock);
		return gKept.size();
	}

	std::vector<KeptLight> KeptLightList(std::size_t a_most)
	{
		// copied out first: the scene's queue lock is never taken inside ours
		std::vector<RE::NiPointer<RE::NiLight>> kept;
		{
			RE::BSSpinLockGuard lock(gLock);
			for (const auto& entry : gKept) {
				if (kept.size() >= a_most) {
					break;
				}
				kept.push_back(entry.second);
			}
		}
		std::vector<KeptLight> out;
		auto*                  scene = LightKit::Scene();
		for (const auto& held : kept) {
			const auto* parent = held->parent;
			const auto* root = parent;
			while (root && root->parent) {
				root = root->parent;
			}
			KeptLight k{};
			k.name = held->name.c_str();
			// the copy above holds one more
			k.refs = held->GetRefCount() - 1;
			k.parent = parent ? parent->name.c_str() : "";
			k.root = root ? root->name.c_str() : "";
			k.culled = held->GetAppCulled();
			if (scene) {
				auto&      lists = scene->GetRuntimeData();
				const auto look = [&held](const auto& a_list, int& a_count) {
					for (const auto& bsLight : a_list) {
						if (bsLight && bsLight->light.get() == held.get()) {
							++a_count;
						}
					}
				};
				look(lists.activeLights, k.active);
				look(lists.activeShadowLights, k.shadow);
				RE::BSSpinLockGuard queue(lists.lightQueueLock);
				look(lists.lightQueueAdd, k.queuedAdd);
				look(lists.lightQueueRemove, k.queuedRemove);
				look(lists.unk190, k.unnamed);
			}
			out.push_back(std::move(k));
		}
		return out;
	}
}
