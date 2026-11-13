#pragma once

// Shared payload lookup for the proxy DLLs (version, dwmapi, xinput1_4).
//
// Every proxy has to find OpenSteamTool.dll without relying on the DLL search
// order (a same-named file next to the launched process must never win).
// Order:
//   1. next to the proxy itself
//   2. next to the host executable (steam.exe)
//   3. IFEO "OSTPayloadPath" written by the manager at install time
//   4. HKCU\Software\OpenSteamTool\PayloadPath (portable / hot-inject installs)
//
// The header is intentionally dependency-free besides windows.h/advapi32 so
// each proxy stays a self-contained module on Steam's load path.

#include <windows.h>

namespace ProxyPayload {

constexpr const char* kPayloadName = "OpenSteamTool.dll";

inline bool FileExistsA(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool DirectoryOf(const char* path, char* out, size_t capacity) {
    if (!path || !path[0] || !out || capacity == 0) return false;
    const char* slash = nullptr;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') slash = cursor;
    }
    if (!slash) return false;
    const size_t length = static_cast<size_t>(slash - path);
    if (length + 1 > capacity) return false;
    for (size_t i = 0; i < length; ++i) out[i] = path[i];
    out[length] = '\0';
    return true;
}

inline bool JoinPath(const char* directory, const char* name, char* out, size_t capacity) {
    if (!directory || !name || !out) return false;
    const size_t dirLength = lstrlenA(directory);
    const size_t nameLength = lstrlenA(name);
    if (dirLength + 1 + nameLength + 1 > capacity) return false;
    lstrcpynA(out, directory, static_cast<int>(capacity));
    lstrcatA(out, "\\");
    lstrcatA(out, name);
    return true;
}

inline bool ReadRegistryString(HKEY root, const char* subkey, const char* name, char* out, size_t capacity) {
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(capacity);
    const LSTATUS status = RegGetValueA(root, subkey, name,
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type, out, &size);
    return status == ERROR_SUCCESS && size > 1;
}

// Fills `out` with an absolute payload path. Returns false when nothing exists.
inline bool Resolve(HMODULE selfModule, char* out, size_t capacity) {
    char selfPath[MAX_PATH] = {};
    const DWORD selfLength = GetModuleFileNameA(selfModule, selfPath, MAX_PATH);
    if (selfLength == 0 || selfLength >= MAX_PATH) return false;

    char directory[MAX_PATH] = {};
    if (DirectoryOf(selfPath, directory, sizeof(directory))) {
        if (JoinPath(directory, kPayloadName, out, capacity) && FileExistsA(out)) return true;
    }

    char hostPath[MAX_PATH] = {};
    const DWORD hostLength = GetModuleFileNameA(nullptr, hostPath, MAX_PATH);
    if (hostLength > 0 && hostLength < MAX_PATH &&
        DirectoryOf(hostPath, directory, sizeof(directory))) {
        if (JoinPath(directory, kPayloadName, out, capacity) && FileExistsA(out)) return true;
    }

    if (ReadRegistryString(HKEY_LOCAL_MACHINE,
                           "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\steam.exe",
                           "OSTPayloadPath", out, capacity) && FileExistsA(out)) {
        return true;
    }
    if (ReadRegistryString(HKEY_CURRENT_USER, "Software\\OpenSteamTool", "PayloadPath", out, capacity) &&
        FileExistsA(out)) {
        return true;
    }
    return false;
}

inline HMODULE Load(HMODULE selfModule) {
    // An IFEO loader may already have injected a payload from another folder.
    // A second copy would install every hook twice and crash Steam, so reuse
    // the module that is already there.
    if (HMODULE loaded = GetModuleHandleA(kPayloadName)) return loaded;
    char payload[MAX_PATH * 2] = {};
    if (!Resolve(selfModule, payload, sizeof(payload))) return nullptr;
    return LoadLibraryA(payload);
}

} // namespace ProxyPayload
