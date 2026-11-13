#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Hook/Hooks_Package.h"
#include "Hook/Hooks_SteamUI.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <windows.h>

namespace {

// File operations for the Diversion shadow module: all run on the InitThread worker, never under the loader lock.
constexpr int kDiversionCopyRetries = 3;
constexpr std::chrono::milliseconds kDiversionCopyRetryDelay{50};

// Whether steamclient64 was already in memory when we entered the process decides whether the
// first-load hijack has any chance: present = we arrived late, Steam won't Load it a second time;
// absent = the first load hasn't happened yet. GetModuleHandle only reads the PEB, safe under the
// loader lock; thread creation gives happens-before, so a plain read is fine.
bool g_steamClientMappedAtAttach = false;

bool IsDiversionUpToDate() {
    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    const bool origExists =
        GetFileAttributesExA(SteamclientPath, GetFileExInfoStandard, &origAttr) != 0;
    const bool divExists =
        GetFileAttributesExA(DiversionPath, GetFileExInfoStandard, &divAttr) != 0;
    if (!origExists || !divExists) return false;
    return origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
           origAttr.nFileSizeLow == divAttr.nFileSizeLow &&
           origAttr.ftLastWriteTime.dwLowDateTime == divAttr.ftLastWriteTime.dwLowDateTime &&
           origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime;
}

bool CopySteamClientToDiversion() {
    std::error_code ec;
    std::filesystem::path diversionFsPath(DiversionPath);
    std::filesystem::create_directories(diversionFsPath.parent_path(), ec);
    if (ec) {
        LOG_WARN("Diversion: failed to create directory for {} ({})", DiversionPath, ec.message());
        return false;
    }
    // Copy to a temp file first, then replace atomically: when two Steam processes start at once,
    // writing the same target directly yields a torn copy; MoveFileEx replacement is atomic, so a
    // reader sees either the old complete version or the new complete one.
    char tmpPath[kRuntimePathCapacity] = {};
    sprintf_s(tmpPath, "%s.tmp", DiversionPath);
    // An old Steam process may still hold a handle: clear the read-only attribute first, otherwise CopyFile always fails.
    const DWORD attrs = GetFileAttributesA(tmpPath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesA(tmpPath, FILE_ATTRIBUTE_NORMAL);
    }
    for (int attempt = 1; attempt <= kDiversionCopyRetries; ++attempt) {
        if (CopyFileA(SteamclientPath, tmpPath, FALSE) &&
            MoveFileExA(tmpPath, DiversionPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LOG_INFO("Diversion: cloned steamclient64.dll -> {}", DiversionPath);
            return true;
        }
        const DWORD gle = GetLastError();
        // Only sharing violations / access denied are worth retrying; path errors etc. are abandoned at once to save time.
        if (gle != ERROR_SHARING_VIOLATION && gle != ERROR_ACCESS_DENIED) {
            LOG_WARN("Diversion: CopyFile failed, not retryable (err={})", gle);
            break;
        }
        if (attempt < kDiversionCopyRetries) {
            std::this_thread::sleep_for(kDiversionCopyRetryDelay);
        } else {
            LOG_WARN("Diversion: CopyFile failed after {} attempts (err={})",
                     kDiversionCopyRetries, gle);
        }
    }
    return false;
}

} // namespace

// Fall back to the original steamclient: shared by B2 (hijack not installed) and the timeout watchdog
// (Steam never switches). clientHooksArmed: whether the client hooks are installed on the copy
// (watchdog case true — unhook first, then rehook; B2 case false — hooks not installed yet, just swap
// the module). Returns true on success; false if even the original fails to load (caller treats as fatal).
static bool FallbackToOriginalSteamClient(bool clientHooksArmed) {
    if (clientHooksArmed) SteamClient::CoreUnhook();
    // Note: NEVER FreeLibrary the diversion copy! cloud_redirect initialization takes the copy's
    // handle via GetModuleHandle for address resolution + vtable patching and stores absolute
    // addresses; unloading it would make the next stats/cloud RPC crash on a dangling pointer, so the
    // copy is abandoned but kept mapped (cost of one extra module mapping) — hooks already removed,
    // nobody will use it again, safe. client_hModule points straight at the original, so later hijack
    // dispatch and query redirection all go to the original, consistent throughout.
    // Note: after the fallback the whole session is original-world, so the first-load hijack must be
    // bypassed permanently, otherwise a later LoadLibrary(steamclient) picks up the copy and two
    // client instances coexist in a corrupted state. This only ever enables it and never disables it:
    // even the fallback's own Load below is let through.
    Hooks_SteamUI::SetFirstLoadRedirectBypass(true);
    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    g_IsDiversionActive.store(false);
    if (!client_hModule) {
        LOG_ERROR("Diversion fallback: Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    // PatternLoader caches its map by module handle, so it must be re-Loaded after the module swap.
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");
    if (clientHooksArmed) {
        SteamClient::CoreHook();
        PatternLoader::ReportMissingFunctions();
    }
    // Hooks installed late: Steam already finished its ownership queries before they took effect and
    // won't re-query on its own. Set the bootstrap flag so the next CheckAppOwnership forces a single
    // refresh with the now-ready CUser — injection is done by the time the second query wave arrives.
    // The normal path never sets it: zero impact.
    Hooks_Package::RequestRequeryOnce();
    LOG_WARN("Diversion: fallback complete, hooks now on original steamclient64 — "
             "unlock keeps working, isolation disabled");
    return true;
}

// Timeout fallback watchdog: if Steam hasn't switched to the copy within 15s of init completing, it
// is almost certainly using the original loaded at startup (a normal machine switches within a
// second) and every hook on the copy misses (unlock: silent total failure). We then move back to the
// original to keep unlock working; a wrong fallback only costs isolation (= 1.5.2.1 behavior, proven).
// Sliced sleep: respond within 5s of process exit and never wake up in unloaded code.
constexpr auto kDiversionAdoptTimeout = std::chrono::seconds{15};
constexpr auto kWatchdogSlice = std::chrono::seconds{5};
static_assert(kDiversionAdoptTimeout >= kWatchdogSlice,
              "watchdog slice must divide the adopt timeout (else zero slices = instant fallback)");

static uint32_t DiversionWatchdog() {
    const int slices = static_cast<int>(kDiversionAdoptTimeout / kWatchdogSlice);
    for (int i = 0; i < slices; ++i) {
        std::this_thread::sleep_for(kWatchdogSlice);
        // Process is exiting / already unloaded: bail out and touch nothing.
        if (!g_HooksInstalled.load()) return 0;
    }
    if (!g_IsDiversionActive.load() || g_DiversionAdopted.load()) return 0;
    LOG_WARN("Diversion: Steam did not adopt the diversion module within {}s "
             "(still using startup-loaded original), falling back to original steamclient64",
             static_cast<int>(kDiversionAdoptTimeout.count()));
    if (!FallbackToOriginalSteamClient(true)) return 1;
    return 0;
}

bool IsExistingDirectory(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadEnvironmentPath(const char* name, std::string& out) {
    char buffer[kRuntimePathCapacity] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, kRuntimePathCapacity);
    if (length == 0 || length >= kRuntimePathCapacity) return false;
    if (!IsExistingDirectory(buffer)) return false;
    out = buffer;
    return true;
}

bool ReadRegistryPath(const char* valueName, std::string& out) {
    char buffer[kRuntimePathCapacity] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\OpenSteamTool", valueName,
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return false;
    }
    if (!IsExistingDirectory(buffer)) return false;
    out = buffer;
    return true;
}

bool FileExistsA(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool SamePath(const std::string& lhs, const std::string& rhs) {
    return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
}

// Where the tool keeps its own state. The host executable always decides the
// Steam root (steamclient64.dll lives next to steam.exe), but everything the
// tool writes -- opensteamtool.toml, config\lua, the diversion copy, logs and
// the metadata cache -- follows a separate data root. A portable install points
// that root at the folder holding ost.exe / OpenSteamTool.dll, so the tool
// behaves exactly as if it sat inside Steam without writing into Steam.
struct RuntimeRoots {
    std::string steamDir;
    std::string dataDir;
    std::string dataSource;
    std::string configPath;
    std::string luaDir;
    std::string diversionPath;
    bool valid = false;
};

RuntimeRoots ResolveRoots(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    RuntimeRoots roots;
    const std::filesystem::path exePath = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
    const std::string exeName = exePath.filename().string();
    if (exeName.empty() || _stricmp(exeName.c_str(), "steam.exe") != 0) return roots;
    roots.steamDir = exePath.parent_path().string();
    if (roots.steamDir.empty()) return roots;

    roots.dataDir = roots.steamDir;
    roots.dataSource = "steam";

    std::string candidate;
    if (ReadEnvironmentPath("OST_CONFIG_DIR", candidate)) {
        roots.dataDir = candidate;
        roots.dataSource = "OST_CONFIG_DIR";
    } else if (ReadRegistryPath("ConfigDir", candidate)) {
        roots.dataDir = candidate;
        roots.dataSource = "HKCU ConfigDir";
    } else {
        // Auto-detect a portable layout even when no manager value was recorded
        // (the loader was started by hand, or the folder was moved).
        const std::filesystem::path moduleDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
        const std::string payloadDir = moduleDir.string();
        if (!payloadDir.empty() && !SamePath(payloadDir, roots.steamDir) &&
            (FileExistsA(payloadDir + "\\opensteamtool.toml") ||
             IsExistingDirectory(payloadDir + "\\config\\lua"))) {
            roots.dataDir = payloadDir;
            roots.dataSource = "payload directory";
        }
    }

    roots.configPath = roots.dataDir + "\\opensteamtool.toml";
    roots.luaDir = roots.dataDir + "\\config\\lua";
    std::string envLua;
    if (ReadEnvironmentPath("OST_LUA_DIR", envLua)) roots.luaDir = envLua;
    // Mirror the Steam layout under the data root: <steam>\bin\diversion64.dll
    // becomes <data>\bin\diversion64.dll for portable installs.
    roots.diversionPath = roots.dataDir + "\\bin\\diversion64.dll";
    roots.valid = true;
    return roots;
}

// prepare key runtime paths.
bool InitializeSteamComponents(const RuntimeRoots& roots)
{
    if (!roots.valid) {
        LOG_INFO("InitializeSteamComponents: host is not steam.exe, skipping");
        return false;
    }
    LOG_INFO("Steam install path: {}", roots.steamDir);
    if (roots.dataDir != roots.steamDir) {
        LOG_INFO("tool data root: {} (from {})", roots.dataDir, roots.dataSource);
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", roots.steamDir.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s", roots.diversionPath.c_str());
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s", roots.configPath.c_str());
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s", roots.luaDir.c_str());
    
    // Diversion shadow module: every hook is applied only to a full copy of steamclient64.dll, and
    // the original module stays 100% clean so Denuvo/RE engine integrity checks pass.
    // A failed copy load is most likely a torn file from a Steam self-update in progress; delete it
    // and copy once more. Only then fall back to the original with a loud warning — silently
    // falling back means running naked without telling anyone.
    client_hModule = nullptr;
    // Isolation master switch: the first-load hijack only has a window if we enter the process
    // before steamclient's first load. If steamclient64 is already in memory on arrival (dwmapi
    // injected late via vgui2_s), a fresh copy can never be adopted and the watchdog would only wait
    // out the timeout before falling back, wasting a tens-of-MB copy — so skip copy, first-load
    // redirect and watchdog entirely and use the original throughout.
    const bool diversionEnabled = !g_steamClientMappedAtAttach;
    // Only a copy that is "confirmed up to date" or "just copied successfully" is trustworthy; if the
    // copy itself fails, don't touch the old file — fall back to the original (a stale copy is untrusted).
    bool diversionFileUsable = false;
    if (diversionEnabled) {
        diversionFileUsable = IsDiversionUpToDate();
        if (!diversionFileUsable) {
            diversionFileUsable = CopySteamClientToDiversion();
        } else {
            LOG_DEBUG("Diversion module is already up to date ({}), skipping copy", DiversionPath);
        }
    }
    if (diversionFileUsable) {
        client_hModule = OSTPlatform::DynamicLibrary::Load(DiversionPath);
    }
    if (!client_hModule && diversionFileUsable) {
        LOG_WARN("Load diversion module failed (path={}, err={}), deleting possibly torn copy and cloning once more",
                 DiversionPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        DeleteFileA(DiversionPath);
        if (CopySteamClientToDiversion()) {
            client_hModule = OSTPlatform::DynamicLibrary::Load(DiversionPath);
        }
    }
    if (client_hModule) {
        g_IsDiversionActive.store(true);
        LOG_INFO("Loaded diversion module from {}", DiversionPath);
    } else {
        g_IsDiversionActive.store(false);
        client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
        if (!client_hModule) {
            LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        if (diversionEnabled) {
            LOG_WARN("Diversion inactive, loaded fallback steamclient64.dll from {} — hooks run on the original module, integrity-checked games may crash",
                     SteamclientPath);
        } else {
            LOG_INFO("Diversion skipped (arrived after steamclient64 first load), using original steamclient64.dll from {}",
                     SteamclientPath);
        }
    }
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    // Resolve before logging so main.log lands in the data root too.
    const RuntimeRoots roots = ResolveRoots(selfModule);
    Log::Init(selfModule, roots.dataDir);
    LOG_INFO("OpenSteamTool init thread started");

    // Two payload copies mapped into one process (an IFEO loader plus a proxy, or a stale copy next to
    // the manager) would install every detour twice and crash Steam. The first copy wins, the second
    // stands down before it touches anything.
    if (HMODULE first = GetModuleHandleW(L"OpenSteamTool.dll")) {
        if (first != reinterpret_cast<HMODULE>(selfModule)) {
            LOG_WARN("another OpenSteamTool payload ({}) is already loaded in this process; this copy stays inactive",
                     reinterpret_cast<uintptr_t>(first));
            return 0;
        }
    }
    LOG_INFO("OpenSteamTool attach: steamclient64 already mapped = {} ({})",
             g_steamClientMappedAtAttach,
             g_steamClientMappedAtAttach
                 ? "we arrived late, first-load redirect cannot work"
                 : "first load not yet happened, FirstLoadRedirect may catch it");

    if (!InitializeSteamComponents(roots)) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    // Arm the first-load hijack immediately: it only depends on kernel32, nothing from the pattern or
    // SteamUI hooks, and the earlier it is installed the bigger the window to catch Steam's main
    // thread's first LoadLibrary(steamclient).
    if (g_IsDiversionActive.load())
        Hooks_SteamUI::InstallFirstLoadRedirect();

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // Install the SteamUI hooks first: the LoadModuleWithPath hijack must be in place before Steam
    // loads steamclient itself; internally it waits on the g_HooksInstalled barrier before letting
    // Steam through. (The first-load hijack is already in place right after InitializeSteamComponents, see above.)
    SteamUI::CoreHook();

    // No fallback while either swap path is usable: the LoadModuleWithPath hijack (legacy path) or the
    // first-load hijack (1791 path). Only when both are gone is the shadow module a useless ornament,
    // and we must switch back to the original and reinstall the client hooks, or unlock fails silently.
    if (g_IsDiversionActive.load() && !Hooks_SteamUI::IsDiversionRedirectArmed() &&
        !Hooks_SteamUI::IsFirstLoadRedirectArmed()) {
        LOG_WARN("Diversion redirect unavailable (LoadModuleWithPath pattern missing), "
                 "falling back to original steamclient64 — unlock keeps working, isolation disabled");
        if (!FallbackToOriginalSteamClient(false)) {
            LOG_ERROR("Fallback Load steamclient64.dll failed");
            return 1;
        }
    }

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(client_hModule, SteamclientPath);

    // One popup for every metadata file that was missing, instead of one each.
    SteamDiagnostics::ShowMissingMetadataSummary();

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    if (!roots.steamDir.empty() && !SamePath(roots.dataDir, roots.steamDir)) {
        // Portable root: scripts that are still in Steam keep working.
        const std::string steamLua = roots.steamDir + "\\config\\lua";
        if (IsExistingDirectory(steamLua)) watchDirs.push_back(steamLua);
    }
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    g_HooksInstalled.store(true);
    LOG_INFO("OpenSteamTool init complete ({})",
             g_IsDiversionActive.load() ? "Diversion active" : "Diversion bypassed, using original steamclient64");
    // Timeout fallback watchdog: it only has work to do when isolation is enabled; no thread when running on the original.
    if (g_IsDiversionActive.load())
        OSTPlatform::Thread::StartDetached([] { return DiversionWatchdog(); });
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_steamClientMappedAtAttach = (GetModuleHandleW(L"steamclient64.dll") != nullptr);
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false);
        g_IsDiversionActive.store(false);
        g_DiversionAdopted.store(false);
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
