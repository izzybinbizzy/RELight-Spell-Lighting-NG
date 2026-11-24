// BurningBanners.cpp - Burn'em Banners (GhdOne, Nexus 118694) makes Skyrim's banners burn and lights a burning banner itself: a
// Light Placer row on the model's ENBLight01 node (Community Shaders) and the ENB light shape under that node (ENBLight02; ENB, and
// Community Shaders' particle lights). Our lights are the default (rule file 6 1.3, his 647 "add it and don't worry about
// permission"): a burning banner wears our fire light on every lighting and its own light is put out - the ENB shape
// culled here, its Light Placer row waiting on our switch (lagen's copy of its file) - and "Burn'em Banners lights its banners"
// gives its light back. The game says when a banner starts and stops burning (its destruction stage) and when it leaves the loaded
// world, so nothing here looks round each frame. The light is the burning-ground fire's: its hazard light's color (Skyrim.esm
// 0x3E075), fade 1.0 and reach 500 (gen.py's GROUND_FIRE values), under Brightness, Reach, Ground effects and [Ground Effects] fire.
// Shared by Illuminated and RELight (master in Tools\Shared Plugin Code); what differs is each mod's BurnEmConfig.h.

#include "BurnEmConfig.h"
#include "GroundElement.h"
#include "Plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RE
{
	// the game's event (ScriptEventSourceHolder +0x3C8), which our CommonLib only declares; its fields as two public SKSE plugins
	// read them (ObjectImpactFramework, Apache-2.0; DbSkseFunctions)
	struct TESDestructionStageChangedEvent
	{
		NiPointer<TESObjectREFR> target;
		std::uint32_t            oldStage{ 0 };
		std::uint32_t            newStage{ 0 };
	};
	static_assert(sizeof(TESDestructionStageChangedEvent) == 0x10);
}

namespace Plugin
{
	namespace
	{
		constexpr const char*      kName = BurnEm::kLightName;
		constexpr const char*      kTheirShape = "ENBLight02";                 // its ENB light shape, under ENBLight01 in every banner model
		constexpr std::string_view kTheirLights = "LP_Light[BurnEmBanners\\";  // Light Placer's lights from its own file
		constexpr std::string_view kPlugin = "BurnEmBanners.esp";
		constexpr std::string_view kModels = "burnembanners\\";  // every model of its banners lives under this folder
		constexpr RE::FormID       kFireHazard = 0x3E075;        // Skyrim.esm's burning-ground fire, whose light gives the color
		constexpr float            kFade = 1.0f, kReach = 500.0f;
		constexpr std::size_t      kMost = 16;  // banners burning at once that get a light; the game lights few at once off Community Shaders

		struct Lit
		{
			RE::NiPointer<RE::NiPointLight>        light;
			RE::NiPointer<RE::BSLight>             bs;
			RE::NiPointer<RE::NiInterpController>  dimmer;
			RE::NiPointer<RE::NiFloatInterpolator> keys;
		};

		// Burn'em Banners' own light's fade over a burn (its Light Placer file's fadeController), as shares of our dressed fade: the game's
		// dimmer plays it on our light, so the light rises, holds and dies with the fire and no code of ours runs meanwhile
		constexpr std::array<std::pair<float, float>, 5> kBurn{ { { 0.0f, 0.0f }, { 4.0f, 0.1f }, { 6.0f, 1.0f }, { 20.0f, 1.0f }, { 25.0f, 0.0f } } };

		// the game's dimmer controller and its keys (not in our pin of CommonLib; the same ids as Creatures' CreatureLights.cpp)
		[[nodiscard]] bool DimmerReady()
		{
#ifdef ENABLE_SKYRIM_VR
			static const bool ready = REL::IDDB::get().IsVRAddressLibraryAtLeastVersion("0.295.0");
			return ready;
#else
			return true;
#endif
		}

		[[nodiscard]] RE::NiInterpController* NewDimmer()
		{
			using F = RE::NiInterpController* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(72049, 73695) };
			return make();
		}

		[[nodiscard]] RE::NiFloatInterpolator* NewInterpolator()
		{
			using F = RE::NiFloatInterpolator* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(71088, 72686) };
			return make();
		}

		[[nodiscard]] RE::NiFloatData* NewFloatData()
		{
			using F = RE::NiFloatData* (*)();
			static REL::Relocation<F> make{ RELOCATION_ID(71069, 72667) };
			return make();
		}

		[[nodiscard]] RE::NiFloatKey* NewLinearKeys(std::uint32_t a_count)
		{
			using F = RE::NiFloatKey* (*)(std::uint32_t);
			static REL::Relocation<F> make{ RELOCATION_ID(72540, 74222) };
			return make(a_count);
		}

		void ReplaceKeys(RE::NiFloatData* a_data, RE::NiFloatKey* a_keys, std::uint32_t a_count)
		{
			using F = void (*)(RE::NiFloatData*, RE::NiFloatKey*, std::uint32_t, RE::NiAnimationKey::KeyType);
			static REL::Relocation<F> replace{ RELOCATION_ID(71071, 72669) };
			replace(a_data, a_keys, a_count, RE::NiAnimationKey::KeyType::kLink);
		}

		// the burn's keys at a_fade: the values written over the keys already playing when there are some (a slider moved mid-burn, the
		// burn goes on where it is), else new keys
		void BurnKeys(Lit& a_lit, float a_fade)
		{
			if (auto* data = a_lit.keys ? a_lit.keys->floatData.get() : nullptr; data && data->keys && data->numKeys == kBurn.size()) {
				for (std::uint32_t i = 0; i < data->numKeys; ++i) {
					if (auto* key = data->keys->GetKeyAt(i, data->keySize)) {
						key->SetValue(kBurn[i].second * a_fade);
					}
				}
				return;
			}
			RE::NiPointer<RE::NiFloatInterpolator> interpolator(NewInterpolator());
			const RE::NiPointer<RE::NiFloatData>   data(NewFloatData());
			auto*                                  keys = interpolator && data ? NewLinearKeys(static_cast<std::uint32_t>(kBurn.size())) : nullptr;
			if (!keys) {
				return;
			}
			for (std::uint32_t i = 0; i < kBurn.size(); ++i) {
				keys[i].SetTime(kBurn[i].first);
				keys[i].SetValue(kBurn[i].second * a_fade);
			}
			ReplaceKeys(data.get(), keys, static_cast<std::uint32_t>(kBurn.size()));
			interpolator->floatData = data;
			a_lit.keys = interpolator;
		}

		// the burn played once on the light from its start (clamped: the last key, 0, holds until the banner goes)
		void PlayBurn(Lit& a_lit, float a_fade)
		{
			if (!DimmerReady() || !a_lit.light) {
				return;
			}
			BurnKeys(a_lit, a_fade);
			if (!a_lit.keys) {
				return;
			}
			a_lit.dimmer.reset(NewDimmer());
			if (!a_lit.dimmer) {
				return;
			}
			using Flag = RE::NiAVObject::Flag;
			using Time = RE::NiTimeController::Flag;
			a_lit.light->GetFlags().set(Flag::kSelectiveUpdate, Flag::kSelectiveUpdateController);
			a_lit.dimmer->SetTarget(a_lit.light.get());
			a_lit.keys->lastIndex = 0;
			a_lit.dimmer->SetInterpolator(a_lit.keys.get(), 0);
			a_lit.dimmer->flags.reset(Time::kCycleType_Mask);
			a_lit.dimmer->flags.set(Time::kAnimType_AppInit, Time::kComputeScaledTime, Time::kCycleType_Clamp);
			a_lit.dimmer->frequency = 1.0f;
			a_lit.dimmer->phase = 0.0f;
			a_lit.dimmer->loKeyTime = 0.0f;
			a_lit.dimmer->hiKeyTime = kBurn.back().first;
			a_lit.dimmer->Start(0.0f);
		}

		std::atomic<bool>                        gOn{ false };  // decided once at data load
		RE::NiColor                              gColor{ 1.0f, 0.6f, 0.3f };
		std::mutex                               gNotedLock;
		std::vector<std::pair<RE::FormID, bool>> gNoted;  // a banner and whether it burns now (gNotedLock)
		std::atomic<bool>                        gAnyNoted{ false };
		std::unordered_map<RE::FormID, Lit>      gLit;  // main thread
		// main thread: its Light Placer lights we darkened on each banner, with the color each had (their file animates only the fade)
		std::unordered_map<RE::FormID, std::vector<std::pair<RE::NiPointer<RE::NiPointLight>, RE::NiColor>>> gDarkened;
		std::unordered_set<RE::FormID>                                                                       gBurning;  // main thread: every banner burning now, ours lit or not
		std::unordered_map<RE::FormID, int>                                                                  gRetries;  // main thread: frames left to try a burning banner the scene refused
		constexpr int                                                                                        kRetryFrames = 120;

		[[nodiscard]] bool OurBanner(const RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			const auto* model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			if (!path) {
				return false;
			}
			std::string_view p(path);
			if (p.size() >= 7 && _strnicmp(p.data(), "meshes\\", 7) == 0) {
				p.remove_prefix(7);
			}
			return p.size() > kModels.size() && _strnicmp(p.data(), kModels.data(), kModels.size()) == 0;
		}

		// a sink only notes (it may run off the main thread); the player update acts
		void Note(RE::FormID a_id, bool a_burning)
		{
			const std::lock_guard l(gNotedLock);
			gNoted.emplace_back(a_id, a_burning);
			gAnyNoted.store(true, std::memory_order_relaxed);
		}

		class Sinks final :
			public RE::BSTEventSink<RE::TESDestructionStageChangedEvent>,
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				// its script disables a banner once its fire is out; a banner carries that script, so the game names its unload here
				LightKit::Guarded("banner unload", [a_event]() {
					if (a_event && !a_event->loaded && OurBanner(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->formID))) {
						Note(a_event->formID, false);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDestructionStageChangedEvent* a_event, RE::BSTEventSource<RE::TESDestructionStageChangedEvent>*) override
			{
				LightKit::Guarded("banner burning", [a_event]() {
					if (a_event && a_event->target && OurBanner(a_event->target.get())) {
						Note(a_event->target->GetFormID(), a_event->newStage >= 1);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				// a banner leaving the loaded world takes its light with it; one coming back burns again only at its next stage
				LightKit::Guarded("banner detach", [a_event]() {
					if (a_event && !a_event->attached && a_event->reference && OurBanner(a_event->reference.get())) {
						Note(a_event->reference->GetFormID(), false);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// this light's numbers under the sliders now (main thread): drawn inverse square where a shader draws it (ArtLights.cpp's way),
		// else plain, its Light Limit Fix words written whole
		void Dress(RE::NiPointLight* a_light)
		{
			const float strength = BurnEm::LightStrength() * GroundElement::StrengthOf(GroundElement::Element::kFire);
			const float reach = kReach * BurnEm::ReachScale() * GroundElement::ReachOf(GroundElement::Element::kFire);
			auto&       data = a_light->GetLightRuntimeData();
			data.diffuse = gColor;
			if (InverseSquare()) {
				a_light->SetLightAttenuation(reach);
				data.fade = kFade * strength;
				// after SetLightAttenuation, which writes the same two words
				LightKit::Isl::SetOwn(a_light);
				LightKit::Isl::SetCutoff(a_light, LightKit::CutoffFor((std::max)(data.fade, 0.001f), reach), 0.01f, 1.0f);
				return;
			}
			const auto plain = LightKit::PlainAt(kFade, 0.0f, reach);
			a_light->SetLightAttenuation(plain.radius);
			data.fade = plain.fade * strength;
			if (LightLimitFix()) {
				LightKit::Isl::SetOwnLinear(a_light);
			} else {
				data.ambient = LightKit::Tenth(gColor);
			}
		}

		[[nodiscard]] bool OursLight() { return BurnEm::OursLight(); }

		// its own ENB light shape on that banner: culled while ours lights it, shown again when ours goes
		void TheirShape(RE::FormID a_id, bool a_show)
		{
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			auto* root = ref && ref->Is3DLoaded() ? ref->Get3D() : nullptr;
			if (auto* shape = root ? root->GetObjectByName(kTheirShape) : nullptr) {
				shape->SetAppCulled(!a_show);
			}
			// its Light Placer lights (a player running Light Placer beside us): dark while ours lights it, their color back after
			if (a_show) {
				if (const auto it = gDarkened.find(a_id); it != gDarkened.end()) {
					for (auto& [light, color] : it->second) {
						if (light) {
							light->GetLightRuntimeData().diffuse = color;
						}
					}
					gDarkened.erase(it);
				}
				return;
			}
			if (!root || gDarkened.contains(a_id)) {
				return;
			}
			auto& kept = gDarkened[a_id];
			RE::BSVisit::TraverseScenegraphLights(root, [&kept](RE::NiPointLight* a_light) {
				const char* name = a_light ? a_light->name.c_str() : nullptr;
				if (name && std::string_view(name).starts_with(kTheirLights)) {
					auto& data = a_light->GetLightRuntimeData();
					kept.emplace_back(RE::NiPointer<RE::NiPointLight>(a_light), data.diffuse);
					data.diffuse = RE::NiColor{};
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void PutOut(RE::FormID a_id)
		{
			if (const auto it = gLit.find(a_id); it != gLit.end()) {
				auto* scene = LightKit::Scene();
				LightKit::TakeOff(scene, it->second.bs);
				if (auto* parent = it->second.light ? it->second.light->parent : nullptr) {
					parent->DetachChild(it->second.light.get());
				}
				gLit.erase(it);
			}
			TheirShape(a_id, true);
		}

		void Light(RE::FormID a_id)
		{
			if (gLit.contains(a_id) || gLit.size() >= kMost) {
				return;
			}
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			auto* root = ref && ref->Is3DLoaded() ? ref->Get3D() : nullptr;
			if (!root) {
				return;
			}
			// its own light's node, where Burn'em Banners hangs its light; else the banner's root
			auto* at = root->GetObjectByName("ENBLight01");
			auto* node = at && at->AsNode() ? at->AsNode() : (at && at->parent ? at->parent : root->AsNode());
			auto* light = node ? LightKit::CloneLight() : nullptr;
			if (!light) {
				return;
			}
			light->name = kName;
			light->local.translate = node == (at ? at->parent : nullptr) ? at->local.translate : RE::NiPoint3{};
			light->local.scale = 1.0f;
			Dress(light);
			if (auto* bs = LightKit::Hang(node, light, LightKit::Scene(), 1.0f, false)) {
				auto& lit = gLit.emplace(a_id, Lit{ RE::NiPointer<RE::NiPointLight>(light), RE::NiPointer<RE::BSLight>(bs), nullptr, nullptr }).first->second;
				PlayBurn(lit, light->GetLightRuntimeData().fade);
				TheirShape(a_id, false);
			}
		}
	}

	void WatchBurningBanners()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh || !dh->LookupModByName(kPlugin)) {
			return;
		}
		if (const auto* hazard = dh->LookupForm<RE::BGSHazard>(kFireHazard, "Skyrim.esm"); hazard && hazard->data.light) {
			const auto& c = hazard->data.light->data.color;
			gColor = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
		}
		static Sinks sinks;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESDestructionStageChangedEvent>(&sinks);
			events->AddEventSink<RE::TESCellAttachDetachEvent>(&sinks);
			events->AddEventSink<RE::TESObjectLoadedEvent>(&sinks);
			gOn.store(true, std::memory_order_relaxed);
			SKSE::log::info("Burn'em Banners: a burning banner wears {}'s fire light in place of its own, unless \"Burn'em Banners lights its banners\" is on", BurnEm::kModName);
		}
	}

	void TickBurningBanners()
	{
		if (!gAnyNoted.exchange(false, std::memory_order_relaxed)) {
			return;
		}
		std::vector<std::pair<RE::FormID, bool>> noted;
		{
			const std::lock_guard l(gNotedLock);
			noted.swap(gNoted);
		}
		for (const auto& [id, burning] : noted) {
			// each new stage may swap the banner's model: its light is hung again on the 3D it has now
			PutOut(id);
			gRetries.erase(id);
			if (burning) {
				gBurning.insert(id);
				if (OursLight()) {
					Light(id);
				}
			} else {
				gBurning.erase(id);
			}
		}
		// a burning banner the scene could not take a light for yet (its 3D not in, the cap reached) is tried again for a few
		// frames, then left to its next stage or a settings change; the window ends itself
		bool again = false;
		for (const auto id : gBurning) {
			if (!OursLight() || gLit.contains(id)) {
				continue;
			}
			if (auto& left = gRetries.try_emplace(id, kRetryFrames).first->second; left > 0) {
				--left;
				Light(id);
				again = again || !gLit.contains(id);
			}
		}
		if (again) {
			gAnyNoted.store(true, std::memory_order_relaxed);
		}
	}

	void RetuneBurningBanners()
	{
		if (!OursLight()) {
			while (!gLit.empty()) {
				PutOut(gLit.begin()->first);
			}
			return;
		}
		for (const auto id : gBurning) {
			Light(id);
		}
		for (auto& [id, lit] : gLit) {
			if (lit.light) {
				Dress(lit.light.get());
				BurnKeys(lit, lit.light->GetLightRuntimeData().fade);
			}
		}
	}

	void DropBurningBanners()
	{
		while (!gLit.empty()) {
			PutOut(gLit.begin()->first);
		}
		// a load or a loading screen: the old 3D's Light Placer lights get their color back first (a save that fails to load keeps them)
		for (auto& [id, kept] : gDarkened) {
			for (auto& [light, color] : kept) {
				if (light) {
					light->GetLightRuntimeData().diffuse = color;
				}
			}
		}
		gDarkened.clear();
		gBurning.clear();
		gRetries.clear();
		const std::lock_guard l(gNotedLock);
		gNoted.clear();
	}
}
