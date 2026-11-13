#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ost {

// Locates the Steam installation. Resolution order:
//   1. explicit override (--steam-dir)
//   2. OST_STEAM_DIR (set by the IFEO loader for the child process)
//   3. HKCU\Software\Valve\Steam\SteamPath
//   4. HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath
//   5. %ProgramFiles(x86)%\Steam, %ProgramFiles%\Steam
// A candidate is only accepted when steam.exe exists inside it.
std::optional<std::wstring> FindSteamInstall(const std::wstring& overrideDir = L"");

struct SteamProcess {
    std::uint32_t pid = 0;
    std::wstring exePath;
    bool payloadLoaded = false;
    std::wstring payloadPath; // empty when not loaded or not readable
};

std::vector<SteamProcess> FindSteamProcesses(bool detectPayload = true);
bool IsModuleLoadedInProcess(std::uint32_t pid, const std::wstring& moduleName);
std::wstring GetProcessExePath(std::uint32_t pid);

} // namespace ost
