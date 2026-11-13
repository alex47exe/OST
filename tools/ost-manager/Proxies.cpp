#include "Proxies.h"

#include "Common.h"
#include "Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace ost {
namespace {

constexpr const wchar_t* kDeployedValue = L"DeployedFiles";

std::wstring RootPayload(const std::wstring& steamDir) { return steamDir + L"\\" + kPayloadFileName; }
std::wstring BinPayload(const std::wstring& steamDir) {
    return steamDir + L"\\bin\\" + kPayloadFileName;
}
std::wstring BinDwmapi(const std::wstring& steamDir) { return steamDir + L"\\bin\\dwmapi.dll"; }
std::wstring RootVersion(const std::wstring& steamDir) { return steamDir + L"\\version.dll"; }
std::wstring RootVersionDisabled(const std::wstring& steamDir) { return steamDir + L"\\version.dll.disabled"; }
std::wstring RootXinput(const std::wstring& steamDir) { return steamDir + L"\\xinput1_4.dll"; }

std::wstring BaseNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Files that make one of our entry points reachable. Only these are ever
// deleted from a Steam installation.
bool IsEntryPointFileName(const std::wstring& name) {
    return IEquals(name, L"dwmapi.dll") || IEquals(name, L"version.dll") ||
           IEquals(name, L"version.dll.disabled") || IEquals(name, L"xinput1_4.dll") ||
           IEquals(name, kPayloadFileName);
}

bool SameBytes(const std::wstring& lhs, const std::wstring& rhs) {
    if (!FileExists(lhs) || !FileExists(rhs)) return false;
    const std::uint64_t size = FileSize(lhs);
    if (size == 0 || size != FileSize(rhs)) return false;

    std::ifstream left(lhs, std::ios::binary);
    std::ifstream right(rhs, std::ios::binary);
    if (!left || !right) return false;

    std::vector<char> leftBuffer(64 * 1024);
    std::vector<char> rightBuffer(64 * 1024);
    while (left && right) {
        left.read(leftBuffer.data(), static_cast<std::streamsize>(leftBuffer.size()));
        right.read(rightBuffer.data(), static_cast<std::streamsize>(rightBuffer.size()));
        const std::streamsize leftRead = left.gcount();
        const std::streamsize rightRead = right.gcount();
        if (leftRead != rightRead) return false;
        if (leftRead == 0) break;
        if (std::memcmp(leftBuffer.data(), rightBuffer.data(), static_cast<size_t>(leftRead)) != 0) return false;
    }
    return true;
}

// The copies of `name` this build ships next to the manager.
std::vector<std::wstring> LocalArtifacts(const std::wstring& name) {
    std::vector<std::wstring> artifacts{SelfDir() + L"\\" + name};
    if (IEquals(name, L"version.dll")) artifacts.push_back(SelfDir() + L"\\version.dll.disabled");
    return artifacts;
}


std::vector<std::wstring> ReadDeployed() {
    std::vector<std::wstring> files;
    RegReadMultiString(HKEY_CURRENT_USER, kAppRegKey, kDeployedValue, files);
    return files;
}

void AddDeployed(const std::wstring& path) {
    auto files = ReadDeployed();
    const auto it = std::find_if(files.begin(), files.end(),
                                 [&](const std::wstring& item) { return IEquals(item, path); });
    if (it != files.end()) return;
    files.push_back(path);
    std::wstring error;
    if (!RegWriteMultiString(HKEY_CURRENT_USER, kAppRegKey, kDeployedValue, files, error)) {
        LogWarn(L"could not record the deployed file list: " + error);
    }
}

void ForgetDeployed(const std::wstring& path) {
    auto files = ReadDeployed();
    const auto before = files.size();
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const std::wstring& item) { return IEquals(item, path); }),
                files.end());
    if (files.size() == before) return;
    std::wstring error;
    if (!RegWriteMultiString(HKEY_CURRENT_USER, kAppRegKey, kDeployedValue, files, error)) {
        LogWarn(L"could not update the deployed file list: " + error);
    }
}

std::optional<std::wstring> LocalArtifact(const wchar_t* name) {
    const std::wstring candidate = SelfDir() + L"\\" + name;
    if (FileExists(candidate)) return candidate;
    return std::nullopt;
}

bool IsTracked(const std::wstring& path) {
    const auto files = ReadDeployed();
    return std::any_of(files.begin(), files.end(),
                       [&](const std::wstring& item) { return IEquals(item, path); });
}

// Our payload and every proxy carry the product name in their import strings,
// so a leftover from an older build is still recognised as ours, while an
// unrelated mod's DLL is not.
bool ContainsProductMarker(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string marker = "OpenSteamTool";
    std::vector<char> buffer(64 * 1024);
    std::string window;
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = file.gcount();
        if (read <= 0) break;
        window.append(buffer.data(), static_cast<size_t>(read));
        if (window.find(marker) != std::string::npos) return true;
        if (window.size() > marker.size()) window.erase(0, window.size() - marker.size() + 1);
    }
    return false;
}

// A file this tool may delete: recorded at install time, byte identical to the
// artifact this build ships, or carrying our marker. Anything else belongs to
// another mod and is left alone.
bool IsOursOnDisk(const std::wstring& path) {
    if (!FileExists(path)) return false;
    if (IsTracked(path)) return true;
    for (const auto& artifact : LocalArtifacts(BaseNameOf(path))) {
        if (SameBytes(path, artifact)) return true;
    }
    if (ContainsProductMarker(path)) return true;
    // Unreadable (locked by a running Steam): we cannot prove the file is
    // somebody else's, so treat it as ours. The caller then tries to remove it,
    // fails, and aborts the switch instead of silently keeping two entry points.
    std::ifstream probe(path, std::ios::binary);
    return !probe;
}

bool RemoveOwnedFile(const std::wstring& path) {
    if (!FileExists(path)) return true;
    if (!IsOursOnDisk(path)) {
        LogWarn(L"left in place (not installed by this tool): " + path);
        return true;
    }
    std::wstring error;
    if (!DeleteFileIfExists(path, error)) {
        LogWarn(error);
        return false;
    }
    LogInfo(L"removed " + path);
    return true;
}

// Never destroy a file this tool did not put there: a foreign version.dll /
// dwmapi.dll (another Steam mod) is renamed once and left on disk.
void BackupForeignFile(const std::wstring& destination) {
    if (!FileExists(destination) || IsOursOnDisk(destination)) return;
    const std::wstring backup = destination + L".ost-backup";
    if (FileExists(backup)) return;
    std::wstring error;
    if (MoveFileInto(destination, backup, error)) {
        LogWarn(L"backed up an existing file before overwriting it: " + backup);
    } else {
        LogWarn(L"could not back up " + destination + L": " + error);
    }
}

bool DeployFile(const std::wstring& source, const std::wstring& destination, std::wstring& outError) {
    if (IEquals(source, destination)) return true;
    BackupForeignFile(destination);
    if (!CopyFileInto(source, destination, true, outError)) return false;
    AddDeployed(destination);
    LogInfo(L"deployed " + destination);
    return true;
}

bool RequireArtifact(const wchar_t* name, std::wstring& outPath, std::wstring& outError) {
    const auto artifact = LocalArtifact(name);
    if (!artifact) {
        outError = std::wstring(L"missing ") + name + L" next to this executable";
        return false;
    }
    outPath = *artifact;
    return true;
}

} // namespace

std::wstring ProxyKindName(ProxyKind kind) {
    switch (kind) {
        case ProxyKind::Dwmapi:  return L"dwmapi";
        case ProxyKind::Version: return L"version";
        case ProxyKind::Xinput:  return L"xinput";
        case ProxyKind::None:
        default:                 return L"none";
    }
}

ProxyKind ParseProxyKind(const std::wstring& value) {
    if (IEquals(value, L"dwmapi")) return ProxyKind::Dwmapi;
    if (IEquals(value, L"version")) return ProxyKind::Version;
    if (IEquals(value, L"xinput") || IEquals(value, L"xinput1_4")) return ProxyKind::Xinput;
    return ProxyKind::None;
}

ProxyStatus QueryProxies(const std::wstring& steamDir) {
    ProxyStatus status;
    if (steamDir.empty()) return status;
    // Only entry points that load *our* payload count: a foreign version.dll
    // planted by another mod is reported by the install path, not as our hook.
    status.payloadInRoot = IsOursOnDisk(RootPayload(steamDir));
    status.payloadInBin = IsOursOnDisk(BinPayload(steamDir));
    status.dwmapiInBin = IsOursOnDisk(BinDwmapi(steamDir));
    status.versionEnabled = IsOursOnDisk(RootVersion(steamDir));
    status.versionDisabled = FileExists(RootVersionDisabled(steamDir));
    status.xinputInRoot = IsOursOnDisk(RootXinput(steamDir));
    return status;
}

bool DeployProxy(ProxyKind kind, const std::wstring& steamDir, const std::wstring& payloadSource,
                 std::wstring& outError) {
    if (kind == ProxyKind::None) {
        outError = L"unknown proxy method";
        return false;
    }
    if (steamDir.empty() || !FileExists(steamDir + L"\\steam.exe")) {
        outError = L"Steam installation not found";
        return false;
    }
    if (!FileExists(payloadSource)) {
        outError = L"payload DLL not found: " + payloadSource;
        return false;
    }

    // One hook at a time: every other proxy and payload copy goes away before
    // the new one is written, so a failed switch can never leave two active.
    if (!RemoveDeployedProxies(steamDir, outError)) return false;

    std::wstring artifact;
    switch (kind) {
        case ProxyKind::Dwmapi: {
            if (!RequireArtifact(L"dwmapi.dll", artifact, outError)) return false;
            // dwmapi is imported by bin\vgui2_s.dll, so both the proxy and the
            // payload it loads belong in bin.
            if (!DeployFile(payloadSource, BinPayload(steamDir), outError)) return false;
            if (!DeployFile(artifact, BinDwmapi(steamDir), outError)) return false;
            break;
        }
        case ProxyKind::Version: {
            std::wstring source = SelfDir() + L"\\version.dll";
            if (!FileExists(source)) {
                // The release ships the proxy disabled; enabling it is this rename.
                const std::wstring disabled = SelfDir() + L"\\version.dll.disabled";
                if (!FileExists(disabled)) {
                    outError = L"missing version.dll (or version.dll.disabled) next to this executable";
                    return false;
                }
                source = disabled;
            }
            if (!DeployFile(payloadSource, RootPayload(steamDir), outError)) return false;
            if (!DeployFile(source, RootVersion(steamDir), outError)) return false;
            break;
        }
        case ProxyKind::Xinput: {
            if (!RequireArtifact(L"xinput1_4.dll", artifact, outError)) return false;
            if (!DeployFile(payloadSource, RootPayload(steamDir), outError)) return false;
            if (!DeployFile(artifact, RootXinput(steamDir), outError)) return false;
            break;
        }
        case ProxyKind::None:
            break;
    }
    return true;
}

std::wstring DeployedPayloadPath(ProxyKind kind, const std::wstring& steamDir) {
    if (steamDir.empty()) return L"";
    return kind == ProxyKind::Dwmapi ? BinPayload(steamDir) : RootPayload(steamDir);
}

bool RemoveDeployedProxies(const std::wstring& steamDir, std::wstring& outError) {
    outError.clear();

    std::vector<std::wstring> candidates;
    if (!steamDir.empty()) {
        for (const wchar_t* name : {L"version.dll", L"version.dll.disabled", L"xinput1_4.dll", kPayloadFileName}) {
            candidates.push_back(steamDir + L"\\" + name);
        }
        candidates.push_back(BinDwmapi(steamDir));
        candidates.push_back(BinPayload(steamDir));
    }
    // Paths an earlier install recorded, which may point outside this Steam
    // directory (a moved installation, an old build folder).
    for (const auto& tracked : ReadDeployed()) {
        if (IsEntryPointFileName(BaseNameOf(tracked))) candidates.push_back(tracked);
    }

    bool ok = true;
    std::wstring failed;
    std::vector<std::wstring> seen;
    for (const auto& path : candidates) {
        if (std::any_of(seen.begin(), seen.end(), [&](const std::wstring& item) { return IEquals(item, path); }))
            continue;
        seen.push_back(path);
        if (!RemoveOwnedFile(path)) {
            ok = false;
            failed = path;
            // Stop at the first file that cannot go away: continuing would
            // dismantle the old entry point half-way (proxy without payload).
            break;
        }
    }

    // Leaving an old entry point behind while a new one is deployed is exactly
    // the two-payload state that crashes Steam, so the caller must not proceed.
    if (!ok) {
        outError = L"could not remove the previous entry point " + failed +
                   L" (close Steam and try again)";
        return false;
    }

    // Keep the tracked entries that are not entry points (root-mode loader copy).
    std::vector<std::wstring> remaining;
    for (const auto& file : ReadDeployed()) {
        if (!IsEntryPointFileName(BaseNameOf(file))) remaining.push_back(file);
    }
    std::wstring listError;
    if (!RegWriteMultiString(HKEY_CURRENT_USER, kAppRegKey, kDeployedValue, remaining, listError)) {
        LogWarn(L"could not update the deployed file list: " + listError);
    }
    return ok;
}

bool RemoveRuntimeArtifacts(const std::wstring& steamDir, std::wstring& outError) {
    (void)outError;
    if (steamDir.empty()) return true;
    std::wstring localError;
    // Best effort: Steam may still hold the module open.
    DeleteFileIfExists(steamDir + L"\\bin\\diversion64.dll", localError);
    return true;
}

void TrackDeployedFile(const std::wstring& path) { AddDeployed(path); }

bool RemoveTrackedFiles(const std::wstring& steamDir, std::wstring& outError) {
    (void)steamDir;
    (void)outError;
    const auto files = ReadDeployed();
    for (const auto& file : files) {
        std::wstring localError;
        if (!DeleteFileIfExists(file, localError)) {
            LogWarn(localError);
            continue;
        }
        LogInfo(L"removed " + file);
    }
    RegDeleteValue(HKEY_CURRENT_USER, kAppRegKey, kDeployedValue);
    return true;
}

} // namespace ost
