// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// What a hand holds, and how full it is.
//
// Charge lives on the weapon's own copy in the inventory (its ExtraDataList):
//   ExtraCharge       the charge left; absent means full
//   ExtraEnchantment  an enchantment made at the table, with its maximum charge; absent means the weapon record's own
// For a weapon enchanted in its record, the maximum is the record's EAMT (TESEnchantableForm::amountofEnchantment).
// The equipped copy is the extra list carrying ExtraWorn (right hand) or ExtraWornLeft (left hand).
//
// A bound weapon has no charge: its fraction is the time its spell has left (Glow::BoundFraction), read from the
// actor's active effect whose archetype is Bound Weapon and whose associated item is that weapon.

#include "Fade.h"

namespace Fade
{
	namespace
	{
		// the extra list of the copy in this hand; nullptr if the entry has none (a weapon never touched by the game)
		const RE::ExtraDataList* ListInHand(const RE::InventoryEntryData* a_entry, bool a_left)
		{
			if (!a_entry || !a_entry->extraLists) {
				return nullptr;
			}
			const auto               worn = a_left ? RE::ExtraDataType::kWornLeft : RE::ExtraDataType::kWorn;
			const RE::ExtraDataList* first = nullptr;
			for (const auto* list : *a_entry->extraLists) {
				if (!list) {
					continue;
				}
				if (list->HasType(worn)) {
					return list;
				}
				if (!first) {
					first = list;
				}
			}
			return first;
		}

		struct BoundTime
		{
			float elapsed{ 0.0f }, duration{ 0.0f };
			bool  found{ false };
		};

		class FindBound : public RE::MagicTarget::ForEachActiveEffectVisitor
		{
		public:
			FindBound(const RE::TESObjectWEAP* a_weapon, bool a_left) :
				weapon(a_weapon), left(a_left) {}

			RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
			{
				using Flag = RE::ActiveEffect::Flag;
				const auto* base = a_effect && a_effect->effect ? a_effect->effect->baseEffect : nullptr;
				if (!base || base->GetArchetype() != RE::EffectArchetypes::ArchetypeID::kBoundWeapon ||
					base->data.associatedForm != weapon || a_effect->flags.any(Flag::kInactive, Flag::kDispelled)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// two of the same bound weapon, one in each hand: the effect cast from this hand is this hand's
				const auto src = a_effect->castingSource;
				const bool handMatches = src == (left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
				if (!time.found || handMatches) {
					time = { a_effect->elapsedSeconds, a_effect->duration, true };
				}
				return handMatches ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
			}

			const RE::TESObjectWEAP* weapon;
			bool                     left;
			BoundTime                time;
		};
	}

	Reading ReadHand(RE::Actor* a_actor, bool a_left)
	{
		Reading r;
		if (!a_actor) {
			return r;
		}
		const auto* entry = a_actor->GetEquippedEntryData(a_left);
		const auto* weapon = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
		if (!weapon) {
			return r;
		}
		if (a_left && (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow())) {
			return r;  // held in both hands: the right hand's reading covers it
		}
		r.weapon = weapon;
		r.instance = ListInHand(entry, a_left);

		if (weapon->IsBound()) {
			r.bound = true;
			FindBound visitor(weapon, a_left);
			if (auto* target = a_actor->GetMagicTarget()) {
				target->VisitEffects(visitor);
			}
			if (!visitor.time.found) {
				return r;  // summoned by something other than a spell's active effect: nothing to count down
			}
			r.tracked = true;
			r.current = (std::max)(visitor.time.duration - visitor.time.elapsed, 0.0f);
			r.max = visitor.time.duration;
			// its fraction is the time left over the fade window a rule may change: FadeLights.cpp works it out with the verdict
			return r;
		}

		const auto* xEnch = r.instance ? r.instance->GetByType<RE::ExtraEnchantment>() : nullptr;
		r.ench = xEnch && xEnch->enchantment ? xEnch->enchantment : weapon->formEnchanting;
		if (!r.ench) {
			return r;  // not enchanted
		}
		r.tracked = true;
		r.max = xEnch && xEnch->enchantment ? static_cast<float>(xEnch->charge) : static_cast<float>(weapon->amountofEnchantment);
		// the game's own value: the entry in her hand shows no charge until the next equip when a weapon's first hit made one (measured)
		auto* values = a_actor->AsActorValueOwner();
		r.current = values ? values->GetActorValue(a_left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge) : r.max;
		// a maximum of 0 is an enchantment that never spends charge: always full
		r.fraction = r.max > 0.0f ? Glow::Clamp01(r.current / r.max) : 1.0f;
		return r;
	}
}
