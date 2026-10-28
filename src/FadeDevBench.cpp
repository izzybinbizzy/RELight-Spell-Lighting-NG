// RELight - Spell Lighting NG - the fading module's DevBench glue (Illuminated's FadeDevBench.cpp, the same for this mod - his order
// 2026-10-14: "relight brought up to illuminated standards in every way that matters"; each mod keeps its own copy of this
// file, FadeConfig.h's twin)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it):
//   inspect kind=relightfade    the fading state as JSON (what the testing DLL's debug page shows) - each tracked hand, and the
//                               fade each of its lights holds now next to the base it was scaled from.
//   relightfade.control         action = preview (value = charge 0-1, -1 stops) | pulse | flare | setting (key = a
//                               RELightSpellLightingNG/Fading.ini key, value = a whole number) | settings (every key and its value) |
//                               reloadrules. Each reply is what the plugin holds after the change. DevBench never saves; a
//                               later change made in the menu saves the settings as they are then.
//   menu invoke name=relightfade the same actions with `set` for `action`.
//   event relightfade.rulesLoaded {rules, files, problems} each time the rule files are read.

#include "Fade.h"

#include "DevBenchGlue.h"

namespace Fade
{
	namespace
	{
		using DevBenchGlue::json;

		constexpr const char* kKey = "relightfade";

		constexpr const char* kInspect =
			R"json({"description":"RELight - Spell Lighting NG - each tracked hand (weapon, enchantment, charge, brightness, reach, cooling, lights found), and every light being scaled with the fade it holds now and the base it was scaled from. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})json";

		constexpr const char* kTool =
			R"json({"description":"RELight - Spell Lighting NG (enchanted weapon and spell-hand lights that fade with charge or magicka): drive its fading (a preview charge, a pulse or a flare) and settings. action=preview with value = the charge to show on every tracked hand (0..1, -1 stops the preview); pulse / flare start a hit pulse or recharge flare on every tracked hand; setting sets one RELightSpellLightingNG/Fading.ini key (key, value = whole number) the way the Settings page does, not saved; settings lists every key and its value; reloadrules reads the rule files again. Read state with inspect kind=relightfade.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["preview","pulse","flare","setting","settings","reloadrules"]},"value":{"type":"number","description":"preview: charge 0..1, or -1 to stop; setting: the whole-number value"},"key":{"type":"string","description":"setting: a RELightSpellLightingNG/Fading.ini key, e.g. EmptyBrightness, Curve, Sputter (action=settings lists them)"}},"required":["action"]}})json";

		constexpr const char* kMenu =
			R"json({"description":"RELight - Spell Lighting NG - the same as the relightfade.control tool, with set for action: set=preview value=charge 0..1 (-1 stops), set=pulse, set=flare, set=setting key=<RELightSpellLightingNG/Fading.ini key> value=<whole number>, set=settings, set=reloadrules. Not saved from here.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})json";

		void InspectNow(void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			json out;
			out["hands"] = json::array();
			for (const auto& h : Snapshot()) {
				out["hands"].push_back({ { "actor", h.actor }, { "left", h.left }, { "weapon", h.weapon }, { "enchantment", h.enchantment },
					{ "bound", h.bound }, { "spell", h.spell }, { "exempt", h.exempt }, { "fraction", h.fraction }, { "hudFraction", h.hudFraction }, { "current", h.current }, { "max", h.max },
					{ "brightness", h.brightness }, { "reach", h.reach }, { "cool", h.cool }, { "lights", h.lights }, { "roots", h.roots },
					{ "why", h.why } });
			}
			out["lights"] = json::array();
			for (const auto& l : LightsNow()) {
				out["lights"].push_back({ { "fade", l.fade }, { "base", l.base }, { "radius", l.radius }, { "frozen", l.frozen } });
			}
			const auto& p = PreviewState();
			const auto  s = Config();
			out["preview"] = p.on.load();
			out["previewCharge"] = p.fraction.load();
			out["rules"] = RuleCount();
			out["ruleFiles"] = RuleFileCount();
			out["problems"] = RuleProblems().size();
			out["enabled"] = s.enabled;
			out["floor"] = s.tuning.floor;
			out["curve"] = static_cast<int>(s.tuning.curve);
			out["reachFollows"] = s.tuning.reachFollows;
			out["sputter"] = s.tuning.sputter;
			out["cool"] = s.tuning.cool;
			out["pulse"] = s.tuning.pulse;
			out["flare"] = s.tuning.flare;
			out["weapons"] = s.weapons;
			out["spells"] = s.spells;
			out["staves"] = s.staves;
			out["bound"] = s.bound;
			out["who"] = static_cast<int>(s.who);
			out["dimShader"] = s.dimShader;
			out["dimmedGlows"] = DimmedGlowCount();
			out["art"] = json::array();
			for (const auto& [base, now] : ArtNow()) {
				out["art"].push_back({ { "base", base }, { "now", now } });
			}
			out["hideChargeBar"] = s.hideChargeBar;
			DevBenchGlue::Reply(a_sink, a_write, out);
		}

		json KeyNames()
		{
			json names = json::array();
			for (const auto& k : SettingsText::kKeys) {
				names.push_back(k.name);
			}
			return names;
		}

		json SettingValue(std::string_view a_key)
		{
			// the same case-insensitive lookup Fading.ini and ApplySetting use (the re-score: this one matched case-sensitively)
			const auto* k = SettingsText::Find(a_key);
			return k ? json(k->get(Config())) : json(nullptr);
		}

		json RulesNow()
		{
			return json{ { "rules", RuleCount() }, { "files", RuleFileCount() }, { "problems", RuleProblems().size() } };
		}

		// one action, from the tool (`action`) or the menu alias (`set`)
		json Act(const json& a_args)
		{
			auto action = DevBenchGlue::Text(a_args, "action");
			if (action.empty() || action == "invoke") {  // `menu invoke` hands over its own args: action=invoke, the verb in set
				action = DevBenchGlue::Text(a_args, "set");
			}
			auto& p = PreviewState();
			if (action == "preview") {
				// value: the charge to preview, 0 to 1; a negative number turns the preview off; no number is refused
				const auto v = DevBenchGlue::Number(a_args, "value");
				if (!v) {
					return DevBenchGlue::Refusal("preview needs value: a charge 0..1, or -1 to stop");
				}
				if (*v >= 0.0) {
					p.fraction = Glow::Clamp01(static_cast<float>(*v));
				}
				p.on = *v >= 0.0;
				return json{ { "ok", true }, { "preview", p.on.load() }, { "previewCharge", p.fraction.load() } };
			}
			if (action == "pulse" || action == "flare") {
				(action == "pulse" ? p.pulse : p.flare) = true;  // read by the next frame's update
				return json{ { "ok", true }, { "started", action }, { "note", "plays on every tracked hand from the next frame" } };
			}
			if (action == "setting") {
				// the Settings page's switch, slider or choice, through the same ApplySetting the file uses; not saved from here
				const auto key = DevBenchGlue::Text(a_args, "key");
				const auto v = DevBenchGlue::Number(a_args, "value");
				if (SettingValue(key).is_null()) {
					return DevBenchGlue::Refusal("unknown key '" + key + "'", KeyNames());
				}
				if (!v || *v != std::trunc(*v) || std::abs(*v) > 1e9) {
					return DevBenchGlue::Refusal("setting needs value: a whole number");
				}
				ApplySetting(key, static_cast<int>(*v));
				return json{ { "ok", true }, { "key", key }, { "value", SettingValue(key) } };
			}
			if (action == "settings") {
				json all = json::object();
				for (const auto& k : SettingsText::kKeys) {
					all[k.name] = SettingValue(k.name);
				}
				return json{ { "ok", true }, { "settings", all } };
			}
			if (action == "reloadrules") {
				// the Debug page's "Reload rule files": on the main thread, between frames
				auto done = DevBenchGlue::OnMainThread([]() { LoadRules(); return RulesNow(); });
				if (!done) {
					return DevBenchGlue::NotRunYet();
				}
				if (DevBenchGlue::Refused(*done)) {
					return *done;  // nothing could be queued - the rules were not read again
				}
				(*done)["ok"] = true;
				return *done;
			}
			return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "preview", "pulse", "flare", "setting", "settings", "reloadrules" }));
		}

		// the refusal after a throw, and a fixed one when even that cannot be built (no memory): nothing leaves through DevBench
		void Refuse(void* a_sink, DevBenchAPI::WriteFn a_write, const char* a_what) noexcept
		{
			if (!a_write) {
				return;
			}
			std::string text;
			try {
				text = DevBenchGlue::Dump(DevBenchGlue::Refusal(a_what));
			} catch (...) {
				text.clear();  // not even the refusal could be built: the fixed one below
			}
			try {
				a_write(a_sink, text.empty() ? "{\"ok\":false,\"error\":\"out of memory\"}" : text.c_str());
			} catch (...) {
				return;  // the writer is called once: a throw out of it is not retried
			}
		}

		// DevBench calls these across the DLL boundary: nothing may be thrown back through it
		void Inspect(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				InspectNow(a_sink, a_write);
			} catch (...) {
				Refuse(a_sink, a_write, "RELight - Spell Lighting NG could not build its report");
			}
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, Act(DevBenchGlue::Args(a_args)));
			} catch (...) {
				Refuse(a_sink, a_write, "RELight - Spell Lighting NG could not handle that");
			}
		}
	}

	void RulesLoaded()
	{
		DevBenchGlue::Emit("relightfade.rulesLoaded", RulesNow());
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < DevBenchGlue::kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterTool("relightfade.control", kTool, Control, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Control, nullptr);
		SKSE::log::info("DevBench: inspect kind={}, the tool relightfade.control and menu invoke name={} registered", kKey, kKey);
	}
}
