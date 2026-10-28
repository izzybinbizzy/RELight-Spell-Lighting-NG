// BeamRow - where the row of small lights along a spray, stream or bolt sits, and how strong each one is
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// HIS ANSWER 2026-10-20 (checklist 140): one light that stretches along the beam first - and "if you plain out cannot make
// part 1 of 140 to happen then revert to this as a backup plan", the row of lights. One stretched light cannot be drawn by any
// lighting our mods run under (the game, ENB and Community Shaders draw single points only - his research page, part 8), so
// this is the row: lights no farther apart than ~70 units (3 at least), each reaching 2.2x the spacing but never under 140 or
// over 160 units, the two end ones halved for soft ends once there are 5 or more, every strength scaled so the ground under
// the middle of the beam glows as one light of the target strength would. The game tests of 2026-10-20 set those numbers: at
// ~125 apart reaching 260-440 the light lit the steps and walls beside her as well as the beam (17:11, 18:03); the scratch
// model for these: spots under 3 % along a long beam (under 7 % on a short one), nothing 200 units to the side.
// Worked out with the game's smooth falloff (1 - d / R)^2 and the ground's cosine (h / d), the research's `ripple vanilla.py`:
// 12 lights at 125 apart leave spots of 2-5 % and spill 120-230 units to the side.
// Game-free and byte-identical in Illuminated and RELight - Spell Lighting NG (`PC Runner\fade copies check.py`); tested off
// the game (tests\test_fade.cpp, BeamRowChecks).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Pulse.h"

namespace BeamRow
{
	struct Spot
	{
		float along{ 0.0f };     // 0 at the hand, 1 at the far end
		float strength{ 1.0f };  // times the target strength
		float reach{ 0.0f };     // in game units
	};

	inline constexpr float kSpacing = 70.0f;  // the farthest two lights may stand apart
	inline constexpr int   kFewest = 3;       // a beam cut short still gets this many (spaced closer)
	inline constexpr float kReachTimes = 2.2f;
	inline constexpr float kLeastReach = 140.0f;  // over a beam's usual height above the ground with room to spare: under that a light barely lands
	inline constexpr float kMostReach = 160.0f;   // the light stays this close to the beam
	inline constexpr float kEndScale = 0.5f;
	inline constexpr int   kHalveFrom = 5;  // fewer lights keep full ends: halving one of 3 or 4 dims a third of the beam

	inline constexpr float  kFlicker = 0.70f;   // how far a flickering light swings either side of its steady strength
	inline constexpr double kTogether = 0.85;   // the share of that swing every light of the row has in common (phases of neighbours cancel)
	inline constexpr float  kHeadroom = 0.55f;  // a capped light's steady strength, as a share of its cap
	static_assert(kHeadroom * (1.0f + kFlicker) <= 1.0f, "the flicker's peak must stay under the cap");

	// a cone that leaves a cloud (a wind spell's world-space particles): its one light sits this far along the cone's path, from
	// where it was launched to where it is now, and once the cone ends it stays there and fades out over kCloudLife
	inline constexpr float kCloudAlong = 0.5f;
	inline constexpr float kCloudLife = 3.0f;  // seconds; not measured yet - a game run of the cloud sets it

	// a row light's steady strength where its lighting caps it at a_top (Community Shaders: a light's strength for its reach):
	// a light held at its cap cannot swing up and burns white in the middle. No cap (a_top 0): what was asked
	[[nodiscard]] inline float Steady(float a_want, float a_top)
	{
		return a_top > 0.0f ? (std::min)(a_want, a_top * kHeadroom) : a_want;
	}

	// the strength factor light a_index of the row has a_seconds into the session while the Flicker switch is on: two waves shared
	// by the whole row plus two on the light's own phase, so the row flickers as one and still shimmers; always within 1 +- kFlicker.
	// Doubles throughout: a float clock loses its steps after hours of play
	[[nodiscard]] inline float Flicker(double a_seconds, std::size_t a_index)
	{
		const double phase = static_cast<double>(a_index) * 1.7;
		const double shared = 0.6 * std::sin(a_seconds * 43.0) + 0.4 * std::sin(a_seconds * 71.0 + 1.3);
		const double own = 0.6 * std::sin(a_seconds * 47.0 + phase) + 0.4 * std::sin(a_seconds * 83.0 + phase * 1.7);
		return 1.0f + kFlicker * static_cast<float>(kTogether * shared + (1.0 - kTogether) * own);
	}

	// the same factor under Realistic: a row whose spell has a kind (a_kind, Pulse.h) pulses in that kind's shape, as the hand
	// casting it does - kTogether of it on the row's own seed (a_seed, one per row) so the row gusts as one, the rest on each
	// light's, so it still shimmers. No kind: the Regular flicker. Within 1 +- kFlicker for every kind, so Steady's headroom holds
	[[nodiscard]] inline float Pulsing(Pulse::Kind a_kind, double a_seconds, std::size_t a_index, std::uint32_t a_seed)
	{
		if (a_kind == Pulse::Kind::kNone) {
			return Flicker(a_seconds, a_index);
		}
		const double shared = Pulse::Factor(a_kind, a_seconds, a_seed) - 1.0;
		const double own = Pulse::Factor(a_kind, a_seconds, a_seed + 1u + static_cast<std::uint32_t>(a_index)) - 1.0;
		return 1.0f + static_cast<float>(kTogether * shared + (1.0 - kTogether) * own);
	}

	// the share of a cloud light's strength a_sinceEnd seconds after its cone ended (below 0: the cone still flies), easing out
	// to 0 at a_life; 0 for a time that is not a number
	[[nodiscard]] inline float CloudFade(float a_sinceEnd, float a_life)
	{
		if (std::isnan(a_sinceEnd)) {
			return 0.0f;
		}
		if (a_sinceEnd < 0.0f) {
			return 1.0f;
		}
		if (!(a_life > 0.0f)) {
			return 0.0f;
		}
		const float t = (std::min)(a_sinceEnd / a_life, 1.0f);
		return 1.0f - t * t * (3.0f - 2.0f * t);
	}

	// the light a ground point at (a_x along the beam, a_side across it) gets from the row, the beam a_height above the
	// ground: the game's smooth falloff and the ground's cosine (the research's model)
	[[nodiscard]] inline float Ground(const std::vector<Spot>& a_row, float a_length, float a_x, float a_side, float a_height)
	{
		float total = 0.0f;
		for (const auto& s : a_row) {
			const float dx = a_x - s.along * a_length;
			const float d = std::sqrt(dx * dx + a_side * a_side + a_height * a_height);
			if (d > 0.0f && d < s.reach) {
				const float f = 1.0f - d / s.reach;
				total += s.strength * f * f * a_height / d;
			}
		}
		return total;
	}

	// the row for a beam a_length long, at most a_most lights (a scene limit, or 4 on a setup that lets an object take only a
	// few lights): evenly spaced, the ends halved, then all scaled so the middle of the beam, a_height above the ground, glows
	// as one light of strength 1 and reach a_reach would right under it. An empty row for a beam with no length.
	[[nodiscard]] inline std::vector<Spot> Lay(float a_length, int a_most, float a_reach, float a_height)
	{
		std::vector<Spot> row;
		if (!(a_length > 1.0f) || !std::isfinite(a_length) || a_most < 1 || !(a_reach > 0.0f)) {
			return row;
		}
		// clamped in double before it becomes an int: a mod's range can be any float, and a huge one must not overflow it
		const double needed = (std::min)(std::ceil(static_cast<double>(a_length) / kSpacing), static_cast<double>(a_most));
		const int    n = std::clamp((std::max)(static_cast<int>(needed), kFewest), 1, a_most);
		const float  spacing = a_length / static_cast<float>(n);
		const float  reach = std::clamp(kReachTimes * spacing, kLeastReach, kMostReach);
		row.reserve(static_cast<std::size_t>(n));
		for (int i = 0; i < n; ++i) {
			const bool end = n >= kHalveFrom && (i == 0 || i == n - 1);
			row.push_back({ (static_cast<float>(i) + 0.5f) / static_cast<float>(n), end ? kEndScale : 1.0f, reach });
		}
		const std::vector<Spot> one{ { 0.5f, 1.0f, a_reach } };
		const float             want = Ground(one, a_length, a_length * 0.5f, 0.0f, a_height);
		const float             got = Ground(row, a_length, a_length * 0.5f, 0.0f, a_height);
		if (got > 0.0f && want > 0.0f) {
			for (auto& s : row) {
				s.strength *= want / got;
			}
		}
		return row;
	}
}
