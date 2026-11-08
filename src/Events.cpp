// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// The game's own "it happened" moments, so a pass that waited for them every frame runs when they come: SKSE's weapon draw
// events (the way RE::Light by Truman sees a draw or sheathe - its disableLights.cpp, GPL-3.0, written here from SKSE's
// event, not copied) and her cell changes. A sink only notes what happened; the main thread's passes act on it.

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
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void WatchEvents()
	{
		static Draws draws;
		static Cells cells;
		auto*        actions = SKSE::GetActionEventSource();
		auto*        player = RE::PlayerCharacter::GetSingleton();
		if (actions) {
			actions->AddEventSink(&draws);
		}
		if (player) {
			player->AsBGSActorCellEventSource()->AddEventSink(&cells);
		}
		if (!actions || !player) {
			// without them the held lights fall back to the half-second check alone (Held.cpp)
			SKSE::log::warn("events: weapon draws {}, her cell changes {}", actions ? "watched" : "NOT watched", player ? "watched" : "NOT watched");
			return;
		}
		SKSE::log::info("events: weapon draws and her cell changes watched");
	}
}
