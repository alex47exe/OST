#include "Hooks_SteamUI.h"
#include "Hooks_Package.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace
{
    using namespace std::chrono_literals;
    // Barrier polling: 10ms × 500 = wait at most 5s; on timeout abandon the hijack and
    // return the original handle. Steam's own module load must never be blocked.
    constexpr int kMaxRetry = 500;
    constexpr auto kRetryInterval = 10ms;

    static std::string_view ExtractFileName(std::string_view path) {
        const size_t pos = path.find_last_of("\\/");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    static std::wstring_view ExtractFileNameW(std::wstring_view path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
    }

    // Quick file-name length check, cheaper than scanning for a suffix;
    // note "steamclient64.dll"=17 and "steamclient.dll"=15 — a miscount kills all redirection.
    static bool IsSteamClientPath(const char* path) {
        if (!path) return false;
        std::string_view fn = ExtractFileName(path);
        switch (fn.size()) {
        case 17: return _strnicmp(fn.data(), "steamclient64.dll", 17) == 0;
        case 15: return _strnicmp(fn.data(), "steamclient.dll", 15) == 0;
        case 13: return _strnicmp(fn.data(), "steamclient64", 13) == 0;
        case 11: return _strnicmp(fn.data(), "steamclient", 11) == 0;
        default: return false;
        }
    }

    static bool IsSteamClientPathW(const wchar_t* path) {
        if (!path) return false;
        std::wstring_view fn = ExtractFileNameW(path);
        switch (fn.size()) {
        case 17: return _wcsnicmp(fn.data(), L"steamclient64.dll", 17) == 0;
        case 15: return _wcsnicmp(fn.data(), L"steamclient.dll", 15) == 0;
        case 13: return _wcsnicmp(fn.data(), L"steamclient64", 13) == 0;
        case 11: return _wcsnicmp(fn.data(), L"steamclient", 11) == 0;
        default: return false;
        }
    }

    // System module lookup redirect: every name-based lookup of steamclient returns the
    // diversion copy handle; otherwise Steam keeps the original module handle internally and
    // every hook misses. FROM_ADDRESS lookups by address are unaffected. Note cloud_redirect
    // must get the copy too: its vtable hook can only catch Cloud RPC when patched into the
    // module Steam actually calls; the original makes its install fail (Steam no longer uses
    // it), cloud sync degrades to the packet layer before KV is ready, and games report cloud
    // sync failure at startup.
    static decltype(&GetModuleHandleA) oGetModuleHandleA = &GetModuleHandleA;
    static decltype(&GetModuleHandleW) oGetModuleHandleW = &GetModuleHandleW;
    static decltype(&GetModuleHandleExA) oGetModuleHandleExA = &GetModuleHandleExA;
    static decltype(&GetModuleHandleExW) oGetModuleHandleExW = &GetModuleHandleExW;

    HMODULE WINAPI hkGetModuleHandleA(LPCSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPath(lpModuleName)) {
            return static_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleA(lpModuleName);
    }

    HMODULE WINAPI hkGetModuleHandleW(LPCWSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPathW(lpModuleName)) {
            return static_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleW(lpModuleName);
    }

    BOOL WINAPI hkGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPath(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                HMODULE dummy = nullptr;
                if (!oGetModuleHandleExA((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCSTR>(client_hModule), &dummy))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
            }
            *phModule = static_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExA(dwFlags, lpModuleName, phModule);
    }

    BOOL WINAPI hkGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPathW(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                HMODULE dummy = nullptr;
                if (!oGetModuleHandleExW((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCWSTR>(client_hModule), &dummy))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
            }
            *phModule = static_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExW(dwFlags, lpModuleName, phModule);
    }

    // ── first-load hijack (1791 isolation fix) ─────────────────────────
    // The 1791 steam.exe no longer fetches steamclient through LoadModuleWithPath but calls
    // LoadLibraryExW directly during startup, outside the hijack's field of view, so adoption
    // never arrives. Hook kernel32's first load as well: any first load that wants steamclient
    // gets the copy handle directly. GetProcAddress locates the kernel32 functions instead of
    // using a pattern (system library addresses are exact, no toml needed).
    static decltype(&LoadLibraryExW) oLoadLibraryExW = nullptr;
    static decltype(&LoadLibraryW) oLoadLibraryW = nullptr;
    static decltype(&LoadLibraryExA) oLoadLibraryExA = nullptr;
    static decltype(&LoadLibraryA) oLoadLibraryA = nullptr;
    static std::atomic<bool> g_bypassFirstLoadRedirect{false};

    static HMODULE TryDivertFirstLoad(LPCWSTR lpLibFileName) {
        if (g_bypassFirstLoadRedirect.load(std::memory_order_acquire)) return nullptr;
        if (!g_IsDiversionActive.load() || !client_hModule) return nullptr;
        if (!IsSteamClientPathW(lpLibFileName)) return nullptr;
        // First-load semantics: the original is already in memory, so this is a repeat
        // request and we pass it through to the original. Otherwise (e.g. we arrived late,
        // the startup load was missed) we would create a split world where the main client
        // runs the original and a latecomer gets the copy, plus setting adopted would block
        // the fallback and kill unlock. This probe must go through oGetModuleHandleW (the real
        // API / trampoline): name lookups are redirected to the copy by this module, so while
        // isolation is active a literal call always hits the copy (non-null) and the hijack is
        // fooled by its own redirect into thinking "the original is present", never stealing.
        auto realGetModuleHandleW = oGetModuleHandleW ? oGetModuleHandleW : &GetModuleHandleW;
        if (realGetModuleHandleW(lpLibFileName) || realGetModuleHandleW(L"steamclient64.dll"))
            return nullptr;
        // Pin the copy before handing it out: later FreeLibrary calls from Steam only drop
        // down to our pin, so the copy stays resident and is never unloaded (the same rule as
        // "never FreeLibrary the copy"). Calling the real API directly (the :: prefix does not
        // bypass Detours' inline patch either, but FROM_ADDRESS makes the hijack pass straight
        // through to the original function, so both pointer kinds are safe; the null check only
        // guards against wild pointers).
        HMODULE dummy = nullptr;
        auto pinFn = oGetModuleHandleExW ? oGetModuleHandleExW : ::GetModuleHandleExW;
        pinFn(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
              reinterpret_cast<LPCWSTR>(client_hModule), &dummy);
        // Goes to the main logger: arming happens before the module log exists, so this can fire at any time.
        LOG_INFO("FirstLoadRedirect: diverted steamclient first load -> diversion {} (adopted)",
                 static_cast<void*>(client_hModule));
        g_DiversionAdopted.store(true);
        return static_cast<HMODULE>(client_hModule);
    }

    HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        // Resource/datafile loads must not be hijacked: those want file contents, not an executable module.
        constexpr DWORD kDataFlags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE |
                                     LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE;
        if (!hFile && !(dwFlags & kDataFlags)) {
            if (HMODULE diverted = TryDivertFirstLoad(lpLibFileName))
                return diverted;
        }
        return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }

    HMODULE WINAPI hkLoadLibraryW(LPCWSTR lpLibFileName)
    {
        if (HMODULE diverted = TryDivertFirstLoad(lpLibFileName))
            return diverted;
        return oLoadLibraryW(lpLibFileName);
    }

    // ANSI mirror: steam.exe's import table also uses the A-series LoadLibrary, and
    // intercepting only the W variants would miss first loads issued through the A ones.
    static HMODULE TryDivertFirstLoadA(LPCSTR lpLibFileName) {
        if (g_bypassFirstLoadRedirect.load(std::memory_order_acquire)) return nullptr;
        if (!g_IsDiversionActive.load() || !client_hModule) return nullptr;
        if (!IsSteamClientPath(lpLibFileName)) return nullptr;
        auto realGetModuleHandleA = oGetModuleHandleA ? oGetModuleHandleA : &GetModuleHandleA;
        if (realGetModuleHandleA(lpLibFileName) || realGetModuleHandleA("steamclient64.dll"))
            return nullptr;
        HMODULE dummy = nullptr;
        auto pinFn = oGetModuleHandleExA ? oGetModuleHandleExA : ::GetModuleHandleExA;
        pinFn(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
              reinterpret_cast<LPCSTR>(client_hModule), &dummy);
        LOG_INFO("FirstLoadRedirect: diverted steamclient first load (ANSI) -> diversion {} (adopted)",
                 static_cast<void*>(client_hModule));
        g_DiversionAdopted.store(true);
        return static_cast<HMODULE>(client_hModule);
    }

    HMODULE WINAPI hkLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        constexpr DWORD kDataFlags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE |
                                     LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE;
        if (!hFile && !(dwFlags & kDataFlags)) {
            if (HMODULE diverted = TryDivertFirstLoadA(lpLibFileName))
                return diverted;
        }
        return oLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }

    HMODULE WINAPI hkLoadLibraryA(LPCSTR lpLibFileName)
    {
        if (HMODULE diverted = TryDivertFirstLoadA(lpLibFileName))
            return diverted;
        return oLoadLibraryA(lpLibFileName);
    }

    // ── API surface redirect (M3): late-bound GetProcAddress must be covered too ──────────
    // When someone takes the original handle first (FROM_ADDRESS lookup, direct startup link)
    // and then calls GetProcAddress for exports such as CreateInterface, hand out the copy's
    // implementation; otherwise we get a half-isolated world where the module runs the copy
    // but the interface calls back into the original. Redirection only happens while isolation
    // is active, everything else passes through; the original is located by handle equality,
    // file names are never read, and the hot path pays only one extra atomic load.
    static decltype(&GetProcAddress) oGetProcAddress = nullptr;

    FARPROC WINAPI hkGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
    {
        if (lpProcName && hModule && client_hModule &&
            g_IsDiversionActive.load(std::memory_order_acquire) &&
            !g_bypassFirstLoadRedirect.load(std::memory_order_acquire) &&
            hModule != static_cast<HMODULE>(client_hModule) && oGetProcAddress)
        {
            HMODULE orig = nullptr;
            if (oGetModuleHandleW) {
                orig = oGetModuleHandleW(L"steamclient64.dll");
                if (!orig)
                    orig = oGetModuleHandleW(L"steamclient.dll");
            }
            if (orig && hModule == orig) {
                if (FARPROC diverted = oGetProcAddress(
                        static_cast<HMODULE>(client_hModule), lpProcName))
                    return diverted;
            }
        }
        return oGetProcAddress(hModule, lpProcName);
    }

    HOOK_FUNC(LoadModuleWithPath, void*, const char* path, bool flags)
    {
        LOG_STEAMUI_INFO("LoadModuleWithPath called with path: {}, flags: {}",
                         path ? path : "(null)", flags);

        const bool isSteamClient = IsSteamClientPath(path);

        if (isSteamClient) {
            // Wait for the client-side hooks to install; on timeout abandon the hijack and
            // return the original handle, never block.
            bool logged = false;
            for (int i = 0; i < kMaxRetry && !g_HooksInstalled.load(); ++i) {
                if (!logged) {
                    LOG_STEAMUI_DEBUG("LoadModuleWithPath: waiting for hooks to be installed...");
                    logged = true;
                }
                std::this_thread::sleep_for(kRetryInterval);
            }
        }

        void* h = oLoadModuleWithPath(path, flags);

        if (isSteamClient) {
            if (!g_HooksInstalled.load()) {
                LOG_STEAMUI_WARN("LoadModuleWithPath: hooks initialization timed out after {} ms, aborting diversion",
                                 kMaxRetry * static_cast<int>(kRetryInterval.count()));
                return h;
            }

            if (client_hModule) {
                if (g_IsDiversionActive.load()) {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: diverted {} (original {}) -> diversion {}{}",
                                     path ? path : "steamclient64.dll", h, client_hModule,
                                     (h == client_hModule) ? " (already loaded)" : "");
                    // Steam switched to the copy = isolation is active, the timeout fallback watchdog stops acting.
                    g_DiversionAdopted.store(true);
                } else {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: returned fallback steamclient64.dll ({})",
                                     client_hModule);
                }
                return client_hModule;
            }
        }

        return h;
    }

    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    // Currently logged-in account, fetched once and cached. UI refreshes keep running into
    // the ownership backfill path, and the account never changes during the process lifetime,
    // so there is no point reading the registry every time. Cleared on uninstall and re-fetched
    // after a hot reload so the previous round's account is not reused.
    std::atomic<uint32_t> g_selfAccountId{0};

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID, false))
        {
            // An empty owner identity makes the details page show "from Family Library". Fake-licensed
            // games have no real ownership and Steam fills the placeholder accountid 0, so patch in
            // the current account. Games genuinely borrowed from someone else must not be touched:
            // they carry a lender record and the UI should show the shared source.
            if (pApp->SteamID.GetAccountID() == 0 &&
                Hooks_Package::GetFamilySharedOwner(pApp->nAppID) == 0)
            {
                uint32_t selfAccount = g_selfAccountId.load(std::memory_order_relaxed);
                if (selfAccount == 0)
                {
                    std::wstring universe;
                    if (OSTPlatform::SteamCredentialStore::GetActiveUser(selfAccount, universe) ==
                        OSTPlatform::SteamCredentialStore::Status::Ok)
                        g_selfAccountId.store(selfAccount, std::memory_order_relaxed);
                    else
                        selfAccount = 0;
                }

                if (selfAccount != 0)
                {
                    pApp->SteamID.Set(selfAccount, k_EUniversePublic, k_EAccountTypeIndividual);
                    LOG_STEAMUI_DEBUG("FillInAppOverview: appId={} owner set to self ({})",
                                      pApp->nAppID, selfAccount);
                }
            }

            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if (t)
            {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // Apps to drop from the library: queued off-thread, marked on the UI thread.
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::unordered_set<AppId_t> g_removedAppIds;

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> draining;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                draining.swap(g_pendingRemovals);
            }
            for (AppId_t appId : draining)
            {
                if (LuaConfig::IsOwned(appId))
                {
                    LOG_STEAMUI_DEBUG("RunFrame: appId {} is owned again, skipping removal", appId);
                    continue;
                }
                if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                {
                    // Only remove from the library if it's not already uninstalled
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    if(pApp->AppStateFlags == k_EAppStateUninstalled){
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }
                }
                
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
            }
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(LoadModuleWithPath);
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        // System module handle redirect (Diversion shadow isolation): must happen in the same
        // transaction — calling DetourAttach outside one fails (DetourAttach failed) and the
        // hook is effectively not installed.
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));
        HOOK_END();
    }

    bool IsDiversionRedirectArmed()
    {
        return oLoadModuleWithPath != nullptr;
    }

    void InstallFirstLoadRedirect()
    {
        // kernel32 is always resident and GetProcAddress locates the exports exactly, no
        // pattern/toml needed; there is no dependency on the SteamUI hooks, so install as early
        // as possible after injection — the earlier we arm, the wider the window for catching
        // Steam's first load. Logs always go to the main logger, which may fire before the
        // module log exists.
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) {
            LOG_WARN("FirstLoadRedirect: kernel32 handle missing, skipping");
            return;
        }
        oLoadLibraryExW = reinterpret_cast<decltype(oLoadLibraryExW)>(
            GetProcAddress(kernel32, "LoadLibraryExW"));
        oLoadLibraryW = reinterpret_cast<decltype(oLoadLibraryW)>(
            GetProcAddress(kernel32, "LoadLibraryW"));
        oLoadLibraryExA = reinterpret_cast<decltype(oLoadLibraryExA)>(
            GetProcAddress(kernel32, "LoadLibraryExA"));
        oLoadLibraryA = reinterpret_cast<decltype(oLoadLibraryA)>(
            GetProcAddress(kernel32, "LoadLibraryA"));
        oGetProcAddress = reinterpret_cast<decltype(oGetProcAddress)>(
            GetProcAddress(kernel32, "GetProcAddress"));
        if (!oLoadLibraryExW || !oLoadLibraryW) {
            LOG_WARN("FirstLoadRedirect: GetProcAddress failed, skipping");
            oLoadLibraryExW = nullptr;
            oLoadLibraryW = nullptr;
            oLoadLibraryExA = nullptr;
            oLoadLibraryA = nullptr;
            oGetProcAddress = nullptr;
            return;
        }
        HOOK_BEGIN();
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryExW),
                                    reinterpret_cast<void*>(hkLoadLibraryExW));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryW),
                                    reinterpret_cast<void*>(hkLoadLibraryW));
        // The A-series hooks are a mirror; failing to install them does not affect the main W path.
        if (oLoadLibraryExA) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryExA),
                                        reinterpret_cast<void*>(hkLoadLibraryExA));
        }
        if (oLoadLibraryA) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryA),
                                        reinterpret_cast<void*>(hkLoadLibraryA));
        }
        // API surface fallback: redirect late-bound lookups of the original exports to the
        // copy; the first-load logic works without it too.
        if (oGetProcAddress) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetProcAddress),
                                        reinterpret_cast<void*>(hkGetProcAddress));
        }
        HOOK_END();
        LOG_INFO("FirstLoadRedirect: armed (steamclient first load will be diverted)");
    }

    bool IsFirstLoadRedirectArmed()
    {
        return oLoadLibraryExW != nullptr;
    }

    void SetFirstLoadRedirectBypass(bool bypass)
    {
        g_bypassFirstLoadRedirect.store(bypass, std::memory_order_release);
    }

    void Uninstall()
    {
        // Re-fetch the account after a hot reload instead of reusing the previous cached value.
        g_selfAccountId.store(0, std::memory_order_relaxed);
        UNHOOK_BEGIN();
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));
        UNINSTALL_HOOK(LoadModuleWithPath);
        UNINSTALL_HOOK(LoadLibraryExW);
        UNINSTALL_HOOK(LoadLibraryW);
        UNINSTALL_HOOK(LoadLibraryExA);
        UNINSTALL_HOOK(LoadLibraryA);
        UNINSTALL_HOOK(GetProcAddress);
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }
}
