// The shield's ward at once, shared by our lighting mods (master: Tools\Shared Plugin Code, every copy kept equal by shared code sync.py)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.
//
// A worn ward comes up through its enchantment, whose effects carry the condition IsBlocking == 1; the game re-checks an active effect's conditions only on
// its own timer (fActiveEffectConditionUpdateInterval; measured: the dome showed 0.1-1.0 s after the block). So when her block starts or ends, each of her
// active effects from an ENCHANTMENT whose conditions ask IsBlocking is checked at once. At that event the graph's IsBlocking already reads true, so once is
// enough. It runs with or without Dynamic Wards: the wait is the game's, not the dome's.
// Every graph event of hers passes her own sink, so one hook on its slot needs nothing attached to each graph or attached again when one is rebuilt. The
// method is the one valhallaCombat (D7ry, BSD-3-Clause) and TK Dodge RE (max-su-2019, MIT) use. A plugin that keeps it off VR says so where it calls Install.

#pragma once

#include "LightKit.h"

#include <vector>

namespace ShieldWardWatch
{
	namespace detail
	{
		// the shield wards: Spellbreaker, Artificer's two extra Spellbreaker effects, the Crusader shields, any mod's shield that works the same way
		[[nodiscard]] inline bool WaitsForBlock(const RE::ActiveEffect* a_effect)
		{
			using Flag = RE::ActiveEffect::Flag;
			if (!a_effect || !a_effect->effect || !a_effect->spell || a_effect->flags.none(Flag::kHasConditions) ||
				a_effect->flags.any(Flag::kDispelled) || a_effect->spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
				return false;
			}
			for (const auto* item = a_effect->effect->conditions.head; item; item = item->next) {
				if (item->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kIsBlocking) {
					return true;
				}
			}
			return false;
		}

		class ShieldWards : public RE::MagicTarget::ForEachActiveEffectVisitor
		{
		public:
			RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
			{
				if (WaitsForBlock(a_effect)) {
					found.push_back(a_effect);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			std::vector<RE::ActiveEffect*> found;
		};

		// main thread: collected first, then checked, so the list is never walked while an effect changes state
		inline void Recheck()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* target = player ? player->AsMagicTarget() : nullptr;
			if (!target) {
				return;
			}
			ShieldWards wards;
			target->VisitEffects(wards);
			for (auto* effect : wards.found) {
				effect->EvaluateConditions(0.0f, true);
			}
		}

		// "blockStart" is the input event's name and never reaches the graph. Held for the session; the game's strings are pooled, so a tag compares by pointer
		inline RE::BSFixedString gBlockStart;
		inline RE::BSFixedString gBlockEnd;

		// off the main thread, on every graph event of hers: the two compares come first, and the effects are read on the main thread
		struct GraphEvent
		{
			static RE::BSEventNotifyControl thunk(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_self, const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
			{
				const auto result = func(a_self, a_event, a_source);
				if (a_event && (a_event->tag == gBlockStart || a_event->tag == gBlockEnd)) {
					LightKit::Guarded("block watch", []() {
						if (auto* tasks = SKSE::GetTaskInterface()) {
							tasks->AddTask([]() { LightKit::Guarded("shield ward check", Recheck); });
						}
					});
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	// data load, main thread
	inline void Install()
	{
		detail::gBlockStart = "blockStartOut";
		detail::gBlockEnd = "blockStop";
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[2] };
		detail::GraphEvent::func = vtbl.write_vfunc(0x1, detail::GraphEvent::thunk);
		SKSE::log::info("wards: her block events are followed - the shield wards are checked as a block starts and ends");
	}
}
