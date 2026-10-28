// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The one place a light the GAME made is held. The spell lights, the lights put out while sneaking, the hand lights,
// the lights the sliders scale and the lights a switch holds out are lists of plain pointers; the one reference that
// keeps such a light alive for them is here (the fading module, Lights by Item and the ground twins hold some too, so a light
// can stay a few frames past its last use). A light has left the game when this reference is its last, and then
// every list forgets it before it is freed, on the main thread.
// (Each list used to hold a reference of its own and call a light gone at "one reference left". A hand light sat in three
// of them, so that was never true: it was never let go.)

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		RE::BSSpinLock                                               gLock;
		std::unordered_map<RE::NiLight*, RE::NiPointer<RE::NiLight>> gKept;
	}

	void KeepLight(RE::NiLight* a_light)
	{
		if (a_light) {
			RE::BSSpinLockGuard lock(gLock);
			gKept.try_emplace(a_light, a_light);
		}
	}

	void SweepKeptLights()
	{
		std::vector<RE::NiPointer<RE::NiLight>> gone;  // freed when this returns, after every list has forgotten them
		{
			RE::BSSpinLockGuard lock(gLock);
			for (auto it = gKept.begin(); it != gKept.end();) {
				if (it->second->GetRefCount() <= 1) {
					gone.push_back(std::move(it->second));
					it = gKept.erase(it);
				} else {
					++it;
				}
			}
		}
		if (gone.empty()) {
			return;
		}
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
