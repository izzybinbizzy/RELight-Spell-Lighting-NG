// Pulse - the shape a spell's light pulses in, one per kind of spell (the Realistic choice of Dynamic Lighting)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Fire sporadic, absorb a heartbeat, healing a calm breath and the rest - one shape per kind of spell, as he picked.
// Stateless: the factor is worked out from the time and the light's seed alone, so it is safe on any thread and needs no memory
// per light; "random" is value noise from a hash of (seed, step), so it never repeats and two lights never move together.
// Game-free and byte-identical in Illuminated and RELight - Spell Lighting NG (`PC Runner\fade copies check.py`); tested off
// the game (tests\test_fade.cpp, PulseChecks).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Pulse
{
	enum class Kind : std::uint8_t
	{
		kNone,
		kFire,
		kFrost,
		kShock,
		kHealing,
		kAbsorbHealth,
		kAbsorbMagicka,
		kAbsorbStamina,
		kPoison,
		kIllusion,
		kConjuration,
		kAlteration,
		kMagelight,
		kWard,
		kRune,
	};

	inline constexpr std::array<std::string_view, 15> kNames{ "none", "fire", "frost", "shock", "healing", "absorbhealth",
		"absorbmagicka", "absorbstamina", "poison", "illusion", "conjuration", "alteration", "magelight", "ward", "rune" };

	[[nodiscard]] constexpr std::string_view NameOf(Kind a_kind)
	{
		const auto i = static_cast<std::size_t>(a_kind);
		return i < kNames.size() ? kNames[i] : kNames[0];
	}

	[[nodiscard]] constexpr std::optional<Kind> KindOf(std::string_view a_name)
	{
		for (std::size_t i = 0; i < kNames.size(); ++i) {
			if (kNames[i] == a_name) {
				return static_cast<Kind>(i);
			}
		}
		return std::nullopt;
	}

	inline constexpr double kTwoPi = 6.283185307179586;

	[[nodiscard]] constexpr std::uint32_t Hash(std::uint32_t a_seed, std::uint32_t a_step)
	{
		std::uint32_t h = a_seed * 0x9E3779B1u ^ (a_step * 0x85EBCA77u + 0x7F4A7C15u);
		h ^= h >> 16;
		h *= 0x85EBCA6Bu;
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 16;
		return h;
	}

	// 0 .. 1, the same for one (seed, step) every time
	[[nodiscard]] constexpr double Unit(std::uint32_t a_seed, std::int64_t a_step)
	{
		return static_cast<double>(Hash(a_seed, static_cast<std::uint32_t>(a_step)) >> 8) * (1.0 / 16777216.0);
	}

	// smooth value noise, -1 .. 1, a_rate new values a second
	[[nodiscard]] inline double Noise(double a_seconds, double a_rate, std::uint32_t a_seed)
	{
		const double x = a_seconds * a_rate;
		const double f = std::floor(x);
		const auto   i = static_cast<std::int64_t>(f);
		const double u = x - f;
		const double s = u * u * (3.0 - 2.0 * u);
		const double a = Unit(a_seed, i) * 2.0 - 1.0;
		const double b = Unit(a_seed, i + 1) * 2.0 - 1.0;
		return a + (b - a) * s;
	}

	// a short event that may fall in each a_slot-second slot (with a_chance), a_length long at a random place in it: its size
	// (a_least .. a_most, random per event) at this moment, smooth in and out or, a_hard, square
	[[nodiscard]] inline double Event(double a_seconds, double a_slot, double a_chance, double a_length, double a_least, double a_most,
		std::uint32_t a_seed, bool a_hard = false)
	{
		const auto slot = static_cast<std::int64_t>(std::floor(a_seconds / a_slot));
		double     out = 0.0;
		for (const std::int64_t k : { slot - 1, slot }) {
			if (Unit(a_seed ^ 0xA5A5A5A5u, k) >= a_chance) {
				continue;
			}
			const double start = (static_cast<double>(k) + Unit(a_seed ^ 0x5A5A5A5Au, k) * (1.0 - a_length / a_slot)) * a_slot;
			const double p = (a_seconds - start) / a_length;
			if (p >= 0.0 && p < 1.0) {
				const double size = a_least + (a_most - a_least) * Unit(a_seed ^ 0x3C3C3C3Cu, k);
				out = (std::max)(out, size * (a_hard ? 1.0 : std::sin(p * 3.141592653589793)));
			}
		}
		return out;
	}

	// a repeating cycle a_period long: where in it this light is, 0 .. 1 (each light starts at its own place)
	[[nodiscard]] inline double Phase(double a_seconds, double a_period, std::uint32_t a_seed)
	{
		const double x = a_seconds / a_period + Unit(a_seed, 0x51ED);
		return x - std::floor(x);
	}

	// a beat a_up long rising and a_down falling from a_at seconds into the cycle: 0 .. 1
	[[nodiscard]] constexpr double Beat(double a_in, double a_at, double a_up, double a_down)
	{
		const double s = a_in - a_at;
		if (s < 0.0 || s >= a_up + a_down) {
			return 0.0;
		}
		return s < a_up ? s / a_up : 1.0 - (s - a_up) / a_down;
	}

	// a slow rise a_rise of the cycle long then a quicker fall: -1 .. 1
	[[nodiscard]] inline double Swell(double a_phase, double a_rise)
	{
		const double s = a_phase < a_rise ? a_phase / a_rise : 1.0 - (a_phase - a_rise) / (1.0 - a_rise);
		return (s * s * (3.0 - 2.0 * s)) * 2.0 - 1.0;
	}

	[[nodiscard]] inline double Heartbeat(double a_seconds, double a_period, std::uint32_t a_seed)
	{
		const double in = Phase(a_seconds, a_period, a_seed) * a_period;
		return 0.90 + 0.35 * Beat(in, 0.0, 0.06, 0.12) + 0.22 * Beat(in, 0.18, 0.06, 0.12);
	}

	// the shape alone, about 1 on average
	[[nodiscard]] inline double Shape(Kind a_kind, double a_seconds, std::uint32_t a_seed)
	{
		const double t = a_seconds;
		const auto   wave = [&](double a_period) { return std::sin(Phase(t, a_period, a_seed) * kTwoPi); };
		switch (a_kind) {
		case Kind::kFire:
			{
				// a restless base broken by short dips (blown) and now and then a flare
				const double base = 1.0 + 0.12 * (0.6 * Noise(t, 2.5, a_seed) + 0.4 * Noise(t, 7.0, a_seed + 1));
				const double dip = Event(t, 1.3, 0.8, 0.12, 0.35, 0.55, a_seed + 2);
				const double flare = Event(t, 3.0, 0.8, 0.20, 0.15, 0.25, a_seed + 3);
				return base * (1.0 - dip) * (1.0 + flare);
			}
		case Kind::kFrost:
			return 1.0 + 0.06 * Noise(t, 0.4, a_seed) + Event(t, 0.25, 0.9, 0.04, 0.08, 0.08, a_seed + 1);
		case Kind::kShock:
			{
				// steady a little low, sharp spikes, and rarely a brief cut like an arc gap
				const double spike = Event(t, 0.3, 0.5, 0.045, 0.30, 0.45, a_seed, true);
				const bool   cut = Event(t, 4.0, 0.9, 0.05, 1.0, 1.0, a_seed + 1, true) > 0.0;
				return cut ? 0.40 : 0.85 + spike;
			}
		case Kind::kHealing:
			return 1.0 + 0.10 * wave(4.0);
		case Kind::kAbsorbHealth:
			return Heartbeat(t, 1.0, a_seed);
		case Kind::kAbsorbMagicka:
			return Heartbeat(t, 1.2, a_seed);
		case Kind::kAbsorbStamina:
			return Heartbeat(t, 0.8, a_seed);
		case Kind::kPoison:
			{
				const double x = t / 1.6 + Unit(a_seed, 0x51ED);
				const auto   cycle = static_cast<std::int64_t>(std::floor(x));
				const double height = 0.15 * (0.6 + 0.4 * Unit(a_seed + 1, cycle));
				return 1.0 + height * Swell(x - std::floor(x), 0.7);
			}
		case Kind::kIllusion:
			return 1.0 + 0.06 * (std::sin(t * kTwoPi * 0.9 + Unit(a_seed, 1) * kTwoPi) + std::sin(t * kTwoPi * 1.1 + Unit(a_seed, 2) * kTwoPi));
		case Kind::kConjuration:
			return 1.0 + 0.18 * Swell(Phase(t, 3.0, a_seed), 2.2 / 3.0);
		case Kind::kAlteration:
			return 1.0 + 0.04 * wave(6.0);
		case Kind::kMagelight:
			return 1.0 + 0.06 * wave(3.0);
		case Kind::kWard:
			return 1.0 + 0.03 * wave(0.5);
		case Kind::kRune:
			return 0.95 + 0.15 * Swell(Phase(t, 2.5, a_seed), 0.2);
		case Kind::kNone:
		default:
			return 1.0;
		}
	}

	// the factor a light of kind a_kind and seed a_seed has a_seconds into the session, its swing scaled by a_depth (0 = steady,
	// 1 = the shape as designed; cloaks 0.5, enchanted weapons a third). Doubles throughout: a float clock loses its steps
	[[nodiscard]] inline float Factor(Kind a_kind, double a_seconds, std::uint32_t a_seed, float a_depth = 1.0f)
	{
		if (!std::isfinite(a_seconds) || !(a_depth > 0.0f)) {
			return 1.0f;
		}
		const double f = 1.0 + static_cast<double>(a_depth) * (Shape(a_kind, a_seconds, a_seed) - 1.0);
		return static_cast<float>(std::clamp(f, 0.05, 2.0));
	}
}
