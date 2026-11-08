// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The game's own "it happened" moments, so a pass that waited for them every frame runs when they come: SKSE's weapon draw
// events (the way RE::Light by Truman sees a draw or sheathe - its disableLights.cpp, GPL-3.0, written here from SKSE's
// event, not copied) and her cell changes. A sink only notes what happened; the main thread's passes act on it.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// may come from an animation thread: only a form id and a counter are handed on
		class Draws final : public RE::BSTEventSink<SKSE::ActionEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				NoteSweep();
				LightKit::Guarded("draw event", [a_event]() {
					using T = SKSE::ActionEvent::Type;
					const auto type = a_event && a_event->actor ? a_event->type.get() : T::kWeaponSwing;
					if (type == T::kBeginSheathe || type == T::kEndSheathe) {
						NoteOptionsLook();  // the sheathed net looks at the enchantment lights again (Options.cpp)
						return;
					}
					if (type != T::kBeginDraw && type != T::kEndDraw) {
						return;
					}
					NoteOptionsLook();
					NoteDrawing(a_event->actor->GetFormID());
					if (a_event->actor->IsPlayerRef()) {
						WatchWeaponLights();
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class Cells final : public RE::BSTEventSink<RE::BGSActorCellEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				if (a_event && a_event->flags.get() == RE::BGSActorCellEvent::CellFlag::kEnter) {
					WatchWeaponLights();
					NoteOptionsLook();
					NoteArtStarted();  // an art left behind comes back with its cell, with no start of its own
					NoteSweep();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// lights leave with an actor's 3D, and an actor may arrive with its weapon out and an enchantment light on it
		void Note3D(RE::Actor*, bool a_in)
		{
			NoteSweep();
			Fade::NoteActor3D(a_in);
			if (a_in) {
				NoteActorLoaded();
				NoteOptionsLook();
				NoteArtStarted();  // an art on it comes back with its 3D, with no start of its own
			}
		}

		// An actor's 3D in and out, at its own two slots: the game's load event names only a reference with a script or in an alias (read in
		// the 1.6.1170 exe). A queued load returns its body at the first on a loader thread and the game stores it later; a null at the
		// second is the body let go. Any thread: only noted
		template <class T>
		struct Actor3D
		{
			static RE::NiAVObject* LoadThunk(T* a_this, bool a_backgroundLoading)
			{
				auto* root = load(a_this, a_backgroundLoading);
				if (root) {
					LightKit::Guarded("actor 3D in", [a_this]() { Note3D(a_this, true); });
				}
				return root;
			}
			static void SetThunk(T* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
			{
				set(a_this, a_object, a_queue3DTasks);
				LightKit::Guarded("actor 3D set", [a_this, a_object]() { Note3D(a_this, a_object != nullptr); });
			}
			static inline REL::Relocation<decltype(LoadThunk)> load;
			static inline REL::Relocation<decltype(SetThunk)>  set;
			static void                                        Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
				load = vtbl.write_vfunc(0x6A, LoadThunk);
				set = vtbl.write_vfunc(0x6C, SetThunk);
			}
		};
	}

	void WatchEvents()
	{
		static Draws draws;
		static Cells cells;
		Actor3D<RE::Character>::Install();
		Actor3D<RE::PlayerCharacter>::Install();
		auto* actions = SKSE::GetActionEventSource();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (actions) {
			actions->AddEventSink(&draws);
		}
		if (player) {
			player->AsBGSActorCellEventSource()->AddEventSink(&cells);
		}
		if (!actions || !player) {
			// without them nothing tells the held lights of a draw: they look only at an actor's load and a game's load (Held.cpp)
			SKSE::log::warn("events: weapon draws {}, her cell changes {}", actions ? "watched" : "NOT watched", player ? "watched" : "NOT watched");
			return;
		}
		SKSE::log::info("events: weapon draws and her cell changes watched");
	}
}
