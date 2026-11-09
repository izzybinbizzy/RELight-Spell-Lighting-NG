// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The one place a light the GAME made is held. The spell lights, the lights put out while sneaking, the hand lights,
// the lights the sliders scale and the lights a switch holds out are lists of plain pointers; the one reference that
// keeps such a light alive for them is here (the fading module, Lights by Item and the ground twins hold some too, so a light
// can stay a few frames past its last use). A light has left the game when this reference is its last, or when the only other
// holder is its entry in the scene's lists (KeptLights.h), and then every list forgets it before it goes, on the main thread.
// (Each list used to hold a reference of its own and call a light gone at "one reference left". A hand light sat in three
// of them, so that was never true: it was never let go.)

#include "Plugin.h"

#include "KeptLights.h"

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
		if (a_why != SweepWhy::kRefDrop && a_why != SweepWhy::kDeath && a_why != SweepWhy::kArtDetach) {
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
						lost.push_back({ it->first, refs, 0 });
					}
					++it;
				}
			}
		}
		// its entry there holds it too, and the scene's own pass takes it out only when that is the last: we let go and that pass sees it
		if (auto* scene = lost.empty() ? nullptr : LightKit::Scene()) {
			const auto count = [](const auto& a_list) {
				for (const auto& bsLight : a_list) {
					const auto* light = bsLight ? bsLight->light.get() : nullptr;
					if (const auto it = light ? std::ranges::find(lost, static_cast<const void*>(light), &KeptLights::Seen::key) : lost.end(); it != lost.end()) {
						++it->entries;
					}
				}
			};
			const auto& lists = scene->GetRuntimeData();
			count(lists.activeLights);
			count(lists.activeShadowLights);
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
			gSweepFrames.store(1, std::memory_order_relaxed);  // its second look; it ends with that look
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
}
