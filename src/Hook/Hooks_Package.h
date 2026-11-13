#pragma once

#include "dllmain.h"

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // Fallback-scenario bootstrap: Steam has already finished checking ownership
    // before the hook takes effect and will not re-query on its own afterwards.
    // On the next CheckAppOwnership call, use the now-available CUser to force one
    // license refresh. Fires only once to avoid loops. The normal path (with a
    // steady stream of natural queries) is unaffected.
    void RequestRequeryOnce();

    // Look up the lender accountid recorded for an AppId; 0 means it was not
    // borrowed (no record). When reporting play state this value must be written
    // back into owner so the server recognizes it as a shared session.
    AccountID_t GetFamilySharedOwner(AppId_t appId);

}
