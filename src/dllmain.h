#ifndef DLLMAIN_H
#define DLLMAIN_H

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>
#include <fstream>
#include <filesystem>
#include <array>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <memory>
#include <atomic>
#include <format>

#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Callback.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/Config.h"


inline OSTPlatform::DynamicLibrary::ModuleHandle client_hModule = nullptr;
inline OSTPlatform::DynamicLibrary::ModuleHandle ui_hModule = nullptr;

// SteamUI's LoadModuleWithPath hijack waits on this barrier so the client-side hooks
// are installed before Steam goes for the steamclient handle; otherwise it gets a half-initialized state.
inline std::atomic<bool> g_HooksInstalled{false};
// Whether the Diversion shadow module is in effect. false = fallback to the original
// steamclient (hooks applied in place on the original module, visible to Denuvo/RE
// integrity checks, sensitive games may crash): must be warned about loudly.
inline std::atomic<bool> g_IsDiversionActive{false};
// Whether Steam really switched to the diversion copy (LoadModuleWithPath hijack dispatched).
// Not switched within the 15s timeout = Steam runs the original loaded at startup, every hook
// on the copy misses, and we must fall back to the original to keep unlock (1.5.2.1); else silent total failure.
inline std::atomic<bool> g_DiversionAdopted{false};

inline constexpr size_t kRuntimePathCapacity = 260;

inline char SteamInstallPath[kRuntimePathCapacity] = {};
inline char SteamclientPath[kRuntimePathCapacity]  = {};
inline char SteamUIPath[kRuntimePathCapacity]      = {};
inline char DiversionPath[kRuntimePathCapacity]    = {};
inline char LuaDir[kRuntimePathCapacity]           = {};
inline char ConfigPath[kRuntimePathCapacity]       = {};

// The fake AppId used by -onlinefix (SpaceWar).
constexpr AppId_t kOnlineFixAppId = 480;

// Family-shared + lua games are reported under this AppId so the server does not bill the
// shared license. It is an impossible ID: a real AppId would be broadcast as usual, while
// this unresolvable one neither takes a slot nor shows up on the friends side.
constexpr AppId_t kFamilySharedMaskAppId = 0x7FFFFFFF;

#endif // DLLMAIN_H
