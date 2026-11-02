// LightKit - small helpers Illuminated and RELight - Spell Lighting NG both use
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// The two mods never run together, so each carries its own copy of this file - kept byte for byte the same as the other
// by PC Runner\fade copies check.py (in preflight), as the fading module is. A change is made in one copy and copied to the
// other in the same pass. Nothing here knows which mod it is in.
//
//   Relaxed      a value one thread writes and another reads, with nothing else depending on it
//   NewForm      a form made in memory by the game's own factory; IsCallAt, a call site's check before it is hooked
//   Isl          the two words Community Shaders' inverse-square lighting reads off a light
//   CloneLight   a new light for the scene, and AddToScene to register it (ReLight's method, Truman's permission)
//   Daylight     how much dimmer the lights are by day and in bright rooms: the "Dim in daylight" setting
//   FleshFx      which flesh spells wear FleshFX's look, and the color their lights then take
//   ArtColor     an art model's color off its effect shaders (an automatic hand light's third source)
//   Guarded      runs a hook's or a frame's own work and lets no exception out into the game
// The plain-number helpers (Lower, CutoffFor, PlainOf, ToScreen, KeyAt ...) are in LightMath.h, tested off the game.

#pragma once

#include "PCH.h"

#include "LightMath.h"
#include "Pulse.h"

#include <mutex>

namespace LightKit
{
	// A value read and written on different threads (the menu's render thread, the main thread, the game's loader threads)
	// with no other value depending on it: relaxed atomic loads and stores. Copyable, so it can live in a vector that is
	// built once at load.
	template <class T>
	class Relaxed
	{
	public:
		Relaxed(T a_value = T{}) noexcept : value(a_value) {}  // NOLINT(google-explicit-constructor): a drop-in for a plain T
		Relaxed(const Relaxed& a_other) noexcept : value(a_other.load()) {}
		Relaxed& operator=(const Relaxed& a_other) noexcept
		{
			store(a_other.load());
			return *this;
		}
		Relaxed& operator=(T a_value) noexcept
		{
			store(a_value);
			return *this;
		}
		Relaxed& operator++() noexcept
		{
			value.fetch_add(1, std::memory_order_relaxed);
			return *this;
		}
		// (no move of its own: a move copies, which is what a vector built at load needs)

						operator T() const noexcept { return load(); }  // NOLINT(google-explicit-constructor)
		[[nodiscard]] T load() const noexcept { return value.load(std::memory_order_relaxed); }
		void            store(T a_value) noexcept { value.store(a_value, std::memory_order_relaxed); }

	private:
		std::atomic<T> value;
	};

	// A new form of type T, made in memory by the game's own factory: it never reaches a save, and the game owns it for the
	// session (nothing frees it). nullptr when the game could not make one. Create() is not const, so neither is the factory.
	template <class T>
	[[nodiscard]] T* NewForm()
	{
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<T>();
		return factory ? factory->Create() : nullptr;
	}

	// Every form of type T the game loaded, or an empty list when the data handler is not there (never a null read)
	template <class T>
	[[nodiscard]] RE::BSTArray<T*>& FormsOf()
	{
		static RE::BSTArray<T*> none;
		auto*                   dh = RE::TESDataHandler::GetSingleton();
		return dh ? dh->GetFormArray<T>() : none;
	}

	// A 5-byte relative call (opcode E8) is at a_address - checked before a call site inside a game function is wrapped;
	// when another plugin has rewritten the site, it is left alone and the caller says so in the log.
	[[nodiscard]] inline bool IsCallAt(std::uintptr_t a_address) noexcept
	{
		std::uint8_t opcode = 0;
		std::memcpy(&opcode, reinterpret_cast<const void*>(a_address), sizeof(opcode));
		return opcode == 0xE8;
	}

	// Community Shaders' inverse-square flag on a light RECORD (TESObjectLIGH): its falloff is then the cutoff
	inline constexpr std::uint32_t kRecordInverseSquare = 1u << 14;
	inline constexpr RE::NiPoint3  kHandPoint{ 0.0f, -40.0f, 0.0f };  // a hand light out of the palm, in the magic node's frame: in it the fingers throw long shadows

	// Community Shaders' inverse-square lighting reads a flag and the cutoff from the first eight bytes of a light's runtime
	// data (its ambient color's red and green, as the game lays them out); without Community Shaders those words are plain
	// ambient color. They are read and written as bytes (memcpy), never through a pointer of another type, and always AFTER
	// SetLightAttenuation, which writes the same two words.
	namespace Isl
	{
		[[nodiscard]] inline std::array<std::uint32_t, 2> Read(RE::NiLight* a_light) noexcept
		{
			std::array<std::uint32_t, 2> w{};
			std::memcpy(w.data(), reinterpret_cast<const std::byte*>(&a_light->GetLightRuntimeData()), sizeof(w));
			return w;
		}
		inline void Write(RE::NiLight* a_light, const std::array<std::uint32_t, 2>& a_words) noexcept
		{
			std::memcpy(reinterpret_cast<std::byte*>(&a_light->GetLightRuntimeData()), a_words.data(), sizeof(a_words));
		}
		[[nodiscard]] inline bool On(RE::NiLight* a_light) noexcept { return (Read(a_light)[0] & kFlag) != 0; }
		inline void               SetOn(RE::NiLight* a_light) noexcept
		{
			auto w = Read(a_light);
			w[0] |= kFlag;
			Write(a_light, w);
		}
		[[nodiscard]] inline float Cutoff(RE::NiLight* a_light) noexcept { return std::bit_cast<float>(Read(a_light)[1]); }
		inline void                SetCutoff(RE::NiLight* a_light, float a_cutoff, float a_lowest = kLowestCutoff,
			float a_highest = kHighestCutoff) noexcept
		{
			auto w = Read(a_light);
			w[1] = std::bit_cast<std::uint32_t>(std::clamp(a_cutoff, a_lowest, a_highest));
			Write(a_light, w);
		}
		// a light of our OWN (cloned, never made by the game's GenDynamic, so Community Shaders set none of its words), after
		// SetLightAttenuation: the flag word written whole - Initialised, as Light Limit Fix marks every light it makes, and
		// InverseSquare (the attenuation's float left there could read as Disabled: RELight 2.1's Bottled Shaders report) - and
		// the third word, Community Shaders' light-record id (a clone carries 1.0f there, which it would look up as a form),
		// cleared. Then SetCutoff. (Dynamic Wards' review, 2026-10-14; Open Shaders' LightLimitFix.cpp / PointLightFlags.h)
		inline void SetOwn(RE::NiLight* a_light) noexcept
		{
			Write(a_light, { kFlag | kInitialised, 0u });
			a_light->GetLightRuntimeData().ambient.blue = 0.0f;
		}
	}

	// THE TURN (his checklist 317, item 1 - one turn-taking rule for every light write): every pass that writes a light's
	// fields does it inside a turn, so a game thread and the main thread never write one light at the same moment. Its order:
	// the sneak / dim locks, then the fading module's, then the turn, then the small list locks (the handshakes, the flicker
	// list). Inside a turn nothing waits on a game lock, and nothing after it asks for it or for an earlier lock - a turn can
	// never close a freeze. Recursive: a writer may call another inside its own turn.
	[[nodiscard]] inline std::unique_lock<std::recursive_mutex> Turn()
	{
		static std::recursive_mutex turn;
		return std::unique_lock<std::recursive_mutex>{ turn };
	}

	// Runs a_work and lets no exception out: inside a hook, a game callback or per-frame code one would end the game (his C++
	// rule). The first failure at each place is logged, later ones there are skipped quietly. One flag per call site - each
	// a_work is a lambda of its own type, so each call site has its own instance of this function.
	template <class F>
	inline void Guarded(const char* a_where, F&& a_work) noexcept
	{
		static std::atomic<bool> told{ false };
		try {
			std::forward<F>(a_work)();
		} catch (const std::exception& e) {
			if (!told.exchange(true)) {
				SKSE::log::error("{}: stopped by an error ({}); later ones here are skipped quietly", a_where, e.what());
			}
		} catch (...) {
			if (!told.exchange(true)) {
				SKSE::log::error("{}: stopped by an unknown error; later ones here are skipped quietly", a_where);
			}
		}
	}

	// An SKSE Menu Framework page or HUD element, registered as GuardedDraw<Draw>: it is drawn inside that DLL on the render
	// thread, where no exception of ours may land (each instance's lambda is its own type, so each page has its own flag)
	template <void (*Draw)()>
	void __stdcall GuardedDraw() noexcept
	{
		Guarded("menu draw", [] { Draw(); });
	}

	// A new light for the scene, cloned from one master light made once: ReLight found that a freshly made light, attached
	// straight away, crashes. nullptr when the game could not make one. Main thread.
	[[nodiscard]] inline RE::NiPointLight* CloneLight()
	{
		static RE::NiPointer<RE::NiPointLight> master;
		if (!master) {
			const RE::NiPointer<RE::NiPointLight> fresh(RE::NiPointLight::Create());  // let go once cloned
			auto*                                 clone = fresh ? netimmerse_cast<RE::NiPointLight*>(fresh->Clone()) : nullptr;
			if (!clone) {
				return nullptr;
			}
			master.reset(clone);
		}
		return netimmerse_cast<RE::NiPointLight*>(master->Clone());
	}

	// the game's shadow scene node, which every light of ours is registered with (none before the world is up)
	[[nodiscard]] inline RE::ShadowSceneNode* Scene() { return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]; }

	// Registers a light already attached under a node and updated once: a dynamic light, as ReLight registers its own (field of
	// view 90, the one a shadowless light is given), never a shadow caster - shadows from lights are the shader mod's job.
	// a_portalStrict false: a row that lets its light through portals. nullptr when the scene refused it - the caller then
	// detaches it. Main thread.
	[[nodiscard]] inline RE::BSLight* AddToScene(RE::ShadowSceneNode* a_scene, RE::NiPointLight* a_light, float a_falloff = 1.0f,
		bool a_portalStrict = true, bool a_shadow = false)
	{
		RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
		params.dynamic = true;
		params.shadowLight = a_shadow;  // only RELight's optional Bottled Shaders shadow patch asks
		params.portalStrict = a_portalStrict;
		params.affectLand = true;
		params.affectWater = true;
		params.neverFades = true;
		params.fov = a_shadow ? 6.2831855f : 90.0f;  // an omni shadow caster sees all round
		params.falloff = a_falloff;
		params.nearDistance = 5.0f;
		params.depthBias = 1.0f;
		params.sceneGraphIndex = 0;
		params.restrictedNode = nullptr;
		params.lensFlareData = nullptr;
		return a_scene && a_light ? a_scene->AddLight(a_light, params) : nullptr;
	}

	// A new light hung under a_parent, updated once (its world place now, not when the parent next updates) and registered
	// (AddToScene); nullptr when the scene refused it, which then takes it off again - freed as Hang returns. Main thread.
	[[nodiscard]] inline RE::BSLight* Hang(RE::NiNode* a_parent, RE::NiPointLight* a_light, RE::ShadowSceneNode* a_scene, float a_falloff = 1.0f,
		bool a_portalStrict = true, bool a_shadow = false)
	{
		if (!a_parent || !a_light || !a_scene) {
			return nullptr;
		}
		const RE::NiPointer<RE::NiPointLight> hold(a_light);
		a_parent->AttachChild(a_light, true);
		RE::NiUpdateData update{};
		a_light->Update(update);
		auto* bs = AddToScene(a_scene, a_light, a_falloff, a_portalStrict, a_shadow);
		if (!bs) {
			a_parent->DetachChild(a_light);
		}
		return bs;
	}

	// the ambient a light of ours wears off Community Shaders: a tenth of its color (RE::Light's rule, Truman)
	[[nodiscard]] inline RE::NiColor Tenth(const RE::NiColor& a_color) { return { a_color.red * 0.1f, a_color.green * 0.1f, a_color.blue * 0.1f }; }

	// "Dim in daylight" (LightMath.h's DaylightShare): 1 at night and in the dark; a_pick 0 off, 1 a little, 2 more. Main thread:
	// it reads the player's cell (an interior by its own light, outdoors by the hour).
	[[nodiscard]] inline float Daylight(int a_pick)
	{
		auto* player = a_pick > 0 ? RE::PlayerCharacter::GetSingleton() : nullptr;
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return 1.0f;
		}
		float bright = 0.0f;
		if (cell->IsInteriorCell()) {
			if (const auto* l = cell->GetLighting()) {
				const auto lum = [](const RE::Color& a_c) {
					return (0.2126f * a_c.red + 0.7152f * a_c.green + 0.0722f * a_c.blue) / 255.0f;
				};
				bright = DaylightOfRoom((std::max)(lum(l->ambient), lum(l->directional)));
			}
		} else if (const auto* calendar = RE::Calendar::GetSingleton()) {
			bright = DaylightOfHour(calendar->GetHour());
		}
		return DaylightShare(a_pick, bright);
	}

	// FleshFX (LightMath.h kFleshLooks): which flesh spells wear its look in this load order. Find() runs once at data load, on
	// the main thread, before any light is drawn; HueOf and Shown only read after it, from any thread.
	namespace FleshFx
	{
		inline constexpr std::string_view kPlugin = "FleshFX.esp";

		[[nodiscard]] inline std::unordered_map<RE::FormID, RE::NiColor>& Hues()
		{
			static std::unordered_map<RE::FormID, RE::NiColor> hues;  // shader -> its dim color under FleshFX
			return hues;
		}

		// the five flesh shaders this load order has, FleshFX or not (Find() fills it; Illuminated's record route lights them all)
		[[nodiscard]] inline std::vector<std::pair<const RE::TESEffectShader*, const FleshLook*>>& Resolved()
		{
			static std::vector<std::pair<const RE::TESEffectShader*, const FleshLook*>> resolved;
			return resolved;
		}

		// A flesh shader wears FleshFX's look when FleshFX.esp is loaded and its record of that shader is the one that wins (it
		// blackens the rim and lays its own textures on the skin). A shader a later plugin wins keeps its light's own color.
		// Returns how many of the five wear the look; every answer is logged.
		inline std::size_t Find()
		{
			auto& hues = Hues();
			auto& resolved = Resolved();
			hues.clear();
			resolved.clear();
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (const auto& look : kFleshLooks) {
				if (const auto* found = dh ? dh->LookupForm<RE::TESEffectShader>(look.skyrimFormID, "Skyrim.esm") : nullptr) {
					resolved.emplace_back(found, &look);
				} else {
					SKSE::log::warn("FleshFX: {} (Skyrim.esm {:06X}) is not in this load order", look.editorID, look.skyrimFormID);
				}
			}
			if (!dh || !(dh->LookupLoadedModByName(kPlugin) || dh->LookupLoadedLightModByName(kPlugin))) {
				SKSE::log::info("FleshFX: {} is not loaded - the flesh spells' lights keep their own colors", kPlugin);
				return 0;
			}
			for (const auto& [shader, lookOf] : resolved) {
				const auto& look = *lookOf;
				// FleshFX's look wins when the LAST file that changes the shader (the winning record) is FleshFX.esp or a patch made
				// for it (FleshFX.esp is one of its masters - e.g. a Mysticism FleshFX patch keeps FleshFX's look); the rim color is
				// only logged - measured 2026-10-12 23:11, the loaded edge color read non-zero in game while the winning record's
				// colors are all 0 (houseCARL), so it cannot decide this
				const auto* files = shader->sourceFiles.array;
				const auto* last = files && !files->empty() ? files->back() : nullptr;
				// case-blind, as the data handler's own lookup above is (a load order may store "fleshfx.esp")
				const auto isFleshFx = [](const RE::TESFile* a_file) { return a_file && SameText(a_file->GetFilename(), kPlugin); };
				const bool edited = files && std::ranges::any_of(*files, isFleshFx);
				bool       wins = isFleshFx(last);
				for (std::uint32_t i = 0; !wins && last && last->masterPtrs && i < last->masterCount; ++i) {
					wins = isFleshFx(last->masterPtrs[i]);
				}
				const auto& rim = shader->data.edgeEffectColor;
				if (!wins) {
					SKSE::log::info("FleshFX: {} keeps its light's own color ({}; rim {},{},{})", look.editorID,
						edited ? "a later plugin wins it" : "FleshFX does not edit it", rim.red, rim.green, rim.blue);
					continue;
				}
				hues.insert_or_assign(shader->GetFormID(), RE::NiColor{ look.rgb[0] / 255.0f, look.rgb[1] / 255.0f, look.rgb[2] / 255.0f });
				SKSE::log::info("FleshFX: {} wears FleshFX's look ({}) - its light is drawn in {},{},{} (rim as loaded {},{},{})",
					look.editorID, last->GetFilename(), look.rgb[0], look.rgb[1], look.rgb[2], rim.red, rim.green, rim.blue);
			}
			return hues.size();
		}

		// the dim color a light on this shader takes, or nullptr (not a flesh shader, or FleshFX's look does not win on it)
		[[nodiscard]] inline const RE::NiColor* HueOf(const RE::TESEffectShader* a_shader)
		{
			const auto& hues = Hues();
			if (hues.empty() || !a_shader) {
				return nullptr;
			}
			const auto it = hues.find(a_shader->GetFormID());
			return it != hues.end() ? &it->second : nullptr;
		}

		// what such a light shows: the hue at the strength the light carries now (LightMath.h AtStrengthOf)
		[[nodiscard]] inline RE::NiColor Shown(const RE::NiColor& a_hue, const RE::NiColor& a_now) noexcept
		{
			const auto c = AtStrengthOf({ a_hue.red, a_hue.green, a_hue.blue }, { a_now.red, a_now.green, a_now.blue });
			return { c[0], c[1], c[2] };
		}
	}

	// An art model's color off the emissive colors of its effect shaders (LightMath.h ChromaMean), for an automatic hand light
	// whose spell has no light of its own and fires no lit projectile (his order of 2026-10-18). The model is read through the
	// game's own model loader, once per model for the session, and only while a_load is true (the data load): later callers may
	// hold a lock a game thread waits on, so they get only what was read before. Main thread only. Not on VR: kept SE / AE
	// only, untested there - the loader itself resolves on VR (id 74040 is in the VR Address Library).
	[[nodiscard]] inline std::optional<RE::Color> ArtColor(const char* a_model, bool a_load = true)
	{
		static std::unordered_map<std::string, std::optional<RE::Color>> read;  // model -> its color, or none
		if (REL::Module::IsVR() || !a_model || !*a_model) {
			return std::nullopt;
		}
		if (!a_load) {
			const auto known = read.find(Lower(a_model));
			return known != read.end() ? known->second : std::nullopt;
		}
		const auto [it, added] = read.try_emplace(Lower(a_model));
		if (!added) {
			return it->second;
		}
		RE::NiPointer<RE::NiNode> root;
		if (RE::BSModelDB::Demand(a_model, root, RE::BSModelDB::DBTraits::ArgsType{}) != RE::BSResource::ErrorCode::kNone || !root) {
			return std::nullopt;
		}
		std::vector<std::array<float, 3>> layers;
		RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* a_geometry) {
			auto* shader = netimmerse_cast<RE::BSEffectShaderProperty*>(a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr);
			if (auto* material = shader ? static_cast<RE::BSEffectShaderMaterial*>(shader->GetBaseMaterial()) : nullptr) {
				layers.push_back({ material->baseColor.red, material->baseColor.green, material->baseColor.blue });
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (const auto rgb = ChromaMean(layers)) {
			it->second = RE::Color((*rgb)[0], (*rgb)[1], (*rgb)[2], 0);
		}
		return it->second;
	}

	// which pulse shape a spell's light takes under Realistic, read off its magic effect, so a spell nobody patched gets one too
	[[nodiscard]] inline Pulse::Kind PulseKindOf(const RE::EffectSetting* a_effect)
	{
		using AV = RE::ActorValue;
		using Arch = RE::EffectArchetypes::ArchetypeID;
		if (!a_effect) {
			return Pulse::Kind::kNone;
		}
		const auto& d = a_effect->data;
		if (d.archetype == Arch::kAbsorb) {
			return d.primaryAV == AV::kMagicka ? Pulse::Kind::kAbsorbMagicka :
			       d.primaryAV == AV::kStamina ? Pulse::Kind::kAbsorbStamina :
			                                     Pulse::Kind::kAbsorbHealth;
		}
		if (d.primaryAV == AV::kWardPower) {
			return Pulse::Kind::kWard;
		}
		if (d.archetype == Arch::kLight) {
			return Pulse::Kind::kMagelight;
		}
		switch (d.resistVariable) {
		case AV::kResistFire:
			return Pulse::Kind::kFire;
		case AV::kResistFrost:
			return Pulse::Kind::kFrost;
		case AV::kResistShock:
			return Pulse::Kind::kShock;
		case AV::kPoisonResist:
			return Pulse::Kind::kPoison;
		default:
			break;
		}
		switch (d.associatedSkill) {
		case AV::kRestoration:
			return Pulse::Kind::kHealing;
		case AV::kIllusion:
			return Pulse::Kind::kIllusion;
		case AV::kConjuration:
			return Pulse::Kind::kConjuration;
		case AV::kAlteration:
			return Pulse::Kind::kAlteration;
		default:
			return Pulse::Kind::kNone;
		}
	}

	// the pulse kind of a spell: its costliest effect's (what a hand casts, or what a sprayed / fired projectile carries)
	[[nodiscard]] inline Pulse::Kind PulseKindOfSpell(const RE::MagicItem* a_spell)
	{
		const auto* item = a_spell ? a_spell->GetCostliestEffectItem() : nullptr;
		return PulseKindOf(item ? item->baseEffect : nullptr);
	}

	// the pulse kind of what a hand holds: the spell it casts now, else the one readied in it (a hand light is made, and pulses,
	// before any cast)
	[[nodiscard]] inline Pulse::Kind PulseKindOfHand(RE::Actor* a_actor, bool a_left)
	{
		// read straight off the actor: GetMagicCaster may make a missing caster, and the light hooks also run off the main thread
		const auto* caster = a_actor ? a_actor->GetActorRuntimeData().magicCasters[a_left ? RE::Actor::SlotTypes::kLeftHand : RE::Actor::SlotTypes::kRightHand] :
		                               nullptr;
		const auto* spell = caster ? caster->currentSpell : nullptr;
		if (!spell) {
			auto* equipped = a_actor ? a_actor->GetEquippedObject(a_left) : nullptr;
			spell = equipped ? equipped->As<RE::MagicItem>() : nullptr;
		}
		return PulseKindOfSpell(spell);
	}
}
