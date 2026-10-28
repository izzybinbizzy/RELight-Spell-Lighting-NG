// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Our own Brightness and Reach sliders. RE::Light's multiplier scales every RE::Light light in the game; these touch
// only this mod's lights (RE::Light's lights on objects our data names, our hand lights, spray lights and the lights of
// the beam and breath projectiles our `stream` lines name). Under inverse square lighting reach = sqrt(K * fade / cutoff - size²), so:
//     Brightness  scales fade and cutoff by one ratio  -> the peak moves, the reach is held
//     Reach       scales the radius, cutoff re-derived -> the reach moves, the peak is held
// RE::Light rewrites fade every frame on a light that flickers or pulses, so each light remembers the fade we last
// wrote; anything else it carries is a new base, so no slider ever scales a value we wrote. A hand light with Dynamic
// Lighting breathes or crackles here (Oscillate), and a light on an object an art pick recolors (`tint`) takes that
// color. Runs in the player update after the switches and before the fading module, the sneak dim and the ground twins.
//
// Illuminated's light settings, ported (his "add everything from illuminated into relight ... sister mods" -
// RE::Light lights the objects; what it has no setting for is done here, on the lights this pass already looks after):
//   Dim in daylight   Brightness x a daylight factor: outdoors by the hour (full by day, none at night, a ramp at dawn and
//                     dusk), indoors by how bright the room's own light is - looked at every two seconds
// (Light colors and the Fire / Frost / Shock color picks were here until his order took the color variations out of the
// menu, 2026-10-09; Light colors came back 2026-10-20 and left again 2026-10-21 - "pale and deep also need to be removed":
// every light keeps its own color; an art pick's tint still recolors.)
// The color is remembered like the fade: what RE::Light writes is the new base, what we wrote is never taken for one.
// A flesh spell's light where FleshFX's look wins (LightKit.h FleshFx) is drawn in FleshFX's hue at the strength of that base.
// The fading module (Fade*.cpp, Illuminated's) scales some of the same lights by charge and magicka right after this pass
// and after each enchantment effect's update; it reports each fade it writes (NoteFadeWrite) and each reach it writes
// (NoteReachWrite), so its write is never taken for a new base here and the two never compound. Lock order: the fading
// module's lock, then gSeenLock.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// keyed by the light, which Keep.cpp holds; ForgetSliderLights drops the row before the light is freed
		struct Seen
		{
			const HandFx*        fx{ nullptr };
			Pulse::Kind          kind{ Pulse::Kind::kNone };  // its spell's kind, for Realistic
			const RE::NiColor*   tint{ nullptr };
			float                base{ 0.0f };        // the fade RE::Light last gave it
			float                peak{ 0.0f };        // the most it gave: held while Dynamic Lighting is off, if it flickers
			float                rlPhase{ -1.0f };    // RE::Light's flicker phase (the point light's constAttenuation) last frame
			bool                 flickers{ false };   // RE::Light moved that phase: a flickering config, not a one-shot fade
			bool                 ours{ false };       // a hand light this plugin made: our re-dress moves its phase, RE::Light never flickers it
			bool                 rebase{ true };      // ours: take the base from the light once - it was just made or re-dressed
			float                written{ -1.0f };    // the fade we last wrote
			float                baseRadius{ 0.0f };  // the reach RE::Light last gave it
			float                wroteRadius{ -1.0f };
			float                baseCutoff{ -1.0f };  // the cutoff RE::Light (or its editor) last gave it
			float                wroteCutoff{ -2.0f };
			std::array<float, 3> phase{};
			RE::NiColor          baseDiffuse{};  // the color RE::Light (or the hand light) last gave it
			RE::NiColor          wroteDiffuse{ -1.0f, -1.0f, -1.0f };
		};

		[[nodiscard]] bool SameColor(const RE::NiColor& a, const RE::NiColor& b) noexcept
		{
			return a.red == b.red && a.green == b.green && a.blue == b.blue;
		}

		float gDaylight = 1.0f, gDaylightClock = 2.0f;  // main thread only (UpdateBrightness)

		// Dynamic Lighting on a hand light, as Let There Be Glow's Light Placer curves look:
		//   Pulse (a breathe): RE::Light's pulse - one phase advancing perSecond * dt * 5, a smooth swing of `intensity`.
		//   Flicker (a crackle): Light Placer's stepped fade - the light JUMPS between a high and a low level, twice a cycle,
		//   `intensity` deep. A random walk of smooth sines (RE::Light's flicker, the first version here) averaged out to about
		// 8% and read as static (his report, measured).
		[[nodiscard]] float Oscillate(Seen& a_s, float a_dt)
		{
			static std::minstd_rand                      rng{ 20260923 };
			static std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
			constexpr float                              tau = std::numbers::pi_v<float> * 2.0f;
			constexpr float                              kRatePerHz = 1.2566f;  // gen.RELIGHT_RATE_PER_HZ: perSecond back to cycles a second
			if (a_s.kind != Pulse::Kind::kNone && PulseRealistic()) {
				static const auto start = std::chrono::steady_clock::now();
				const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				return Pulse::Factor(a_s.kind, seconds, static_cast<std::uint32_t>(std::hash<const void*>{}(&a_s)));
			}
			if (!a_s.fx) {
				return 1.0f;  // no handfx line (an automatic hand light): steady unless Realistic gives its kind a shape
			}
			const auto& fx = *a_s.fx;
			if (!fx.flicker) {
				a_s.phase[0] = std::fmod(a_s.phase[0] + fx.perSecond * a_dt * 5.0f, tau);
				return (std::sin(a_s.phase[0]) + 1.0f) / 2.0f * fx.intensity + 1.0f - fx.intensity;
			}
			// phase[0]: time into this step; phase[1]: the level held; phase[2]: 1 while on a high step, 0 on a low one
			const float hold = 0.5f / (std::max)(fx.perSecond / kRatePerHz, 0.1f);
			a_s.phase[0] += a_dt;
			if (a_s.phase[1] <= 0.0f || a_s.phase[0] >= hold) {
				a_s.phase[0] = std::fmod(a_s.phase[0], hold);
				a_s.phase[2] = a_s.phase[2] > 0.5f ? 0.0f : 1.0f;
				const float dip = a_s.phase[2] > 0.5f ? 0.3f * unit(rng) : 0.6f + 0.4f * unit(rng);  // high steps near full, low near the bottom
				a_s.phase[1] = 1.0f - fx.intensity * dip;
			}
			return a_s.phase[1];
		}

		std::unordered_map<const RE::NiLight*, Seen> gSeen;
		std::mutex                                   gSeenLock;  // the fading module reports from the effect updates too
		std::mutex                                   gNewLock;
		struct Made
		{
			RE::NiLight*  light{ nullptr };
			const HandFx* fx{ nullptr };
			Pulse::Kind   kind{ Pulse::Kind::kNone };
			bool          hand{ false };
			Dressed       dressed{};  // what its dress wrote (fade -1: none known)
		};
		std::vector<Made> gNew;  // lights made since the last frame

		// The lights that are NOT ours, so the walk up to the reference and the data lookup are not repeated for each of them
		// every frame (main thread). A freed light's address can come back as another light, so each is remembered with its
		// name text and its parent, and asked again when either differs; RE::Light's enchantment lights are never cached here
		// (one becomes ours when its effect starts - Options.cpp finds those each frame).
		struct NotOurs
		{
			const void*   name{ nullptr };
			const void*   parent{ nullptr };
			std::uint32_t frame{ 0 };
		};
		std::unordered_map<const RE::NiLight*, NotOurs> gNotOurs;
		std::uint32_t                                   gFrame = 0;

		[[nodiscard]] bool KnownNotOurs(const RE::NiLight* a_light)
		{
			const auto it = gNotOurs.find(a_light);
			if (it == gNotOurs.end() || it->second.name != a_light->name.c_str() || it->second.parent != a_light->parent) {
				return false;
			}
			it->second.frame = gFrame;
			return true;
		}

		[[nodiscard]] const RE::TESBoundObject* BaseOf(RE::NiLight* a_light)
		{
			const auto* ref = ReferenceOf(a_light);
			return ref ? ref->GetBaseObject() : nullptr;
		}

		// a spray's or (2.3) a bolt's light, however it was made: it hangs on a flame, cone or beam projectile that wears one of
		// our raised copies (or a spray record, where no copy could be made) - a beam's light is made where our hook does not
		// see it, so this walk is what brings it to the sliders
		[[nodiscard]] bool IsSprayProjectileLight(const RE::TESBoundObject* a_base)
		{
			const auto* proj = a_base ? a_base->As<RE::BGSProjectile>() : nullptr;
			return proj && (proj->IsFlamethrower() || proj->IsCone() || proj->IsBeam()) && IsSprayLight(proj->data.light);
		}

		// RE::Light names every light it makes from a config "RL" + the node it hung it on. An enchantment light hangs on
		// the actor holding the weapon, so it is found by its shader instead (Options.cpp) - his report: "the
		// sliders don't work for enchantments"
		[[nodiscard]] bool OursByObject(RE::NiLight* a_light, const RE::TESBoundObject* a_base)
		{
			if (IsSprayProjectileLight(a_base)) {
				return true;
			}
			return IsReLightLight(a_light) && (OptionOf(a_base) != kNone || EnchantOptionOf(a_light) != kNone);
		}

		// a_hue: a flesh spell's light under FleshFX (Options.cpp FleshHueOf) - drawn in that hue at the strength RE::Light gave it
		void Apply(RE::NiLight* a_light, Seen& a_s, float a_scale, float a_reach, float a_dt, const RE::NiColor* a_hue)
		{
			auto& data = a_light->GetLightRuntimeData();
			// a hand light of ours has the bases DressHandLight gave it - fade, reach and cutoff - taken once: a later write (another
			// pass scaling it each frame) never becomes its base, or the next frame scales that again - the 3.0 white-out on ENB, x1.6
			// a frame; and readied at low magicka, the fading's first write would stay its reach for life
			const bool rebase = a_s.ours && a_s.rebase;
			a_s.rebase = false;
			if (a_s.ours ? rebase : data.fade != a_s.written) {
				a_s.base = data.fade;  // written since we last touched it (or ours, just dressed): that is the new base
			}
			// the peak follows a rise at once and sinks slowly (kPeakSink a second) toward a lower base, so a lasting change in what
			// RE::Light gives (its own brightness slider, the reference's scale) is followed within seconds while a flicker's dips
			// (a fraction of a second) barely move it
			constexpr float kPeakSink = 0.02f;
			a_s.peak = a_s.base >= a_s.peak ? a_s.base : (std::max)(a_s.base, a_s.peak * (1.0f - kPeakSink * a_dt));
			if (a_s.ours ? rebase : data.radius.x != a_s.wroteRadius) {
				a_s.baseRadius = data.radius.x;  // x and y are the reach, z is the size
			}
			// Dynamic Lighting off: an RE::Light light that flickers by its config is held at the most it reached. Only one that
			// flickers: RE::Light (7.6, everyFrame.h) writes a point light's attenuation phases only in its flicker / pulse branch,
			// so a phase that moved marks it; a one-shot fade (an explosion's or a rune's burst - 2.5's plain configs carry 349)
			// keeps fading as RE::Light draws it, never held at its brightest
			if (const auto* point = netimmerse_cast<RE::NiPointLight*>(a_light)) {
				if (!a_s.ours && a_s.rlPhase >= 0.0f && point->constAttenuation != a_s.rlPhase) {
					a_s.flickers = true;
				}
				a_s.rlPhase = point->constAttenuation;
			}
			const bool  dynamic = DynamicLightingThisSession();
			const float held = dynamic || !a_s.flickers ? a_s.base : a_s.peak;
			const float fade = held * a_scale * (dynamic && (a_s.fx || a_s.kind != Pulse::Kind::kNone) ? Oscillate(a_s, a_dt) : 1.0f);
			data.fade = fade;
			a_s.written = fade;
			const float radius = a_s.baseRadius * a_reach;
			const bool  moved = data.radius.x != radius;
			data.radius.x = radius;
			data.radius.y = radius;
			a_s.wroteRadius = radius;
			// off Community Shaders the drawn reach is the point light's attenuation, which the stated radius alone never moved
			// (Held.cpp SetPlainReach, measured): a steady RE::Light light is attenuated again; a flickering one is left alone - its
			// attenuation holds RE::Light's flicker phases - and so is a hand light of ours, which DressHandLight attenuates
			if (auto* point = moved && !InverseSquare() && !a_s.flickers && !a_s.ours ? netimmerse_cast<RE::NiPointLight*>(a_light) : nullptr) {
				const auto ambient = LightKit::Isl::Read(point);  // the call writes these two words, the ambient color off Community Shaders
				point->SetLightAttenuation(radius);
				LightKit::Isl::Write(point, ambient);
				a_s.rlPhase = point->constAttenuation;  // our write, never read as RE::Light's flicker
			}
			if (!SameColor(data.diffuse, a_s.wroteDiffuse)) {
				a_s.baseDiffuse = data.diffuse;  // RE::Light (or the hand light) gave it this color: the new base
			}
			const RE::NiColor own = a_s.tint ? *a_s.tint : a_s.baseDiffuse;
			const RE::NiColor color = a_hue ? LightKit::FleshFx::Shown(*a_hue, own) : own;  // every light keeps its own color
			data.diffuse = color;
			a_s.wroteDiffuse = color;
			// the cutoff follows the same rule, so an edit in RE::Light's editor sticks: a cutoff we did not write is the
			// new base; the reach it gives this frame's own fade is scaled by Reach, so a flickering light's reach flickers
			// with it at every setting, as at 100 % / 100 % (his checklist 337, "flicker with reach").
			// Without Community Shaders' inverse square lighting these words are the ambient color (Plugin.h), so they are
			// neither read nor written - as Held.cpp and Streams.cpp
			if (!InverseSquare() || !Isl::On(a_light)) {
				return;
			}
			const float current = Isl::Cutoff(a_light);
			if (a_s.ours ? rebase || a_s.baseCutoff < 0.0f : current != a_s.wroteCutoff) {
				a_s.baseCutoff = current;
			}
			float cutoff = a_s.baseCutoff;
			if ((a_scale != 1.0f || a_reach != 1.0f) && a_s.baseCutoff > 0.0f && a_scale > 0.0f && fade > 0.0f) {
				const float size = data.radius.z;
				const float reach = std::sqrt((std::max)(kK * (fade / a_scale) / a_s.baseCutoff - size * size, 1.0f)) * a_reach;
				cutoff = CutoffFor(fade, reach, size);
			}
			if (cutoff != current) {
				Isl::SetCutoff(a_light, cutoff);
			}
			a_s.wroteCutoff = Isl::Cutoff(a_light);
		}
	}

	void RememberLight(RE::NiLight* a_light, const HandFx* a_fx, Pulse::Kind a_kind, bool a_hand, Dressed a_dressed)
	{
		if (a_light) {
			KeepLight(a_light);
			std::lock_guard l{ gNewLock };
			gNew.push_back({ a_light, a_fx, a_kind, a_hand, a_dressed });
		}
	}

	float DaylightFactor() { return gDaylight; }

	void ForgetSliderLights(const GoneLights& a_gone)
	{
		{
			std::lock_guard l{ gNewLock };
			std::erase_if(gNew, [&](const Made& a_m) { return a_gone.contains(a_m.light); });
		}
		std::lock_guard l{ gSeenLock };
		std::erase_if(gSeen, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
		std::erase_if(gNotOurs, [&](const auto& a_kv) { return a_gone.contains(a_kv.first); });
	}

	void NoteReachWrite(const RE::NiPointLight* a_light, float a_before, float a_after)
	{
		std::lock_guard l{ gSeenLock };
		// as NoteFadeWrite: only when the fading module started from the reach this pass wrote. Without it the two passes each
		// took the other's write for a new base whenever Reach was not 100% and the fading's reach was below full (a spell
		// hand with magicka spent), and the reach crept every frame until the fading's loop guard froze it
		if (const auto it = gSeen.find(a_light); it != gSeen.end() && it->second.wroteRadius == a_before) {
			it->second.wroteRadius = a_after;
		}
	}

	void NoteCutoffWrite(const RE::NiPointLight* a_light, float a_before, float a_after)
	{
		std::lock_guard l{ gSeenLock };
		// as NoteFadeWrite: the fading's cutoff is never taken here for the light's own (the cutoff block of Apply)
		if (const auto it = gSeen.find(a_light); it != gSeen.end() && it->second.wroteCutoff == a_before) {
			it->second.wroteCutoff = a_after;
		}
	}

	void NoteFadeWrite(const RE::NiPointLight* a_light, float a_before, float a_after)
	{
		std::lock_guard l{ gSeenLock };
		// only when the fading module started from the fade this pass wrote (Illuminated measured why: noting
		// every write froze steady lights this pass had never scaled at full strength)
		if (const auto it = gSeen.find(a_light); it != gSeen.end() && it->second.written == a_before) {
			it->second.written = a_after;
		}
	}

	void UpdateBrightness(float a_delta)
	{
		static std::vector<Made> made;  // main thread; its storage and gNew's are swapped back and forth, never reallocated
		made.clear();
		{
			std::lock_guard l{ gNewLock };
			made.swap(gNew);
		}
		++gFrame;
		{
			std::lock_guard l{ gSeenLock };
			for (const auto& m : made) {
				auto& s = gSeen[m.light];
				s.fx = m.fx;
				if (m.hand) {
					s.ours = true;
					s.flickers = false;
					// made or re-dressed: what its dress wrote is its base - a pass that scaled it before this list came round (the
					// fading module, the frame it was made) is never baked in; nothing known: its values now (rebase)
					s.rebase = !(m.dressed.fade >= 0.0f);
					if (!s.rebase) {
						s.base = m.dressed.fade;
						s.baseRadius = m.dressed.radius;
						s.baseCutoff = m.dressed.cutoff;
					}
				}
				if (m.kind != Pulse::Kind::kNone) {
					s.kind = m.kind;
				}
				// the walk may have met a hand light before its task handed it here, and cached it as not ours for good
				gNotOurs.erase(m.light);
			}
		}
		auto* ssn = LightKit::Scene();
		if (!ssn) {
			return;
		}
		const float dt = std::clamp(a_delta, 0.0f, 0.25f);
		gDaylightClock += dt;
		if (gDaylightClock >= 2.0f) {
			gDaylightClock = 0.0f;
			gDaylight = LightKit::Daylight(DimInDaylight());
		}
		const float scale = Brightness() * gDaylight;
		const float reach = Reach();
		for (const auto& bsLight : ssn->GetRuntimeData().activeLights) {
			if (!bsLight || !bsLight->light) {
				continue;
			}
			auto* niLight = bsLight->light.get();
			// gNotOurs is the main thread's alone (this pass and the Keep sweep), so it is read with no lock
			// a row light hangs under a spray / bolt (so it would read as ours) but Rows.cpp draws it whole every frame, the
			// sliders included: scaled here too it would take them twice and lose its cutoff
			if (KnownNotOurs(niLight) || IsRowLight(niLight)) {
				continue;
			}
			// gSeenLock is taken per light, not across the walk: the fading module's NoteFadeWrite (the effect updates)
			// never waits for the whole pass (the re-score's RELight issue 2); the turn first (LightKit.h's order)
			const auto      turn = LightKit::Turn();
			std::lock_guard l{ gSeenLock };
			auto            it = gSeen.find(niLight);
			if (it == gSeen.end()) {
				// only OUR lights are remembered; the others are remembered as not ours
				const auto* base = BaseOf(niLight);
				if (!OursByObject(niLight, base)) {
					// a light whose reference has no base yet (its 3D still loading) is not cached: it may turn out ours
					if (base && !IsEnchantLight(niLight)) {
						gNotOurs.insert_or_assign(niLight, NotOurs{ niLight->name.c_str(), niLight->parent, gFrame });
					}
					continue;
				}
				gNotOurs.erase(niLight);
				// a spray's or bolt's light made where our hook did not dress it moves with its projectile all the same: the
				// moving-light mark, so RE::Light's flicker prevention lets it reach every surface (Streams.cpp DressStreamLight)
				if (IsSprayProjectileLight(base)) {
					niLight->fadeAmount = kMovingLightMark;
				}
				KeepLight(niLight);
				it = gSeen.emplace(niLight, Seen{ .tint = TintOf(base) }).first;
			}
			Apply(niLight, it->second, scale, reach, dt, IsEnchantLight(niLight) ? FleshHueOf(niLight) : nullptr);
		}
		// a light not seen for a few seconds has left the scene: let it go from the cache
		constexpr std::uint32_t kForget = 300;
		if (gFrame % kForget == 0) {
			std::erase_if(gNotOurs, [](const auto& a_kv) { return gFrame - a_kv.second.frame > kForget; });
		}
	}
}
