#pragma once
// DevBenchGlue.h - ONE identical copy in every plugin of ours that offers itself to DevBench (master: Dynamic Wards'
// src\DevBenchGlue.h; `PC Runner\devbench glue check.py` fails a copy that differs). GPL-3.0-or-later like the plugin.
//
// How our plugins meet DevBench (alandtse's "Use devbench from your mod", adopted 2026-10-06):
//   inspect kind=<mod>  a READ of what the plugin holds now (RegisterToolExtension: "inspect" already means "read state").
//   <mod>.control       a top-level TOOL for everything that changes something (RegisterTool, named consumer.verb as
//                       devbench asks): an `action` enum, a real
//                       inputSchema and description, and a reply that is the result read back on the main thread.
//                       (A cold agent skips extra verbs hidden under `menu`, so `menu invoke name=<mod>` stays only as an
//                       alias for scripts that already call it.)
//   events              "<mod>.<what>" topics through EmitEvent, for what happens without a call.
// Every handler runs on DevBench's listener thread and must not throw back across the DLL boundary.

#include "DevBenchAPI.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace DevBenchGlue
{
	using json = nlohmann::json;

	// devbench 1.5.0: RegisterToolExtension (RegisterTool and EmitEvent are older)
	constexpr unsigned kNeedsBuild = 10500;

	// every string escaped (text in the ANSI code page replaced, never passed through) and a number that is not finite
	// written as null: the reply is always valid JSON
	inline std::string Dump(const json& a_j) { return a_j.dump(-1, ' ', false, json::error_handler_t::replace); }

	inline void Reply(void* a_sink, DevBenchAPI::WriteFn a_write, const json& a_j)
	{
		if (a_write)
			a_write(a_sink, Dump(a_j).c_str());
	}

	// a refusal an agent can act on: what was wrong and the values that would have worked
	inline json Refusal(const std::string& a_what, json a_valid = nullptr)
	{
		json j{ { "ok", false }, { "error", a_what } };
		if (!a_valid.is_null())
			j["valid"] = std::move(a_valid);
		return j;
	}

	// the tool's arguments; a discarded value (never an exception) when they are not JSON
	inline json Args(const char* a_args) { return json::parse(a_args ? a_args : "", nullptr, false); }

	inline std::string Text(const json& a_args, const char* a_key)
	{
		if (!a_args.is_object() || !a_args.contains(a_key))
			return {};
		const auto& v = a_args[a_key];
		return v.is_string() ? v.get<std::string>() : v.is_number() ? v.dump() :
		                                                              std::string();
	}

	// a number field, sent as a number or as its text ("5", "-1", "0.25"); nullopt when it is missing, not a number or
	// not finite. Parsed without the C locale (a decimal comma never sneaks in).
	inline std::optional<double> Number(const json& a_args, const char* a_key)
	{
		if (!a_args.is_object() || !a_args.contains(a_key))
			return std::nullopt;
		const auto& v = a_args[a_key];
		double      d = 0.0;
		if (v.is_number()) {
			d = v.get<double>();
		} else if (v.is_string()) {
			const auto& s = v.get_ref<const std::string&>();
			const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
			if (s.empty() || ec != std::errc{} || end != s.data() + s.size())
				return std::nullopt;
		} else {
			return std::nullopt;
		}
		return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
	}

	// what a tool replies when nothing could be put on the main thread at all (SKSE gave no task interface): a refusal
	inline json NotQueued()
	{
		json j = Refusal("no SKSE task interface - nothing was queued, so nothing was applied");
		j["queued"] = false;
		return j;
	}

	// true for a refusal ({"ok": false}) - a caller that wraps or adds to what OnMainThread returned passes one on as it is
	inline bool Refused(const json& a_reply)
	{
		return a_reply.is_object() && a_reply.contains("ok") && a_reply["ok"].is_boolean() && !a_reply["ok"].get<bool>();
	}

	// Runs a_fn on the main thread through SKSE's task interface and waits for what it returns, so a reply carries what
	// the game holds after the change. nullopt when the game did not run it in time (a loading screen, a paused game): the
	// task still runs later (NotRunYet says so). When nothing could be queued at all - no task interface - it returns the
	// NotQueued refusal (a value, never an exception: a handler that forgot its try must not end the game), so a caller
	// that wraps the result checks Refused first. a_fn must return json and must not touch anything the listener thread owns.
	template <class F>
	std::optional<json> OnMainThread(F&& a_fn, std::chrono::milliseconds a_timeout = std::chrono::milliseconds(3000))
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks)
			return NotQueued();
		auto promise = std::make_shared<std::promise<json>>();
		auto result = promise->get_future();
		tasks->AddTask([promise, fn = std::forward<F>(a_fn)]() mutable {
			try {
				promise->set_value(fn());
			} catch (...) {
				try {
					promise->set_value(Refusal("the change failed on the main thread"));
				} catch (...) {
					// out of memory even for the refusal: the promise breaks and the caller reads no answer
				}
			}
		});
		if (result.wait_for(a_timeout) != std::future_status::ready)
			return std::nullopt;
		// ready with no answer (a broken promise) is a refusal, never "not run yet"
		try {
			return result.get();
		} catch (...) {
			try {
				return Refusal("the change failed on the main thread");
			} catch (...) {
				return std::nullopt;
			}
		}
	}

	// what a tool replies when the main thread did not run its work within the wait
	inline json NotRunYet()
	{
		return json{ { "ok", true }, { "queued", true },
			{ "note", "the game has not run it yet (loading or paused); it runs on the next frame - read inspect to see it" } };
	}

	// topic is "<mod>.<what>"; nothing happens without DevBench
	inline void Emit(const char* a_topic, const json& a_payload)
	{
		if (auto* devbench = DevBenchAPI::GetDevBenchInterface001())
			devbench->EmitEvent(a_topic, Dump(a_payload).c_str());
	}
}
