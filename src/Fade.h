// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// What the files share. The file map is at the top of FadeMain.cpp.

#pragma once

#include "FadeGlow.h"
#include "FadeSettingsText.h"
#include "LightMath.h"

namespace Fade
{
	using LightKit::StringHash;  // LightMath.h: std::string keys looked up by std::string_view without a temporary string
	using LightKit::StringMap;

	using LightKit::Lower;  // LightMath.h: the one lower-casing helper

	// A path as UTF-8 text. path::string() converts to the ANSI code page on Windows and THROWS for a name that code
	// page cannot show (a Japanese file name on a Western system); this cannot.
	[[nodiscard]] inline std::string PathText(const std::filesystem::path& a_path)
	{
		const auto u = a_path.generic_u8string();
		return std::string(u.begin(), u.end());
	}

	// ------------------------------------------------------------------ FadeSettings.cpp: the settings (FadeSettingsText.h)
	// The one shared copy lives in FadeSettings.cpp behind a lock: the menu (render thread) and DevBench (its own thread)
	// change it while the main thread reads it every frame, so everyone works on a copy. The two switches the per-call hooks
	// read (the HUD charge bar, every effect update) are kept beside it as atomics, so those hooks never lock or copy.
	[[nodiscard]] Settings Config();                                           // a copy, safe from any thread
	void                   SetConfig(const Settings& a_settings);              // replaces it whole; live on the next frame
	bool                   ApplySetting(std::string_view a_key, int a_value);  // one Fading.ini line; false if unknown
	[[nodiscard]] bool     DebugLogOn() noexcept;                              // lock-free
	[[nodiscard]] bool     HideChargeBarOn() noexcept;                         // lock-free; the switch AND Fading on
	void                   LoadSettings();
	void                   SaveSettings();

	// the Debug page's preview: never saved. The menu (or DevBench) sets it; the next frame reads it.
	struct Preview
	{
		std::atomic<bool>  on{ false };
		std::atomic<float> fraction{ 0.5f };                // every tracked hand shows this charge instead of its own
		std::atomic<bool>  pulse{ false }, flare{ false };  // one-shot: start a pulse / flare on every tracked hand
	};
	[[nodiscard]] Preview& PreviewState();

	// ------------------------------------------------------------------ FadeEditorIDs.cpp: names the game throws away
	// (weapons and enchantments for the rule files, magic effects for the soul trap test and the plugins' own readers)
	void                      RememberEditorID(const RE::TESForm* a_form, const char* a_id) noexcept;
	void                      InstallEditorIDHooks();               // weapons, enchantments, magic effects: before the game reads its plugins
	void                      ForgetPassEditorIDs();                // after data load: only weapons, enchantments and magic effects are kept
	[[nodiscard]] std::string EditorID(const RE::TESForm* a_form);  // "" when unknown (a copy: the table may be rewritten)
	[[nodiscard]] std::string Label(const RE::TESForm* a_form);     // "Name [EditorID] Skyrim.esm|0004E4EE" for the log

	// powerofthree's Tweaks keeps editor IDs itself (its Load EditorIDs fix, on by default) and exports GetFormEditorID: while it
	// does, EditorID asks it and our recorder stays out for the types it keeps
	[[nodiscard]] bool        TweaksKeepsEditorIDs();
	[[nodiscard]] const char* TweaksEditorID(RE::FormID a_id);  // nullptr without Tweaks or for a form it does not know

	// the types Tweaks' cache covers (its CacheFormEditorIDs fix) - only these are left to it
	template <class T>
	inline constexpr bool kTweaksKeeps = std::is_same_v<T, RE::TESObjectWEAP> || std::is_same_v<T, RE::EnchantmentItem> ||
	                                     std::is_same_v<T, RE::EffectSetting> || std::is_same_v<T, RE::BGSProjectile> ||
	                                     std::is_same_v<T, RE::BGSExplosion> || std::is_same_v<T, RE::BGSHazard> ||
	                                     std::is_same_v<T, RE::TESObjectLIGH> || std::is_same_v<T, RE::TESEffectShader>;

	// the hook RecordEditorIDs<T> installs: SetFormEditorID (vtable slot 0x33) on T, recording the ID, then the game's own
	template <class T>
	struct EditorIDRecorder
	{
		static bool thunk(RE::TESForm* a_this, const char* a_id)
		{
			RememberEditorID(a_this, a_id);
			return func(a_this, a_id);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline bool                             installed{ false };  // SKSE's post-load message, main thread
	};

	// the editor IDs of form type T recorded as they load; each type hooked once, whoever asks
	template <class T>
	void RecordEditorIDs()
	{
		if constexpr (kTweaksKeeps<T>) {
			if (TweaksKeepsEditorIDs()) {
				return;
			}
		}
		if (!std::exchange(EditorIDRecorder<T>::installed, true)) {
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
			EditorIDRecorder<T>::func = vtbl.write_vfunc(0x33, EditorIDRecorder<T>::thunk);
		}
	}

	// ------------------------------------------------------------------ FadeRules.cpp: rule files
	void                                   LoadRules();  // at data load, after the settings
	[[nodiscard]] Verdict                  Judge(const RE::TESObjectWEAP* a_weapon, const RE::EnchantmentItem* a_ench, const Settings& a_settings);
	void                                   ForgetRuleMatches();  // a game loads: the forms made in play are new ones
	[[nodiscard]] std::uint32_t            RulesGeneration();    // changes whenever the rule files are read again
	[[nodiscard]] std::size_t              RuleCount();
	[[nodiscard]] std::size_t              RuleFileCount();
	[[nodiscard]] std::vector<std::string> RuleProblems();  // a copy: the menu reads it while a reload may run

	// ------------------------------------------------------------------ FadeSpells.cpp: spells in hand
	[[nodiscard]] Kind KindOf(const RE::MagicItem* a_item);  // its strongest effect's kind; Element::kOther, no school, for none
	// a soul trap effect: the Soul Trap archetype, or - as every vanilla one is - a Script effect whose editor ID says so
	[[nodiscard]] bool IsSoulTrap(const RE::EffectSetting* a_effect);
	// this frame, every tracked actor's spell hands: their lights and glow follow the caster's magicka (from UpdateHands,
	// under its lock)
	struct SpellHand
	{
		const RE::SpellItem* spell{ nullptr };
		float                fraction{ 1.0f };
		float                current{ 0.0f }, max{ 0.0f };  // magicka
		RE::NiAVObject*      nodes[2]{};                    // the hand's magic node, third and first person
		RE::NiPointLight*    casterLight{ nullptr };        // the game's casting light (the effect's hand light)
	};
	[[nodiscard]] bool ReadSpellHand(RE::Actor* a_actor, bool a_left, SpellHand& a_out);
	// where a spell hand's lights are: its magic node in each view the actor has (the player's first person too), and the game's
	// casting light - the fading pass and Lights by Item reach the same lights
	void SpellHandLights(RE::Actor* a_actor, bool a_left, RE::NiAVObject* (&a_nodes)[2], RE::NiPointLight*& a_casterLight);

	// ------------------------------------------------------------------ FadeCharge.cpp: what a hand holds
	struct Reading
	{
		const RE::TESObjectWEAP*   weapon{ nullptr };
		const RE::EnchantmentItem* ench{ nullptr };
		const RE::ExtraDataList*   instance{ nullptr };  // which copy of the weapon: a swap to an identical one is still a swap
		bool                       tracked{ false };     // false: nothing of ours to do with this hand
		bool                       bound{ false };
		float                      fraction{ 1.0f };
		float                      current{ 0.0f }, max{ 0.0f };  // charge points, or seconds for a bound weapon
	};

	[[nodiscard]] Reading ReadHand(RE::Actor* a_actor, bool a_left);

	// ------------------------------------------------------------------ FadeLights.cpp: the lights on a weapon
	// the weapon's 3D in a biped: a left-hand weapon sits in the shield slot, a right-hand one in its type's slot (main thread)
	[[nodiscard]] RE::NiAVObject* WeaponPart(RE::Actor* a_actor, bool a_firstPerson, bool a_left, const RE::TESForm* a_weapon);
	void                          UpdateHands(float a_delta);  // every frame, from the player's update
	// the roots an actor's weapon-hand enchantment effects hang their lights on - the fading pass's own, or (Fading off, the
	// weapon exempt) Lights by Item's own record the effect hooks keep (under its lock: Lights by Item asks from inside the pass)
	[[nodiscard]] std::vector<RE::NiAVObject*> EffectRootsOf(const RE::Actor* a_actor, bool a_left);
	// a light a plugin hangs for a hand away from that hand's own 3D (Illuminated's first-person weapon and art lights sit on the
	// body node: the arms are not posed there) - the fading pass and Lights by Item reach it as the hand's this frame (main thread)
	void                                              NoteHandLight(const RE::Actor* a_actor, bool a_left, RE::NiPointLight* a_light);
	[[nodiscard]] const std::vector<RE::NiAVObject*>& NotedLightsOf(const RE::Actor* a_actor, bool a_left);               // this frame's, under gLock (Items::Apply)
	void                                              AfterReferenceEffect(RE::ReferenceEffect* a_effect, bool a_own3D);  // after the effect's own update; a_own3D: search
																														  // the effect's own model too
	void AfterCaster(RE::ActorMagicCaster* a_caster);                                                                     // after a hand caster's update: Lights by Item's choices again on its lights
	void AfterShaderEffect(RE::ShaderReferenceEffect* a_effect);                                                          // after the game animates an enchantment's shader
	void ReapplyAll();                                                                                                    // after a cell's light animation: this frame's numbers again, every hand's
	void Rebase(RE::NiPointLight* a_light, float a_ratio, float a_now);                                                   // the plugin's Brightness moved a steady light
	void ReleaseAll();                                                                                                    // every light back as we found it
	// Lights by Item wrote this light (FadeItems.cpp, under gLock): the fading pass and Brightness keep their own base
	void NoteItemWrite(RE::NiPointLight* a_light, float a_fadeBefore, float a_fadeAfter, const RE::NiColor& a_colorBefore,
		const RE::NiColor& a_colorAfter);

	struct HandView
	{
		std::string actor, weapon, enchantment, why;
		bool        left{ false }, bound{ false }, exempt{ false }, spell{ false }, player{ false };
		float       fraction{ 1.0f }, current{ 0.0f }, max{ 0.0f };
		float       hudFraction{ 1.0f };  // what the HUD's gem shows (a bound weapon: its spell's whole time - Glow::HudFraction)
		float       brightness{ 1.0f }, reach{ 1.0f }, cool{ 0.0f };
		std::size_t lights{ 0 }, roots{ 0 };
		float       chargeAV{ -1.0f };  // the game's own RightItemCharge / LeftItemCharge, to compare
	};
	[[nodiscard]] std::vector<HandView> Snapshot();  // the Debug page and DevBench (takes the per-frame lock)
	struct LightNow
	{
		float fade{ 0.0f }, base{ 0.0f }, radius{ 0.0f };
		bool  frozen{ false };
	};
	[[nodiscard]] std::vector<LightNow>                LightsNow();  // every light being scaled: what it held after our last write, and its base
	[[nodiscard]] std::size_t                          ScaledLightCount();
	[[nodiscard]] std::size_t                          DimmedGlowCount();  // enchantment shaders and art swirls being dimmed
	[[nodiscard]] std::vector<std::pair<float, float>> ArtNow();           // each art mesh's emissive scale: its base, and what we wrote
	[[nodiscard]] std::size_t                          FrozenLightCount();

	// The HUD (the charge gems, and the bars offered to Reticle Arcs), published once a frame at the end of UpdateHands: the
	// render thread and Reticle Arcs' updates read this small copy and never wait on the per-frame pass. [0] is the player's
	// left hand, [1] the right.
	struct HudHand
	{
		bool      shown{ false };  // a tracked spell or weapon in that hand that fades (not left alone)
		bool      spell{ false }, bound{ false };
		float     fraction{ 1.0f };
		bool      lit{ false };  // a light of that hand was found this frame: `color` is its color (top channel 1)
		Glow::Rgb color{};
	};
	struct HudState
	{
		bool                   gems{ false };       // the gems' switch (and the module is on)
		bool                   drawn{ false };      // the player's weapons or spells are out
		bool                   hudHidden{ false };  // a HUD toggle mod has hidden the game's HUD
		float                  gemSize{ 1.0f }, gemOpacity{ 0.9f };
		bool                   gemPercent{ true };
		bool                   gemFull{ false };  // a gem at full charge too (the HUD page's Show at full charge)
		float                  low{ 0.15f };      // the sputter point: under it a Reticle Arcs bar reads low
		std::array<HudHand, 2> hands{};
		bool                   shoutLit{ false };  // the shout the player has ready carries a light: `shoutColor` is its color
		Glow::Rgb              shoutColor{};
	};
	[[nodiscard]] HudState Hud();
	// a game menu is open (inventory, map, dialogue, the console ...): the HUD elements hide, as the game's own HUD does
	// (his report). Kept by a menu open / close watcher (FadeMenu.cpp), read by the render thread
	[[nodiscard]] bool MenusHideHud() noexcept;

	// ------------------------------------------------------------------ FadeTuning.cpp: the all-in-one advanced settings file
	// (his rule: every value worth tuning that the menu does not show, one file, a section per category). A value is
	// registered with its default and range before Load (data load); Load reads the file, then writes it back whole. A key
	// that was renamed names its old name (a_was): a file that still carries the old line keeps its value, written back under
	// the new name (the rule for any renamed setting: write the new, read both).
	namespace Tuning
	{
		void Register(std::string_view a_section, std::string_view a_key, float a_default, float a_lo, float a_hi, std::string_view a_note,
			std::atomic<float>& a_value, std::string_view a_was = {});
		void Load();
		// the fading module's own: [Fading] (Glow::Tuning's values the menu does not show) and [HUD] (where the HUD sits)
		inline std::atomic<float>  gMinReach{ 0.35f }, gPulseSeconds{ 0.30f }, gFlareSeconds{ 0.70f }, gFallSeconds{ 0.15f }, gRiseSeconds{ 0.35f };
		inline std::atomic<float>  gGemDistance{ 215.0f }, gGemHeight{ 62.0f };
		[[nodiscard]] inline float Get(const std::atomic<float>& a_v) noexcept { return a_v.load(std::memory_order_relaxed); }
	}

	// FadeItems.cpp: Lights by Item (his order)
	namespace Items
	{
		void                                     BuildCatalog();                                           // data load, main thread: every spell, staff and enchanted / bound weapon, for the search
		void                                     Load();                                                   // data load: the choices in Mod::kItemsPath
		void                                     Apply();                                                  // every frame, main thread, last in the fading pass: each choice onto the lights at its hand
		void                                     Reapply();                                                // under gLock, after ReapplyAll wrote the lights again: this frame's choices once more
		void                                     ReapplyUnder(RE::NiAVObject* a_root);                     // under gLock, after Light Placer animated an effect's or a caster's lights: the same under one root
		[[nodiscard]] bool                       Holding() noexcept;                                       // a choice is on a light now (any thread)
		[[nodiscard]] std::optional<RE::NiColor> ChoiceColor(RE::FormID a_id);                             // main thread: an item's own color, black when off, none on Auto
		bool                                     SetFromText(RE::FormID a_id, std::string_view a_choice);  // "auto", "off" or "r,g,b", as the menu would
		void                                     Release();                                                // every light back as its owner left it (a load)
		void                                     Render();                                                 // the Choose Your Own Color list
	}

	// ------------------------------------------------------------------ FadeMenu.cpp
	void RegisterMenu();

	// ------------------------------------------------------------------ FadeArcs.cpp: the bars offered to Reticle Arcs
	void RegisterArcBars();  // SKSE's post-load: Reticle Arcs takes add-on bars only then (its log says how many it took)

	// ------------------------------------------------------------------ FadeDevBench.cpp
	void OfferToDevBench();
	void RulesLoaded();

	// ------------------------------------------------------------------ FadeMain.cpp: called by the plugin's main.cpp
	void OnPluginLoad();  // SKSE's post-load (every plugin loaded, the game's data not read yet)
	void OnPostLoad();
	// Reticle Arcs is loaded: the module's bars are offered to it (FadeArcs.cpp) - the module draws no reticle of its own (his
	// order 2026-10-19). Read once at OnPluginLoad; any thread may ask
	[[nodiscard]] bool OtherReticleLoaded();
	void               OnDataLoaded();
	void               OnGameLoading();  // a save is about to load, or a new game starts
}
