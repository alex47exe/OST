#include "Inject.h"

#include "Common.h"
#include "Log.h"
#include "Steam.h"

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ost {
namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"OpenSteamToolWatcher";
constexpr const wchar_t* kWatcherEvent = L"Local\\OpenSteamTool.Watcher.Stop";
constexpr const wchar_t* kWatcherMutex = L"Local\\OpenSteamTool.Watcher.Instance";

std::atomic<bool> g_watcherStop{false};

BOOL WINAPI ConsoleControlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_watcherStop.store(true);
        return TRUE;
    }
    return FALSE;
}

bool IsTarget64Bit(HANDLE process) {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) return false;
    return wow64 == FALSE;
}

FARPROC RemoteLoadLibraryAddress(HANDLE process) {
    HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!localKernel32) return nullptr;
    FARPROC localLoadLibrary = GetProcAddress(localKernel32, "LoadLibraryW");
    if (!localLoadLibrary) return nullptr;

    // The DLL is x64 and so is Steam, so the local address is valid in the target.
    (void)process;
    return localLoadLibrary;
}

} // namespace

std::wstring InjectResultName(InjectResult result) {
    switch (result) {
        case InjectResult::Ok:                     return L"injected";
        case InjectResult::AlreadyLoaded:          return L"already loaded";
        case InjectResult::NoSteamProcess:         return L"no Steam process";
        case InjectResult::OpenProcessFailed:      return L"cannot open Steam (protected or elevated)";
        case InjectResult::UnsupportedArchitecture: return L"32-bit Steam is not supported";
        case InjectResult::RemoteLoadFailed:       return L"remote LoadLibrary failed";
        case InjectResult::PayloadMissing:         return L"payload DLL missing";
        default:                                   return L"unknown";
    }
}

InjectOutcome InjectIntoPid(std::uint32_t pid, const std::wstring& payloadPath) {
    InjectOutcome outcome;
    outcome.pid = pid;
    if (!FileExists(payloadPath)) {
        outcome.result = InjectResult::PayloadMissing;
        outcome.detail = payloadPath;
        return outcome;
    }
    if (IsModuleLoadedInProcess(pid, kPayloadFileName)) {
        outcome.result = InjectResult::AlreadyLoaded;
        return outcome;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        outcome.result = InjectResult::OpenProcessFailed;
        outcome.detail = L"OpenProcess failed (" + std::to_wstring(GetLastError()) + L")";
        return outcome;
    }

    if (!IsTarget64Bit(process)) {
        CloseHandle(process);
        outcome.result = InjectResult::UnsupportedArchitecture;
        return outcome;
    }

    FARPROC loadLibrary = RemoteLoadLibraryAddress(process);
    if (!loadLibrary) {
        CloseHandle(process);
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"LoadLibraryW address not resolvable";
        return outcome;
    }

    const std::wstring nativePath = payloadPath;
    const SIZE_T byteSize = (nativePath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        CloseHandle(process);
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"VirtualAllocEx failed (" + std::to_wstring(GetLastError()) + L")";
        return outcome;
    }

    SIZE_T written = 0;
    const bool writtenOk = WriteProcessMemory(process, remotePath, nativePath.c_str(), byteSize, &written) &&
                           written == byteSize;
    if (!writtenOk) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"WriteProcessMemory failed (" + std::to_wstring(GetLastError()) + L")";
        return outcome;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                       remotePath, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"CreateRemoteThread failed (" + std::to_wstring(GetLastError()) + L")";
        return outcome;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (wait != WAIT_OBJECT_0) {
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"remote LoadLibrary timed out";
        return outcome;
    }
    if (exitCode == 0 && !IsModuleLoadedInProcess(pid, kPayloadFileName)) {
        outcome.result = InjectResult::RemoteLoadFailed;
        outcome.detail = L"remote LoadLibrary returned 0";
        return outcome;
    }

    outcome.result = InjectResult::Ok;
    return outcome;
}

InjectOutcome InjectIntoRunningSteam(const std::wstring& payloadPath) {
    const auto processes = FindSteamProcesses();
    if (processes.empty()) {
        InjectOutcome outcome;
        outcome.result = InjectResult::NoSteamProcess;
        return outcome;
    }
    InjectOutcome best;
    best.result = InjectResult::NoSteamProcess;
    for (const auto& process : processes) {
        InjectOutcome outcome = InjectIntoPid(process.pid, payloadPath);
        if (outcome.result == InjectResult::Ok) return outcome;
        if (best.result == InjectResult::NoSteamProcess) best = outcome;
    }
    return best;
}

InjectOutcome LaunchSteamAndInject(const std::wstring& steamDir, const std::wstring& payloadPath) {
    InjectOutcome outcome;
    const std::wstring exe = steamDir + L"\\steam.exe";
    if (!FileExists(exe)) {
        outcome.result = InjectResult::NoSteamProcess;
        outcome.detail = L"steam.exe not found";
        return outcome;
    }

    // Already running? Then this is a plain hot injection.
    const auto running = FindSteamProcesses();
    if (!running.empty()) return InjectIntoRunningSteam(payloadPath);

    std::wstring commandLine = Quote(exe);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, steamDir.c_str(),
                        &startup, &info)) {
        outcome.result = InjectResult::NoSteamProcess;
        outcome.detail = L"CreateProcess failed (" + std::to_wstring(GetLastError()) + L")";
        return outcome;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    LogInfo(L"launched " + exe + L", waiting for the process to appear");

    // Inject as soon as the process exists; Steam loads steamclient64 a moment later.
    for (int attempt = 0; attempt < 150; ++attempt) {
        const auto processes = FindSteamProcesses(false);
        if (!processes.empty()) {
            InjectOutcome injected = InjectIntoPid(processes.front().pid, payloadPath);
            if (injected.ok()) return injected;
            if (injected.result == InjectResult::OpenProcessFailed && attempt < 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            return injected;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    outcome.result = InjectResult::NoSteamProcess;
    outcome.detail = L"Steam did not start within 30 seconds";
    return outcome;
}

bool StopWatcher() {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kWatcherEvent);
    if (!event) return false;
    SetEvent(event);
    CloseHandle(event);
    return true;
}

bool IsWatcherRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kWatcherMutex);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

int RunWatcher(const std::wstring& steamDir, const std::wstring& payloadPath) {
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kWatcherEvent);
    if (!stopEvent) {
        LogError(L"cannot create the watcher stop event (" + std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    // A leftover named event from an earlier run stays signaled; clear it or the
    // loop would exit immediately.
    ResetEvent(stopEvent);
    HANDLE instance = CreateMutexW(nullptr, TRUE, kWatcherMutex);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        LogWarn(L"another watcher instance is already running");
        CloseHandle(stopEvent);
        if (instance) CloseHandle(instance);
        return 0;
    }

    g_watcherStop.store(false);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    LogInfo(L"watcher started (Steam: " + (steamDir.empty() ? L"auto-detect" : steamDir) + L")");

    while (!g_watcherStop.load()) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

        const std::wstring dir = steamDir.empty() ? FindSteamInstall().value_or(L"") : steamDir;
        const std::wstring payload =
            payloadPath.empty() && !dir.empty() ? dir + L"\\" + kPayloadFileName : payloadPath;

        if (!payload.empty() && FileExists(payload)) {
            const auto processes = FindSteamProcesses(true);
            for (const auto& process : processes) {
                if (process.payloadLoaded) continue;
                InjectOutcome outcome = InjectIntoPid(process.pid, payload);
                if (outcome.result == InjectResult::Ok) {
                    LogInfo(L"injected into steam.exe (pid " + std::to_wstring(process.pid) + L")");
                } else if (outcome.result != InjectResult::AlreadyLoaded) {
                    LogWarn(L"injection into pid " + std::to_wstring(process.pid) + L" failed: " +
                            InjectResultName(outcome.result) +
                            (outcome.detail.empty() ? L"" : L" (" + outcome.detail + L")"));
                }
            }
        }

        // Short sleeps keep the stop event responsive.
        for (int slice = 0; slice < 15 && !g_watcherStop.load(); ++slice) {
            if (WaitForSingleObject(stopEvent, 100) == WAIT_OBJECT_0) break;
        }
    }

    LogInfo(L"watcher stopped");
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    ReleaseMutex(instance);
    CloseHandle(instance);
    CloseHandle(stopEvent);
    return 0;
}

namespace {

bool AnyProcessNamed(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (IEquals(entry.szExeFile, name)) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool WaitForSteamExit(int timeoutMs, std::wstring& outError) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (GetTickCount64() < deadline) {
        if (!AnyProcessNamed(L"steam.exe")) {
            // steam.exe exits before the web helper / services finish; starting a
            // new client into that window makes it hand off and quit again.
            const ULONGLONG helperDeadline = GetTickCount64() + 10000;
            while (GetTickCount64() < helperDeadline && AnyProcessNamed(L"steamwebhelper.exe")) Sleep(250);
            Sleep(1500);
            return true;
        }
        Sleep(250);
    }
    outError = L"Steam is still running";
    return false;
}

bool WaitForSteamStart(int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (GetTickCount64() < deadline) {
        if (AnyProcessNamed(L"steam.exe")) return true;
        Sleep(250);
    }
    return false;
}

} // namespace

bool StopSteam(const std::wstring& steamDir, bool force, std::wstring& outError) {
    const auto processes = FindSteamProcesses(false);
    if (processes.empty()) {
        LogInfo(L"Steam is not running");
        return true;
    }

    std::wstring exe = steamDir.empty() ? processes.front().exePath : steamDir + L"\\steam.exe";
    if (FileExists(exe)) {
        // -shutdown is Steam's own exit path: it flushes cloud saves and stops
        // downloads, unlike terminating the process.
        std::wstring commandLine = Quote(exe) + L" -shutdown";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           steamDir.empty() ? nullptr : steamDir.c_str(), &startup, &info)) {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            LogInfo(L"asked Steam to shut down");
        } else {
            LogWarn(L"could not start 'steam.exe -shutdown' (" + std::to_wstring(GetLastError()) + L")");
        }
    }

    if (WaitForSteamExit(20000, outError)) {
        LogInfo(L"Steam closed");
        return true;
    }

    if (!force) {
        outError = L"Steam did not exit within 20 s (a game may still be running); close it manually or pass --force";
        return false;
    }

    for (const auto& process : FindSteamProcesses(false)) {
        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, process.pid);
        if (!handle) continue;
        if (TerminateProcess(handle, 0)) {
            LogWarn(L"terminated steam.exe (pid " + std::to_wstring(process.pid) + L")");
        }
        CloseHandle(handle);
    }
    if (WaitForSteamExit(5000, outError)) {
        LogInfo(L"Steam closed");
        return true;
    }
    return false;
}

bool StartSteam(const std::wstring& steamDir, std::wstring& outError) {
    const std::wstring exe = steamDir + L"\\steam.exe";
    if (!FileExists(exe)) {
        outError = L"steam.exe not found in " + steamDir;
        return false;
    }

    // A just-closed client can make the next steam.exe hand off and quit, and an
    // IFEO intercept adds a loader hop in between. Launch, confirm a process
    // actually appeared, and retry a couple of times before reporting failure.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        std::wstring commandLine = Quote(exe);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, steamDir.c_str(),
                            &startup, &info)) {
            outError = L"could not start Steam (" + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);

        if (!FindSteamProcesses(false).empty() || WaitForSteamStart(15000)) {
            LogInfo(L"Steam started");
            return true;
        }
        LogWarn(L"steam.exe exited immediately after launch (attempt " + std::to_wstring(attempt) +
                L" of 3), retrying");
        Sleep(2000);
    }
    outError = L"Steam did not stay running after the launch";
    return false;
}

bool SetAutostart(bool enable, const std::wstring& steamDir, std::wstring& outError) {
    if (!enable) {
        if (!RegDeleteValue(HKEY_CURRENT_USER, kRunKey, kRunValue) &&
            RegReadString(HKEY_CURRENT_USER, kRunKey, kRunValue, outError)) {
            outError = L"failed to remove the autostart entry";
            return false;
        }
        return true;
    }

    std::wstring command = Quote(SelfExePath()) + L" watch --quiet";
    if (!steamDir.empty()) command += L" --steam-dir " + Quote(steamDir);
    if (!RegWriteString(HKEY_CURRENT_USER, kRunKey, kRunValue, command, outError)) return false;
    return true;
}

bool IsAutostartEnabled(std::wstring* outCommand) {
    std::wstring command;
    if (!RegReadString(HKEY_CURRENT_USER, kRunKey, kRunValue, command)) return false;
    if (outCommand) *outCommand = command;
    return true;
}

} // namespace ost
