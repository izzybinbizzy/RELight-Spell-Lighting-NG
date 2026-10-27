// RELight - Spell Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE and the notice at the top of main.cpp.
//
// Reads the data files the installer put down, once, when the game has loaded its data.
//
// A line is TAB-separated (the build writes them; `relightgen.py read_data_text` is the same reader in Python and the
// build refuses a file it cannot read):
//   version 1 | file <download> <option> | order <n> | menu <n> | switch <0|1> | mesh <key> | base <0xID~Plugin>
//   stream <key> <node> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>
//   hand <key> <r> <g> <b> <fade> <radius> <size> <cutoff> <inverse square 0|1> <portal strict 0|1>
//   spraylight <0xID~Plugin> [<r> <g> <b>]   a light record a spray makes; since 2.3 each spray wears a raised copy of it
//                              (Streams.cpp); the color only where his eye set one, else the record's own
//   beamlight <0xID~Plugin> [<r> <g> <b>]    the same for a beam our data lights (the bolts)
//   handfx <key> <pulse|flicker> <per second> <intensity>   a hand light's Dynamic Lighting (installed with it)
//   menugroup <category> <author or ->   where a patch sits on the Patches page
//   desc <text>   what the option lights, broadly - the menu shows it over the option's switch
//   download <Spells|Weapons>   the download the file came in (a file with no line: its `file` download)
//   tint <key or 0xID~Plugin> <r> <g> <b>   an art pick's color for the lights on an object (highest order wins)
//   held <key or 0xID~Plugin> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>   a weapon's own light while it is
//        drawn (Held.cpp); a key is matched exactly against the weapon's model (its full path key, then its bare name), or
//        its first-person model in first person
//   body <key> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>   the light for that art model when the game hangs
//        it on an actor (a cloak, a heal - BodyLights.cpp; never a spell's casting art); matched exactly, as `held` is
//   cloud <key> <r> <g> <b> <fade> <radius> <size> <cutoff> <x> <y> <z>   a cone whose one light stays where its cloud is
//        (Rows.cpp), in place of RE::Light's, which rides the cone away; matched exactly, as `body` is
//
// Objects are matched as RE::Light matches them: a form first, then the bare mesh key ("Magic\RuneFire01.nif" is
// "runefire01"), exactly, then the longest of our keys the object's key contains.

#include "DataText.h"
#include "Fade.h"
#include "FadeFormText.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		namespace fs = std::filesystem;
		constexpr std::string_view kFolder = "Data/SKSE/Plugins/RELightSpellLightingNG";
		// Dynamic Wards 2.x wrote its ward-hand data file into this mod's folder under the old name (RelightSpellAddon); 3.x
		// writes none. Read that one file there for players still on 2.x, never anything else of the old folder
		constexpr std::string_view kOldDynamicWards = "Data/SKSE/Plugins/RelightSpellAddon/Dynamic Wards.txt";

		struct Tint
		{
			RE::NiColor color;
			int         order{ 0 };
		};

		std::vector<Option>                         gOptions;
		std::vector<std::size_t>                    gMenuOrder;  // gOptions by `menu`, made once with the data (main thread)
		StringMap<std::size_t>                      gMeshOwner;  // key -> option (highest order wins)
		std::unordered_map<RE::FormID, std::size_t> gBaseOwner;  // form -> option
		StringMap<Stream>                           gStreams;    // key -> recipe (highest order wins)
		StringMap<Stream>                           gHeld;       // weapon mesh key -> its held light (highest order wins)
		std::unordered_map<RE::FormID, Stream>      gHeldBase;   // weapon form -> its held light (highest order wins)
		StringMap<Stream>                           gBody;       // art model key -> its body light (highest order wins)
		StringMap<Stream>                           gCloud;      // a cloud cone's model key -> its light (highest order wins)
		std::unordered_map<RE::FormID, Stream>      gBodyBase;   // an effect shader's form -> its body light
		// key -> EVERY layer's hand light, highest order first: a switched-off option hands the hand to the next layer
		StringMap<std::vector<Hand>>   gHands;
		std::unordered_set<RE::FormID> gSprayLights;
		std::unordered_set<RE::FormID> gBeamLights;  // a beam's own light record (Streams.cpp raises a copy of it)
		struct RecordTint
		{
			RE::Color color;
			int       order{ 0 };
		};
		// his color for a spray / beam record, where the data names one (highest order wins, as every other per-key line -
		// CodeRabbit on #18: the file read last decided it before)
		std::unordered_map<RE::FormID, RecordTint>  gRecordColors;
		StringMap<HandFx>                           gHandFx;
		StringMap<Tint>                             gMeshTints;
		std::unordered_map<RE::FormID, std::size_t> gShaderOwner;  // effect shader -> option, by its editor ID (enchantments)
		std::unordered_map<RE::FormID, Tint>        gBaseTints;
		std::vector<std::string>                    gKeysLongestFirst, gStreamKeysLongestFirst, gTintKeysLongestFirst;
		std::size_t                                 gFiles = 0;

		// what a base form resolves to never changes after the data has loaded; Load3D asks from loader threads too
		std::mutex                                         gCacheLock;
		std::unordered_map<RE::FormID, std::size_t>        gOwnerCache;
		std::unordered_map<RE::FormID, const Stream*>      gStreamCache;
		std::unordered_map<RE::FormID, const RE::NiColor*> gTintCache;
		std::unordered_map<RE::FormID, const Stream*>      gHeldCache[2];  // [first person]: asked every frame per drawn weapon
		std::unordered_map<RE::FormID, const Stream*>      gBodyCache;     // art object -> its body light: asked every frame per art

		// a bad data line never gives a negative or over-bright light
		[[nodiscard]] RE::NiColor ColorOf(float a_r, float a_g, float a_b)
		{
			const auto unit = [](float a_v) { return std::clamp(a_v, 0.0f, 255.0f) / 255.0f; };
			return { unit(a_r), unit(a_g), unit(a_b) };
		}

		[[nodiscard]] RE::FormID ResolveBase(std::string_view a_text)
		{
			// "0x1540E~Dawnguard.esm"
			const auto tilde = a_text.find('~');
			if (tilde == std::string_view::npos || tilde < 3) {
				return 0;
			}
			std::uint32_t local = 0;
			if (!FormText::ParseHex(a_text.substr(0, tilde), local)) {  // the whole text is the number (2.2's CodeRabbit round)
				return 0;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh ? dh->LookupFormID(local, a_text.substr(tilde + 1)) : 0;
		}

		template <class Map, class Value>
		void KeepHighest(Map& a_map, const typename Map::key_type& a_key, Value&& a_value, int a_order)
		{
			if (auto it = a_map.find(a_key); it == a_map.end() || it->second.order <= a_order) {
				a_map.insert_or_assign(a_key, std::forward<Value>(a_value));
			}
		}

		// one file -> one option; false when it does not read, and nothing of it is kept. The text is read by DataText.h
		// (game-free, tested off the game); here its forms are resolved and its lights become the game's values
		[[nodiscard]] bool ReadFile(const fs::path& a_path)
		{
			std::ifstream in(a_path, std::ios::binary);
			if (!in) {
				return false;
			}
			const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			const auto        read = DataText::ReadText(text);
			if (read.badLine) {
				SKSE::log::warn("{} line {}: not a line this plugin reads", Fade::PathText(a_path.filename()), read.badLine);
			}
			if (!read.ok) {
				return false;
			}
			const auto& file = read.file;
			// an art pick's file counts only while that pick is chosen, a Dynamic Lighting file only while it is on (the installer's
			// questions until 2.6 - his order to move them into the menu)
			if (const int g = DataText::PickGroup(file.option.pickGroup); g >= 0 && ArtPick(g) != DataText::PickChoice(g, file.option.pickName)) {
				return true;
			}
			if (file.option.dynamic && !DynamicLightingThisSession()) {
				return true;
			}
			Option opt;
			opt.download = file.option.download;
			opt.name = file.option.name;
			opt.id = file.option.id;
			opt.category = file.option.category;
			opt.author = file.option.author;
			opt.desc = file.option.desc;
			opt.order = file.option.order;
			opt.menu = file.option.menu;
			opt.switchable = file.option.switchable;
			opt.weapons = file.sawDownload ? file.option.weapons : opt.download == "Weapons";  // no `download` line: the heading was the download
			std::vector<RE::FormID> bases, sprays;
			for (const auto& b : file.bases) {
				if (const auto id = ResolveBase(b)) {
					bases.push_back(id);
				}
			}
			for (const auto& s : file.sprays) {
				if (const auto id = ResolveBase(s)) {
					sprays.push_back(id);
				}
			}
			const auto light = [](const DataText::LightText& a_text) {
				const auto& v = a_text.v;
				Stream      s;
				s.key = a_text.key;
				s.node = a_text.node;
				s.color = ColorOf(v[0], v[1], v[2]);
				s.fade = v[3];
				s.radius = v[4];
				s.size = v[5];
				s.cutoff = v[6];
				s.at = { v[7], v[8], v[9] };
				return s;
			};
			std::vector<Stream> streams, helds, bodies, clouds;
			for (const auto& s : file.streams) {
				streams.push_back(light(s));
			}
			for (const auto& s : file.helds) {
				helds.push_back(light(s));
			}
			for (const auto& s : file.bodies) {
				bodies.push_back(light(s));
			}
			for (const auto& s : file.clouds) {
				clouds.push_back(light(s));
			}
			const auto idx = gOptions.size();
			opt.meshes = file.meshes.size();
			opt.bases = bases.size();
			opt.streams = streams.size();
			gOptions.push_back(opt);
			const auto wins = [&](std::size_t a_current) { return gOptions[a_current].order <= opt.order; };
			const auto own = [&](auto& a_map, const auto& a_key) {
				if (auto it = a_map.find(a_key); it == a_map.end() || wins(it->second)) {
					a_map.insert_or_assign(a_key, idx);
				}
			};
			for (const auto& m : file.meshes) {
				own(gMeshOwner, m);
			}
			for (const auto id : bases) {
				own(gBaseOwner, id);
			}
			// a later layer takes a stream over (RE::Light lights it; the line only makes its object ours for the menu)
			for (auto& s : streams) {
				s.order = opt.order;
				s.option = idx;
				own(gMeshOwner, s.key);  // a stream's object is ours too, for the slider and the switch
				if (auto it = gStreams.find(s.key); it == gStreams.end() || it->second.order <= s.order) {
					gStreams.insert_or_assign(s.key, std::move(s));
				}
			}
			for (const auto id : sprays) {
				gSprayLights.insert(id);
			}
			for (const auto& b : file.beams) {
				if (const auto id = ResolveBase(b)) {
					gBeamLights.insert(id);
				}
			}
			for (const auto& [form, c] : file.recordColors) {
				if (const auto id = ResolveBase(form)) {
					const RecordTint tint{ RE::Color(static_cast<std::uint8_t>(c[0]), static_cast<std::uint8_t>(c[1]), static_cast<std::uint8_t>(c[2]), 0), opt.order };
					KeepHighest(gRecordColors, id, tint, opt.order);
				}
			}
			for (auto& s : helds) {
				s.order = opt.order;
				s.option = idx;
				const auto key = s.key;
				if (key.find('~') == std::string::npos) {
					KeepHighest(gHeld, key, std::move(s), opt.order);
				} else if (const auto id = ResolveBase(key)) {
					KeepHighest(gHeldBase, id, std::move(s), opt.order);
				}
			}
			for (auto& s : bodies) {
				s.order = opt.order;
				s.option = idx;
				const auto key = s.key;
				if (key.find('~') == std::string::npos) {
					KeepHighest(gBody, key, std::move(s), opt.order);
				} else if (const auto id = ResolveBase(key)) {
					KeepHighest(gBodyBase, id, std::move(s), opt.order);
				}
			}
			for (auto& s : clouds) {
				s.order = opt.order;
				s.option = idx;
				own(gMeshOwner, s.key);  // its cone is ours, for the switch
				const auto key = s.key;
				KeepHighest(gCloud, key, std::move(s), opt.order);
			}
			for (const auto& fx : file.fx) {
				KeepHighest(gHandFx, fx.key, HandFx{ fx.flicker, fx.perSecond, fx.intensity, opt.order }, opt.order);
			}
			for (const auto& tint : file.tints) {
				const auto& key = tint.key;
				const Tint  t{ ColorOf(tint.rgb[0], tint.rgb[1], tint.rgb[2]), opt.order };
				if (const auto id = key.find('~') != std::string::npos ? ResolveBase(key) : 0) {
					KeepHighest(gBaseTints, id, t, opt.order);
				} else {
					KeepHighest(gMeshTints, key, t, opt.order);
				}
			}
			for (const auto& hand : file.hands) {
				const auto& v = hand.v;
				Hand        h;
				h.key = hand.key;
				for (std::size_t i = 0; i < 3; ++i) {
					h.rgb[i] = static_cast<std::uint8_t>(std::clamp(v[i], 0.0f, 255.0f));
				}
				h.color = ColorOf(v[0], v[1], v[2]);
				h.fade = v[3];
				h.radius = v[4];
				h.size = v[5];
				h.cutoff = v[6];
				h.inverseSquare = v[7] != 0.0f;
				h.portalStrict = v[8] != 0.0f;
				h.order = opt.order;
				h.option = idx;
				gHands[h.key].push_back(std::move(h));
			}
			return true;
		}

		[[nodiscard]] std::string KeyOf(const RE::TESForm* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			return (path && *path) ? MeshKey(path) : std::string{};
		}

		// RE::Light's rule: the exact key, else the longest of ours the object's key contains
		template <class Map>
		[[nodiscard]] auto Find(const Map& a_map, const std::vector<std::string>& a_longestFirst, std::string_view a_key)
		{
			if (auto it = a_map.find(a_key); it != a_map.end()) {
				return it;
			}
			for (const auto& k : a_longestFirst) {
				if (k.size() < a_key.size() && a_key.find(k) != std::string_view::npos) {
					return a_map.find(k);
				}
			}
			return a_map.end();
		}

		template <class Map>
		[[nodiscard]] std::vector<std::string> LongestFirst(const Map& a_map)
		{
			std::vector<std::string> keys;
			keys.reserve(a_map.size());
			for (const auto& k : a_map | std::views::keys) {
				keys.push_back(k);
			}
			std::ranges::sort(keys, [](const auto& a, const auto& b) { return a.size() != b.size() ? a.size() > b.size() : a < b; });
			return keys;
		}

		// RE::Light reads an effect shader's editor ID through powerofthree's Tweaks (clib_util::editorID) - the game keeps
		// none for this form type - and so does this
		[[nodiscard]] std::string EditorIdOf(const RE::TESForm* a_form)
		{
			using GetFormEditorID = const char* (*)(std::uint32_t);
			static const auto fn = [] {
				const auto tweaks = REX::W32::GetModuleHandleW(L"po3_Tweaks");
				return tweaks ? reinterpret_cast<GetFormEditorID>(REX::W32::GetProcAddress(tweaks, "GetFormEditorID")) : nullptr;
			}();
			const char* id = fn ? fn(a_form->GetFormID()) : a_form->GetFormEditorID();
			return id ? std::string(id) : std::string{};
		}

		// RE::Light's enchantment light (ShaderReferenceEffect::Init): a config by the shader's form, else by its
		// lower-cased editor ID as an EXACT `meshPath` key (findConfigsForMeshPath is a plain lookup, no substring)
		void FindShaderOwners()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			std::size_t named = 0;
			for (const auto* shader : dh->GetFormArray<RE::TESEffectShader>()) {
				if (!shader) {
					continue;
				}
				if (auto it = gBaseOwner.find(shader->GetFormID()); it != gBaseOwner.end()) {
					gShaderOwner.emplace(shader->GetFormID(), it->second);
					continue;
				}
				const auto id = Lower(EditorIdOf(shader));
				named += id.empty() ? 0 : 1;
				if (auto it = id.empty() ? gMeshOwner.end() : gMeshOwner.find(id); it != gMeshOwner.end()) {
					gShaderOwner.emplace(shader->GetFormID(), it->second);
				}
			}
			SKSE::log::info("enchantment lights: {} effect shader(s) are ours, {} of {} had an editor ID (powerofthree's Tweaks {})",
				gShaderOwner.size(), named, dh->GetFormArray<RE::TESEffectShader>().size(),
				REX::W32::GetModuleHandleW(L"po3_Tweaks") ? "loaded" : "NOT loaded");
		}

		// a per-form answer, worked out once and cached. A form made while the game runs (its ID starts FF) is never cached:
		// those IDs are handed out again to other forms after a load, so a cached answer could be another form's (the audit,
		// 2026-10-14)
		template <class V, class F>
		[[nodiscard]] V Cached(std::unordered_map<RE::FormID, V>& a_cache, const RE::TESForm* a_base, F&& a_work)
		{
			const auto id = a_base->GetFormID();
			if ((id >> 24) == 0xFF) {
				return a_work();
			}
			{
				std::lock_guard l{ gCacheLock };
				if (auto it = a_cache.find(id); it != a_cache.end()) {
					return it->second;
				}
			}
			const V         value = a_work();
			std::lock_guard l{ gCacheLock };
			return a_cache.emplace(id, value).first->second;
		}
	}

	std::string MeshKey(std::string_view a_path)
	{
		if (const auto slash = a_path.find_last_of("\\/"); slash != std::string_view::npos) {
			a_path.remove_prefix(slash + 1);
		}
		std::string out = Lower(a_path);
		if (out.size() > 4 && out.ends_with(".nif")) {
			out.resize(out.size() - 4);
		}
		return out;
	}

	// The full key, for two mods' art under one file name: a `hand` line may name the whole path, looked up before the
	// bare name. Same rule as relightgen.path_key: lowercase, backslashes, no leading `meshes\`, no `.nif`.
	std::string PathKey(std::string_view a_path)
	{
		std::string out = Lower(a_path);
		std::ranges::replace(out, '/', '\\');
		const auto first = out.find_first_not_of('\\');
		out.erase(0, first == std::string::npos ? out.size() : first);
		if (out.starts_with("meshes\\")) {
			out.erase(0, 7);
		}
		if (out.size() > 4 && out.ends_with(".nif")) {
			out.resize(out.size() - 4);
		}
		return out;
	}

	void LoadData()
	{
		gOptions.clear();
		gMeshOwner.clear();
		gBaseOwner.clear();
		gStreams.clear();
		gHeld.clear();
		gHeldBase.clear();
		gBody.clear();
		gCloud.clear();
		gBodyBase.clear();
		gHands.clear();
		gSprayLights.clear();
		gBeamLights.clear();
		gRecordColors.clear();
		gHandFx.clear();
		gMeshTints.clear();
		gBaseTints.clear();
		gShaderOwner.clear();
		{
			std::lock_guard l{ gCacheLock };
			gOwnerCache.clear();
			gStreamCache.clear();
			gTintCache.clear();
			gHeldCache[0].clear();
			gHeldCache[1].clear();
			gBodyCache.clear();
		}
		gFiles = 0;
		std::vector<fs::path> files;
		std::error_code       ec;
		for (fs::directory_iterator it{ fs::path(kFolder), ec }, end; !ec && it != end; it.increment(ec)) {
			if (it->is_regular_file() && it->path().extension() == ".txt") {
				files.push_back(it->path());
			}
		}
		std::ranges::sort(files);
		const fs::path  oldDynamicWards{ kOldDynamicWards };
		std::error_code oldEc;  // its own: the folder scan's ec above stays that scan's
		if (fs::is_regular_file(oldDynamicWards, oldEc) &&
			std::ranges::none_of(files, [&](const fs::path& f) { return f.filename() == oldDynamicWards.filename(); })) {
			files.push_back(oldDynamicWards);
		}
		std::size_t bad = 0;
		for (const auto& f : files) {
			if (ReadFile(f)) {
				++gFiles;
			} else {
				++bad;
				SKSE::log::warn("{} does not read - its lights stay as RE::Light makes them", Fade::PathText(f.filename()));
			}
		}
		gKeysLongestFirst = LongestFirst(gMeshOwner);
		FindShaderOwners();
		gStreamKeysLongestFirst = LongestFirst(gStreams);
		gTintKeysLongestFirst = LongestFirst(gMeshTints);
		for (auto& list : gHands | std::views::values) {
			std::ranges::stable_sort(list, std::greater{}, &Hand::order);
		}
		gMenuOrder.resize(gOptions.size());
		std::iota(gMenuOrder.begin(), gMenuOrder.end(), std::size_t{ 0 });
		std::ranges::stable_sort(gMenuOrder, [](std::size_t a, std::size_t b) {
			const auto& x = gOptions[a];
			const auto& y = gOptions[b];
			return x.menu != y.menu ? x.menu < y.menu : x.id < y.id;
		});
		const auto switches = std::ranges::count_if(gOptions, &Option::switchable);
		SKSE::log::info(
			"data: {} file(s) read, {} did not; {} switch(es), {} object key(s), {} form(s), {} stream(s), {} hand key(s), "
			"{} tint(s), {} held weapon light(s), {} body light(s)",
			gFiles, bad, switches, gMeshOwner.size(), gBaseOwner.size(), gStreams.size(), gHands.size(),
			gMeshTints.size() + gBaseTints.size(), gHeld.size() + gHeldBase.size(), gBody.size());
	}

	std::vector<Option>& Options() { return gOptions; }
	std::size_t          DataFiles() { return gFiles; }

	const std::vector<std::size_t>& OptionsInMenuOrder() { return gMenuOrder; }

	std::size_t OptionOf(const RE::TESForm* a_base)
	{
		if (!a_base) {
			return kNone;
		}
		return Cached(gOwnerCache, a_base, [&] {
			if (auto it = gBaseOwner.find(a_base->GetFormID()); it != gBaseOwner.end()) {
				return it->second;
			}
			const auto key = KeyOf(a_base);
			const auto it = key.empty() ? gMeshOwner.cend() : Find(gMeshOwner, gKeysLongestFirst, key);
			return it != gMeshOwner.cend() ? it->second : kNone;
		});
	}

	std::size_t OptionOfShader(const RE::TESEffectShader* a_shader)
	{
		const auto it = a_shader ? gShaderOwner.find(a_shader->GetFormID()) : gShaderOwner.end();
		return it != gShaderOwner.end() ? it->second : kNone;
	}

	bool OptionLit(std::size_t a_option)
	{
		if (a_option >= gOptions.size()) {
			return true;
		}
		const auto& o = gOptions[a_option];
		return (!o.switchable || o.on) && (!o.weapons || WeaponLightsOn());
	}

	const Stream* StreamOf(const RE::TESForm* a_base)
	{
		if (!a_base || gStreams.empty()) {
			return nullptr;
		}
		return Cached(gStreamCache, a_base, [&]() -> const Stream* {
			const auto key = KeyOf(a_base);
			const auto it = key.empty() ? gStreams.cend() : Find(gStreams, gStreamKeysLongestFirst, key);
			return it != gStreams.cend() ? &it->second : nullptr;
		});
	}

	namespace
	{
		// the model keys of the game's own projectiles and explosions: the art the picks recolor, whoever's form wears it
		[[nodiscard]] bool GameArtKey(const std::string& a_key)
		{
			static const auto keys = [] {
				std::unordered_set<std::string>           out;
				constexpr std::array<std::string_view, 5> kGame{ "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm" };
				const auto                                add = [&](const RE::TESForm* a_form) {
					const auto* file = a_form ? a_form->GetFile(0) : nullptr;
					if (file && std::ranges::find(kGame, Lower(file->GetFilename())) != kGame.end()) {
						if (auto key = KeyOf(a_form); !key.empty()) {
							out.insert(std::move(key));
						}
					}
				};
				for (const auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
					add(proj);
				}
				for (const auto* expl : LightKit::FormsOf<RE::BGSExplosion>()) {
					add(expl);
				}
				for (const auto* hazard : LightKit::FormsOf<RE::BGSHazard>()) {
					add(hazard);
				}
				return out;
			}();
			return keys.contains(a_key);
		}

		// the hazards a spell leaves: a Spawn Hazard effect's, the wall pieces an effect's impact data places (Wall of Flames ...),
		// and the burning ground its explosion places - a trap's hazard is not one
		[[nodiscard]] bool SpellHazard(const RE::TESForm* a_form)
		{
			static const auto hazards = [] {
				std::unordered_set<const RE::TESForm*> out;
				const auto                             addPlaced = [&out](const RE::BGSExplosion* a_expl) {
					if (a_expl) {
						out.insert(a_expl->data.impactPlacedObject);
					}
				};
				for (const auto* mgef : LightKit::FormsOf<RE::EffectSetting>()) {
					if (!mgef) {
						continue;
					}
					if (mgef->HasArchetype(RE::EffectSetting::Archetype::kSpawnHazard)) {
						out.insert(mgef->data.associatedForm);
					}
					if (const auto* set = mgef->data.impactDataSet) {
						for (const auto& entry : set->impactMap) {
							out.insert(entry.second ? entry.second->hazard : nullptr);
						}
					}
					addPlaced(mgef->data.explosion);
					if (const auto* proj = mgef->data.projectileBase) {
						addPlaced(proj->data.explosionType);
					}
				}
				out.erase(nullptr);
				return out;
			}();
			return hazards.contains(a_form);
		}

		// a projectile, explosion or spell hazard of the game's own art wears its element's hand color for the pick, ahead of the
		// pick's tint lines, so every light of the spell is one color; atronach and wraith art is left to the elementals' own mods
		[[nodiscard]] const RE::NiColor* ElementHandColor(const RE::TESForm* a_base, const std::string& a_key)
		{
			if (!a_base || !(a_base->Is(RE::FormType::Projectile) || a_base->Is(RE::FormType::Explosion) || a_base->Is(RE::FormType::Hazard)) ||
				a_key.empty() || !GameArtKey(a_key) || (a_base->Is(RE::FormType::Hazard) && !SpellHazard(a_base))) {
				return nullptr;
			}
			const auto has = [&](std::string_view a_word) { return a_key.find(a_word) != std::string::npos; };
			if (has("atronach") || has("wraith")) {
				return nullptr;
			}
			const std::string_view hand = has("shock") || has("lightning") || has("spark") ? "shockhandeffects" :
			                              has("fire") || has("flame")                      ? "fireball01handeffects" :
			                              has("frost") || (has("ice") && !has("voice"))    ? "frostbolthandfx01" :
			                                                                                 "";
			if (hand.empty()) {
				return nullptr;
			}
			if (const auto t = gMeshTints.find(hand); t != gMeshTints.end()) {
				return &t->second.color;
			}
			const auto h = gHands.find(hand);
			if (h == gHands.end() || h->second.empty()) {
				return nullptr;
			}
			return &std::ranges::max_element(h->second, {}, &Hand::order)->color;
		}
	}

	const RE::NiColor* HandColorOf(const RE::TESForm* a_base)
	{
		return a_base ? ElementHandColor(a_base, KeyOf(a_base)) : nullptr;
	}

	const RE::NiColor* TintOf(const RE::TESForm* a_base)
	{
		if (!a_base || (gMeshTints.empty() && gBaseTints.empty() && gHands.empty())) {
			return nullptr;
		}
		return Cached(gTintCache, a_base, [&]() -> const RE::NiColor* {
			const auto key = KeyOf(a_base);
			if (const auto* hand = ElementHandColor(a_base, key)) {
				return hand;
			}
			if (auto it = gBaseTints.find(a_base->GetFormID()); it != gBaseTints.end()) {
				return &it->second.color;
			}
			const auto it = key.empty() ? gMeshTints.cend() : Find(gMeshTints, gTintKeysLongestFirst, key);
			return it != gMeshTints.cend() ? &it->second.color : nullptr;
		});
	}

	const Stream* HeldOf(const RE::TESObjectWEAP* a_weapon, bool a_firstPerson)
	{
		if (!a_weapon || (gHeld.empty() && gHeldBase.empty())) {
			return nullptr;
		}
		return Cached(gHeldCache[a_firstPerson ? 1 : 0], a_weapon, [&]() -> const Stream* {
			if (auto it = gHeldBase.find(a_weapon->GetFormID()); it != gHeldBase.end()) {
				return &it->second;
			}
			// exact: a weapon's own mesh by its full path first (two mods' staves can share a file name), then its bare name -
			// never a key inside another. In first person the game draws the first-person model, which a config may light
			const auto find = [](const RE::TESForm* a_form) -> const Stream* {
				const auto* model = a_form ? a_form->As<RE::TESModel>() : nullptr;
				const char* path = model ? model->GetModel() : nullptr;
				if (!path || !*path) {
					return nullptr;
				}
				auto it = gHeld.find(PathKey(path));
				if (it == gHeld.end()) {
					it = gHeld.find(MeshKey(path));
				}
				return it != gHeld.end() ? &it->second : nullptr;
			};
			const Stream* first = a_firstPerson ? find(a_weapon->firstPersonModelObject) : nullptr;
			return first ? first : find(a_weapon);
		});
	}

	const Stream* BodyOf(const RE::BGSArtObject* a_art)
	{
		if (!a_art || gBody.empty()) {
			return nullptr;
		}
		// exact, as RE::Light's enchantment light and our held lights match: the art's model by its full path, then its bare
		// name - never a key inside another (the silver bowls, 2.2)
		return Cached(gBodyCache, a_art, [&]() -> const Stream* {
			const char* path = a_art->GetModel();
			if (!path || !*path) {
				return nullptr;
			}
			auto it = gBody.find(PathKey(path));
			if (it == gBody.end()) {
				it = gBody.find(MeshKey(path));
			}
			return it != gBody.end() ? &it->second : nullptr;
		});
	}

	const Stream* BodyOf(const RE::TESEffectShader* a_shader)
	{
		const auto it = a_shader ? gBodyBase.find(a_shader->GetFormID()) : gBodyBase.end();
		return it != gBodyBase.end() ? &it->second : nullptr;
	}

	const Stream* CloudOf(const RE::BGSProjectile* a_projectile)
	{
		// exact, as `body` matches: the cone's model by its full path, then its bare name (asked once, at data load)
		const char* path = a_projectile && !gCloud.empty() ? a_projectile->GetModel() : nullptr;
		if (!path || !*path) {
			return nullptr;
		}
		auto it = gCloud.find(PathKey(path));
		if (it == gCloud.end()) {
			it = gCloud.find(MeshKey(path));
		}
		return it != gCloud.end() ? &it->second : nullptr;
	}

	const StringMap<std::vector<Hand>>&   Hands() { return gHands; }
	const std::unordered_set<RE::FormID>& SprayLightRecords() { return gSprayLights; }
	const std::unordered_set<RE::FormID>& BeamLightRecords() { return gBeamLights; }

	std::optional<RE::Color> RecordColor(RE::FormID a_record)
	{
		const auto it = gRecordColors.find(a_record);
		return it == gRecordColors.end() ? std::nullopt : std::optional<RE::Color>(it->second.color);
	}

	const HandFx* HandFxOf(std::string_view a_key)
	{
		const auto it = gHandFx.find(a_key);
		return it == gHandFx.end() ? nullptr : &it->second;
	}
}
