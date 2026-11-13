#pragma once
// Which lighting the game draws with - found once, at data load, never picked by the player (his word). ONE copy for every
// lighting plugin of ours (Tools\Shared Plugin Code, shared.json); each plugin's own Lighting.cpp keeps only its rulings.
//
//   Community Shaders   CommunityShaders.dll loaded (Bottled Shaders and Open Shaders keep the name). Light Limit Fix then reads the
//                       first words of every light as its flags, cutoff and record, so a light of ours carries its words. Inverse
//                       square only where one of its shaders draws it: the 1.4.6 add-on's file, or Light Limit Fix's attenuation
//                       naming InverseSquare (built in since Bottled Shaders 1.0.10); with neither every light is drawn linear
//   ENB                 a loaded module exports ENBGetSDKVersion (ENB's SDK): its d3d11.dll proxy, or any name an injector gave it
//   Vanilla             neither
// Left-behind files never decide: ENB's settings and the shader files without their DLL are only named in the log. Community
// Shaders wins over an ENB loaded beside it, since Light Limit Fix reads the words whatever ENB does.
//
// ENB is found the way hakasapl's PGPatcher finds it (PGLite/src/ShaderFrameworks.cpp, GPL-3.0; "how TerrainHelper looks for
// it"): one list of the loaded modules, each asked for the export. A list that cannot be read is no ENB, said in the log - the
// copies before this one let ENB's leftover settings files decide there, which made a Vanilla game read as ENB.
//
// Include it from one .cpp only, after <Windows.h>.

#include <Psapi.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameLighting
{
	enum class Lighting : int
	{
		kShaders = 0,
		kEnb = 1,
		kVanilla = 2
	};

	struct Found
	{
		// before Read: Vanilla, the only value that agrees with every flag below at its default (no DLL, no shader, no ENB)
		Lighting lighting{ Lighting::kVanilla };
		bool     communityShaders{ false };       // CommunityShaders.dll loaded: Light Limit Fix reads every light's first words
		bool     inverseSquare{ false };          // ... and one of its shaders draws inverse square
		bool     shaderAddOn{ false };            // the 1.4.6 add-on's shader file is in Data (logged; it counts only with the DLL)
		bool     shaderInLightLimitFix{ false };  // Light Limit Fix's attenuation names InverseSquare (the same)
		bool     enb{ false };                    // a loaded module exports ENBGetSDKVersion
		bool     enbFilesLeft{ false };           // enbseries.ini / enblocal.ini in the game folder (logged only)
		bool     modulesListed{ true };           // false: the module list could not be read, so no ENB was assumed
	};

	[[nodiscard]] constexpr Lighting Decide(bool a_communityShaders, bool a_enb) noexcept
	{
		return a_communityShaders ? Lighting::kShaders : a_enb ? Lighting::kEnb :
		                                                         Lighting::kVanilla;
	}
	static_assert(Decide(true, true) == Lighting::kShaders && Decide(true, false) == Lighting::kShaders &&
				  Decide(false, true) == Lighting::kEnb && Decide(false, false) == Lighting::kVanilla);

	[[nodiscard]] constexpr const char* Name(Lighting a_lighting) noexcept
	{
		switch (a_lighting) {
		case Lighting::kEnb:
			return "ENB";
		case Lighting::kVanilla:
			return "Vanilla";
		default:
			return "Community Shaders";
		}
	}

	namespace Detail
	{
		[[nodiscard]] inline bool FileNames(const std::filesystem::path& a_file, std::string_view a_word)
		{
			std::ifstream in{ a_file, std::ios::binary };
			if (!in) {
				return false;
			}
			const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			return text.find(a_word) != std::string::npos;
		}

		[[nodiscard]] inline std::optional<bool> EnbLoaded()
		{
			std::vector<HMODULE> modules(256);
			DWORD                needed = 0;
			for (;;) {
				const auto size = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
				if (!::K32EnumProcessModules(::GetCurrentProcess(), modules.data(), size, &needed)) {
					return std::nullopt;
				}
				if (needed <= size) {
					break;
				}
				modules.resize(needed / sizeof(HMODULE));
			}
			const auto count = needed / sizeof(HMODULE);
			for (std::size_t i = 0; i < count; ++i) {
				// a counted reference: a module unloaded after the list cannot be read through a stale handle
				HMODULE held = nullptr;
				if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i]), &held)) {
					const bool found = ::GetProcAddress(held, "ENBGetSDKVersion") != nullptr;
					::FreeLibrary(held);
					if (found) {
						return true;
					}
				}
			}
			return false;
		}
	}

	// once, at data load, on the main thread
	[[nodiscard]] inline Found Read()
	{
		Found           out;
		std::error_code rootError;
		const auto      root = std::filesystem::current_path(rootError);
		out.communityShaders = ::GetModuleHandleW(L"CommunityShaders.dll") != nullptr;
		if (!rootError) {
			std::error_code ec;
			out.shaderAddOn = std::filesystem::exists(root / "Data/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli", ec);
			out.shaderInLightLimitFix = Detail::FileNames(root / "Data/Shaders/LightLimitFix/Attenuation.hlsli", "InverseSquare");
			out.enbFilesLeft = std::filesystem::exists(root / "enbseries.ini", ec) || std::filesystem::exists(root / "enblocal.ini", ec);
		}
		out.inverseSquare = out.communityShaders && (out.shaderAddOn || out.shaderInLightLimitFix);
		const auto enb = Detail::EnbLoaded();
		out.modulesListed = enb.has_value();
		out.enb = enb.value_or(false);
		out.lighting = Decide(out.communityShaders, out.enb);
		return out;
	}

	// the one log line every carrier writes, and the two warnings
	inline void Log(const Found& a_found)
	{
		const bool leftover = (a_found.enbFilesLeft && !a_found.enb && !a_found.communityShaders) ||
		                      ((a_found.shaderAddOn || a_found.shaderInLightLimitFix) && !a_found.communityShaders);
		SKSE::log::info(
			"lighting: {} (found by itself - CommunityShaders.dll {}, inverse square shader {}, in Light Limit Fix {}, ENB running {}, "
			"ENB settings in the game folder {}{}); lights drawn {}",
			Name(a_found.lighting), a_found.communityShaders ? "loaded" : "not loaded", a_found.shaderAddOn ? "installed" : "not installed",
			a_found.shaderInLightLimitFix ? "yes" : "no", a_found.enb ? "yes" : "no", a_found.enbFilesLeft ? "yes" : "no",
			leftover ? " - left over, not used" : "", a_found.inverseSquare ? "inverse square" : "plain");
		if (!a_found.modulesListed) {
			SKSE::log::warn("lighting: the loaded modules could not be listed - no ENB assumed");
		}
		if (a_found.communityShaders && !a_found.inverseSquare) {
			SKSE::log::warn("lighting: CommunityShaders.dll is loaded but no shader of it names inverse square - every light of ours is drawn plain (linear)");
		}
	}
}
