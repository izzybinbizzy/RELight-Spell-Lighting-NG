// RadiantBowColors.cpp - Radiant Bow (korosukenari, Nexus 198744) recolors its bow and its Sky Rune effects by a pick on its MCM's
// Colours page; our lights on them follow it, as every light of ours follows the art the player sees. Its picks are read from MCM
// Helper's files at data load and each time the Journal menu (where the MCM lives) closes: the bow and the summon's hand art take
// the Bow pick, the Sky Rune flames and the Hallowed Bolt burst the Sky impact pick. A pick takes our light's own color to its hue
// and keeps the light's saturation and strength; White takes the color out. The named colors' hues are written nowhere Radiant Bow
// ships, so a named pick keeps our own color until each hue is read off its art in game (kNamedHue).
// Shared by Illuminated and RELight (master in Tools\Shared Plugin Code).

#include "Fade.h"
#include "Plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kPlugin = "RadiantBow.esp";
		constexpr RE::FormID       kBowSpell = 0x80D;  // SMF_RB_Spell: its casting art is the summon's hand art
		constexpr RE::FormID       kBow = 0x80B;       // SMF_RB_Bow
		constexpr const char*      kDefaults = "Data/MCM/Config/RadiantBow/settings.ini";
		constexpr const char*      kUser = "Data/MCM/Settings/RadiantBow.ini";

		// our rows' own colors (gen.RADIANTBOW): the hand art, the bow, the Sky Rune flames, the Hallowed Bolt burst
		constexpr std::array<float, 3> kHand{ 93.0f, 174.0f, 255.0f };
		constexpr std::array<float, 3> kBowColor{ 7.0f, 46.0f, 255.0f };
		constexpr std::array<float, 3> kInferno{ 75.0f, 171.0f, 255.0f };
		constexpr std::array<float, 3> kBolt{ 18.0f, 92.0f, 255.0f };

		// Radiant Bow's color ids: 0 original, 1 gold, 2 red, 3 purple, 4 green, 5 cyan, 6 pink, 7 white, 8 custom (by hue); the
		// named hues stay unset (below 0) until read off its art in game - a pick without one keeps our color
		constexpr std::array<float, 7> kNamedHue{ -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
		constexpr int                  kOriginal = 0, kWhite = 7, kCustom = 8;

		struct Pick
		{
			int   id{ kOriginal };
			float hue{ 210.0f };
		};

		std::mutex                 gLock;
		std::optional<RE::NiColor> gInferno, gBolt;  // gLock
		std::atomic<bool>          gOn{ false };
		std::atomic<std::uint32_t> gGeneration{ 0 };  // moves on with each read, so a color cached per light is read again

		[[nodiscard]] std::string Trim(std::string_view a_s)
		{
			const auto first = a_s.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_s.find_last_not_of(" \t\r\n");
			return std::string(a_s.substr(first, last - first + 1));
		}

		// one ini file's [Colours] keys, lower-cased, into a_out (a later file's keys win)
		void ReadColours(const char* a_path, std::map<std::string, std::string>& a_out)
		{
			std::ifstream in(a_path);
			std::string   line;
			bool          colours = false;
			while (std::getline(in, line)) {
				auto text = Trim(line);
				if (text.starts_with("\xEF\xBB\xBF")) {
					text = Trim(std::string_view(text).substr(3));
				}
				if (text.empty() || text[0] == ';' || text[0] == '#') {
					continue;
				}
				if (text[0] == '[') {
					colours = _strnicmp(text.c_str(), "[Colours]", 9) == 0;
					continue;
				}
				const auto eq = text.find('=');
				if (!colours || eq == std::string::npos) {
					continue;
				}
				auto key = Trim(std::string_view(text).substr(0, eq));
				auto value = Trim(std::string_view(text).substr(eq + 1));
				if (const auto cut = value.find(';'); cut != std::string::npos) {
					value = Trim(std::string_view(value).substr(0, cut));
				}
				std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				a_out[key] = value;
			}
		}

		[[nodiscard]] float Number(const std::map<std::string, std::string>& a_keys, const char* a_key, float a_default)
		{
			const auto it = a_keys.find(a_key);
			if (it == a_keys.end()) {
				return a_default;
			}
			char*       end = nullptr;
			const float v = std::strtof(it->second.c_str(), &end);
			return end != it->second.c_str() && std::isfinite(v) ? v : a_default;
		}

		// an item's pick: its own (0 = as All, else the id + 1), else All's
		[[nodiscard]] Pick PickOf(const std::map<std::string, std::string>& a_keys, const char* a_item, const char* a_hue)
		{
			const Pick global{ std::clamp(static_cast<int>(Number(a_keys, "iglobal", 0.0f)), 0, kCustom), Number(a_keys, "fglobalhue", 210.0f) };
			const int  own = std::clamp(static_cast<int>(Number(a_keys, a_item, 0.0f)), 0, kCustom + 1);
			return own == 0 ? global : Pick{ own - 1, Number(a_keys, a_hue, 210.0f) };
		}

		// our light's color for a pick, or none when the pick leaves it as made
		[[nodiscard]] std::optional<RE::NiColor> Tint(const std::array<float, 3>& a_ours, const Pick& a_pick, float a_saturation)
		{
			float hue = -1.0f;
			if (a_pick.id == kCustom) {
				hue = a_pick.hue;
			} else if (a_pick.id > kOriginal && a_pick.id < kWhite) {
				hue = kNamedHue[static_cast<std::size_t>(a_pick.id)];
			}
			if (a_pick.id != kWhite && hue < 0.0f) {
				return std::nullopt;
			}
			// our color's value and saturation, the pick's hue (HSV)
			const float r = a_ours[0] / 255.0f, g = a_ours[1] / 255.0f, b = a_ours[2] / 255.0f;
			const float v = (std::max)({ r, g, b });
			const float low = (std::min)({ r, g, b });
			float       s = v > 0.0f ? (v - low) / v : 0.0f;
			s = a_pick.id == kWhite ? 0.0f : std::clamp(s * a_saturation, 0.0f, 1.0f);
			const float          h = std::fmod(std::fmod(hue < 0.0f ? 0.0f : hue, 360.0f) + 360.0f, 360.0f) / 60.0f;
			const float          c = v * s;
			const float          x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
			const float          m = v - c;
			std::array<float, 3> out{};
			switch (static_cast<int>(h)) {
			case 0:
				out = { c, x, 0.0f };
				break;
			case 1:
				out = { x, c, 0.0f };
				break;
			case 2:
				out = { 0.0f, c, x };
				break;
			case 3:
				out = { 0.0f, x, c };
				break;
			case 4:
				out = { x, 0.0f, c };
				break;
			default:
				out = { c, 0.0f, x };
				break;
			}
			return RE::NiColor{ out[0] + m, out[1] + m, out[2] + m };
		}

		void Read()
		{
			std::map<std::string, std::string> keys;
			ReadColours(kDefaults, keys);
			ReadColours(kUser, keys);
			const float saturation = std::clamp(Number(keys, "fsaturation", 1.0f), 0.0f, 2.0f);
			const Pick  bow = PickOf(keys, "ibow", "fbowhue");
			const Pick  sky = PickOf(keys, "iskyimpact", "fskyimpacthue");
			auto*       dh = RE::TESDataHandler::GetSingleton();
			const auto* spell = dh ? dh->LookupForm(kBowSpell, kPlugin) : nullptr;
			const auto* weapon = dh ? dh->LookupForm(kBow, kPlugin) : nullptr;
			if (spell) {
				Fade::Items::SetAutoColor(spell->GetFormID(), Tint(kHand, bow, saturation));
			}
			if (weapon) {
				Fade::Items::SetAutoColor(weapon->GetFormID(), Tint(kBowColor, bow, saturation));
			}
			{
				std::lock_guard lock(gLock);
				gInferno = Tint(kInferno, sky, saturation);
				gBolt = Tint(kBolt, sky, saturation);
			}
			gGeneration.fetch_add(1, std::memory_order_release);
			SKSE::log::info("Radiant Bow: Bow pick {} (hue {}), Sky impact pick {} (hue {}), saturation {}", bow.id, bow.hue, sky.id, sky.hue, saturation);
		}

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				// the MCM is in the Journal menu: its picks are read again once it closes, on the main thread
				if (a_event && !a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([]() { LightKit::Guarded("radiant bow colors", Read); });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void WatchRadiantBow()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh || !dh->LookupModByName(kPlugin)) {
			return;
		}
		Read();
		static MenuSink sink;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
		}
		gOn.store(true, std::memory_order_relaxed);
	}

	std::uint32_t RadiantBowGeneration() { return gGeneration.load(std::memory_order_acquire); }

	bool RadiantBowSkyModel(std::string_view a_model, bool a_hazard)
	{
		// only the two models our rows light (gen.RADIANTBOW): any other of its effects keeps the colors it was made with
		constexpr std::string_view kInfernoModel = "smf\\radiantbow\\sky\\rb_sky_inferno.nif";
		constexpr std::string_view kBoltModel = "smf\\radiantbow\\sky\\rb_sky_hbolt2.nif";
		if (!gOn.load(std::memory_order_relaxed)) {
			return false;
		}
		std::string model(a_model);
		std::ranges::replace(model, '/', '\\');
		std::string_view path(model);
		if (path.size() >= 7 && _strnicmp(path.data(), "meshes\\", 7) == 0) {
			path.remove_prefix(7);
		}
		const auto want = a_hazard ? kInfernoModel : kBoltModel;
		return path.size() == want.size() && _strnicmp(path.data(), want.data(), want.size()) == 0;
	}

	std::optional<RE::NiColor> RadiantBowSkyColor(std::string_view a_model, bool a_hazard)
	{
		if (!RadiantBowSkyModel(a_model, a_hazard)) {
			return std::nullopt;
		}
		std::lock_guard lock(gLock);
		return a_hazard ? gInferno : gBolt;
	}
}
