#pragma once

// Where this build of OpenSteamTool lives and publishes its metadata.
// Changing the hosting repository only requires editing this file.
namespace ProjectInfo {

    inline constexpr const char* kRepoUrl   = "https://github.com/matheeshapathirana/OpenSteamTool";
    inline constexpr const char* kIssuesUrl = "https://github.com/matheeshapathirana/OpenSteamTool/issues";

    // Pattern / IPC TOMLs are published to the `pattern` and `ipc` branches of
    // the repository, laid out as {component}/{sha256}.toml (the same layout
    // steam-monitor uses). Placeholders: {channel} {component} {sha256}.
    inline constexpr const char* kMetadataBrowseTemplate =
        "https://github.com/matheeshapathirana/OpenSteamTool/tree/{channel}/{component}";

    struct MetadataSource {
        const char* origin;        // sources sharing an origin mirror the same data
        const char* urlTemplate;
    };

    // Tried in order. A 404 skips the remaining mirrors of the same origin
    // and moves on to the next origin.
    inline constexpr MetadataSource kMetadataSources[] = {
        {"fork",  "https://raw.githubusercontent.com/matheeshapathirana/OpenSteamTool/{channel}/{component}/{sha256}.toml"},
        {"fork",  "https://cdn.jsdelivr.net/gh/matheeshapathirana/OpenSteamTool@{channel}/{component}/{sha256}.toml"},
        // Original upstream bot, kept as a last resort in case it publishes again.
        {"steam-monitor", "https://raw.githubusercontent.com/OpenSteam001/steam-monitor/{channel}/{component}/{sha256}.toml"},
    };

} // namespace ProjectInfo
