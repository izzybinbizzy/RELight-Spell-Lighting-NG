// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Sprays, breath shouts and beams. Each spray, flamethrower and beam projectile our data lights wears a PLAIN copy of its own
// light record - the light it had, in his color where the data names one - and a row of small lights along its length
// (Rows.cpp, Illuminated's row): one light raised to the whole length lit the whole scene. The copy is made by the game's own
// light function, never by RE::Light's hook (Hooks.cpp MakeMagicLight, as the hand lights are), with the moving-light mark.
// The records' OTHER users: a spray record is taken off them (RE::Light's own configs light those objects - a second light
// would double them); a beam record's other users (a lightning arrow, a shock explosion ...) wear a plain copy - the light
// they had - made the same way, so they keep it even where RE::Light is set to switch off game lights it has no config for.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::unordered_set<const RE::TESObjectLIGH*> gSpraySet;   // the data's spray records
		std::unordered_set<const RE::TESObjectLIGH*> gRowCopies;  // the copies the sprays / bolts with a row wear
		std::unordered_set<const RE::TESObjectLIGH*> gPlain;      // every plain copy made here (those and the other users')
		std::atomic<bool>                            gReady{ false };

		// how many of each kind of user a claim changed
		struct LightUsers
		{
			std::size_t projectiles{ 0 }, muzzles{ 0 }, effects{ 0 }, explosions{ 0 }, hazards{ 0 };
		};

		// every record field that names a light record - a projectile's own light and its muzzle flash, a magic effect's, an
		// explosion's, a hazard's - handed to a_change(the projectile whose own light it is, or nullptr; the field), which returns
		// true when it changed that field. One walk, so a user kind is never in one claim and missing from another (2.2's review)
		template <class F>
		LightUsers ForEachLightField(RE::TESDataHandler& a_dh, F&& a_change)
		{
			LightUsers n;
			for (auto* p : a_dh.GetFormArray<RE::BGSProjectile>()) {
				if (p) {
					n.projectiles += a_change(p, p->data.light) ? 1 : 0;
					n.muzzles += a_change(nullptr, p->data.muzzleFlashLight) ? 1 : 0;
				}
			}
			for (auto* e : a_dh.GetFormArray<RE::EffectSetting>()) {
				n.effects += e && a_change(nullptr, e->data.light) ? 1 : 0;
			}
			for (auto* x : a_dh.GetFormArray<RE::BGSExplosion>()) {
				n.explosions += x && a_change(nullptr, x->data.light) ? 1 : 0;
			}
			for (auto* h : a_dh.GetFormArray<RE::BGSHazard>()) {
				n.hazards += h && a_change(nullptr, h->data.light) ? 1 : 0;
			}
			return n;
		}

		// the whole light as its user had it: look, sound, emittance, lens flare and model (Illuminated's FormCopies.cpp CopyLight)
		RE::TESObjectLIGH* CopyOf(const RE::TESObjectLIGH* a_light)
		{
			auto* copy = a_light ? LightKit::NewForm<RE::TESObjectLIGH>() : nullptr;
			if (!copy) {
				return nullptr;
			}
			copy->data = a_light->data;
			copy->fade = a_light->fade;
			copy->sound = a_light->sound;
			copy->emittanceColor = a_light->emittanceColor;
			copy->lensFlare = a_light->lensFlare;
			copy->SetModel(a_light->GetModel());
			return copy;
		}

		// Dynamic Lighting off: the record's own flicker and pulse bits come off the copy, so a spray's or bolt's own light holds steady
		void HoldSteady(RE::TESObjectLIGH* a_light)
		{
			using enum RE::TES_LIGHT_FLAGS;
			constexpr auto kMoving = static_cast<std::uint32_t>(kFlicker) | static_cast<std::uint32_t>(kFlickerSlow) |
			                         static_cast<std::uint32_t>(kPulse) | static_cast<std::uint32_t>(kPulseSlow);
			a_light->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(a_light->data.flags.underlying() & ~kMoving);
		}

	}

	// once a session: the hooks read these sets from any thread once gReady is set, and a second walk would find every field
	// already wearing its copy - so it is never redone
	void RaiseStreamLights()
	{
		if (gReady.load(std::memory_order_acquire)) {
			SKSE::log::warn("stream lights: already raised this session - left as they are");
			return;
		}
		std::unordered_set<const RE::TESObjectLIGH*> beams;
		for (const auto id : SprayLightRecords()) {
			if (auto* l = RE::TESForm::LookupByID<RE::TESObjectLIGH>(id)) {
				gSpraySet.insert(l);
			}
		}
		for (const auto id : BeamLightRecords()) {
			if (auto* l = RE::TESForm::LookupByID<RE::TESObjectLIGH>(id); l && !gSpraySet.contains(l)) {
				beams.insert(l);
			}
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if ((gSpraySet.empty() && beams.empty()) || !dh) {
			gReady.store(true, std::memory_order_release);
			return;
		}
		std::unordered_map<const RE::TESObjectLIGH*, RE::TESObjectLIGH*> plainOf, rowPlainOf;  // a record's plain copy: its other users', its rows' projectiles'
		std::unordered_map<RE::FormID, RowRecipe>                        rows;
		std::unordered_set<const RE::TESObjectLIGH*>                     handTinted;  // plain copies already in the hand's color
		std::unordered_map<RE::FormID, const RE::TESObjectLIGH*>         rowCopy;     // each row's projectile -> the copy it wears
		std::size_t                                                      sprays = 0, bolts = 0, failed = 0, dark = 0;
		const auto                                                       n = ForEachLightField(*dh, [&](const RE::BGSProjectile* a_proj, RE::TESObjectLIGH*& a_light) {
			const bool sprayRecord = a_light && gSpraySet.contains(a_light);
			if (!a_light || (!sprayRecord && !beams.contains(a_light))) {
				return false;
			}
			// a spray (flamethrower or cone) on a spray record, or a beam our data lights: its own raised copy
			const bool spray = a_proj && sprayRecord && (a_proj->IsFlamethrower() || a_proj->IsCone());
			const bool bolt = a_proj && a_proj->IsBeam() && StreamOf(a_proj);
			if (spray || bolt) {
				// a plain copy of its own light (his color where the data names one) and a row of small lights along it
				// (Rows.cpp) - one light raised to the whole length lit the whole scene
				auto [it, fresh] = rowPlainOf.try_emplace(a_light, nullptr);
				// the game's own art: the hand's color for the art pick, as every other light of the spell - whichever of the
				// record's projectiles comes first, the copy is shared by all of them
				const auto handColor = [&](RE::TESObjectLIGH* a_copy) {
					const auto* pick = handTinted.contains(a_copy) ? nullptr : HandColorOf(a_proj);
					if (!pick) {
						return false;
					}
					a_copy->data.color.red = static_cast<std::uint8_t>(std::clamp(std::lround(pick->red * 255.0f), 0L, 255L));
					a_copy->data.color.green = static_cast<std::uint8_t>(std::clamp(std::lround(pick->green * 255.0f), 0L, 255L));
					a_copy->data.color.blue = static_cast<std::uint8_t>(std::clamp(std::lround(pick->blue * 255.0f), 0L, 255L));
					handTinted.insert(a_copy);
					return true;
				};
				if (fresh) {
					it->second = CopyOf(a_light);
					if (const auto his = it->second ? RecordColor(a_light->GetFormID()) : std::nullopt) {
						it->second->data.color.red = his->red;
						it->second->data.color.green = his->green;
						it->second->data.color.blue = his->blue;
					}
					if (it->second) {
						if (!DynamicLightingThisSession()) {
							HoldSteady(it->second);
						}
						gPlain.insert(it->second);
						gRowCopies.insert(it->second);
					}
				}
				if (!it->second) {
					++failed;  // no copy: it keeps its record (and RE::Light's config) - said in the log
					return false;
				}
				handColor(it->second);
				const auto& c = it->second->data.color;
				rows[a_proj->GetFormID()] = { { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f }, a_light->fade,
					static_cast<float>(a_light->data.radius), bolt, OptionOf(a_proj) };
				rowCopy[a_proj->GetFormID()] = it->second;
				a_light = it->second;
				(spray ? sprays : bolts) += 1;
				return true;
			}
			// anything else on a spray record goes dark, as in 2.1 / 2.2 (RE::Light's own configs light those objects)
			if (sprayRecord) {
				a_light = nullptr;
				++dark;
				return true;
			}
			// anything else on a beam record keeps the light it had, made by the game's own function (Hooks.cpp)
			auto [it, fresh] = plainOf.try_emplace(a_light, nullptr);
			if (fresh) {
				it->second = CopyOf(a_light);
				if (it->second) {
					gPlain.insert(it->second);
				} else {
					++failed;
				}
			}
			if (!it->second) {
				return false;
			}
			a_light = it->second;
			return true;
		});
		// a later projectile of the record may have put the shared copy in the hand's color: every row wears its copy's final color
		for (auto& [id, recipe] : rows) {
			if (const auto copy = rowCopy.find(id); copy != rowCopy.end()) {
				const auto& c = copy->second->data.color;
				recipe.color = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
			}
		}
		// a cone our data lights as a cloud: one light held where its cloud is (Rows.cpp) - RE::Light has no config for it
		std::size_t clouds = 0;
		for (auto* p : dh->GetFormArray<RE::BGSProjectile>()) {
			if (const auto* c = p && p->IsCone() ? CloudOf(p) : nullptr) {
				rows[p->GetFormID()] = { c->color, c->fade, c->radius, false, OptionOf(p), true };
				++clouds;
			}
		}
		gReady.store(true, std::memory_order_release);
		SetRowRecipes(std::move(rows));
		if (clouds) {
			SKSE::log::info("stream lights: {} cone(s) hold one light where their cloud is", clouds);
		}
		SKSE::log::info(
			"stream lights: {} spray(s) and {} bolt(s) wear a plain copy of their own light ({} copies) and a row; the records' other users - "
			"{} projectile(s), {} muzzle flash(es), {} magic effect(s), {} explosion(s), {} hazard(s): {} on a spray record left dark, "
			"the rest on {} plain copy(ies) of a beam record{}",
			sprays, bolts, gRowCopies.size(), n.projectiles - sprays - bolts, n.muzzles,
			n.effects, n.explosions, n.hazards, dark, gPlain.size(),
			failed ? std::format("; {} copy(ies) could not be made - those users keep the record", failed) : std::string{});
	}

	bool IsOurLight(const RE::TESObjectLIGH* a_light)
	{
		return a_light && gReady.load(std::memory_order_acquire) && gPlain.contains(a_light);
	}

	void DressStreamLight(RE::NiLight* a_light, const RE::TESObjectLIGH* a_record)
	{
		if (!a_light || !a_record || !gReady.load(std::memory_order_acquire)) {
			return;
		}
		// the moving-light mark, as RE::Light's own hook marks the magic lights it passes, so its flicker prevention lets
		// them reach every surface; otherwise the light the game made, as it made it
		a_light->fadeAmount = kMovingLightMark;
	}

	std::size_t RaisedLights() { return gRowCopies.size(); }
	std::size_t PlainLights() { return gPlain.size(); }

	bool IsSprayLight(const RE::TESObjectLIGH* a_light)
	{
		return a_light && gReady.load(std::memory_order_acquire) && (gSpraySet.contains(a_light) || gRowCopies.contains(a_light));
	}

	bool IsStreamObject(const RE::TESObjectREFR* a_ref)
	{
		return a_ref && gReady.load(std::memory_order_acquire) && StreamOf(a_ref->GetBaseObject()) != nullptr;
	}
}
