#include "FadeHudGem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace Fade::HudGem
{
	namespace
	{
		// both mods ship the same movie under their own name (they never run together); the HUD's own clip it lives in
		constexpr std::array kClips{ "_root.HUDMovieBaseInstance.IlluminatedGem", "_root.HUDMovieBaseInstance.RELightGem" };
		// Infinity UI adds the movie while the HUD loads, so it may answer a moment after the HUD opens: a few asks, then none
		constexpr int  kAsksAtOpen = 5;
		constexpr auto kAskGap = std::chrono::seconds(2);

		std::atomic<bool> live{ false };
		std::atomic<int>  clip{ -1 };  // which of kClips answered
		std::atomic<bool> asking{ false };
		std::atomic<int>  asksLeft{ 0 };  // armed by the HUD opening, spent by Publish
		std::atomic<bool> off{ false };   // HUD NG places the gems (StandDown): the movie is never asked again

		// render thread only
		struct Sent
		{
			std::array<Gem, 2> gems{};
			Look               look{};
			bool               hide{ true };
			bool               any{ false };
		};
		Sent                                  sent{};
		std::chrono::steady_clock::time_point lastAsk{};

		[[nodiscard]] RE::GPtr<RE::GFxMovieView> HudView()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
		}

		// UI thread: does one of our clips sit in the HUD and answer ping() with 2
		void Ask()
		{
			if (off.load(std::memory_order_relaxed)) {
				asking.store(false, std::memory_order_relaxed);
				return;
			}
			const auto view = HudView();
			int        found = -1;
			if (view) {
				for (int i = 0; i < static_cast<int>(kClips.size()) && found < 0; ++i) {
					RE::GFxValue c;
					RE::GFxValue answer;
					if (view->GetVariable(&c, kClips[i]) && c.IsDisplayObject() && c.Invoke("ping", &answer) && answer.IsNumber() &&
						answer.GetNumber() == 2.0) {
						found = i;
					}
				}
			}
			clip.store(found, std::memory_order_relaxed);
			const bool was = live.exchange(found >= 0, std::memory_order_relaxed);
			if (found >= 0 && !was) {
				asksLeft.store(0, std::memory_order_relaxed);
				SKSE::log::info("HUD soul gem: drawn in the HUD's own layer ({})", kClips[found]);
			}
			asking.store(false, std::memory_order_relaxed);
		}

		void AskSoon()
		{
			if (asking.exchange(true, std::memory_order_relaxed)) {
				return;
			}
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddUITask([] { Ask(); });
			} else {
				asking.store(false, std::memory_order_relaxed);
			}
		}

		// UI thread: one call on our clip; a clip that went away ends Live (the ImGui gems come back) and asks once more
		template <std::size_t N>
		void Call(const char* a_fn, const std::array<RE::GFxValue, N>& a_args)
		{
			const auto   view = HudView();
			const int    i = clip.load(std::memory_order_relaxed);
			RE::GFxValue c;
			if (!view || i < 0 || !view->GetVariable(&c, kClips[i]) || !c.IsDisplayObject() || !c.Invoke(a_fn, nullptr, a_args.data(), N)) {
				if (live.exchange(false, std::memory_order_relaxed)) {
					asksLeft.store(1, std::memory_order_relaxed);
				}
			}
		}

		struct MenuSink final : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::HUDMenu::MENU_NAME && !off.load(std::memory_order_relaxed)) {
					live.store(false, std::memory_order_relaxed);
					asksLeft.store(a_event->opening ? kAsksAtOpen : 0, std::memory_order_relaxed);
					if (a_event->opening) {
						AskSoon();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool Live() noexcept { return live.load(std::memory_order_relaxed); }

	void Publish(const std::array<Gem, 2>& a_gems, const Look& a_look, bool a_hide)
	{
		if (!Live()) {
			sent.any = false;  // everything is sent again once it answers
			if (asksLeft.load(std::memory_order_relaxed) <= 0) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now - lastAsk >= kAskGap) {
				lastAsk = now;
				asksLeft.fetch_sub(1, std::memory_order_relaxed);
				AskSoon();
			}
			return;
		}
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		if (!sent.any || a_look != sent.look) {
			const Look l = a_look;
			tasks->AddUITask([l] {
				Call<6>("setLook", { RE::GFxValue(static_cast<double>(std::clamp(l.scale, 0.5f, 2.0f))),
									   RE::GFxValue(static_cast<double>(std::clamp(l.alpha, 0.1f, 1.0f) * 100.0f)),
									   RE::GFxValue(l.percent ? 1.0 : 0.0), RE::GFxValue(0.0), RE::GFxValue(0.0), RE::GFxValue(l.place == 1 ? 1.0 : 0.0) });
			});
		}
		if (!sent.any || a_hide != sent.hide) {
			const bool h = a_hide;
			tasks->AddUITask([h] { Call<1>("setHidden", { RE::GFxValue(h ? 1.0 : 0.0) }); });
		}
		for (std::size_t i = 0; i < a_gems.size(); ++i) {
			auto g = a_gems[i];
			// a whole percent, so a draining charge sends at most a hundred changes
			g.fill = std::isfinite(g.fill) ? std::round(std::clamp(g.fill, 0.0f, 1.0f) * 100.0f) / 100.0f : 0.0f;
			if (sent.any && g == sent.gems[i]) {
				continue;
			}
			sent.gems[i] = g;
			tasks->AddUITask([i, g] {
				Call<4>("setGem", { RE::GFxValue(static_cast<double>(i)), RE::GFxValue(g.shown ? 1.0 : 0.0),
									  RE::GFxValue(static_cast<double>(g.fill)), RE::GFxValue(static_cast<double>(g.rgb)) });
			});
		}
		sent.look = a_look;
		sent.hide = a_hide;
		sent.any = true;
	}

	void Register()
	{
		static MenuSink sink{};
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
		}
	}

	void StandDown()
	{
		if (off.load(std::memory_order_relaxed) || off.exchange(true, std::memory_order_relaxed)) {
			return;
		}
		asksLeft.store(0, std::memory_order_relaxed);
		if (live.load(std::memory_order_relaxed)) {
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddUITask([] {
					Call<1>("setHidden", { RE::GFxValue(1.0) });
					live.store(false, std::memory_order_relaxed);
				});
			} else {
				live.store(false, std::memory_order_relaxed);
			}
		}
		SKSE::log::info("HUD soul gem: HUD NG places the gems, the HUD-layer movie stands down");
	}
}
