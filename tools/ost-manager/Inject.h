#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ost {

enum class InjectResult {
    Ok,
    AlreadyLoaded,
    NoSteamProcess,
    OpenProcessFailed,
    UnsupportedArchitecture,
    RemoteLoadFailed,
    PayloadMissing,
};

struct InjectOutcome {
    InjectResult result = InjectResult::NoSteamProcess;
    std::uint32_t pid = 0;
    std::wstring detail;
    bool ok() const { return result == InjectResult::Ok || result == InjectResult::AlreadyLoaded; }
};

std::wstring InjectResultName(InjectResult result);

InjectOutcome InjectIntoPid(std::uint32_t pid, const std::wstring& payloadPath);
InjectOutcome InjectIntoRunningSteam(const std::wstring& payloadPath);

// Starts steam.exe and injects as soon as the process exists. Used when Steam
// is not running and no IFEO/proxy entry point is installed.
InjectOutcome LaunchSteamAndInject(const std::wstring& steamDir, const std::wstring& payloadPath);

int RunWatcher(const std::wstring& steamDir, const std::wstring& payloadPath);
bool StopWatcher();
bool IsWatcherRunning();

// Graceful Steam shutdown (`steam.exe -shutdown`), then a bounded wait.
// `force` terminates the remaining steam.exe processes (a running game can keep
// Steam from exiting on its own).
bool StopSteam(const std::wstring& steamDir, bool force, std::wstring& outError);
bool StartSteam(const std::wstring& steamDir, std::wstring& outError);

bool SetAutostart(bool enable, const std::wstring& steamDir, std::wstring& outError);
bool IsAutostartEnabled(std::wstring* outCommand = nullptr);

} // namespace ost
