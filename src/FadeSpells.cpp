// The fading module (Illuminated and RELight - Spell Lighting NG carry identical copies; FadeConfig.h and FadeDevBench.cpp are each mod's own)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see the LICENSE file and the notice at the top of main.cpp.
//
// Spells in hand. A readied spell's light and glow follow the caster's magicka, the way a weapon's follow its charge.
// Like the weapons, nothing is made: whatever hangs at the casting hand is scaled - the game's own casting light (the
// effect's hand light), and every light and glowing mesh under the hand's magic node, where the casting art is attached
// and where Light Placer, Illuminated and spell art mods hang theirs. So it works with all of them, and with
// none.
//
// Also here: the kind of an enchantment's or a spell's strongest effect (element and school), which the category
// switches in the settings read, for weapons and spells alike.

#include "Fade.h"

namespace Fade
{
	// found testing every enchantment kind in game: Skyrim's own soul trap effects (SoulTrapFFActor,
	// EnchSoulTrapFFContact) are SCRIPT effects, not the Soul Trap archetype, so the archetype alone missed every vanilla one
	bool IsSoulTrap(const RE::EffectSetting* a_effect)
	{
		if (!a_effect) {
			return false;
		}
		if (a_effect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kSoulTrap) {
			return true;
		}
		return Lower(EditorID(a_effect)).find("soultrap") != std::string::npos;
	}

	Kind KindOf(const RE::MagicItem* a_item)
	{
		Kind        k;
		const auto* top = a_item ? a_item->GetCostliestEffectItem() : nullptr;
		const auto* base = top ? top->baseEffect : nullptr;
		if (!base) {
			return k;
		}
		using A = RE::EffectArchetypes::ArchetypeID;
		switch (base->data.resistVariable) {
		case RE::ActorValue::kResistFire:
			k.element = Element::kFire;
			break;
		case RE::ActorValue::kResistFrost:
			k.element = Element::kFrost;
			break;
		case RE::ActorValue::kResistShock:
			k.element = Element::kShock;
			break;
		default:
			if (IsSoulTrap(base)) {
				k.element = Element::kSoulTrap;
				break;
			}
			switch (base->GetArchetype()) {
			case A::kAbsorb:
				k.element = Element::kAbsorb;
				break;
			case A::kSoulTrap:
				k.element = Element::kSoulTrap;
				break;
			case A::kParalysis:
				k.element = Element::kParalyze;
				break;
			case A::kDemoralize:
			case A::kTurnUndead:
			case A::kBanish:
				k.element = Element::kFearTurnBanish;
				break;
			default:
				break;
			}
			break;
		}
		switch (base->data.associatedSkill) {
		case RE::ActorValue::kDestruction:
			k.school = 0;
			break;
		case RE::ActorValue::kRestoration:
			k.school = 1;
			break;
		case RE::ActorValue::kConjuration:
			k.school = 2;
			break;
		case RE::ActorValue::kAlteration:
			k.school = 3;
			break;
		case RE::ActorValue::kIllusion:
			k.school = 4;
			break;
		default:
			break;
		}
		return k;
	}

	void SpellHandLights(RE::Actor* a_actor, bool a_left, RE::NiAVObject* (&a_nodes)[2], RE::NiPointLight*& a_casterLight)
	{
		a_nodes[0] = a_nodes[1] = nullptr;
		a_casterLight = nullptr;
		const auto* strings = RE::FixedStrings::GetSingleton();
		if (!a_actor || !strings) {
			return;
		}
		const auto& name = a_left ? strings->npcLMagicNode : strings->npcRMagicNode;
		for (const bool first : { false, true }) {
			if (first && !a_actor->IsPlayerRef()) {
				break;
			}
			if (auto* root = a_actor->Get3D(first)) {
				a_nodes[first ? 1 : 0] = root->GetObjectByName(name);
			}
		}
		const auto source = a_left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand;
		if (auto* caster = skyrim_cast<RE::ActorMagicCaster*>(a_actor->GetMagicCaster(source)); caster && caster->light) {
			a_casterLight = netimmerse_cast<RE::NiPointLight*>(caster->light->light.get());
		}
	}

	// Rite of Arcana's ritual drains magicka to 0 by design; its players read lights at the empty floor as "spells had no light"
	bool InRitual(RE::Actor* a_actor)
	{
		static const auto* ritual = RE::TESDataHandler::GetSingleton()->LookupForm<RE::EffectSetting>(0x00AA04, "Rite of Arcana.esp");
		auto*              target = ritual ? a_actor->AsMagicTarget() : nullptr;
		auto*              effects = target ? target->GetActiveEffectList() : nullptr;
		if (!effects) {
			return false;
		}
		return std::ranges::any_of(*effects, [](const RE::ActiveEffect* a_effect) {
			return a_effect && a_effect->GetBaseObject() == ritual && !a_effect->flags.any(RE::ActiveEffect::Flag::kInactive);
		});
	}

	bool ReadSpellHand(RE::Actor* a_actor, bool a_left, SpellHand& a_out)
	{
		a_out = {};
		auto* form = a_actor ? a_actor->GetEquippedObject(a_left) : nullptr;
		auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
		// a spell only: a scroll (a SpellItem too) costs no magicka, so its lights have nothing to follow
		if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell ||
			spell->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
			return false;
		}
		a_out.spell = spell;
		auto* av = a_actor->AsActorValueOwner();
		if (av) {
			a_out.current = (std::max)(av->GetActorValue(RE::ActorValue::kMagicka), 0.0f);
			a_out.max = av->GetPermanentActorValue(RE::ActorValue::kMagicka) +
			            a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka);
		}
		a_out.fraction = a_out.max > 0.0f && !InRitual(a_actor) ? Glow::Clamp01(a_out.current / a_out.max) : 1.0f;
		SpellHandLights(a_actor, a_left, a_out.nodes, a_out.casterLight);
		return true;
	}
}
