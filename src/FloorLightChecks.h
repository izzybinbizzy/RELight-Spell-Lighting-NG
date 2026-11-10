// The Floor Light Fix, shared by our lighting mods (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.
//
// The off-game checks of FloorLightMath.h (no game needed), run by each carrying plugin's own tests: call Run with the test's
// check function and add kCount to its floor.
#pragma once

#include "FloorLightMath.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace FloorLightChecks
{
	using Check = void (*)(bool a_ok, const char* a_what, int a_line);

	// the checks one Run makes (raise it when checks are added)
	inline constexpr int kCount = 30;

	namespace Detail
	{
		inline std::array<std::uint8_t, 5> Call(std::int32_t a_rel, std::uint8_t a_op = 0xE8)
		{
			const auto u = static_cast<std::uint32_t>(a_rel);
			return { a_op, static_cast<std::uint8_t>(u), static_cast<std::uint8_t>(u >> 8), static_cast<std::uint8_t>(u >> 16),
				static_cast<std::uint8_t>(u >> 24) };
		}

		// a stand-in for the game's code around the two hooked sites: 5-byte calls written into a buffer at real addresses
		struct Code
		{
			std::array<std::uint8_t, 64> bytes{};

			[[nodiscard]] std::uintptr_t At(std::size_t a_offset) const noexcept { return reinterpret_cast<std::uintptr_t>(bytes.data()) + a_offset; }

			void PutCall(std::uintptr_t a_site, std::uintptr_t a_to)
			{
				const auto call = Call(static_cast<std::int32_t>(static_cast<std::intptr_t>(a_to - (a_site + 5))));
				std::memcpy(bytes.data() + (a_site - At(0)), call.data(), call.size());
			}

			[[nodiscard]] std::array<std::uint8_t, 5> Read(std::uintptr_t a_site) const
			{
				std::array<std::uint8_t, 5> out{};
				std::memcpy(out.data(), bytes.data() + (a_site - At(0)), out.size());
				return out;
			}
		};
	}

	inline void Run(Check a_check)
	{
#define FLC(x) a_check((x), #x, __LINE__)
		using Detail::Call;
		using FloorLightMath::CallTarget;

		// the target counts from the end of the 5-byte call, forward and backward, to the farthest a rel32 reaches
		constexpr std::uintptr_t site = 0x1414A1C72;
		FLC(CallTarget(Call(0x100), site) == site + 5 + 0x100);
		FLC(CallTarget(Call(-0x10), site) == site + 5 - 0x10);
		FLC(CallTarget(Call(0), site) == site + 5);
		FLC(CallTarget(Call(std::numeric_limits<std::int32_t>::max()), site) == site + 5 + 0x7FFFFFFF);
		FLC(CallTarget(Call(std::numeric_limits<std::int32_t>::min()), site) == site + 5 - 0x80000000ull);
		// anything but E8 is not a call this was written for: a jump, a nop, a breakpoint
		FLC(!CallTarget(Call(0x100, 0xE9), site).has_value());
		FLC(!CallTarget(Call(0x100, 0x90), site).has_value());
		FLC(!CallTarget(Call(0x100, 0xCC), site).has_value());

		// the 5 bytes at each hooked site, copied out of the unpacked 1.6.1170 exe at its preferred base: each must read as a call
		// onto the erase, and the absolute addresses are pinned so the offsets cannot all move together
		{
			using namespace FloorLightMath::Game1170;
			constexpr std::uintptr_t              base = 0x140000000;
			constexpr std::array<std::uint8_t, 5> passBytes{ 0xE8, 0x79, 0x67, 0x00, 0x00 };
			constexpr std::array<std::uint8_t, 5> drainBytes{ 0xE8, 0x47, 0x6F, 0x00, 0x00 };
			FLC(base + kPass + kPassCall == 0x1414A1C72);
			FLC(base + kDrain + kDrainCall == 0x1414A14A4);
			FLC(base + kErase == 0x1414A83F0);
			FLC(CallTarget(passBytes, base + kPass + kPassCall) == base + kErase);
			FLC(CallTarget(drainBytes, base + kDrain + kDrainCall) == base + kErase);
		}

		// the lights the hook clears: the ones the game's erase takes, clamped to the array (the game does not clamp; both sites pass 1)
		{
			using FloorLightMath::ErasedRange;
			using R = std::pair<std::uint32_t, std::uint32_t>;
			constexpr auto top = std::numeric_limits<std::uint32_t>::max();
			FLC(ErasedRange(3, 1, 8) == R(3, 4));
			FLC(ErasedRange(0, 1, 1) == R(0, 1));
			FLC(ErasedRange(7, 1, 8) == R(7, 8));
			// a count equal to the size takes the whole array, whatever the index
			FLC(ErasedRange(2, 5, 5) == R(0, 5));
			FLC(ErasedRange(8, 8, 8) == R(0, 8));
			FLC(ErasedRange(top, 8, 8) == R(0, 8));
			// a range running past the end stops at it; nothing past it, nothing for a count of 0 or an empty array
			FLC(ErasedRange(6, 4, 8) == R(6, 8));
			FLC(ErasedRange(9, 1, 8) == R(8, 8));
			FLC(ErasedRange(3, 0, 8) == R(3, 3));
			FLC(ErasedRange(0, 0, 0) == R(0, 0));
			FLC(ErasedRange(top, 2, 8) == R(8, 8));
			FLC(ErasedRange(4, top, 8) == R(4, 8));
		}

		// two copies in one game: the first finds both sites calling the erase and hooks them, the second reads them after the
		// first's writes and stands down, and a pair with one site taken is never read as free
		{
			Detail::Code                        code;
			const auto                          pass = code.At(8);
			const auto                          drain = code.At(24);
			const auto                          erase = code.At(48);
			const auto                          read = [&code](std::uintptr_t a_site) { return code.Read(a_site); };
			const std::array<std::uintptr_t, 2> sites{ pass, drain };
			code.PutCall(pass, erase);
			code.PutCall(drain, erase);
			FLC(!FloorLightMath::FirstTaken(sites, erase, read).has_value());
			// the first copy's writes: each call now goes to a trampoline jump of its plugin
			code.PutCall(pass, code.At(40));
			code.PutCall(drain, code.At(40));
			FLC(FloorLightMath::FirstTaken(sites, erase, read) == std::optional<std::size_t>(0));
			// only the second site taken (another plugin's hook of that call): still taken, so nothing is installed by halves
			code.PutCall(pass, erase);
			FLC(FloorLightMath::FirstTaken(sites, erase, read) == std::optional<std::size_t>(1));
			// a site that is no call at all (nopped out) is taken too
			code.PutCall(drain, erase);
			code.bytes[8] = 0x90;
			FLC(FloorLightMath::FirstTaken(sites, erase, read) == std::optional<std::size_t>(0));
			// and back to the game's own bytes: free again
			code.PutCall(pass, erase);
			FLC(!FloorLightMath::FirstTaken(sites, erase, read).has_value());
		}
#undef FLC
	}
}
