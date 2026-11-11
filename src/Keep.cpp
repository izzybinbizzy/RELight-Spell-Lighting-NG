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

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>  // VirtualQuery, ReadProcessMemory: the holder search reads the game's memory

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
						lost.push_back({ it->first, refs, 0 });
					}
					++it;
				}
			}
		}
		// its entry there holds it too, and the scene's own pass takes it out only when that is the last: we let go and that pass sees it.
		// One queued for removal holds it from the scene's queue, and may sit in an active list as well: each scene light counts once
		if (auto* scene = lost.empty() ? nullptr : LightKit::Scene()) {
			static std::vector<const void*> counted;  // main thread; its storage kept between looks
			counted.clear();
			const auto count = [](const auto& a_list) {
				for (const auto& bsLight : a_list) {
					const auto* light = bsLight ? bsLight->light.get() : nullptr;
					const auto  it = light ? std::ranges::find(lost, static_cast<const void*>(light), &KeptLights::Seen::key) : lost.end();
					if (it != lost.end() && std::ranges::find(counted, static_cast<const void*>(bsLight.get())) == counted.end()) {
						counted.push_back(bsLight.get());
						++it->entries;
					}
				}
			};
			auto& lists = scene->GetRuntimeData();
			count(lists.activeLights);
			count(lists.activeShadowLights);
			RE::BSSpinLockGuard queue(lists.lightQueueLock);
			count(lists.lightQueueRemove);
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

	namespace
	{
		struct Image
		{
			std::uintptr_t base{ 0 };
			std::string    name;
		};

		// the module an address lies in (its base and file name); base 0 off every module
		[[nodiscard]] Image ImageOf(std::uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module) || !module) {
				return {};
			}
			std::array<wchar_t, 260> path{};
			const auto               length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			std::wstring_view        file(path.data(), length);
			if (const auto slash = file.find_last_of(L"\\/"); slash != std::wstring_view::npos) {
				file.remove_prefix(slash + 1);
			}
			Image image{ reinterpret_cast<std::uintptr_t>(module), {} };
			for (const wchar_t c : file) {
				image.name.push_back(c < 128 ? static_cast<char>(c) : '?');
			}
			return image;
		}

		// a read that cannot fault: the memory may change under a search that runs beside the game
		template <class T>
		[[nodiscard]] bool Peek(std::uintptr_t a_address, T& a_out)
		{
			SIZE_T read = 0;
			return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_address), &a_out, sizeof(T), &read) && read == sizeof(T);
		}

		// the class whose vtable sits at a_vtable, from the module's own type data (the compiler's object locator); empty when it is none
		[[nodiscard]] std::string ClassAt(std::uintptr_t a_vtable)
		{
			const auto     image = ImageOf(a_vtable);
			std::uintptr_t locator = 0;
			if (!image.base || !Peek(a_vtable - sizeof(std::uintptr_t), locator) || ImageOf(locator).base != image.base) {
				return {};
			}
			struct Locator
			{
				std::uint32_t signature{ 0 };
				std::uint32_t offset{ 0 };
				std::uint32_t cdOffset{ 0 };
				std::int32_t  type{ 0 };
				std::int32_t  hierarchy{ 0 };
				std::int32_t  self{ 0 };
			};
			Locator col{};
			if (!Peek(locator, col) || col.signature != 1 || col.self <= 0 || image.base + static_cast<std::uintptr_t>(col.self) != locator) {
				return {};
			}
			std::array<char, 96> name{};
			if (col.type <= 0 || !Peek(image.base + static_cast<std::uintptr_t>(col.type) + 16, name)) {
				return {};
			}
			name.back() = '\0';
			return name.data();
		}

		// the nearest vtable at or before a_address: the object the reference most likely sits in, and the reference's place in it
		[[nodiscard]] std::pair<std::string, std::int64_t> HolderOf(std::uintptr_t a_address)
		{
			constexpr std::uintptr_t kBack = 0x600;
			const std::uintptr_t     first = a_address & ~std::uintptr_t{ 7 };
			for (std::uintptr_t at = first; at + kBack >= first && at >= 8; at -= 8) {
				std::uintptr_t word = 0;
				if (!Peek(at, word)) {
					break;
				}
				if (auto name = word ? ClassAt(word) : std::string{}; !name.empty()) {
					return { std::move(name), static_cast<std::int64_t>(a_address - at) };
				}
			}
			return { std::string{}, -1 };
		}
	}

	std::vector<KeptHolder> KeptLightHolders(std::size_t a_most)
	{
		// read under our lock and searched by address only: no reference of ours may be the last one let go on this thread
		struct Target
		{
			std::uintptr_t address{ 0 };
			std::string    name;
			std::uint32_t  refs{ 0 };
		};
		std::vector<Target>         targets;
		std::vector<std::uintptr_t> ours;
		{
			RE::BSSpinLockGuard lock(gLock);
			for (const auto& [light, held] : gKept) {
				ours.push_back(reinterpret_cast<std::uintptr_t>(&light));
				ours.push_back(reinterpret_cast<std::uintptr_t>(&held));
				if (!held->parent && targets.size() < a_most) {
					targets.push_back({ reinterpret_cast<std::uintptr_t>(held.get()), held->name.c_str(), held->GetRefCount() });
				}
			}
		}
		std::vector<std::pair<std::size_t, std::uintptr_t>> hits;
		const auto                                          search = [&ours](const std::vector<std::uintptr_t>& a_targets, auto& a_hits) {
			if (a_targets.empty()) {
				return;
			}
			constexpr std::size_t       kChunk = std::size_t{ 1 } << 20;
			constexpr std::size_t       kMostHits = 48;
			constexpr DWORD             kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			constexpr std::uintptr_t    kStackReach = 0x40000;
			std::vector<std::uintptr_t> buffer(kChunk / sizeof(std::uintptr_t));
			const auto                  low = std::ranges::min(a_targets);
			const auto                  high = std::ranges::max(a_targets);
			const auto                  mineFrom = reinterpret_cast<std::uintptr_t>(buffer.data());
			const auto                  mineTo = mineFrom + kChunk;
			// this thread's own stack holds copies too
			const auto               stack = reinterpret_cast<std::uintptr_t>(&a_hits);
			MEMORY_BASIC_INFORMATION mem{};
			for (std::uintptr_t at = 0x10000; a_hits.size() < kMostHits && VirtualQuery(reinterpret_cast<LPCVOID>(at), &mem, sizeof(mem)) == sizeof(mem);
				at = reinterpret_cast<std::uintptr_t>(mem.BaseAddress) + mem.RegionSize) {
				if (mem.State != MEM_COMMIT || (mem.Protect & kWritable) == 0 || (mem.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
					continue;
				}
				const auto begin = reinterpret_cast<std::uintptr_t>(mem.BaseAddress);
				const auto end = begin + mem.RegionSize;
				for (auto from = begin; from < end && a_hits.size() < kMostHits; from += kChunk) {
					SIZE_T read = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(from), buffer.data(), (std::min)(kChunk, static_cast<std::size_t>(end - from)), &read)) {
						continue;
					}
					for (std::size_t i = 0; i < read / sizeof(std::uintptr_t); ++i) {
						const auto word = buffer[i];
						if (word < low || word > high) {
							continue;
						}
						const auto where = from + i * sizeof(std::uintptr_t);
						const bool mine = (where >= mineFrom && where < mineTo) || (where + kStackReach > stack && where < stack + kStackReach) ||
						                  std::ranges::find(ours, where) != ours.end();
						for (std::size_t t = 0; !mine && t < a_targets.size(); ++t) {
							if (word == a_targets[t]) {
								a_hits.emplace_back(t, where);
							}
						}
					}
				}
			}
		};
		std::vector<std::uintptr_t> addresses;
		for (const auto& target : targets) {
			addresses.push_back(target.address);
		}
		search(addresses, hits);
		std::vector<KeptHolder> out;
		for (std::size_t t = 0; t < targets.size(); ++t) {
			KeptHolder none{};
			none.light = targets[t].name;
			none.refs = targets[t].refs;
			bool any = false;
			for (const auto& [which, where] : hits) {
				if (which != t) {
					continue;
				}
				any = true;
				KeptHolder h = none;
				h.address = where;
				if (const auto image = ImageOf(where); image.base) {
					h.where = std::format("{}+{:X}", image.name, where - image.base);
				}
				auto [holder, offset] = HolderOf(where);
				const bool scene = holder.find("BSLight") != std::string::npos || holder.find("BSShadowLight") != std::string::npos;
				h.holder = std::move(holder);
				h.offset = offset;
				out.push_back(std::move(h));
				// the scene light that holds it: its own count (NiRefObject, +8) and every place that holds the scene light
				if (scene && offset > 0) {
					const auto    light = where - static_cast<std::uintptr_t>(offset);
					std::uint32_t count = 0;
					static_cast<void>(Peek(light + 8, count));
					std::vector<std::pair<std::size_t, std::uintptr_t>> up;
					search({ light }, up);
					for (const auto& found : up) {
						const auto at = found.second;
						KeptHolder s = none;
						s.light = std::format("{} - its scene light {:X}", none.light, light);
						s.refs = count;
						s.address = at;
						if (const auto image = ImageOf(at); image.base) {
							s.where = std::format("{}+{:X}", image.name, at - image.base);
						}
						auto [upHolder, upOffset] = HolderOf(at);
						s.holder = std::move(upHolder);
						s.offset = upOffset;
						out.push_back(std::move(s));
					}
					if (up.empty()) {
						KeptHolder s = none;
						s.light = std::format("{} - its scene light {:X}", none.light, light);
						s.refs = count;
						s.where = "no place holds the scene light";
						out.push_back(std::move(s));
					}
				}
			}
			if (!any) {
				none.where = "no other place holds it";
				out.push_back(std::move(none));
			}
		}
		return out;
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
		auto*                  player = RE::PlayerCharacter::GetSingleton();
		auto*                  scene = LightKit::Scene();
		// the game's own objects that keep a light, around her: who else may still hold a kept light
		struct Owner
		{
			const void* light{ nullptr };
			const char* kind{ "" };
			RE::FormID  id{ 0 };
			bool        deleted{ false };
			bool        loaded{ false };
		};
		std::vector<Owner> owners;
		constexpr float    kOwnerReach = 6000.0f;
		if (auto* tes = player ? RE::TES::GetSingleton() : nullptr) {
			tes->ForEachReferenceInRange(player, kOwnerReach, [&owners](RE::TESObjectREFR* a_ref) {
				const void* light = nullptr;
				const char* kind = "";
				if (const auto* explosion = a_ref ? skyrim_cast<const RE::Explosion*>(a_ref) : nullptr) {
					light = explosion->GetExplosionRuntimeData().light.get();
					kind = "explosion";
				} else if (const auto* hazard = a_ref ? a_ref->As<RE::Hazard>() : nullptr) {
					light = hazard->GetHazardRuntimeData().light.get();
					kind = "hazard";
				} else if (const auto* projectile = a_ref ? a_ref->As<RE::Projectile>() : nullptr) {
					const auto& bs = projectile->GetProjectileRuntimeData().light;
					light = bs ? bs->light.get() : nullptr;
					kind = "projectile";
				}
				if (light) {
					owners.push_back({ light, kind, a_ref->GetFormID(), a_ref->IsDeleted(), a_ref->Is3DLoaded() });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		for (const auto& held : kept) {
			const auto* parent = held->parent;
			const auto* root = parent;
			while (root && root->parent) {
				root = root->parent;
			}
			int caster = -1;
			if (player) {
				const auto& casters = player->GetActorRuntimeData().magicCasters;
				for (std::size_t i = 0; i < std::size(casters); ++i) {
					const auto* bs = casters[i] ? casters[i]->light.get() : nullptr;
					if (bs && bs->light.get() == held.get()) {
						caster = static_cast<int>(i);
					}
				}
			}
			KeptLight k{};
			k.name = held->name.c_str();
			// the copy above holds one more
			k.refs = held->GetRefCount() - 1;
			k.parent = parent ? parent->name.c_str() : "";
			k.root = root ? root->name.c_str() : "";
			k.caster = caster;
			k.culled = held->GetAppCulled();
			k.ground = GroundLightRole(held.get());
			k.weapon = HeldLightHolds(held.get());
			k.body = BodyLightHolds(held.get());
			if (const auto it = std::ranges::find(owners, static_cast<const void*>(held.get()), &Owner::light); it != owners.end()) {
				k.owner = std::format("{} {:08X}{}{}", it->kind, it->id, it->deleted ? " deleted" : "", it->loaded ? "" : " no 3D");
			}
			if (scene) {
				auto&      lists = scene->GetRuntimeData();
				const auto look = [&k, &held](const auto& a_list, int& a_count) {
					for (const auto& bsLight : a_list) {
						if (bsLight && bsLight->light.get() == held.get()) {
							++a_count;
							k.bsRefs = bsLight->GetRefCount();
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
