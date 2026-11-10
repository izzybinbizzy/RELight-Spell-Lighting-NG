// The Floor Light Fix, shared by our lighting mods (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.
//
// What it does and why: FloorLightFix.h. The hook is the merged Floor Light Fix plugin's (its repo's 69762e6), moved here whole.
#include "PCH.h"

#include "FloorLightFix.h"
#include "FloorLightMath.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

namespace FloorLightFix
{
	namespace
	{
		using Lights = RE::BSTArray<RE::NiPointer<RE::BSLight>>;
		namespace Game = FloorLightMath::Game1170;

		REL::Relocation<void(RE::BSLight*)> gClearGeometries{};

		// both sites were checked to be the game's own call onto the same erase, so one thunk and one original serve both
		struct Erase
		{
			static void Thunk(Lights* a_array, std::uint32_t a_index, std::uint32_t a_count)
			{
				// every light the erase takes, though both sites pass a count of 1 (read in the exe); no C++ exception may leave a hook
				// (a fault inside the game's own call is not one, and passes this catch)
				try {
					if (a_array) {
						const auto [first, last] = FloorLightMath::ErasedRange(a_index, a_count, a_array->size());
						for (auto i = first; i < last; ++i) {
							if (auto* light = (*a_array)[i].get(); light && !light->geomList.empty()) {
								gClearGeometries(light);
							}
						}
					}
				} catch (...) {
				}
				original(a_array, a_index, a_count);
			}
			static inline REL::Relocation<decltype(&Thunk)> original{};
		};
	}

	void Install() noexcept
	{
		// nothing may leave into the plugin's load: a log line can throw (a locked log file, low memory)
		try {
			if (const auto game = REL::Module::get().version(); game != REL::Version{ 1, 6, 1170, 0 }) {
				SKSE::log::info("Floor Light Fix not installed: written for Skyrim 1.6.1170 only, and this game is {}", game.string("."));
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			if (trampoline.free_size() < kTrampolineBytes) {
				SKSE::log::warn("Floor Light Fix not installed: the plugin set aside {} of the {} trampoline bytes it needs", trampoline.free_size(),
					kTrampolineBytes);
				return;
			}
			const auto                           erase = REL::Offset(Game::kErase).address();
			const std::array<std::uintptr_t, 2>  sites{ REL::Offset(Game::kPass).address() + Game::kPassCall,
				REL::Offset(Game::kDrain).address() + Game::kDrainCall };
			constexpr std::array<const char*, 2> names{ "the per-light pass", "the RemoveLight queue drain" };
			const auto                           read = [](std::uintptr_t a_site) {
				std::array<std::uint8_t, 5> bytes{};
				std::memcpy(bytes.data(), reinterpret_cast<const void*>(a_site), bytes.size());
				return bytes;
			};
			if (const auto taken = FloorLightMath::FirstTaken(sites, erase, read)) {
				SKSE::log::info(
					"Floor Light Fix not installed here: {} no longer holds the game's call onto the erase - another mod of ours that "
					"carries this fix most likely loaded first (its log says installed), or another plugin hooks that call",
					names[*taken]);
				return;
			}
			gClearGeometries = REL::Relocation<void(RE::BSLight*)>{ REL::Offset(Game::kClear).address() };
			// the original is in place before either call points at the thunk; both sites were read calling this same erase
			Erase::original = REL::Relocation<decltype(&Erase::Thunk)>{ erase };
			for (const auto site : sites) {
				static_cast<void>(trampoline.write_call<5>(site, Erase::Thunk));
			}
			SKSE::log::info("Floor Light Fix installed at {} and {}", names[0], names[1]);
		} catch (...) {
			try {
				SKSE::log::error("Floor Light Fix: the install stopped on an exception (low memory?) - the fix may be in at both sites, one or none");
			} catch (...) {
			}
		}
	}
}
