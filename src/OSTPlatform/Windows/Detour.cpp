#include "include/Detour.h"

#include "include/Log.h"
#include "include/Thread.h"

#include <windows.h>
#include <detours.h>
#include <tlhelp32.h>

#include <vector>

namespace OSTPlatform::Detour {

namespace {

bool DetourSucceeded(LONG result, const char* operation) {
    if (result == NO_ERROR) return true;
    OSTP_LOG_WARN("{} failed (detours_error={})", operation, result);
    return false;
}

std::vector<HANDLE> s_updatedThreads;  // registered thread handles, must stay valid until commit

// An attach/detach transaction must register every thread of the process: when commit
// patches the first few bytes of the target function, a thread running inside the
// trampoline is only suspended and its context corrected if it was registered; registering
// only the current thread means a second attach/detach while traffic is flowing rewrites
// the code under some other thread's feet.
void UpdateAllThreads() {
    const DWORD self = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snapshot, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == self) continue;  // the current thread is registered separately below
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                  THREAD_QUERY_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;  // the thread just exited, skip it
            DWORD exitCode = STILL_ACTIVE;
            if (!GetExitCodeThread(h, &exitCode) || exitCode != STILL_ACTIVE) {
                // Do not register zombie TIDs that exited between the snapshot and the open.
                CloseHandle(h);
                continue;
            }
            if (GetProcessIdOfThread(h) != GetCurrentProcessId()) {
                // Do not register a TID reused by another process either: a cross-process handle
                // makes Detours fail and poisons the whole transaction (every later Attach/Commit
                // fails with it and the hook never installs).
                CloseHandle(h);
                continue;
            }
            // A single registration failure only skips that thread, never propagates out — throwing once poisons the whole transaction.
            if (DetourUpdateThread(h) == NO_ERROR) {
                s_updatedThreads.push_back(h);
            } else {
                CloseHandle(h);
            }
        } while (Thread32Next(snapshot, &te));
    }
    CloseHandle(snapshot);
}

void CloseUpdatedThreads() {
    for (const HANDLE h : s_updatedThreads) CloseHandle(h);
    s_updatedThreads.clear();
}

} // namespace

bool BeginTransaction() {
    const LONG begin = DetourTransactionBegin();
    if (!DetourSucceeded(begin, "DetourTransactionBegin")) return false;
    UpdateAllThreads();
    return DetourSucceeded(
        DetourUpdateThread(Thread::CurrentNativeThreadHandle()),
        "DetourUpdateThread");
}

bool CommitTransaction() {
    const bool ok = DetourSucceeded(DetourTransactionCommit(), "DetourTransactionCommit");
    // Whether commit succeeds or not, the handles opened during registration must be closed,
    // otherwise every attach/detach leaks a small batch of them.
    CloseUpdatedThreads();
    return ok;
}

bool Attach(void** target, void* detour) {
    return DetourSucceeded(
        DetourAttach(reinterpret_cast<PVOID*>(target), reinterpret_cast<PVOID>(detour)),
        "DetourAttach");
}

bool Detach(void** target, void* detour) {
    return DetourSucceeded(
        DetourDetach(reinterpret_cast<PVOID*>(target), reinterpret_cast<PVOID>(detour)),
        "DetourDetach");
}

} // namespace OSTPlatform::Detour
