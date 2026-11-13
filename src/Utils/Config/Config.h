#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    struct InjectionSettings {
        bool enabled = false;
        std::string libraryX86;
        std::string libraryX64;
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    ManifestTimeouts GetManifestTimeouts();
    LogLevel GetLogLevel();
    std::string GetLogDir();
    std::vector<std::string> GetLuaPaths();
    std::string GetRemoteUrlTemplate();
    bool GetRemoteAutoDerive();
    std::string GetRemoteOrder();
    InjectionSettings GetInjectionSettings();
    CloudSettings GetCloudSettings();
    bool GetStatsEnableApi();
    std::string GetStatsApiUrl();

    // [presence] — friend broadcast mode: "spacewar" (default) or "none".
    // Anything other than "none" keeps the current broadcast behavior.
    bool GetPresenceBroadcastEnabled();

    // [manifest] — provider selection lives in ManifestClient (table-driven).
    inline uint32_t manifestTimeoutResolve = 5000;
    inline uint32_t manifestTimeoutConnect = 5000;
    inline uint32_t manifestTimeoutSend    = 10000;
    inline uint32_t manifestTimeoutRecv    = 10000;
    // Ordered-failover switch: when off, only the preferred source is queried.
    inline bool manifestFailover = true;

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // derived from configPath: <steam>/opensteamtool/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

    // [remote]
    inline std::string remoteUrlTemplate;
    // Default: gh-proxy mirror first, GitHub direct connection as fallback; set "github-first" to swap the order;
    // the legacy value "jsdelivr-first" is still accepted and means gh-proxy-first
    inline std::string remoteOrder = "gh-proxy-first";
    // Derive patterns/IPC metadata locally when nothing is published for the installed build.
    inline bool remoteAutoDerive = true;

    // [stats]
    inline bool statsEnableApi = true;
    // Endpoint used for the stats SteamID lookup; must contain {appid}.
    inline std::string statsApiUrl = "https://stats.luastools.xyz/{appid}";

    // [presence] — "spacewar" shows the real game name to friends (default),
    // "none" skips the name broadcast patches.
    inline std::string presenceDisplay = "spacewar";

    // [inject] - optional library injection into game processes.
    inline bool injectEnabled = false;
    inline std::string injectLibraryX86;
    inline std::string injectLibraryX64;

    // [cloud] - optional Steam Cloud save redirection via CloudRedirect.
    inline bool cloudEnabled = false;
    inline std::string cloudLibrary;

}
