#include "Hooks_SteamUI.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/VehCommon.h"
#include <mutex>

namespace {

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);
    RESOLVE_FUNC(ShouldShowAppInLibrary,bool,CSteamApp* pApp);

    HOOK_FUNC(FillInAppOverview,void*,void* pThis,void* pAppOverview,CSteamApp* pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID,false)) {
            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if(t) {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // The library-state change originates on the FileWatcher background thread.
    // MarkAppChange assumes the UI thread and takes no lock, so the watcher thread
    // only enqueues appIds here; the RunFrame hook drains them on the UI thread.
    std::mutex            g_removalMutex;
    std::vector<AppId_t>  g_pendingRemovals;
    constexpr uint32 kBudgetDivisor = 3;

    struct RemovalCandidate {
        AppId_t appId;
        CSteamApp* app;
    };

    void PullQueuedRemovals(std::vector<AppId_t>& draining) {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (g_pendingRemovals.empty()) return;

        draining.insert(draining.end(), g_pendingRemovals.begin(), g_pendingRemovals.end());
        g_pendingRemovals.clear();
    }

    std::vector<RemovalCandidate> CollectVisibleRemovalCandidates(void* pController, const std::vector<AppId_t>& draining)
    {
        std::vector<RemovalCandidate> candidates;

        for (AppId_t appId : draining) {
            if (LuaConfig::IsOwned(appId)) {
                LOG_STEAMUI_WARN("CollectVisibleRemovalCandidates: appId={} is owned, skipping", appId);
                continue;
            }

            CSteamApp* app = oGetAppByID(pController, appId, false);
            if (!app) {
                LOG_STEAMUI_TRACE("CollectVisibleRemovalCandidates: appId={} not found, skipping", appId);
                continue;
            }

            if (!oShouldShowAppInLibrary(app)) {
                LOG_STEAMUI_TRACE("CollectVisibleRemovalCandidates: appId={} is not visible, skipping", appId);
                continue;
            }

            candidates.push_back({appId, app});
        }
        return candidates;
    }

    // Clears the ownership flag and queues an app change so the overview flush
    // re-evaluates a candidate already validated during this UI frame.
    bool RemoveAppAndSendChange(const RemovalCandidate& candidate) {
        if (LuaConfig::IsOwned(candidate.appId)) {
            LOG_STEAMUI_WARN("RemoveAppAndSendChange: appId={} became owned, skipping", candidate.appId);
            return false;
        }

        candidate.app->OwnershipFlags = k_EAppOwnershipFlags_None;
        LOG_STEAMUI_DEBUG("RemoveAppAndSendChange: cleared owned flag for appId={}", candidate.appId);
        oMarkAppChange(g_pAppChangeSource, candidate.appId, EAppChangeFlags::AddedOrCreated);
        return true;
    }

    void DrainRemovalBatch(void* pController, std::vector<AppId_t>& draining) {
        if(!CAPTURE_READY(GetAppByID) || !CAPTURE_READY(MarkAppChange) || !oShouldShowAppInLibrary){
            LOG_STEAMUI_WARN("DrainRemovalBatch: dependencies not ready, skipping drain");
            return;
        }
        if (draining.empty()) return;

        std::vector<RemovalCandidate> candidates = CollectVisibleRemovalCandidates(pController, draining);

        size_t budget = candidates.size() / kBudgetDivisor;
        if (!candidates.empty() && budget == 0)
        budget = 1;

        size_t marked = 0;
        // Rebuild the work queue with candidates deferred to the next frame.
        // Items removed within this frame are intentionally left out.
        draining.clear();
        for (const RemovalCandidate& candidate : candidates) {
            if (marked >= budget) {
                draining.push_back(candidate.appId);
                continue;
            }

            if (RemoveAppAndSendChange(candidate))
                ++marked;
        }

        LOG_STEAMUI_DEBUG("RunFrame: visible removals={}, removed={}, deferred={}",
                            candidates.size(), marked, draining.size());
    }

    // CSteamUIAppController::RunFrame - the controller's per-frame tick on the UI
    // thread; its tail flushes pending overview changes to the JS library. We drain
    // a budgeted batch of removals here so each flush stays on the delta path.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void*, void* pController)
    {
        static std::vector<AppId_t> s_draining;

        PullQueuedRemovals(s_draining);
        DrainRemovalBatch(pController, s_draining);
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI {
    void Install() {

        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(ShouldShowAppInLibrary);
        
        HOOK_BEGIN();
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        HOOK_END();

    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

}
