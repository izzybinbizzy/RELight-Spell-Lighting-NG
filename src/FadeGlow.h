// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// The light's behaviour as plain numbers: no game types, so tests/test_fade.cpp builds and runs it on any compiler.
// A hand's charge fraction goes in; a brightness multiplier, a reach multiplier and a colour blend come out.
//
//   brightness = floor + (1 - floor) * curve(shown)      shown eases toward the real fraction
//              * sputter                                 below the threshold, stepped dips (not a smooth wave)
//              * (1 + pulse)                             a hit that spent charge
//              * (1 + flare)                             a soul gem that refilled it
//   colour     = lerp(base, cool target, amount)         below the threshold, toward grey or an ember tint

#pragma once

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstdint>
#include <numbers>

namespace Glow
{
	enum class Curve : int
	{
		kLinear = 0,
		kGentle = 1  // stays bright longer, drops near the end
	};

	enum class CoolTint : int
	{
		kGrey = 0,
		kEmber = 1
	};

	struct Tuning
	{
		float floor{ 0.10f };  // brightness left at 0% charge
		Curve curve{ Curve::kGentle };
		float reachFollows{ 0.5f };  // 0: reach never changes, 1: reach follows the brightness curve fully
		float minReach{ 0.35f };     // reach never drops below this share of the light's own
		bool  sputter{ true };
		float sputterBelow{ 0.15f };    // fraction under which the light sputters
		float sputterStrength{ 0.6f };  // deepest dip, at the bottom of the sputter band
		bool  emptySteady{ true };      // at exactly 0 the light holds still at the floor
		bool  pulse{ true };
		float pulseStrength{ 0.6f };
		float pulseSeconds{ 0.30f };
		bool  flare{ true };
		float flareStrength{ 0.8f };
		float flareSeconds{ 0.70f };
		bool  cool{ true };
		float coolAmount{ 0.5f };    // toward grey, or ember for an orange fire light (TintFor)
		float fallSeconds{ 0.15f };  // how fast the shown level follows a drop
		float riseSeconds{ 0.35f };  // and a rise

		bool operator==(const Tuning&) const = default;
	};

	[[nodiscard]] inline float Clamp01(float a_v) noexcept { return std::clamp(a_v, 0.0f, 1.0f); }

	// how big the HUD elements draw on a screen this many lines tall: 1 at 1080, held between 0.6 and 2.5. The charge gems
	// take it (his answer 2026-10-13: the HUD grows with the screen - 1440p and 4K no smaller against the screen)
	[[nodiscard]] inline float ScreenScale(float a_lines) noexcept
	{
		return std::isfinite(a_lines) ? std::clamp(a_lines / 1080.0f, 0.6f, 2.5f) : 1.0f;
	}

	[[nodiscard]] inline float ApplyCurve(Curve a_curve, float a_f) noexcept
	{
		a_f = Clamp01(a_f);
		return a_curve == Curve::kGentle ? 1.0f - (1.0f - a_f) * (1.0f - a_f) : a_f;
	}

	// the steady brightness a fraction gives, before sputter, pulse and flare
	[[nodiscard]] inline float Level(const Tuning& a_t, float a_fraction) noexcept
	{
		const float floor = Clamp01(a_t.floor);
		return floor + (1.0f - floor) * ApplyCurve(a_t.curve, a_fraction);
	}

	// reach follows the level by `reachFollows`, never under `minReach`
	[[nodiscard]] inline float Reach(const Tuning& a_t, float a_level) noexcept
	{
		const float r = 1.0f + (Clamp01(a_level) - 1.0f) * Clamp01(a_t.reachFollows);
		return std::clamp(r, Clamp01(a_t.minReach), 1.0f);
	}

	// how far below the sputter threshold a fraction sits: 0 at the threshold, 1 at empty
	[[nodiscard]] inline float Depth(const Tuning& a_t, float a_fraction) noexcept
	{
		if (a_t.sputterBelow <= 0.0f || a_fraction >= a_t.sputterBelow) {
			return 0.0f;
		}
		return Clamp01((a_t.sputterBelow - a_fraction) / a_t.sputterBelow);
	}

	// a small deterministic generator, so a hand's sputter needs no shared state and tests are repeatable
	struct Rng
	{
		std::uint32_t       state{ 0x9E3779B9u };
		[[nodiscard]] float Next() noexcept  // [0, 1)
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
		}
	};

	// Everything one hand remembers from frame to frame.
	struct Hand
	{
		float shown{ -1.0f };  // the eased fraction; < 0 until the first frame
		float lastFraction{ -1.0f };
		float pulseAge{ 1e9f };
		float pulseSize{ 0.0f };
		float flareAge{ 1e9f };
		float fallRate{ 0.0f };  // the fastest the charge has fallen lately, per second (let go over kFallRateSeconds)
		float maxFall{ 0.0f };   // the largest single fall lately (let go the same way)
		float lastDt{ 0.0f };    // the frame before this one, in seconds
		float lastDt2{ 0.0f };   // and the one before that
		// sputter: time into the current step, how long it lasts, the level it holds
		float stepAge{ 0.0f }, stepLength{ 0.0f }, stepLevel{ 1.0f };
		Rng   rng{};

		// the menu's preview buttons: the same pulse a spending hit starts, the same flare a refill starts
		void TriggerPulse(const Tuning& a_t) noexcept
		{
			pulseAge = 0.0f;
			pulseSize = a_t.pulseStrength * (std::max)(shown < 0.0f ? 1.0f : shown, 0.25f);
		}
		void TriggerFlare() noexcept { flareAge = 0.0f; }

		// a new weapon (or none): no pulse, no flare, the level jumps straight to the new fraction
		void Reset(float a_fraction, std::uint32_t a_seed) noexcept
		{
			shown = a_fraction;
			lastFraction = a_fraction;
			pulseAge = flareAge = 1e9f;
			fallRate = maxFall = lastDt = lastDt2 = 0.0f;
			pulseSize = 0.0f;
			stepAge = stepLength = 0.0f;
			stepLevel = 1.0f;
			rng.state = a_seed ? a_seed : 0x9E3779B9u;
		}
	};

	struct Output
	{
		float brightness{ 1.0f };  // multiplies the light's fade
		float reach{ 1.0f };       // multiplies its radius
		float cool{ 0.0f };        // 0..1 blend toward the cool colour
		bool  pulsed{ false };     // this frame started a pulse
		bool  flared{ false };     // this frame started a flare
	};

	// charge that fell by more than this counts as spent on a hit; rose by more, as a recharge
	inline constexpr float kSpendStep = 0.0005f;
	inline constexpr float kRefillStep = 0.01f;
	// A hit is a fall that stands out. A charge drained steadily (a concentration staff, a mod that drains charge over time)
	// falls every frame, by more on a long frame, a frame late after a hitch (when the game spends it after our read), or
	// in steps a few times a second, on its own clock; none of that is a hit. So a fall past kSpendStep is a hit only when
	// it is kHitRatio times both
	//   - the largest single fall lately, and
	//   - what the recent drain rate takes over this frame and the one before (a fall may be a frame late),
	// both let go over kFallRateSeconds (the rate faster while the charge holds still). A steady drain is one pulse when it starts; a hit while a drain runs still stands
	// out when hits are well apart (0.8 s is tested). What cannot be told apart by the numbers: two like hits within about
	// a third of a second are one pulse (the first raises the bar) - within about half a second while a drain runs, as
	// the drain keeps the first hit's rate from being let go quickly; a drain in steps further apart than about a third of
	// a second pulses on each step; and a hitch that swallows two or more of a stepped drain's steps can read as one hit.
	inline constexpr float kHitRatio = 4.0f;
	inline constexpr float kFallRateSeconds = 0.25f;  // how long the recent drain is remembered (a hit counts too)
	inline constexpr float kStillSeconds = 0.05f;     // how fast the drain rate is let go while the charge holds still

	[[nodiscard]] inline float Ease(float a_from, float a_to, float a_dt, float a_seconds) noexcept
	{
		if (a_seconds <= 0.0f) {
			return a_to;
		}
		const float k = 1.0f - std::exp(-a_dt / a_seconds);
		return a_from + (a_to - a_from) * k;
	}

	// One frame of one hand. `a_fraction` is the charge fraction (0..1), `a_dt` the frame time in seconds. `a_timed`: the
	// fraction is a clock running down (a bound weapon's spell), not a charge - it falls every frame, and a fall is not a
	// hit, nor a rise a refill, so it never pulses or flares.
	[[nodiscard]] inline Output Step(const Tuning& a_t, Hand& a_h, float a_fraction, float a_dt, bool a_timed = false) noexcept
	{
		Output out;
		// a NaN would pass through std::clamp and stay in the hand's eased level for good: a fraction the game could not
		// give counts as full (no dimming), a frame time it could not give as no time
		a_fraction = std::isfinite(a_fraction) ? Clamp01(a_fraction) : 1.0f;
		// the frame's real length for the drain's rate (up to 5 s: a hitch's fall is the hitch's), a quarter second at most
		// for the easing and the effects, so one long frame does not jump them
		const float realDt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 5.0f) : 0.0f;
		a_dt = (std::min)(realDt, 0.25f);
		if (a_h.shown < 0.0f) {
			a_h.Reset(a_fraction, a_h.rng.state);
		}

		// what changed since last frame
		const float delta = a_fraction - a_h.lastFraction;
		// let go by time, but across a hitch (a frame more than twice the longer of the two before) by no more than that
		// one, and never more than 1/30 s: a hitch is not a pause in the drain, and what it owes comes due on the frame
		// after. The longer of two, so frames alternating one and two intervals (vsync, a missed deadline) are not hitches;
		// a steady low frame rate is let go in real time
		const float recent = (std::max)(a_h.lastDt, a_h.lastDt2);
		const bool  hitch = recent > 0.0f && realDt > 2.0f * recent;
		const float step = hitch ? (std::min)(recent, 1.0f / 30.0f) : realDt;
		a_h.maxFall *= std::exp(-step / kFallRateSeconds);
		// the rate is a drain's: it holds while the charge keeps falling, and is let go quickly on a frame where it does
		// not (hits come with still frames between them, a drain does not)
		a_h.fallRate *= std::exp(-step / (delta < 0.0f ? kFallRateSeconds : kStillSeconds));
		if (a_timed) {
			// a clock: no moments
		} else if (delta < -kSpendStep) {
			const float usual = (std::max)(a_h.maxFall, a_h.fallRate * (realDt + a_h.lastDt));
			if (a_t.pulse && -delta > kHitRatio * usual + kSpendStep) {
				a_h.pulseAge = 0.0f;
				a_h.pulseSize = a_t.pulseStrength * (std::max)(a_h.shown, 0.25f);  // a near-empty hit still shows
				out.pulsed = true;
			}
		} else if (delta > kRefillStep && a_t.flare) {
			a_h.flareAge = 0.0f;
			out.flared = true;
		}
		a_h.lastFraction = a_fraction;
		// every fall raises the largest fall and the rate at once (so the same fall again, a drain's second frame, is not a
		// hit). A clock's fall is neither
		if (delta < 0.0f && !a_timed) {
			a_h.maxFall = (std::max)(a_h.maxFall, -delta);
			a_h.fallRate = (std::max)(a_h.fallRate, -delta / (std::max)(realDt, 0.001f));
		}
		a_h.lastDt2 = a_h.lastDt;
		a_h.lastDt = realDt;

		a_h.shown = Ease(a_h.shown, a_fraction, a_dt, a_fraction < a_h.shown ? a_t.fallSeconds : a_t.riseSeconds);
		const float shown = a_h.shown;

		float level = Level(a_t, shown);

		// sputter: stepped dips, deeper the closer to empty; empty itself holds still if emptySteady
		const float depth = a_t.sputter ? Depth(a_t, shown) : 0.0f;
		const bool  empty = shown <= 0.001f;
		if (depth > 0.0f && !(empty && a_t.emptySteady)) {
			a_h.stepAge += a_dt;
			if (a_h.stepAge >= a_h.stepLength) {
				a_h.stepAge = 0.0f;
				// short, irregular steps: most near full, some deep - a failing enchantment, not a breathing one
				a_h.stepLength = 0.04f + 0.14f * a_h.rng.Next();
				const float roll = a_h.rng.Next();
				const float dip = roll < 0.35f ? (0.55f + 0.45f * a_h.rng.Next()) : 0.15f * a_h.rng.Next();
				a_h.stepLevel = 1.0f - Clamp01(a_t.sputterStrength) * depth * dip;
			}
			level *= a_h.stepLevel;
		} else {
			a_h.stepLevel = 1.0f;
			a_h.stepAge = a_h.stepLength = 0.0f;
		}

		// pulse: a quick spike that falls off (square of the time left)
		float boost = 0.0f;
		if (a_h.pulseAge < a_t.pulseSeconds) {
			const float left = 1.0f - a_h.pulseAge / a_t.pulseSeconds;
			boost += a_h.pulseSize * left * left;
			a_h.pulseAge += a_dt;
		}
		// flare: a swell and settle (half a sine)
		if (a_h.flareAge < a_t.flareSeconds) {
			boost += a_t.flareStrength * std::sin(std::numbers::pi_v<float> * a_h.flareAge / a_t.flareSeconds);
			a_h.flareAge += a_dt;
		}

		out.brightness = level * (1.0f + boost);
		out.reach = Reach(a_t, Level(a_t, shown));  // reach ignores the flicker and spikes: only the steady level
		out.cool = a_t.cool ? Clamp01(a_t.coolAmount) * Depth(Tuning{ .sputterBelow = a_t.sputterBelow }, shown) : 0.0f;
		return out;
	}

	// Bound weapons have no charge: the fraction is the time the spell has left, full until the last `a_fadeSeconds`.
	[[nodiscard]] inline float BoundFraction(float a_elapsed, float a_duration, float a_fadeSeconds) noexcept
	{
		if (a_duration <= 0.0f) {
			return 1.0f;  // no duration: it lasts until dispelled
		}
		const float left = a_duration - a_elapsed;
		if (a_fadeSeconds <= 0.0f) {
			return left > 0.0f ? 1.0f : 0.0f;
		}
		return Clamp01(left / a_fadeSeconds);
	}

	// What a hand's light follows, and whether it is a clock (see Step's a_timed). A bound weapon a rule treats as bound
	// follows its spell's time left, over the rule's fade window (a clock); a bound weapon a rule puts on charge has none,
	// so it stays full; anything else follows its charge.
	struct Follow
	{
		float fraction{ 1.0f };
		bool  timed{ false };
	};
	[[nodiscard]] inline Follow FollowOf(bool a_bound, bool a_boundRule, float a_current, float a_max, float a_fadeSeconds,
		float a_charge) noexcept
	{
		if (!a_bound) {
			return { a_charge, false };
		}
		if (!a_boundRule) {
			return { 1.0f, false };
		}
		return { BoundFraction(a_max - a_current, a_max, a_fadeSeconds), true };
	}

	// What the HUD's charge gem shows for a hand: the light's fraction (`a_light`: its charge, or the preview) - except a bound
	// weapon's, which counts its spell's WHOLE time down while its light holds full until the last seconds (his report
	// 2026-10-16: "no gem on bound sword"). A bound weapon with no duration (it lasts until dispelled) shows the light's value
	[[nodiscard]] inline float HudFraction(bool a_bound, bool a_preview, float a_current, float a_max, float a_light) noexcept
	{
		if (a_preview || !a_bound || !(a_max > 0.0f)) {
			return a_light;
		}
		return Clamp01(a_current / a_max);
	}

	struct Rgb
	{
		float r{ 1.0f }, g{ 1.0f }, b{ 1.0f };
	};

	// the colour a light cools toward: its own brightness in grey, or that brightness in a dull ember
	[[nodiscard]] inline Rgb CoolTarget(const Rgb& a_c, CoolTint a_tint) noexcept
	{
		const float luma = 0.2126f * a_c.r + 0.7152f * a_c.g + 0.0722f * a_c.b;
		if (a_tint == CoolTint::kGrey) {
			return { luma, luma, luma };
		}
		return { std::min(luma * 1.45f, 1.0f), luma * 0.62f, luma * 0.30f };
	}

	[[nodiscard]] inline Rgb Cool(const Rgb& a_c, CoolTint a_tint, float a_amount) noexcept
	{
		const Rgb   t = CoolTarget(a_c, a_tint);
		const float k = Clamp01(a_amount);
		return { a_c.r + (t.r - a_c.r) * k, a_c.g + (t.g - a_c.g) * k, a_c.b + (t.b - a_c.b) * k };
	}

	// an orange light (any scale): its hue between 10 and 50 degrees, and not washed out
	[[nodiscard]] inline bool IsOrange(const Rgb& a_c) noexcept
	{
		const float top = (std::max)({ a_c.r, a_c.g, a_c.b }), low = (std::min)({ a_c.r, a_c.g, a_c.b });
		if (!(top > 0.0f) || !std::isfinite(top + low) || (top - low) / top < 0.35f || a_c.r < top) {
			return false;
		}
		const float hue = 60.0f * (a_c.g - a_c.b) / (top - low);  // red is the top channel: -60..60
		return hue >= 10.0f && hue <= 50.0f;
	}

	// HIS WORD: "remove cools toward option in color cooling, the only option will be grey, ember cooling only for
	// fire spell types that are orange" - a light cools toward grey, unless its magic is fire AND the light itself is orange
	[[nodiscard]] inline CoolTint TintFor(bool a_fire, const Rgb& a_light) noexcept
	{
		return a_fire && IsOrange(a_light) ? CoolTint::kEmber : CoolTint::kGrey;
	}

	// A scaler that multiplies a value some other code sets (Light Placer rewrites a flickering light's fade every
	// frame, and leaves a steady one alone). Whatever the value holds that we did not write is the new base; we write
	// base * factor. Calling it twice in one frame writes the same number: it never compounds with itself.
	//
	// It CAN compound with another plugin that scales the same value the same way: each sees the other's write as a new
	// base, so the base shrinks (or grows) by the same ratio every frame, toward black or white. Over a few dozen frames a
	// light's own smooth animation (a fade-in, a slow pulse) looks just like that, so a look at the numbers alone cannot
	// tell them apart. Cause and effect can: while a loop is suspected, what we write carries a tiny random dither
	// (0.5%, far below what the eye sees). In a loop, what we read back next frame is our own write times the other
	// plugin's factor, so it follows our write exactly; a value the owner wrote pays no attention to it. So:
	//   - a base that moves by one steady ratio for 30 frames starts a probe, which needs 24 frames where the read-back
	//     followed our dither (a vote comes only on a frame where the dither changed sign: about 48 frames, under 1 s at
	//     60 fps);
	//   - the probe finds the loop: the base is frozen at its value before the drift, and the dither goes on, checking
	//     every frame; the moment the read-back stops following our writes (the loop ended, or it was never one), the
	//     freeze ends and the value read is the base again;
	//   - the probe finds the owner's own animation: nothing is frozen, and no probe runs again for 3 seconds.
	// Values may be negative (a darkness light's fade).
	struct Scaled
	{
		enum class Watch : std::uint8_t
		{
			kFollow,  // the usual case: the value read is the base
			kProbe,   // a loop is suspected: dithering, still following
			kFrozen   // a loop: the base is held, and dithering still checks it
		};

		float base{ 0.0f };
		float written{ 0.0f };  // what we wrote last (meaningful once `touched`)
		bool  touched{ false };
		Watch watch{ Watch::kFollow };
		bool  frozen{ false };  // watch == kFrozen, kept as a plain flag for the menu and the tests

		static constexpr int   kDriftFrames = 30;
		static constexpr int   kProbeFrames = 24;
		static constexpr int   kCooldownFrames = 180;
		static constexpr float kDither = 0.005f;  // half a percent, at frame rate: far below what the eye sees

		[[nodiscard]] float Apply(float a_current, float a_factor) noexcept
		{
			if (!touched) {
				base = a_current;
			} else if (a_current != written) {
				NewRead(a_current);
			}
			const float dither = watch == Watch::kFollow ? 1.0f : 1.0f + kDither * sign;
			const float v = base * a_factor * dither;
			written = v;
			touched = true;
			return v;
		}

		// the base Apply would go on to use if it were given a_current now, in every watch state (ReachCutoffFactor needs it before the write)
		[[nodiscard]] float BaseFor(float a_current) const noexcept
		{
			Scaled next = *this;
			(void)next.Apply(a_current, 1.0f);
			return next.base;
		}

		[[nodiscard]] bool Written() const noexcept { return touched; }

		// what to put back when we let go: our base if the value still holds our write, else leave it
		[[nodiscard]] float Restore(float a_current) const noexcept { return touched && a_current == written ? base : a_current; }

		// the dither's signs from this seed instead of the scaler's own address (the tests run many seeds; the game's lights each
		// keep their address's)
		void Seed(std::uint32_t a_seed) noexcept
		{
			rng = a_seed | 1u;
			seeded = true;
		}

	private:
		// someone else wrote the value since our last write (usually once a frame; twice if its owner writes it in two passes, which only makes the probe decide sooner)
		void NewRead(float a_current) noexcept
		{
			if (watch != Watch::kFollow) {
				Vote(a_current);
				prevSign = sign;
				sign = NextSign();
			}
			follow = a_current;  // what came back this time, for the next frozen vote
			switch (watch) {
			case Watch::kFollow:
				{
					cooldown = cooldown > 0 ? cooldown - 1 : 0;
					const float ratio = base != 0.0f ? a_current / base : 1.0f;
					const bool  moving = std::abs(ratio - 1.0f) > 0.005f;
					// a loop's ratio is the same to the last digit frame after frame; a smooth animation's creeps
					const bool steady = moving && std::abs(ratio - lastRatio) < 0.002f * std::abs(lastRatio);
					if (!steady || drift == 0) {
						driftFrom = base;
					}
					drift = steady ? drift + 1 : (moving ? 1 : 0);
					lastRatio = ratio;
					base = a_current;
					if (drift >= kDriftFrames && cooldown == 0) {
						Enter(Watch::kProbe);
					}
					break;
				}
			case Watch::kProbe:
				base = a_current;
				if (OwnerSeen()) {
					Leave(a_current);  // the owner's own animation
				} else if (votes >= kProbeFrames) {
					// a loop never echoes our dither (measured: 0 echoes in 60,000 freezes); an owner's own fast fade echoes a few
					// times even when it is too quick to show it on most frames - it is let go, never pinned
					if (std::popcount(ownerVotes & 0xFFFFFFu) <= 1) {
						Enter(Watch::kFrozen);
						base = driftFrom;  // another scaler feeds our output back to us: keep the base from before the loop
					} else {
						Leave(a_current);
					}
				}
				break;
			case Watch::kFrozen:
				if (OwnerSeen()) {
					Leave(a_current);  // the read-back stopped following our writes: the value read is the owner's
				}
				break;
			}
		}

		// One frame's vote. x = log(what came back / what we wrote). If the owner wrote it, x carries our dither inverted,
		// so its change from last frame is about -dither * (sign - previous sign); if another scaler multiplied our write,
		// x does not follow our signs at all (it is steady, or carries that scaler's own random dither). A vote counts only
		// on a frame our sign flipped.
		void Vote(float a_current) noexcept
		{
			if (written == 0.0f || a_current == 0.0f || (a_current > 0.0f) != (written > 0.0f)) {
				haveX = false;
				return;
			}
			// frozen, our write is the held base's: what comes back is measured against it moved as the owner's value moved since,
			// so an owner that keeps fading faster than the dither still echoes it and is let go (it never would otherwise)
			const float ref = watch == Watch::kFrozen && base != 0.0f && follow != 0.0f && (follow > 0.0f) == (base > 0.0f) ?
			                      written * (follow / base) :
			                      written;
			const float x = std::log(a_current / ref);
			if (haveX && sign != prevSign) {
				const float want = -kDither * (sign - prevSign);  // what the owner's value would show: +-2 dither
				const float d = x - lastX;
				const bool  echo = std::abs(d - want) < 0.5f * std::abs(want);
				ownerVotes = (ownerVotes << 1) | (echo ? 1u : 0u);
				++votes;
			}
			lastX = x;
			haveX = true;
		}

		// most of the last 8 votes saw our dither come back inverted: the value is the owner's, not a loop's
		[[nodiscard]] bool OwnerSeen() const noexcept { return votes >= 4 && std::popcount(ownerVotes & 0xFFu) >= (votes >= 8 ? 6 : 4); }

		void Enter(Watch a_watch) noexcept
		{
			if (a_watch == Watch::kProbe) {
				votes = 0;
				ownerVotes = 0;
				haveX = false;
				if (!seeded) {
					seeded = true;
					rng = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this) * 2654435761u) | 1u;  // each its own signs
				}
			}
			watch = a_watch;
			frozen = a_watch == Watch::kFrozen;
		}

		void Leave(float a_current) noexcept
		{
			watch = Watch::kFollow;
			frozen = false;
			base = a_current;
			drift = 0;
			lastRatio = 1.0f;
			cooldown = kCooldownFrames;
		}

		float NextSign() noexcept
		{
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return (rng & 1u) ? 1.0f : -1.0f;
		}

		float         driftFrom{ 0.0f };  // the base when the current run of steady drift began
		float         lastRatio{ 1.0f };
		int           drift{ 0 };  // consecutive frames the base moved by about the same ratio
		int           cooldown{ 0 }, votes{ 0 };
		std::uint32_t ownerVotes{ 0 };                 // the latest votes, a bit each, newest lowest: 1 = the owner's value
		float         sign{ 1.0f }, prevSign{ 1.0f };  // this frame's dither sign, and last frame's
		float         lastX{ 0.0f };
		float         follow{ 0.0f };  // the value read last time (a frozen vote measures against it)
		bool          haveX{ false };
		std::uint32_t rng{ 1u };
		bool          seeded{ false };
	};
}
