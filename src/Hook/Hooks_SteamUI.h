#pragma once

#include "dllmain.h"

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    void Install();
    void Uninstall();

    // Whether the diversion hijack is ready (whether the LoadModuleWithPath pattern hit).
    // A miss = Steam cannot be redirected to the shadow module, so we must fall back to the
    // original; otherwise every client hook lands on an unused copy and unlock dies silently.
    bool IsDiversionRedirectArmed();

    // First-load hijack (1791): Steam no longer fetches steamclient through
    // LoadModuleWithPath but calls LoadLibraryExW directly during startup. Once diversion is
    // ready, kernel32's first load is hooked as well: stealing it means isolation is active;
    // if we are too late (first load already happened) the hook simply misses and nothing
    // changes. GetProcAddress is covered too: late-bound lookups of the original exports are
    // redirected to the copy, everything else passes through by default.
    void InstallFirstLoadRedirect();

    // Whether the first-load hijack is armed (whether the kernel32 LoadLibrary hook is installed).
    // Together with IsDiversionRedirectArmed it answers "does isolation still have a redirect
    // path left": only when both paths are gone should we fall back to the original.
    bool IsFirstLoadRedirectArmed();

    // Must be bypassed briefly when we load the original ourselves (fallback), otherwise
    // the load gets hijacked back to the copy.
    void SetFirstLoadRedirectBypass(bool bypass);

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);
}
