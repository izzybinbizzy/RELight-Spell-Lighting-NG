// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it) - Illuminated's DevBench.cpp, the
// same way for this mod (his order 2026-10-14: "relight brought up to illuminated standards in every way that matters"):
//   inspect kind=relight     every setting and switch with its value; the lighting found; what the plugin made and holds now
//                            (hand lights, ground lights, held lights, kept lights) and the spray / beam records it claims.
//   relight.control          action = setting (key = a [Settings] key, or switch:<option id>; value = a whole number: changed
//                            exactly as the menu does - saved, applied on the main thread; the reply is the value it holds) |
//                            settings (every key and switch with its value) | item (form = a spell or weapon's form id,
//                            choice = auto, off or r,g,b: its Lights by Item choice, as the page sets it).
//   menu invoke name=relight set=setting key=... value=... - the same, for scripts that use the menu route.
//   event relight.settingChanged {key, value} for each change made through DevBench.

#include "Fade.h"
#include "FadeFormText.h"
#include "Plugin.h"

#include "DevBenchGlue.h"

namespace Plugin
{
	namespace
	{
		using DevBenchGlue::json;

		constexpr const char* kKey = "relight";

		constexpr const char* kInspect =
			R"json({"description":"RELight - Spell Lighting NG - every setting and switch with its value, the lighting found, and what it made and holds now: hand lights, ground lights, held lights, kept lights, and the spray and beam light records it claims. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})json";

		constexpr const char* kTool =
			R"json({"description":"RELight - Spell Lighting NG (hand, held and ground lights beside RE::Light, with an SKSE menu): change its settings. action=setting sets one [Settings] key (or switch:<option id>) to a whole number exactly as the menu does (saved, applied on the main thread) and replies with the value it now holds; action=settings lists every key and switch with its value; action=item sets one spell's or weapon's Lights by Item choice (form, choice = auto, off or r,g,b) as the page does. Read the state with inspect kind=relight.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["setting","settings","item"]},"key":{"type":"string","description":"setting: a [Settings] key such as Brightness, Reach, HandLights, WardColor, or switch:<option id> (action=settings lists them)"},"value":{"type":"number","description":"setting: a whole number (a switch 0/1, a choice's index, a slider's percent)"},"form":{"type":"string","description":"item: the spell's or weapon's form id, e.g. 0x00012FCD"},"choice":{"type":"string","description":"item: auto, off, or a color r,g,b (0-255 each)"}},"required":["action"]}})json";

		constexpr const char* kMenu =
			R"json({"description":"RELight - Spell Lighting NG - set=setting key=<[Settings] key or switch:<option id>> value=<whole number> changes a setting exactly as the menu does: saved, and applied on the main thread. The same as the relight.control tool.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})json";

		[[nodiscard]] json SettingsNow()
		{
			json all = json::object();
			for (const auto& [key, value] : SettingsSnapshot()) {
				all[key] = value;
			}
			return all;
		}

		[[nodiscard]] json InspectNow()
		{
			json out;
			out["settings"] = SettingsNow();
			out["lighting"] = json::parse(LightingReport());  // Lighting.cpp, as Illuminated's
			out["handLights"] = { { "made", HandLightsMade() }, { "effects", HandEffects() }, { "automatic", AutoHandEffects() } };
			out["groundLights"] = { { "now", GroundLightCount() }, { "made", GroundLightsMade() } };  // GroundLights.cpp
			out["heldLights"] = LiveHeldLights();
			out["bodyLights"] = { { "now", LiveBodyLights() }, { "made", BodyLightsMade() } };  // BodyLights.cpp
			out["keptLights"] = KeptLights();
			out["sprayRecords"] = SprayLightRecords().size();
			out["beamRecords"] = BeamLightRecords().size();
			out["streamCopies"] = { { "raised", RaisedLights() }, { "plain", PlainLights() } };
			out["wardsSteppedDown"] = WardsSteppedDown();
			out["options"] = json::array();
			for (const auto& o : Options()) {
				out["options"].push_back({ { "id", o.id }, { "name", o.name }, { "switchable", o.switchable }, { "on", o.on.load() },
					{ "meshes", o.meshes }, { "bases", o.bases }, { "streams", o.streams }, { "lit", o.lit.load() }, { "heldOut", o.heldOut.load() } });
			}
			return out;
		}

		json Act(const json& a_args)
		{
			auto action = DevBenchGlue::Text(a_args, "action");
			if (action.empty() || action == "invoke") {  // `menu invoke` hands over its own args: action=invoke, the verb in set
				action = DevBenchGlue::Text(a_args, "set");
			}
			if (action == "settings") {
				return json{ { "ok", true }, { "settings", SettingsNow() } };
			}
			if (action == "item") {
				const auto    form = DevBenchGlue::Text(a_args, "form");
				std::uint32_t id = 0;
				// the whole text must be the number: "0x12junk" would otherwise name form 0x12 (2.2's CodeRabbit round)
				if (!FormText::ParseHex(form, id) || !RE::TESForm::LookupByID(id)) {
					return DevBenchGlue::Refusal("item needs form: a loaded spell's or weapon's form id");
				}
				if (!Fade::Items::SetFromText(id, DevBenchGlue::Text(a_args, "choice"))) {
					return DevBenchGlue::Refusal(
						"item needs a spell, staff or enchanted / bound weapon the Lights by Item list holds, and choice: auto, off, r,g,b "
						"or off,r,g,b");
				}
				return json{ { "ok", true }, { "form", form }, { "choice", DevBenchGlue::Text(a_args, "choice") }, { "note", "applied to the lights at the next frame" } };
			}
			if (action != "setting") {
				return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "setting", "settings", "item" }));
			}
			const auto key = DevBenchGlue::Text(a_args, "key");
			const auto v = DevBenchGlue::Number(a_args, "value");
			// the key as the snapshot names it (case left out): a [Settings] key, or switch:<option id>; WardColour, the old
			// spelling, is read as WardColor (Settings.cpp)
			const auto  want = LightKit::SameText(key, "WardColour") ? std::string("WardColor") : key;
			std::string found;
			for (const auto& [k, value] : SettingsSnapshot()) {
				if (LightKit::SameText(k, want)) {
					found = k;
					break;
				}
			}
			if (found.empty()) {
				json keys = json::array();
				for (const auto& [k, value] : SettingsSnapshot()) {
					keys.push_back(k);
				}
				return DevBenchGlue::Refusal("unknown key '" + key + "'", keys);
			}
			if (!v || *v != std::trunc(*v) || std::abs(*v) > 1e9) {
				return DevBenchGlue::Refusal("setting needs value: a whole number");
			}
			// the menu's own route (Menu.cpp): the value, the file saved, then the wards and the hand lights take it - a preset
			// does the same (RepresetSoon); this runs on the main thread already (Control)
			ApplySettingsSnapshot({ { found, static_cast<int>(*v) } });
			SaveSettings();
			ApplyWards("DevBench");
			RefindHandLights();
			ApplyHandLights(true);
			int now = 0;
			for (const auto& [k, value] : SettingsSnapshot()) {
				if (k == found) {
					now = value;
				}
			}
			DevBenchGlue::Emit("relight.settingChanged", json{ { "key", found }, { "value", now } });
			return json{ { "ok", true }, { "key", found }, { "value", now }, { "note", "applied to the lights at the next frame" } };
		}

		// the refusal after a throw, and a fixed one when even that cannot be built (no memory): nothing leaves through DevBench
		void Refuse(void* a_sink, DevBenchAPI::WriteFn a_write, const char* a_what) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal(a_what));
			} catch (...) {
				if (a_write) {
					a_write(a_sink, "{\"ok\":false,\"error\":\"out of memory\"}");
				}
			}
		}

		// DevBench calls these across the DLL boundary: nothing may be thrown back through it
		void Inspect(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				// the lists are the main thread's: read them there
				const auto now = DevBenchGlue::OnMainThread([]() { return InspectNow(); });
				DevBenchGlue::Reply(a_sink, a_write, now ? *now : DevBenchGlue::NotRunYet());
			} catch (...) {
				Refuse(a_sink, a_write, "RELight - Spell Lighting NG could not build its report");
			}
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				// a change is made on the main thread, as the game's forms and the lit scene are its
				const auto now = DevBenchGlue::OnMainThread([args = DevBenchGlue::Args(a_args)]() { return Act(args); });
				DevBenchGlue::Reply(a_sink, a_write, now ? *now : DevBenchGlue::NotRunYet());
			} catch (...) {
				Refuse(a_sink, a_write, "RELight - Spell Lighting NG could not handle that");
			}
		}
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < DevBenchGlue::kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterTool("relight.control", kTool, Control, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Control, nullptr);
		SKSE::log::info("DevBench: inspect kind={}, the tool relight.control and menu invoke name={} registered", kKey, kKey);
	}
}
