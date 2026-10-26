// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// What the files share. The file map is at the top of main.cpp.

#pragma once

#include "LightKit.h"

namespace Plugin
{
	// ------------------------------------------------------------------ Data.cpp: what the installed options light
	//
	// Every layer the installer put down (Core, and each ticked option) brings one small text file into
	// Data\SKSE\Plugins\RELightSpellLightingNG\. The build writes them; this reads them once, when the game has loaded its
	// data. They say which objects this mod lights (the sliders scale those and nothing else), which layers are
	// tick-box options (the menu draws a switch for each), the sprays and beams RE::Light lights (ours for the menu),
	// the hand lights, the held lights, and the color an art pick gives the objects it recolors.
	inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

	// std::string keys looked up by std::string_view without a temporary string
	struct StringHash
	{
		using is_transparent = void;
		[[nodiscard]] std::size_t operator()(std::string_view a_text) const noexcept { return std::hash<std::string_view>{}(a_text); }
	};
	template <class T>
	using StringMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

	using LightKit::Lower;     // LightMath.h: the one lower-casing helper
	using LightKit::Relaxed;   // a value one thread writes and another reads (LightKit.h)
	using LightKit::SameText;  // and the one case-blind compare

	// the mod's own values in the advanced settings file (his rule, Fade::Tuning; registered in main.cpp)
	inline std::atomic<float> gNearby{ 2800.0f };          // [Lights] NearbyDistance: Hand lights for - Everyone nearby
	inline std::atomic<float> gGroundLightsOn{ 1.0f };     // [Vanilla and ENB] GroundLights
	inline std::atomic<float> gGroundLightVRHand{ 0.0f };  // [Vanilla and ENB] GroundLightAtVRHand - VR only (Illuminated's)

	struct Option
	{
		std::string download, name, id;  // id: "Spells - Runes", the settings file's key
		std::string category, author;    // a patch's place on the Patches page (`menugroup`); author empty: listed on its own
		std::string desc;                // what it lights, broadly (`desc`), shown when the mouse is over its switch
		bool        weapons{ false };    // it is a weapon option (`download`): the Weapon lights setting puts it out
		int         order{ 0 };          // a later layer overrides an earlier one
		int         menu{ 0 };           // where it sits in the menu, which is NOT the override order
		bool        switchable{ false };
		// its switch: flipped by the menu (render thread), read every frame and by the 3D-load hooks (loader threads)
		Relaxed<bool> on{ true };
		std::size_t   meshes{ 0 }, bases{ 0 }, streams{ 0 };
		// counted every frame on the main thread, shown in the menu (render thread)
		Relaxed<std::size_t> lit{ 0 }, heldOut{ 0 };
	};

	struct Stream
	{
		std::string  key, node;  // node "-": the light sits at the projectile's own origin
		RE::NiColor  color{ 1.0f, 1.0f, 1.0f };
		float        fade{ 1.0f }, radius{ 133.0f }, size{ 2.5f }, cutoff{ 0.3f };
		RE::NiPoint3 at;  // where the light sits on the mesh (a held light; a stream's is not read - RE::Light lights it)
		int          order{ 0 };
		std::size_t  option{ kNone };
	};

	// one hand light, as one layer lights it: the layer's own light for the casting-art mesh `key`
	struct Hand
	{
		std::string  key;
		RE::NiColor  color{ 1.0f, 1.0f, 1.0f };
		std::uint8_t rgb[3]{ 255, 255, 255 };
		float        fade{ 1.0f }, radius{ 133.0f }, size{ 2.0f }, cutoff{ 0.2215f };
		bool         inverseSquare{ true }, portalStrict{ true };
		int          order{ 0 };
		std::size_t  option{ kNone };
	};

	// a hand light's Dynamic Lighting: a Pulse (breathe) or a Flicker (crackle), run on our hand light (Brightness.cpp)
	struct HandFx
	{
		bool  flicker{ false };
		float perSecond{ 0.0f }, intensity{ 0.0f };
		int   order{ 0 };
	};

	// RE::Light marks every light that moves with its owner (torches, spell lights) `fadeAmount = 4`: its flicker prevention
	// (IsLightAffectingSurface, disableLights.cpp) lets such a light reach every surface instead of only the surfaces whose
	// seven closest lights it was among when they were first counted. Our hand and held lights never pass through
	// RE::Light, so they carry the mark themselves - without it they lit only actors with flicker prevention on (a user's
	// report: "some magic doesn't work in 1st person")
	inline constexpr float kMovingLightMark = 4.0f;

	// RE::Light's marks on a light it made: its name starts "RL" (a config's light, LightManager.cpp), and an enchantment
	// light also carries fadeAmount 5 (ShaderReferenceEffect::Init; its sheathe handler reads the same two)
	[[nodiscard]] inline bool IsReLightLight(const RE::NiLight* a_light) noexcept
	{
		const char* n = a_light ? a_light->name.c_str() : nullptr;
		return n && n[0] == 'R' && n[1] == 'L';
	}
	[[nodiscard]] inline bool IsEnchantLight(const RE::NiLight* a_light) noexcept
	{
		return IsReLightLight(a_light) && a_light->fadeAmount == 5.0f;
	}

	// the build's cutoff: K * fade / (reach² + size²), clamped where Community Shaders reads one (LightKit.h)
	using LightKit::CutoffFor;
	using LightKit::kK;

	// ------------------------------------------------------------------ Hooks.cpp (out of main.cpp since 2.2, as Illuminated's main.cpp)
	void FindSpellBases();          // data load, before InstallLateHooks: what counts as magic for "Lights off while sneaking"
	void InstallSpellLightHooks();  // SKSE's post-load, every plugin loaded: the spell-light call sites (ours before RE::Light's)
	void InstallLateHooks();        // data load: the 3D loads, the player update, effect updates and cell animations

	// ------------------------------------------------------------------ Lighting.cpp (Illuminated's, the same tests)
	// 🔁 HIS WORD: "relight casting light is not working ... please re add the enb and cs and vanilla versions but
	// don't make them manually selectable, just like illuminated it will auto detect" - which lighting the game draws with,
	// read once at data load: Community Shaders (its inverse square shader, Light Limit Fix's built-in one, or
	// CommunityShaders.dll loaded - 2.2), else an ENB that is running or its settings in the game folder (2.2: never over
	// Community Shaders), else Vanilla. Only where a light cannot work without it: the hand lights (drawn plain off Community
	// Shaders), their ground light (GroundLights.cpp), the held and stream lights. RE::Light stays the main driver of the rest.
	enum class Lighting : int
	{
		kShaders = 0,
		kEnb = 1,
		kVanilla = 2
	};
	void                      ReadLighting();  // data load, after the settings, before the hand lights
	[[nodiscard]] Lighting    LightingFound();
	[[nodiscard]] const char* LightingName(Lighting a_lighting);
	// the lights are written in Community Shaders' inverse square words (a flag and the cutoff in two words of a light - LightKit.h
	// has how); off it they are drawn plain (LightKit::PlainOf) and those words are ambient color
	[[nodiscard]] bool        InverseSquare();
	[[nodiscard]] std::string LightingReport();  // the same as JSON, for DevBench
	namespace Isl = LightKit::Isl;

	// GroundLights.cpp: a twin of each hand's casting light that lights the ground, off Community Shaders (Illuminated's)
	void                      TickGroundLights();
	void                      DropGroundLights();  // a game loads, or a new one starts: the twins and the hands they follow are the old game's
	[[nodiscard]] std::size_t GroundLightCount();
	[[nodiscard]] std::size_t GroundLightsMade();
	// Hooks.cpp: a spell light the mod keeps dark now (a switched-off option, a hand light held out) - its twin too
	[[nodiscard]] bool WantedDark(RE::NiLight* a_light);
	// Hooks.cpp: how far the sneak fade has her lights now, 1 = full, down to "Sneaking brightness" (any thread)
	[[nodiscard]] float SneakLevel();
	// Hooks.cpp: a light on the player or on what she cast - only those fade while she sneaks (any thread)
	[[nodiscard]] bool CastByPlayer(const RE::TESObjectREFR* a_ref);

	void                                                LoadData();
	[[nodiscard]] std::vector<Option>&                  Options();
	[[nodiscard]] const std::vector<std::size_t>&       OptionsInMenuOrder();  // by `menu`, never the alphabetical file order
	[[nodiscard]] std::size_t                           DataFiles();
	[[nodiscard]] std::size_t                           OptionOf(const RE::TESForm* a_base);                  // kNone when this mod does not light it
	[[nodiscard]] std::size_t                           OptionOfShader(const RE::TESEffectShader* a_shader);  // an enchantment's, by editor ID
	[[nodiscard]] bool                                  OptionLit(std::size_t a_option);                      // its switch is on, and Weapon lights if it is a weapon's
	[[nodiscard]] const Stream*                         StreamOf(const RE::TESForm* a_base);                  // nullptr unless it is one of our streams
	[[nodiscard]] const RE::NiColor*                    TintOf(const RE::TESForm* a_base);                    // an art pick's color, or nullptr
	[[nodiscard]] const RE::NiColor*                    HandColorOf(const RE::TESForm* a_base);               // game art: its element's hand color
	[[nodiscard]] std::string                           MeshKey(std::string_view a_path);
	[[nodiscard]] std::string                           PathKey(std::string_view a_path);
	[[nodiscard]] const StringMap<std::vector<Hand>>&   Hands();                           // hand key -> the layers that light it, highest order first
	[[nodiscard]] const std::unordered_set<RE::FormID>& SprayLightRecords();               // spray light records, lit through our configs
	[[nodiscard]] const std::unordered_set<RE::FormID>& BeamLightRecords();                // beam light records (2.3: raised copies, Streams.cpp)
	[[nodiscard]] std::optional<RE::Color>              RecordColor(RE::FormID a_record);  // his color for a spray / beam record, if the data names one
	[[nodiscard]] const HandFx*                         HandFxOf(std::string_view a_key);  // nullptr: this hand does not breathe
	// nullptr unless we light this weapon in the hand: its form first, then (first person) its first-person model, then its model
	[[nodiscard]] const Stream* HeldOf(const RE::TESObjectWEAP* a_weapon, bool a_firstPerson);
	// nullptr unless a `body` line lights this art model on an actor (BodyLights.cpp): its full path key, then its bare name
	[[nodiscard]] const Stream* BodyOf(const RE::BGSArtObject* a_art);

	// ------------------------------------------------------------------ Keep.cpp: the one place a game light is held
	using GoneLights = std::unordered_set<const RE::NiLight*>;
	void                      KeepLight(RE::NiLight* a_light);  // any thread: alive for our lists while the game has it
	void                      SweepKeptLights();                // main thread, first each frame: what left the game is forgotten, then freed
	[[nodiscard]] std::size_t KeptLights();
	void                      ForgetSpellLights(const GoneLights& a_gone);   // main.cpp (it calls Options.cpp's too)
	void                      ForgetOptionLights(const GoneLights& a_gone);  // Options.cpp: called by ForgetSpellLights only
	void                      ForgetHandLights(const GoneLights& a_gone);    // HandLights.cpp
	void                      ForgetSliderLights(const GoneLights& a_gone);  // Brightness.cpp

	// ------------------------------------------------------------------ HandLights.cpp: lights on the caster's hands
	void                      MakeHandLights();                               // once, after the data and the settings are read
	void                      RefindHandLights();                             // when a save loads: casting art may have changed
	void                      ApplyHandLights(bool a_log);                    // after any switch that decides who lights a hand
	[[nodiscard]] const Hand* HandOfLight(const RE::TESObjectLIGH* a_light);  // nullptr unless it is one of ours, in use
	void                      DressHandLight(RE::NiLight* a_light, const Hand& a_hand);
	void                      NoteHandLight(RE::NiLight* a_light, const std::string& a_key);  // a lit hand light, so a menu change reaches it (main thread)
	[[nodiscard]] bool        HandLightHeldOut(const RE::NiLight* a_light);                   // put out by a switch until the next cast
	[[nodiscard]] std::size_t HandLightsMade();
	[[nodiscard]] std::size_t HandEffects();
	[[nodiscard]] std::size_t AutoHandEffects();  // spells lit by an automatic hand light now

	// ------------------------------------------------------------------ Settings.cpp: the settings file
	void                LoadSettings(bool a_log = true);  // a_log false: the quiet early read before the data (main.cpp)
	void                SaveSettings();
	[[nodiscard]] int   BrightnessPercent();
	void                SetBrightnessPercent(int a_percent);
	[[nodiscard]] float Brightness();
	[[nodiscard]] int   ReachPercent();
	void                SetReachPercent(int a_percent);
	[[nodiscard]] float Reach();
	[[nodiscard]] bool  SneakOn();
	void                SetSneakOn(bool a_on);
	[[nodiscard]] int   SneakBrightness();  // percent, 0 to 100
	void                SetSneakBrightness(int a_percent);
	[[nodiscard]] bool  RunesFadeOn();  // her runes fade with her lights while she sneaks (Hooks.cpp)
	void                SetRunesFadeOn(bool a_on);
	[[nodiscard]] bool  InvisibleFadeOn();  // her lights fade out while she is invisible or carries a shadow sign (Hooks.cpp)
	void                SetInvisibleFadeOn(bool a_on);
	[[nodiscard]] bool  HandLightsOn();
	void                SetHandLightsOn(bool a_on);
	[[nodiscard]] bool  WeaponLightsOn();
	void                SetWeaponLightsOn(bool a_on);
	[[nodiscard]] int   WardColor();  // 0 vanilla blue, 1 white (Wards.cpp)
	void                SetWardColor(int a_color);
	void                SetOptionOn(std::size_t a_index, bool a_on);
	// Illuminated's settings, ported (his "add everything from illuminated into relight ... sister mods"). The lighting is
	// never a pick here: it is found by itself (LightingFound, Lighting.cpp), as his later word asked
	[[nodiscard]] int  DimInDaylight();  // 0 off, 1 a little, 2 more
	void               SetDimInDaylight(int a_v);
	[[nodiscard]] int  HandLightsFor();  // 0 everyone, 1 everyone nearby, 2 player and followers, 3 player only
	void               SetHandLightsFor(int a_v);
	[[nodiscard]] bool AutoLightsOn();
	void               SetAutoLightsOn(bool a_on);
	[[nodiscard]] bool AllLightsOff();  // Performance: every light of this mod off - the options' lights, hand, held and ground lights
	void               SetAllLightsOff(bool a_off);
	[[nodiscard]] int  ArtPick(int a_group);  // the chosen art of a DataText::kPickGroups group; 0 = the game's own
	void               SetArtPick(int a_group, int a_v);
	void               SetDynamicLighting(bool a_on);
	[[nodiscard]] bool DynamicLighting();             // the saved choice (the menu): the RE::Light lights flicker as their configs say
	[[nodiscard]] bool DynamicLightingThisSession();  // the choice this session's data was read with - it applies at the next start
	void               FixDynamicLighting();          // main.cpp, before the data: the session takes the saved choice
	[[nodiscard]] bool PerformanceMode();             // the Performance page's Quality pick on Performance (his 2026-10-20 order)
	void               SetPerformanceMode(bool a_on);
	// every setting and switch as (key, value), and back - the menu's presets (Menu.cpp)
	[[nodiscard]] std::vector<std::pair<std::string, int>> SettingsSnapshot();
	void                                                   ApplySettingsSnapshot(const std::vector<std::pair<std::string, int>>& a_values);

	// main.cpp: "Hand lights for" (the light budget for big fights) leaves this actor out - no hand light, no held light
	[[nodiscard]] bool LeftOutByBudget(const RE::Actor* a_actor);

	// ------------------------------------------------------------------ Options.cpp: the switches
	// lights handed from a hook that may run on a loader thread to the main thread (an SKSE task holds them until then)
	using LightList = std::vector<RE::NiPointer<RE::NiLight>>;
	void UpdateOptionLights();  // every frame, after the sneaking pass (main thread)
	// on a 3D that has just loaded (any thread): a switched-off option's lights under it are put out at once and returned,
	// with that option in a_option, for AdoptOptionLights to take into the list on the main thread
	[[nodiscard]] LightList          CullOptionLightsUnder(RE::NiAVObject* a_root, std::size_t& a_option);
	void                             AdoptOptionLights(std::size_t a_option, const LightList& a_lights);  // main thread
	[[nodiscard]] bool               HeldOutForOption(const RE::NiLight* a_light);                        // main thread
	[[nodiscard]] RE::TESObjectREFR* ReferenceOf(RE::NiAVObject* a_obj);                                  // the reference a scene-graph object belongs to
	[[nodiscard]] std::size_t        EnchantOptionOf(const RE::NiLight* a_light);                         // kNone unless it is an enchantment light of ours
	// a flesh spell's light this frame where FleshFX's look wins on its shader: the hue it is drawn in (LightKit.h FleshFx),
	// else nullptr. HIS CALL: "do the changes proposed with the dim light in its own color" (main thread)
	[[nodiscard]] const RE::NiColor* FleshHueOf(const RE::NiLight* a_light);

	// ------------------------------------------------------------------ Brightness.cpp: our own sliders
	void UpdateBrightness(float a_delta);  // every frame, last; a_delta is the game's frame time
	// the fading module (Fade*.cpp) wrote this light's fade: taken only when it started from our value (never scaled twice)
	void NoteFadeWrite(const RE::NiPointLight* a_light, float a_before, float a_after);
	// the fading module wrote this light's reach (radius x and y): the same handshake as the fade, so the Reach slider and
	// the fading's "reach follows" never take each other's write for a new base and compound (the audit, 2026-10-14)
	void NoteReachWrite(const RE::NiPointLight* a_light, float a_before, float a_after);
	void RememberLight(RE::NiLight* a_light, const HandFx* a_fx = nullptr);
	// what this pass gives every light of ours, for the lights another file scales (Held.cpp): the daylight factor (looked
	// at every two seconds, main thread)
	[[nodiscard]] float DaylightFactor();

	// ------------------------------------------------------------------ Streams.cpp: sprays, beams, breath shouts
	void RaiseStreamLights();  // once, after the data is read and the lighting found, BEFORE MakeHandLights
	// a light record of ours (a spray's / bolt's raised copy, a beam record's other users' plain copy): made by the game's own
	// function, never RE::Light's (Hooks.cpp), and dressed by DressStreamLight - any thread
	[[nodiscard]] bool        IsOurLight(const RE::TESObjectLIGH* a_light);
	void                      DressStreamLight(RE::NiLight* a_light, const RE::TESObjectLIGH* a_record);
	[[nodiscard]] std::size_t RaisedLights();  // raised copies made
	[[nodiscard]] std::size_t PlainLights();   // plain copies made for the beam records' other users
	[[nodiscard]] bool        IsSprayLight(const RE::TESObjectLIGH* a_light);
	// the light RE::Light (or the game) makes for a projectile a `stream` line names: ours for the sliders
	[[nodiscard]] bool IsStreamObject(const RE::TESObjectREFR* a_ref);

	// ------------------------------------------------------------------ Rows.cpp: a row of small lights along a bolt / spray / cone
	struct RowRecipe
	{
		RE::NiColor color{ 1.0f, 1.0f, 1.0f };
		float       fade{ 1.0f }, reach{ 300.0f };  // the projectile's own light: the row's middle glows as it would right under it
		bool        beam{ true };                   // a bolt (along its model) - else a flame spray or cone (along its aim)
		std::size_t option{ kNone };                // its switch
	};
	void                      SetRowRecipes(std::unordered_map<RE::FormID, RowRecipe>&& a_recipes);  // once, data load (Streams.cpp)
	void                      QueueRow(RE::TESObjectREFR* a_ref);                                    // any thread: a projectile's 3D loaded - its row is hung next frame
	void                      UpdateRows();                                                          // every frame, main thread (the player update)
	void                      DropRows();                                                            // a game loads: the projectiles they hang on are the old game's
	[[nodiscard]] bool        IsRowLight(const RE::NiLight* a_light);                                // one of Rows.cpp's lights (any pass that walks the scene skips it)
	[[nodiscard]] std::size_t LiveRowLights();
	[[nodiscard]] std::size_t RowsMade();

	// ------------------------------------------------------------------ Held.cpp: a weapon's own light while it is drawn
	void                      UpdateHeldLights();  // every frame, after UpdateOptionLights
	[[nodiscard]] std::size_t LiveHeldLights();
	// one light of our own made and registered the ReLight way (main thread; nullptr when it could not be) - held and body lights
	[[nodiscard]] RE::BSLight* MakeOwnLight(const Stream& a_s, float a_fade, float a_reach, RE::NiNode* a_parent,
		RE::ShadowSceneNode* a_scene, const char* a_name, RE::NiPointLight*& a_made);
	// the sliders and the daylight onto such a light (fade and reach as the caller worked them out)
	void RetuneOwnLight(RE::NiPointLight* a_light, const Stream& a_s, float a_fade, float a_reach, bool a_reachMoved);
	// what a held or body light's file fade and radius are multiplied by: Brightness x daylight, Reach, and off Community
	// Shaders the plain drawing (fade x 1.14, reach x 178 / 133)
	struct OwnScale
	{
		float fade, reach;
	};
	[[nodiscard]] OwnScale OwnLightScale();

	// ------------------------------------------------------------------ BodyLights.cpp: the body glow while casting
	void                      UpdateBodyLights();  // every frame, after UpdateHeldLights
	void                      DropBodyLights();    // a game loads, or a new one starts: the art they hang on is the old game's
	void                      ForgetBodyArt();     // casting art changed (Dynamic Wards' ranked hand art): its list is learned again
	[[nodiscard]] std::size_t LiveBodyLights();
	[[nodiscard]] std::size_t BodyLightsMade();

	// ------------------------------------------------------------------ Wards.cpp: one ward, one dome, and its color
	void                                     ApplyWards(const char* a_why);  // one dome per ward and the color pick; nothing while Dynamic Wards is loaded
	[[nodiscard]] bool                       WardsSteppedDown();             // Dynamic Wards is loaded: the wards are its
	[[nodiscard]] bool                       StrangeWardsHandShows();        // Strange Wards installed and 360 Ward not: the game's ward hand shows
	[[nodiscard]] std::optional<RE::NiColor> StrangeWardsPick();             // its Amber / Sswaye Special color; none = its Blue pick

	// ------------------------------------------------------------------ VaerSwirls.cpp: VAER Reborn's swirl on Thaumaturgy's effects
	void VaerSwirls();  // once, after the data is read: Thaumaturgy's nine effect copies wear VAER's swirl art and shader

	// ------------------------------------------------------------------ Menu.cpp
	void RegisterMenu();
	void RegisterMenuTail();  // after the fading module's pages: the testing DLL's Diagnostics page last (nothing in the main DLL)

	// ------------------------------------------------------------------ DevBench.cpp: the settings and what was made, for a test bench
	void OfferToDevBench();  // SKSE's post-load: inspect kind=relight and relight.control, when DevBench is loaded (optional)
}
