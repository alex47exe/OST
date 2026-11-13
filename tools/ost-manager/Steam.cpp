#include "Steam.h"

#include "Common.h"

#include <tlhelp32.h>

#include <algorithm>

namespace ost {
namespace {

bool LooksLikeSteamDir(const std::wstring& dir) {
    return !dir.empty() && FileExists(dir + L"\\steam.exe");
}

std::wstring NormalizePath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}

} // namespace

std::optional<std::wstring> FindSteamInstall(const std::wstring& overrideDir) {
    if (!overrideDir.empty()) {
        const std::wstring candidate = NormalizePath(overrideDir);
        return LooksLikeSteamDir(candidate) ? std::optional<std::wstring>(candidate) : std::nullopt;
    }

    const std::wstring fromEnv = GetEnvVar(kSteamDirEnv);
    if (!fromEnv.empty() && LooksLikeSteamDir(NormalizePath(fromEnv))) return NormalizePath(fromEnv);

    std::wstring value;
    if (RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", value)) {
        value = NormalizePath(value);
        if (LooksLikeSteamDir(value)) return value;
    }
    if (RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", value)) {
        value = NormalizePath(value);
        if (LooksLikeSteamDir(value)) return value;
    }

    const wchar_t* envRoots[] = {L"ProgramFiles(x86)", L"ProgramFiles"};
    for (const wchar_t* root : envRoots) {
        const std::wstring base = GetEnvVar(root);
        if (base.empty()) continue;
        const std::wstring candidate = NormalizePath(base + L"\\Steam");
        if (LooksLikeSteamDir(candidate)) return candidate;
    }
    return std::nullopt;
}

std::wstring GetProcessExePath(std::uint32_t pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
            CloseHandle(process);
            return std::wstring(buffer.data(), size);
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() > 32768) break;
        buffer.resize(buffer.size() * 2, L'\0');
    }
    CloseHandle(process);
    return L"";
}

std::wstring GetModulePathInProcess(std::uint32_t pid, const std::wstring& moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return L"";
    std::wstring path;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (IEquals(entry.szModule, moduleName)) {
                path = entry.szExePath;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return path;
}

bool IsModuleLoadedInProcess(std::uint32_t pid, const std::wstring& moduleName) {
    return !GetModulePathInProcess(pid, moduleName).empty();
}

std::vector<SteamProcess> FindSteamProcesses(bool detectPayload) {
    std::vector<SteamProcess> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!IEquals(entry.szExeFile, L"steam.exe")) continue;
            // Skip the 32-bit Steam bootstrapper stub: only the 64-bit client hosts
            // the payload. The check is best effort -- when OpenProcess fails (Steam
            // started elevated, or protected by AV) the process still counts as
            // running, otherwise stop/restart would silently do nothing.
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process) {
                BOOL wow64 = FALSE;
                if (IsWow64Process(process, &wow64) && wow64) {
                    CloseHandle(process);
                    continue;
                }
                CloseHandle(process);
            }

            SteamProcess item;
            item.pid = entry.th32ProcessID;
            item.exePath = GetProcessExePath(item.pid);
            if (detectPayload) {
                item.payloadPath = GetModulePathInProcess(item.pid, kPayloadFileName);
                item.payloadLoaded = !item.payloadPath.empty();
            }
            result.push_back(std::move(item));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::sort(result.begin(), result.end(), [](const SteamProcess& lhs, const SteamProcess& rhs) {
        return lhs.pid < rhs.pid;
    });
    return result;
}

} // namespace ost
