#pragma once

#include <string>
#include <vector>

namespace ost {

enum class ProxyKind { None, Dwmapi, Version, Xinput };

struct ProxyStatus {
    bool payloadInRoot = false;
    bool payloadInBin = false;
    bool dwmapiInBin = false;
    bool versionEnabled = false;
    bool versionDisabled = false;
    bool xinputInRoot = false;

    bool AnyProxyActive() const { return dwmapiInBin || versionEnabled || xinputInRoot; }
};

ProxyStatus QueryProxies(const std::wstring& steamDir);

// Where the payload lives for a given entry point. A proxy resolves the payload
// from its own directory first, so the copy must sit next to the proxy:
//   dwmapi -> <steam>\bin\OpenSteamTool.dll
//   version, xinput1_4, ifeo -> <steam>\OpenSteamTool.dll
std::wstring DeployedPayloadPath(ProxyKind kind, const std::wstring& steamDir);

// Deploys one proxy method and removes every other entry point first, so
// exactly one hook is active afterwards.
bool DeployProxy(ProxyKind kind, const std::wstring& steamDir, const std::wstring& payloadSource,
                 std::wstring& outError);

// Removes every proxy entry point (and the payload copies that belong to it),
// whether or not it is still in the tracked-file list: a leftover proxy from an
// earlier install would load a second payload next to an IFEO loader.
bool RemoveDeployedProxies(const std::wstring& steamDir, std::wstring& outError);

// Files installed by `install ifeo-root` (a copy of this executable in the
// Steam root) are tracked with the same registry list.
void TrackDeployedFile(const std::wstring& path);
bool RemoveTrackedFiles(const std::wstring& steamDir, std::wstring& outError);

// Removes runtime artifacts that are not proxies (diversion shadow module).
// The diversion copy follows the data root, so both locations are cleaned.
bool RemoveRuntimeArtifacts(const std::wstring& steamDir, const std::wstring& dataRoot,
                            std::wstring& outError);

std::wstring ProxyKindName(ProxyKind kind);
ProxyKind ParseProxyKind(const std::wstring& value);

} // namespace ost
