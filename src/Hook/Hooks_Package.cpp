#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    void* g_pCUser = nullptr;
    PackageInfo* g_pInjectedPackageInfo = nullptr;

    // Family Sharing lender accountid, recorded per AppId.
    // While bFamilyShared is set, Steam fills AppOwnership::SteamId32 with the
    // lender. Once we clear that flag, this is the only field that still lets us
    // trace ownership back. It is needed when reporting play state to restore
    // owner to the real lender, otherwise the server sees an account without a
    // license running a shared game.
    std::unordered_map<AppId_t, AccountID_t> g_familySharedOwners;
    std::mutex g_familySharedOwnersMutex;

    // Whether this AppId has a lender record. Needed when deciding self-purchase:
    // a shared game's bFamilyShared is cleared on the first pass, so the flag can
    // no longer identify it later — only this table can.
    bool HasFamilySharedOwner(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
        return g_familySharedOwners.find(appId) != g_familySharedOwners.end();
    }

    bool  g_licenseInitialized = false;
    bool  g_licenseRefreshPending = false;
    // One-shot fallback bootstrap flag: set by the watchdog thread, consumed by
    // the Steam IPC thread (atomic, cross-thread).
    std::atomic<bool> g_forceRequeryOnce{false};
    // Injection circuit breaker: once tripped by an exception, injection stays
    // disabled for this session (the game may simply stay locked, Steam survives).
    std::atomic<bool> g_injectDisabled{false};
    static DWORD g_injectExCode = 0;
    static void* g_injectExAddr = nullptr;

    static LONG WINAPI InjectionFilter(EXCEPTION_POINTERS* ep) {
        g_injectExCode = ep->ExceptionRecord->ExceptionCode;
        g_injectExAddr = ep->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        if (!g_pCUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(g_pCUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(g_pCUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    void TryProcessPendingLicenseRefresh() {
        if (!g_licenseRefreshPending)
            return;
        if (MarkLicenseAsChangedAndProcessUpdates())
            g_licenseRefreshPending = false;
    }

    bool CUtlMemoryGrowWrap(CUtlVector<AppId_t>* pVec, int grow_size) {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }
        return oCUtlMemoryGrow(pVec, grow_size);
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg) {
        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license. 
        std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
        if (!appIds.empty()) {
            uint32 oldSize = pPkg->AppIdVec.m_Size;
            uint32 numToAdd = static_cast<uint32>(appIds.size());
            LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): adding {} apps, oldSize={}", kInjectedPackageId, numToAdd, oldSize);
            // Capacity gate: if a new build changes the CUtlVector/PackageInfo
            // layout, reading/writing at the old offsets corrupts the heap.
            // Validate twice (before and after grow); on implausible values give
            // up on injection and log loudly rather than touch memory. The limit
            // is very generous ("a few hundred per normal package"), so only
            // garbled values are rejected, not normal variation.
            constexpr uint32 kMaxSaneAppCount = 100000;
            const uint32 allocBefore = pPkg->AppIdVec.m_Memory.m_nAllocationCount;
            if (oldSize > allocBefore || numToAdd > kMaxSaneAppCount || allocBefore > kMaxSaneAppCount) {
                LOG_PACKAGE_ERROR("InitFakeLicense: implausible vector state (size={} alloc={} add={}), skipping injection (struct layout may have changed)",
                                  oldSize, allocBefore, numToAdd);
                return false;
            }
            if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, numToAdd)) {
                LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                return false;
            }
            const uint32 allocAfter = pPkg->AppIdVec.m_Memory.m_nAllocationCount;
            if (allocAfter < oldSize + numToAdd || allocAfter > kMaxSaneAppCount) {
                LOG_PACKAGE_ERROR("InitFakeLicense: grow result implausible (size={} alloc={} add={}), skipping write (keeping original length)",
                                  oldSize, allocAfter, numToAdd);
                return false;
            }
            for (uint32 i = 0; i < numToAdd; i++)
                pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = appIds[i];
            // Grow only extends capacity, not length; write the length back so the
            // injected AppIds are visible to code that iterates by length.
            pPkg->AppIdVec.m_Size = oldSize + numToAdd;
        }

        g_licenseInitialized = true;
        g_licenseRefreshPending = true;
        TryProcessPendingLicenseRefresh();
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_licenseInitialized) return true;
        if(CAPTURE_READY(GetPackageInfo)){
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
            if(!pPkg) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            if(!g_pInjectedPackageInfo) g_pInjectedPackageInfo = pPkg;
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }


    // Catch-all around the injection path: on a new build with layout drift, no
    // exception from injection or its follow-up work may take Steam down. One
    // crash records the code and address and permanently disables injection for
    // this session (the game may stay locked, but Steam survives and the log tells why).
} // namespace

namespace {

    static bool CheckAppOwnershipBody(void* pObj, AppId_t appId, AppOwnership* pOwn, bool result) {
        if (!g_pCUser) {
            g_pCUser = pObj;
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", g_pCUser);
        }

        TryInitFakeLicenseOnce();

        // Bootstrap refresh (once only): in the fallback scenario this call is
        // most likely the first one, the GetPackageInfo capture is not ready yet
        // and injection was only half done. g_pCUser is set by now, so force a
        // license refresh; by the time the second wave of queries arrives the
        // capture is ready and injection completes. The atomic exchange clears the
        // flag, so it cannot loop.
        if (!g_licenseInitialized && g_forceRequeryOnce.exchange(false)) {
            LOG_PACKAGE_INFO("CheckAppOwnership: priming license re-query after late hook install");
            MarkLicenseAsChangedAndProcessUpdates();
        }

        // Record lender ownership. Must happen before the flags are cleared below:
        // once cleared it can never be identified again.
        // This table serves two purposes:
        //   1. Non-lua shared games: restore owner to the lender when reporting,
        //      so the countdown cannot kick the session offline
        //   2. Tell whether an AppId is a shared game from the family library
        //      (lua games must be recorded too; reporting then rewrites it to the
        //      replacement AppId so it does not consume a lender slot)
        // Only the borrow flags are trusted: Steam overwrites SteamId32 once they
        // are cleared, so it is unreliable for decisions. Records are never
        // rewritten afterwards — a missed record breaks the whole sharing fix, so
        // a redundant one is preferable.
        if (pOwn && (pOwn->bFamilyShared || pOwn->bBorrowed) && pOwn->SteamId32 != 0) {
            bool recorded = false;
            {
                std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
                AccountID_t& slot = g_familySharedOwners[appId];
                if (slot != pOwn->SteamId32) {
                    slot = pOwn->SteamId32;
                    recorded = true;
                }
            }
            if (recorded) {
                LOG_PACKAGE_INFO("FamilySharing: recorded lender {} for AppId={} (familyShared={} borrowed={})",
                                 pOwn->SteamId32, appId, pOwn->bFamilyShared, pOwn->bBorrowed);
            }
        }

        // The second argument is true: AppIds already marked as genuinely owned
        // are skipped without re-evaluating.
        if (pOwn && LuaConfig::HasDepot(appId, true)) {
            // A genuinely owned game is usually listed in lua to add DLC; the main
            // game itself must not be routed through the fake license: that would
            // override Steam's license decision and tie purchase state,
            // achievements and playtime to the fake license.
            //
            // All three conditions must hold:
            //   1. result: Steam accepts this license (it also accepts fake ones,
            //      so this alone is not enough)
            //   2. ExistInPackageNums > 1: covered by both the real package and the
            //      fake license package
            //   3. no lender record: a shared game also reaches 2 because of the
            //      injected fake package, and its bFamilyShared was cleared by us
            //      on the first pass, so the flag alone cannot identify it.
            //      The g_familySharedOwners record is written before the flags are
            //      cleared, so it is never missed.
            const bool borrowed = pOwn->bFamilyShared || pOwn->bBorrowed;
            const bool knownShared = HasFamilySharedOwner(appId);
            if (result && pOwn->ExistInPackageNums > 1 && !borrowed && !knownShared) {
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
                LOG_PACKAGE_INFO("FakeLicense: AppId={} owned by self, treating as genuine", appId);
                return result;
            }

            // Everything else goes through the fake license: games that are also in
            // the family library therefore do not consume a lender slot.
            // Borrow-related flags must be cleared as well, otherwise Steam keeps
            // tracking this AppId under the shared license, and slot usage and
            // expiry reclamation still happen.
            pOwn->PackageId          = kInjectedPackageId;
            pOwn->ReleaseState       = EAppReleaseState::Released;
            pOwn->bOwnsLicense       = true;
            pOwn->bFreeLicense       = false;
            pOwn->bFamilyShared      = false;
            pOwn->bBorrowed          = false;
            pOwn->bLicenseLocked     = false;
            pOwn->bLicensePending    = false;
            pOwn->bLicenseExpired    = false;
            pOwn->bGuestPass         = false;
            pOwn->bLicensePermanent  = true;
            return true;
        }

        return result;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        // After the circuit breaker trips, return the original result without
        // touching any memory.
        if (g_injectDisabled.load()) return result;
        // This function has only POD locals and no C++ objects, so __try is valid
        // here (/EHsc).
        bool out = result;
        __try {
            out = CheckAppOwnershipBody(pObj, appId, pOwn, result);
        } __except (InjectionFilter(GetExceptionInformation())) {
            g_injectDisabled.store(true);
            LOG_PACKAGE_ERROR("CheckAppOwnership: license path crashed (code=0x{:X} addr={}), injection disabled for session",
                              g_injectExCode, g_injectExAddr);
            return result;
        }
        return out;
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }

    void RequestRequeryOnce() {
        g_forceRequeryOnce.store(true);
    }

    AccountID_t GetFamilySharedOwner(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
        auto it = g_familySharedOwners.find(appId);
        return it == g_familySharedOwners.end() ? 0 : it->second;
    }

    void NotifyLicenseChanged() {
        // Even with a hot reload after the breaker trips, touch no memory: return
        // immediately and leave the pending queue for next time (tiny volume,
        // negligible).
        if (g_injectDisabled.load()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injection disabled after crash, skipping");
            return;
        }
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        // ── Remove depots that were unloaded ──
        std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} removals", removals.size());
        uint32_t removedCount = 0;
        for (AppId_t id : removals) {
            if (pPkg->AppIdVec.FindAndFastRemove(id)) {
                ++removedCount;
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
            }else {
                LOG_PACKAGE_WARN("NotifyLicenseChanged: AppId {} not found in package AppIdVec during removal", id);
            }
        }

        // ── Add depots that are newly loaded ──
        std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        std::unordered_set<AppId_t> addedIds;
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} additions", additions.size());
        if (!additions.empty()) {
            uint32_t oldSize = pPkg->AppIdVec.m_Size;
            if (CUtlMemoryGrowWrap(&pPkg->AppIdVec, additions.size())) {
                // An applied addition invalidates any UI removal that has not
                // reached the UI thread yet.
                for (AppId_t id : additions)
                    Hooks_SteamUI::CancelRemoval(id);

                for (size_t i = 0; i < additions.size(); ++i) {
                    pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = additions[i];
                    addedIds.insert(additions[i]);
                    LOG_PACKAGE_DEBUG("NotifyLicenseChanged: inserted AppId {} at [{}]", additions[i], oldSize + i);
                }
                // Grow only extends capacity, not length; write the length back so
                // the injected AppIds are visible to code that iterates by length.
                pPkg->AppIdVec.m_Size = oldSize + static_cast<uint32_t>(additions.size());
            }else {
                LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
            }
        }

        if (addedIds.empty() && removedCount == 0) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // Mark package 0 as changed and trigger library refresh.
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
            return;
        }
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", addedIds.size(), removedCount);

        // Queue UI removals for the main-thread RunFrame hook to drain.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        size_t queuedRemovalCount = 0;
        for (AppId_t id : removals) {
            // ParseFile unloads the old file before parsing the replacement.
            // Do not queue that transient removal when the id was added again.
            if (!addedIds.contains(id)) {
                Hooks_SteamUI::QueueRemoval(id);
                ++queuedRemovalCount;
            }
        }
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: queued {} UI removals, skipped {} transient removals",
                          queuedRemovalCount, removals.size() - queuedRemovalCount);
    }
}
