#pragma once

#include <string>

namespace SteamDiagnostics {

    // Capture the Steam build id and DLL hashes used in support popups.
    void Initialize(const std::string& steamclientPath,
                    const std::string& steamUIPath);

    // Reuse captured hashes for known Steam DLLs and hash other files on demand.
    std::string Sha256Of(const std::string& path);

    // Display a warning asynchronously with the captured Steam diagnostics.
    void ShowWarning(std::string title, std::string message);

    // A metadata file (pattern / IPC TOML) that could not be obtained.
    struct MissingMetadata {
        std::string description;  // e.g. "steamui.dll signatures"
        std::string cachePath;    // relative to the opensteamtool dir in Steam
        std::string upstreamUrl;  // where the bot publishes it
    };

    // Queue a missing metadata file; nothing is shown until
    // ShowMissingMetadataSummary() so one Steam update yields one popup.
    void ReportMissingMetadata(MissingMetadata entry);

    // Show a single popup covering everything queued, then clear the queue.
    // No-op when nothing was reported.
    void ShowMissingMetadataSummary();

} // namespace SteamDiagnostics
