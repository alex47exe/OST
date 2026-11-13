#pragma once

#include <string>
#include <string_view>

// Where this build lives and where it publishes Steam metadata.
// Changing the hosting repository only requires editing this file.
namespace ProjectInfo {

    inline constexpr const char* kRepoUrl   = "https://github.com/alex47exe/OST";
    inline constexpr const char* kIssuesUrl = "https://github.com/alex47exe/OST/issues";

    // Pattern / IPC TOMLs are published to the `pattern` and `ipc` branches of
    // this repository, laid out as {component}/{sha256}.toml (the same layout
    // steam-monitor uses). Placeholders: {channel} {component} {sha256}.
    inline constexpr const char* kMetadataBrowseTemplate =
        "https://github.com/alex47exe/OST/tree/{channel}/{component}";

    // Metadata sources, tried in the order RemoteToml builds them.
    // GitHub raw direct.
    inline constexpr const char* kLocalRawTemplate =
        "https://raw.githubusercontent.com/alex47exe/OST/{channel}/{component}/{sha256}.toml";
    // gh-proxy wraps the raw URL; more stable for users behind restrictive networks.
    inline constexpr const char* kLocalGhProxyTemplate =
        "https://gh-proxy.com/https://raw.githubusercontent.com/alex47exe/OST/{channel}/{component}/{sha256}.toml";
    // jsDelivr mirror of this repository's branches.
    inline constexpr const char* kLocalJsdelivrTemplate =
        "https://cdn.jsdelivr.net/gh/alex47exe/OST@{channel}/{component}/{sha256}.toml";
    // Maintained fork that publishes the same layout; used when our branches lack a build.
    inline constexpr const char* kForkRawTemplate =
        "https://raw.githubusercontent.com/matheeshapathirana/OpenSteamTool/{channel}/{component}/{sha256}.toml";
    // Original tracker, kept as the last resort.
    inline constexpr const char* kUpstreamRawTemplate =
        "https://raw.githubusercontent.com/huanyuejue/steam-monitor/main/{channel}/{component}/{sha256}.toml";

    // Expand kMetadataBrowseTemplate for a concrete channel/component pair.
    inline std::string MetadataBrowseUrl(std::string_view channel, std::string_view component)
    {
        std::string url = kMetadataBrowseTemplate;
        const auto replaceAll = [&url](std::string_view from, std::string_view to) {
            for (size_t pos = 0; (pos = url.find(from, pos)) != std::string::npos; pos += to.size())
                url.replace(pos, from.size(), to);
        };
        replaceAll("{channel}", channel);
        replaceAll("{component}", component);
        return url;
    }

} // namespace ProjectInfo
