// Shared by Illuminated and RELight - Spell Lighting NG (the master: Tools\Shared Plugin Code\CreatureSpells.h)
// GPL-3.0-or-later; see each plugin's LICENSE.txt.
//
// All creature lighting is Creatures' (Illuminated - Creature Lighting NG): a spell only creatures cast - the Spriggan Matron's
// swarm, the Ice Wraith's bite - gets no automatic hand light from us. An effect counts as a creature's when every magic item
// that carries it is a spell held only by creatures (a race or an NPC with the Creature or Animal keyword, in its spell list or
// cast by one of its attacks): no person carries
// it, no book teaches it, no shout, scroll, staff, enchantment or potion holds it. A spell nothing carries (a quest's, a
// script's, a perk's) stays the player's. Read once at data load from the records: no hook and no pass.

#pragma once

#include "LightKit.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CreatureSpells
{
	// Skyrim.esm's ActorTypeCreature and ActorTypeAnimal
	inline constexpr RE::FormID kCreatureKeyword = 0x13795;
	inline constexpr RE::FormID kAnimalKeyword = 0x13798;

	[[nodiscard]] inline std::unordered_set<const RE::EffectSetting*> CreatureOnlyEffects()
	{
		const auto* creature = RE::TESForm::LookupByID<RE::BGSKeyword>(kCreatureKeyword);
		const auto* animal = RE::TESForm::LookupByID<RE::BGSKeyword>(kAnimalKeyword);
		const auto  isCreature = [&](const RE::BGSKeywordForm* a_form) {
			return a_form && ((creature && a_form->HasKeyword(creature)) || (animal && a_form->HasKeyword(animal)));
		};
		std::unordered_set<const RE::SpellItem*> byCreature;
		std::unordered_set<const RE::SpellItem*> byPerson;
		const auto                               note = [&](const RE::TESSpellList::SpellData* a_list, bool a_creature) {
			if (!a_list) {
				return;
			}
			auto& into = a_creature ? byCreature : byPerson;
			for (std::uint32_t i = 0; a_list->spells && i < a_list->numSpells; ++i) {
				into.insert(a_list->spells[i]);
			}
			// a leveled list may hold further leveled lists: every spell at any depth counts, each list read once
			std::vector<const RE::TESLevSpell*>        open;
			std::unordered_set<const RE::TESLevSpell*> seen;
			for (std::uint32_t i = 0; a_list->levSpells && i < a_list->numlevSpells; ++i) {
				open.push_back(a_list->levSpells[i]);
			}
			while (!open.empty()) {
				const auto* lev = open.back();
				open.pop_back();
				if (!lev || !seen.insert(lev).second) {
					continue;
				}
				for (std::uint8_t k = 0; k < lev->numEntries; ++k) {
					auto* form = lev->entries[k].form;
					if (auto* spell = form ? form->As<RE::SpellItem>() : nullptr) {
						into.insert(spell);
					} else if (auto* inner = form ? form->As<RE::TESLevSpell>() : nullptr) {
						open.push_back(inner);
					}
				}
			}
		};
		// an attack can cast a spell of its own (ATKD): the Ice Wraith's bite lives in its race's attack data, not in its spell list
		const auto noteAttacks = [&](const RE::BGSAttackDataForm* a_form, bool a_creature) {
			const auto* map = a_form ? a_form->attackDataMap.get() : nullptr;
			if (!map) {
				return;
			}
			auto& into = a_creature ? byCreature : byPerson;
			for (const auto& entry : map->attackDataMap) {
				if (const auto& attack = entry.second; attack && attack->data.attackSpell) {
					into.insert(attack->data.attackSpell);
				}
			}
		};
		for (const auto* race : LightKit::FormsOf<RE::TESRace>()) {
			if (race) {
				note(race->actorEffects, isCreature(race));
				noteAttacks(race, isCreature(race));
			}
		}
		for (auto* npc : LightKit::FormsOf<RE::TESNPC>()) {
			if (npc) {
				const bool creatureNpc = isCreature(npc) || isCreature(npc->GetRace());
				note(npc->actorEffects, creatureNpc);
				noteAttacks(npc, creatureNpc);
			}
		}
		// a spell a perk gives (an ability entry) is a person's: perks are the player's
		for (const auto* perk : LightKit::FormsOf<RE::BGSPerk>()) {
			if (!perk) {
				continue;
			}
			for (const auto* entry : perk->perkEntries) {
				if (entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kAbility) {
					byPerson.insert(static_cast<const RE::BGSAbilityPerkEntry*>(entry)->ability);
				}
			}
		}
		for (auto* book : LightKit::FormsOf<RE::TESObjectBOOK>()) {
			if (book && book->TeachesSpell()) {
				byPerson.insert(book->GetSpell());
			}
		}
		for (const auto* shout : LightKit::FormsOf<RE::TESShout>()) {
			if (!shout) {
				continue;
			}
			for (const auto& v : shout->variations) {
				byPerson.insert(v.spell);
			}
		}
		// true while every item seen carrying the effect is a creature's spell
		std::unordered_map<const RE::EffectSetting*, bool> theirs;
		const auto                                         weigh = [&](const RE::MagicItem* a_item, bool a_creature) {
			for (const auto* e : a_item->effects) {
				if (e && e->baseEffect) {
					auto [it, fresh] = theirs.try_emplace(e->baseEffect, a_creature);
					if (!fresh) {
						it->second = it->second && a_creature;
					}
				}
			}
		};
		for (const auto* spell : LightKit::FormsOf<RE::SpellItem>()) {
			if (spell) {
				weigh(spell, byCreature.contains(spell) && !byPerson.contains(spell));
			}
		}
		const auto weighAll = [&](const auto& a_forms) {
			for (const auto* item : a_forms) {
				if (item) {
					weigh(item, false);
				}
			}
		};
		weighAll(LightKit::FormsOf<RE::ScrollItem>());
		weighAll(LightKit::FormsOf<RE::EnchantmentItem>());
		weighAll(LightKit::FormsOf<RE::AlchemyItem>());
		weighAll(LightKit::FormsOf<RE::IngredientItem>());
		std::unordered_set<const RE::EffectSetting*> out;
		for (const auto& [effect, creatureOnly] : theirs) {
			if (creatureOnly) {
				out.insert(effect);
			}
		}
		return out;
	}
}
