// LightMath - the plain-number part of LightKit (Illuminated and RELight - Spell Lighting NG both use it)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Nothing of the game in here, so tests/test_fade.cpp builds and checks it off the game (PC Runner\fade tests.py, against
// both mods' copies). LightKit.h includes it; the game-facing helpers stay there. Kept byte for byte the same in both mods
// by PC Runner\fade copies check.py, as LightKit.h is.
//
//   Lower, SameText    the one lower-casing helper and the one case-blind compare every file uses
//   StringMap          std::string keys looked up by std::string_view without a temporary string
//   CutoffFor          the cutoff of an inverse-square light (the build's K, as gen.py and relightgen.py write every config)
//   IslReach, PlainOf  how far such a light reaches, and the same light drawn by the game's own lighting (ENB, Vanilla);
//                      PlainAt, the same from a reach already worked out; LightPlacerSize, a record's size as Light Placer reads it;
//                      NeverDimmer, the fade a raised light keeps so it is never dimmer than the light it was raised from
//   ToScreen           linear light -> the screen's color (Dynamic Wards' wardgen.srgb); ToLinear the way back
//   DaylightShare      how bright the day or the room is, for "Dim in daylight"
//   KeyAt              a fade controller's value at a time (Truman's KeyframeSequence maths, RE::Light's controller.h)
//   kFleshLooks        the five flesh spells' light colors when FleshFX's look wins on them; AtStrengthOf, a color carried
//                      at another light's strength
//   ChromaMean         an art's color off its effect shaders' emissive colors (an automatic hand light's third source)

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LightKit
{
	// std::string keys looked up by std::string_view without a temporary string
	struct StringHash
	{
		using is_transparent = void;
		[[nodiscard]] std::size_t operator()(std::string_view a_text) const noexcept { return std::hash<std::string_view>{}(a_text); }
	};
	template <class T>
	using StringMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

	// a lower-case copy (ASCII: editor IDs, keys, file names)
	[[nodiscard]] inline std::string Lower(std::string_view a_text)
	{
		std::string out(a_text);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	// the same text whatever its case
	[[nodiscard]] inline bool SameText(std::string_view a, std::string_view b) noexcept
	{
		return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}

	// The build's cutoff constant (0.8 * 69.99², as gen.py and relightgen.py write every config's cutoff) and the size a
	// light made here carries in its radius' z
	inline constexpr float kK = 3918.88f;
	inline constexpr float kLightSize = 1.414f;

	namespace Isl
	{
		inline constexpr std::uint32_t kFlag = 1u << 10;
		inline constexpr std::uint32_t kInitialised = 1u << 8;  // Light Limit Fix marks every light it makes so (LightFlags::Initialised)
		inline constexpr float         kLowestCutoff = 0.01f, kHighestCutoff = 0.99f;
	}

	// cutoff = K * fade / (reach² + size²), kept where Community Shaders reads a cutoff
	[[nodiscard]] inline float CutoffFor(float a_fade, float a_reach, float a_size = kLightSize) noexcept
	{
		return std::clamp(kK * a_fade / (a_reach * a_reach + a_size * a_size), Isl::kLowestCutoff, Isl::kHighestCutoff);
	}

	// How far an inverse-square light reaches (reach² = K x fade / cutoff - size²), and the same light drawn by the game's own
	// lighting (ENB, Vanilla): Dynamic Wards' house light - LTBG section 4's reach 133 drawn at radius 178 with fade 1.14
	// (wardgen.plain_light), never shorter than the radius the light already states. The one copy of this maths in both mods.
	[[nodiscard]] inline float IslReach(float a_fade, float a_cutoff, float a_size = kLightSize) noexcept
	{
		if (a_cutoff <= 0.0f || a_fade <= 0.0f) {
			return 0.0f;
		}
		return std::sqrt((std::max)(kK * a_fade / a_cutoff - a_size * a_size, 0.0f));
	}
	struct Plain
	{
		float fade{ 0.0f }, radius{ 0.0f };
	};
	// the plain light of a reach already worked out (a light that states its reach, not its cutoff)
	[[nodiscard]] inline Plain PlainAt(float a_fade, float a_radius, float a_reach) noexcept
	{
		constexpr float kPlainReach = 178.0f / 133.0f, kPlainFade = 1.14f;
		return { a_fade * kPlainFade, (std::max)(a_radius, a_reach * kPlainReach) };
	}
	[[nodiscard]] inline Plain PlainOf(float a_fade, float a_radius, float a_cutoff, float a_size = kLightSize) noexcept
	{
		return PlainAt(a_fade, a_radius, IslReach(a_fade, a_cutoff, a_size));
	}

	// the size an inverse-square light record carries in its field of view, as Light Placer reads it (its GetSize): 50 or
	// more is a real field of view, not a size, so the house size
	[[nodiscard]] inline float LightPlacerSize(float a_fov) noexcept
	{
		return a_fov >= 50.0f ? kLightSize : std::clamp(a_fov, 0.01f, 50.0f);
	}

	// the fade a raised light takes so it is never dimmer at its source than the light it was raised from (his answer
	// 2026-10-13: a bolt's raised light is never dimmer than its own original). An inverse-square light's peak is
	// fade x K / size², so the original's fade is first carried to the raised light's size; a_ownSize 0 (a light not drawn
	// inverse square) compares the two fades as they are
	[[nodiscard]] inline float NeverDimmer(float a_raisedFade, float a_raisedSize, float a_ownFade, float a_ownSize) noexcept
	{
		if (!std::isfinite(a_ownFade) || a_ownFade <= 0.0f) {
			return a_raisedFade;
		}
		const float own = a_ownSize > 0.0f && a_raisedSize > 0.0f ? a_ownFade * (a_raisedSize * a_raisedSize) / (a_ownSize * a_ownSize) : a_ownFade;
		return (std::max)(a_raisedFade, own);
	}

	// linear light -> the screen's color, per channel (0..1)
	[[nodiscard]] inline float ToScreen(float a_x) noexcept
	{
		const float x = std::clamp(a_x, 0.0f, 1.0f);
		return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
	}
	// the screen's color -> linear light, per channel (0..1): ToScreen's way back
	[[nodiscard]] inline float ToLinear(float a_x) noexcept
	{
		const float x = std::clamp(a_x, 0.0f, 1.0f);
		return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
	}
	// the same for one 0..255 channel
	[[nodiscard]] inline std::uint8_t ScreenChannel(std::uint8_t a_linear) noexcept
	{
		const float v = ToScreen(static_cast<float>(a_linear) / 255.0f);
		return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
	}
	// ToLinear for one 0..255 channel
	[[nodiscard]] inline std::uint8_t LinearChannel(std::uint8_t a_screen) noexcept
	{
		const float v = ToLinear(static_cast<float>(a_screen) / 255.0f);
		return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
	}

	// "Dim in daylight": how bright it is, 0 (night, a dark room) to 1 (full day, a bright room). Outdoors it follows the
	// hour (full day 8 to 17, a three-hour dawn and dusk); indoors the room's own light, as the luminance (0..1) of its
	// ambient or directional color, whichever is brighter.
	[[nodiscard]] inline float DaylightOfHour(float a_hour) noexcept
	{
		if (a_hour < 5.0f || a_hour >= 20.0f) {
			return 0.0f;
		}
		if (a_hour < 8.0f) {
			return (a_hour - 5.0f) / 3.0f;
		}
		if (a_hour < 17.0f) {
			return 1.0f;
		}
		return (20.0f - a_hour) / 3.0f;
	}
	[[nodiscard]] inline float DaylightOfRoom(float a_luminance) noexcept
	{
		return std::clamp((a_luminance - 0.15f) / 0.3f, 0.0f, 1.0f);
	}
	// the share the lights keep: a_pick 0 off, 1 a little (at most a quarter dimmer), 2 more (at most half)
	[[nodiscard]] inline float DaylightShare(int a_pick, float a_bright) noexcept
	{
		if (a_pick <= 0) {
			return 1.0f;
		}
		const float most = a_pick == 1 ? 0.25f : 0.5f;
		return 1.0f - most * std::clamp(a_bright, 0.0f, 1.0f);
	}

	// A fade controller's value at a_time (inside its loop): Truman's KeyframeSequence::Interpolate (RE::Light,
	// controller.h, GPL-3.0; his permission for RELight's code, credited) - step (0), linear (1) or cubic Hermite (2; the
	// configs give no tangents, so they are 0). a_keys: (time, value), sorted by time; empty gives 0.
	[[nodiscard]] inline float KeyAt(const std::vector<std::pair<float, float>>& a_keys, int a_interpolation, float a_time) noexcept
	{
		if (a_keys.empty()) {
			return 0.0f;
		}
		const auto next = std::ranges::upper_bound(a_keys, a_time, {}, &std::pair<float, float>::first);
		if (next == a_keys.begin()) {
			return a_keys.front().second;
		}
		if (next == a_keys.end()) {
			return a_keys.back().second;
		}
		const auto& [t0, v0] = *std::prev(next);
		const auto& [t1, v1] = *next;
		const float dt = t1 - t0;
		if (dt <= 0.0f || a_interpolation == 0) {
			return v0;
		}
		const float u = (a_time - t0) / dt;
		if (a_interpolation == 1) {
			return (1.0f - u) * v0 + u * v1;
		}
		const float u2 = u * u, u3 = u2 * u;
		return (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 + (-2.0f * u3 + 3.0f * u2) * v1;
	}

	// The five flesh spells, by their effect shader in Skyrim.esm, and the dim light color each takes when FleshFX's look wins
	// on it (LightKit.h FleshFx finds that in the load order). FleshFX turns the shader's glowing rim black and dresses the
	// skin in its own textures, laid OVER the skin, so a light in the vanilla rim's hue colors a glow that is not there. These
	// are the textures' own hues as the shader shows them, at the flesh rows' subtle top channel of 184 (the color rework's
	// findings, the flesh section). HIS CALL: "do the changes proposed with the dim light in its own color".
	struct FleshLook
	{
		std::uint32_t               skyrimFormID{ 0 };  // the shader's form in Skyrim.esm
		std::string_view            editorID;           // as the configs claim it
		std::array<std::uint8_t, 3> rgb{};
	};
	inline constexpr std::array<FleshLook, 5> kFleshLooks{ {
		{ 0x094160, "MagicArmorOakFleshFXS", { 184, 156, 129 } },    // brown bark
		{ 0x094161, "MagicArmorStoneFleshFXS", { 170, 184, 178 } },  // grey stone
		{ 0x094163, "MagicArmorIronFleshFXS", { 179, 182, 184 } },   // grey metal
		{ 0x094162, "MagicArmorEbonyFleshFXS", { 92, 56, 184 } },    // dark violet
		{ 0x0E9AC8, "MagicArmorDragonFleshFXS", { 184, 119, 50 } },  // amber scales
	} };

	// a_hue at the strength of a_now (channels 0..1): scaled so its brightest channel is a_now's brightest. A light whose
	// strength moves (a Dynamic Lighting color key, the full-strength switch) keeps moving, in a_hue's color; given its own
	// result again it changes nothing, so it may be applied every frame over a value it wrote. A black a_hue stays black.
	[[nodiscard]] inline std::array<float, 3> AtStrengthOf(const std::array<float, 3>& a_hue, const std::array<float, 3>& a_now) noexcept
	{
		const float top = (std::max)({ a_hue[0], a_hue[1], a_hue[2] });
		const float want = (std::max)({ a_now[0], a_now[1], a_now[2], 0.0f });
		if (top <= 0.0f) {
			return a_hue;
		}
		const float k = want / top;
		return { a_hue[0] * k, a_hue[1] * k, a_hue[2] * k };
	}

	// an art's color off its effect shaders' emissive colors (0..1 each), for an automatic hand light (LightKit.h ArtColor): each
	// layer weighs by its chroma (top channel minus bottom one, on the 0..255 scale), so grey and white layers count for
	// nothing, and the mean is scaled so its top channel is 255. No colored layer = no color. The rule the 2026-10-19 research
	// measured (`Misc\Build work\auto light colors 2026-10-19\`): where it gives a color it points the way the author's own hand
	// light does on 75 of 80 effects.
	// a color whose top channel (0..255) is under this reads as black: an automatic light stays dark for it ("black stays dark")
	inline constexpr int kDarkTop = 16;

	[[nodiscard]] inline std::optional<std::array<std::uint8_t, 3>> ChromaMean(const std::vector<std::array<float, 3>>& a_layers)
	{
		std::array<double, 3> sum{};
		double                weight = 0.0;
		for (const auto& layer : a_layers) {
			std::array<double, 3> c{};
			for (std::size_t i = 0; i < 3; ++i) {
				c[i] = std::isfinite(layer[i]) ? std::round(std::clamp(layer[i], 0.0f, 1.0f) * 255.0) : 0.0;
			}
			// a nearly black layer counts for nothing: scaled to a top channel of 255 it would turn bright (CodeRabbit, RELight #27)
			const double w = std::ranges::max(c) - std::ranges::min(c);
			if (w <= 0.0 || std::ranges::max(c) < kDarkTop) {
				continue;
			}
			for (std::size_t i = 0; i < 3; ++i) {
				sum[i] += c[i] * w;
			}
			weight += w;
		}
		if (weight <= 0.0) {
			return std::nullopt;
		}
		const double                top = std::ranges::max(sum) / weight;  // > 0: a layer with chroma has a channel above 0
		std::array<std::uint8_t, 3> out{};
		for (std::size_t i = 0; i < 3; ++i) {
			out[i] = static_cast<std::uint8_t>((std::min)(255.0, std::round(sum[i] / weight * 255.0 / top)));
		}
		return out;
	}
}
