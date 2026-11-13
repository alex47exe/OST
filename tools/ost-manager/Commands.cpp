#include "Commands.h"

#include "Common.h"
#include "Ifeo.h"
#include "Inject.h"
#include "Log.h"
#include "Proxies.h"
#include "Steam.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace ost {
namespace {

std::wstring PayloadNextToExe() { return SelfDir() + L"\\" + kPayloadFileName; }

// The IFEO Debugger value can only run the windowless loader, which the build
// stages next to the manager.
std::wstring FindLoaderExe() {
    const std::wstring bundled = SelfDir() + L"\\ost-loader.exe";
    return FileExists(bundled) ? bundled : std::wstring();
}

std::wstring ResolvePayload(const Options& options, const std::wstring& steamDir) {
    if (!options.payloadPath.empty()) return options.payloadPath;
    const std::wstring beside = PayloadNextToExe();
    if (FileExists(beside)) return beside;
    if (!steamDir.empty() && FileExists(steamDir + L"\\" + kPayloadFileName))
        return steamDir + L"\\" + kPayloadFileName;
    return beside; // reported as missing by the caller
}

// The payload resolves everything it writes -- opensteamtool.toml, config\lua,
// logs, metadata caches and the diversion copy -- from a data root. Steam's own
// files (steamclient64.dll, userdata) always come from the real Steam folder,
// but the tool's state lives where the manager lives, so a portable ost behaves
// exactly as if it sat inside Steam without writing into Steam.
std::wstring RuntimeDataRoot(const Options& options, const std::wstring& steamDir) {
    return ResolveDataRoot(options.dataDir, steamDir);
}

void PublishRuntimePaths(const Options* options, const std::wstring& steamDir,
                         const std::wstring& payloadPath) {
    if (payloadPath.empty()) return;
    // The proxies fall back to HKCU\Software\OpenSteamTool\PayloadPath when the
    // payload is not next to them or next to steam.exe (hot-inject / portable).
    std::wstring error;
    if (!RegWriteString(HKEY_CURRENT_USER, kAppRegKey, kAppValuePayloadPath, payloadPath, error)) {
        LogWarn(L"could not record the payload path: " + error);
    }

    // A hot-injected payload cannot inherit our environment, so the data root
    // is published through the registry; the loader passes OST_CONFIG_DIR to
    // the Steam it starts, which wins over this value.
    static const Options kNoOverride;
    const std::wstring dataRoot = RuntimeDataRoot(options ? *options : kNoOverride, steamDir);
    if (dataRoot.empty() || IEquals(dataRoot, steamDir)) {
        RegDeleteValue(HKEY_CURRENT_USER, kAppRegKey, kAppValueConfigDir);
        return;
    }
    if (!RegWriteString(HKEY_CURRENT_USER, kAppRegKey, kAppValueConfigDir, dataRoot, error)) {
        LogWarn(L"could not record the data root: " + error);
        return;
    }
    // Create the folders the payload will use, so the layout is visible before
    // the first launch (and lua scripts have a place to go).
    if (EnsureDirectory(dataRoot + L"\\config\\lua")) {
        LogInfo(L"tool data root: " + dataRoot + L" (config\\lua created)");
    } else {
        LogInfo(L"tool data root: " + dataRoot);
    }
}

bool RequireSteam(const Options& options, std::wstring& outSteamDir) {
    const auto steam = FindSteamInstall(options.steamDir);
    if (!steam) {
        LogError(L"Steam installation not found; pass --steam-dir <path>");
        return false;
    }
    outSteamDir = *steam;
    return true;
}

bool EnsureAdministrator(const Options& options, bool& outRelaunched, DWORD& outExitCode) {
    outRelaunched = false;
    outExitCode = 0;
    if (IsElevated()) return true;
    std::wstring error;
    LogWarn(L"administrator rights required, requesting elevation");
    if (!RelaunchElevated(options.rawArgs, error, &outExitCode)) {
        LogError(L"elevation failed: " + error);
        return false;
    }
    outRelaunched = true;
    LogInfo(L"the elevated instance finished with exit code " + std::to_wstring(outExitCode));
    return true;
}

std::wstring JsonEscape(const std::wstring& value) {
    std::string utf8 = Utf8FromWide(value);
    std::string escaped;
    for (char ch : utf8) {
        switch (ch) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += ch; break;
        }
    }
    return WideFromUtf8(escaped);
}

void EmitRaw(const std::wstring& text) { ConsoleWrite(text + L"\n"); }

struct StatusSnapshot {
    std::wstring steamDir;
    std::vector<SteamProcess> processes;
    IfeoStatus ifeo;
    ProxyStatus proxies;
    std::wstring payloadPath;
    bool payloadExists = false;
    std::uint64_t payloadSize = 0;
    std::wstring dataRoot;
    bool autostart = false;
    bool watcherRunning = false;
};

StatusSnapshot CollectStatus(const Options& options) {
    StatusSnapshot snapshot;
    if (const auto steam = FindSteamInstall(options.steamDir)) snapshot.steamDir = *steam;
    snapshot.processes = FindSteamProcesses();
    snapshot.ifeo = QueryIfeo();
    snapshot.proxies = QueryProxies(snapshot.steamDir);
    snapshot.dataRoot = RuntimeDataRoot(options, snapshot.steamDir);
    const std::wstring deployed =
        snapshot.steamDir.empty() ? L"" : snapshot.steamDir + L"\\" + kPayloadFileName;
    snapshot.payloadPath =
        (!deployed.empty() && FileExists(deployed)) ? deployed : ResolvePayload(options, snapshot.steamDir);
    snapshot.payloadExists = FileExists(snapshot.payloadPath);
    snapshot.payloadSize = snapshot.payloadExists ? FileSize(snapshot.payloadPath) : 0;
    snapshot.autostart = IsAutostartEnabled();
    snapshot.watcherRunning = IsWatcherRunning();
    return snapshot;
}

std::wstring DescribeProcesses(const std::vector<SteamProcess>& processes) {
    if (processes.empty()) return L"not running";
    std::wstring text;
    for (size_t i = 0; i < processes.size(); ++i) {
        if (i) text += L", ";
        text += L"pid " + std::to_wstring(processes[i].pid);
        text += processes[i].payloadLoaded ? L" (payload loaded)" : L" (payload not loaded)";
    }
    return text;
}

std::wstring DescribeProxies(const ProxyStatus& proxies) {
    std::wstring text;
    auto append = [&](const wchar_t* label) {
        if (!text.empty()) text += L", ";
        text += label;
    };
    if (proxies.dwmapiInBin) append(L"dwmapi (bin)");
    if (proxies.versionEnabled) append(L"version (root)");
    if (proxies.xinputInRoot) append(L"xinput1_4 (root)");
    if (text.empty()) {
        if (proxies.versionDisabled) return L"none (version.dll.disabled present in Steam)";
        return L"none";
    }
    return text;
}

// Exactly one entry point may be active. Two of them load two payload copies
// into one Steam process, which crashes Steam, so the count is reported (and
// flagged) everywhere the state is shown.
struct EntryPointSummary {
    int count = 0;
    std::wstring description;
};

EntryPointSummary SummarizeEntryPoints(const IfeoStatus& ifeo, const ProxyStatus& proxies) {
    EntryPointSummary summary;
    std::wstring active;
    auto append = [&](const std::wstring& label) {
        if (!active.empty()) active += L" + ";
        active += label;
        ++summary.count;
    };
    if (ifeo.state == IfeoState::InstalledSelf) {
        append(ifeo.mode.empty() ? std::wstring(L"ifeo") : L"ifeo (" + ifeo.mode + L")");
    }
    if (proxies.dwmapiInBin) append(L"dwmapi");
    if (proxies.versionEnabled) append(L"version");
    if (proxies.xinputInRoot) append(L"xinput");

    if (summary.count == 0) {
        summary.description = L"none";
    } else if (summary.count == 1) {
        summary.description = active;
    } else {
        summary.description = active + L"  -- " + std::to_wstring(summary.count) +
                              L" entry points active: run 'install <method>' to keep exactly one";
    }
    return summary;
}

int CmdStatus(const Options& options) {
    const StatusSnapshot snapshot = CollectStatus(options);
    const EntryPointSummary entry = SummarizeEntryPoints(snapshot.ifeo, snapshot.proxies);

    if (options.json) {
        std::wstring json = L"{";
        json += L"\"version\":\"" + JsonEscape(ToolVersion()) + L"\",";
        json += L"\"steam\":{\"dir\":\"" + JsonEscape(snapshot.steamDir) + L"\",\"processes\":[";
        for (size_t i = 0; i < snapshot.processes.size(); ++i) {
            if (i) json += L",";
            json += L"{\"pid\":" + std::to_wstring(snapshot.processes[i].pid) + L",\"payloadLoaded\":" +
                    (snapshot.processes[i].payloadLoaded ? L"true" : L"false") + L"}";
        }
        json += L"]},";
        json += L"\"ifeo\":{\"state\":\"" + JsonEscape(IfeoStateName(snapshot.ifeo.state)) +
                L"\",\"debugger\":\"" + JsonEscape(snapshot.ifeo.debuggerCommand) + L"\",\"payload\":\"" +
                JsonEscape(snapshot.ifeo.payloadPath) + L"\",\"mode\":\"" + JsonEscape(snapshot.ifeo.mode) + L"\"},";
        json += L"\"proxies\":{\"dwmapi\":" + std::wstring(snapshot.proxies.dwmapiInBin ? L"true" : L"false") +
                L",\"version\":" + (snapshot.proxies.versionEnabled ? L"true" : L"false") + L",\"xinput\":" +
                (snapshot.proxies.xinputInRoot ? L"true" : L"false") + L"},";
        json += L"\"entryPoints\":{\"count\":" + std::to_wstring(entry.count) + L",\"description\":\"" +
                JsonEscape(entry.description) + L"\"},";
        json += L"\"payload\":{\"path\":\"" + JsonEscape(snapshot.payloadPath) + L"\",\"exists\":" +
                (snapshot.payloadExists ? L"true" : L"false") + L",\"size\":" +
                std::to_wstring(snapshot.payloadSize) + L"},";
        json += L"\"dataRoot\":\"" + JsonEscape(snapshot.dataRoot) + L"\",";
        json += L"\"autostart\":" + std::wstring(snapshot.autostart ? L"true" : L"false") + L",";
        json += L"\"watcher\":" + std::wstring(snapshot.watcherRunning ? L"true" : L"false");
        json += L"}";
        EmitRaw(json);
        return 0;
    }

    LogInfo(L"OpenSteamTool manager " + ToolVersion());
    PrintFields({
        {L"Steam install", snapshot.steamDir.empty() ? L"not found" : snapshot.steamDir},
        {L"Steam process", DescribeProcesses(snapshot.processes)},
        {L"IFEO entry", IfeoStateName(snapshot.ifeo.state) +
                            (snapshot.ifeo.mode.empty() ? L"" : L" (" + snapshot.ifeo.mode + L")")},
        {L"IFEO command", snapshot.ifeo.debuggerCommand.empty() ? L"-" : snapshot.ifeo.debuggerCommand},
        {L"Proxy DLLs", DescribeProxies(snapshot.proxies)},
        {L"Entry point", entry.description},
        {L"Payload", snapshot.payloadExists
                          ? snapshot.payloadPath + L" (" + std::to_wstring(snapshot.payloadSize) + L" bytes)"
                          : L"missing (" + snapshot.payloadPath + L")"},
        {L"Data root", snapshot.dataRoot.empty() ? L"-" : snapshot.dataRoot},
        {L"Autostart", snapshot.autostart ? L"enabled" : L"disabled"},
        {L"Watcher", snapshot.watcherRunning ? L"running" : L"stopped"},
        {L"Log file", LogFilePath().empty() ? L"disabled" : LogFilePath()},
    });
    if (entry.count > 1) {
        LogWarn(L"more than one entry point is active; Steam would load two payload copies. "
                L"Run 'ost-manager install <method>' to keep exactly one.");
    }
    return 0;
}

int InstallProxy(const Options& options, ProxyKind kind, const std::wstring& steamDir) {
    const std::wstring payload = ResolvePayload(options, steamDir);
    if (!FileExists(payload)) {
        LogError(L"payload DLL not found: " + payload + L" (build OpenSteamTool.dll or pass --payload)");
        return 1;
    }
    LogDetail(L"steam: " + steamDir);
    LogDetail(L"payload: " + payload + L" (" + std::to_wstring(FileSize(payload)) + L" bytes)");

    const IfeoStatus ifeo = QueryIfeo();
    if (ifeo.state == IfeoState::InstalledSelf) {
        if (!IsElevated()) {
            // Switching methods must not leave two entry points behind; removing
            // the IFEO key needs administrator rights, so ask for them here.
            bool relaunched = false;
            DWORD elevatedExitCode = 0;
            if (EnsureAdministrator(options, relaunched, elevatedExitCode)) {
                if (relaunched) return static_cast<int>(elevatedExitCode);
            } else {
                LogWarn(L"the IFEO entry stays installed (needs administrator rights to remove); "
                        L"both entry points will be active");
            }
        } else {
            std::wstring error;
            if (!UninstallIfeo(error)) LogWarn(L"could not remove the IFEO entry: " + error);
            else LogInfo(L"removed the previous IFEO entry");
        }
    }

    if (options.dryRun) {
        LogInfo(L"dry run: would deploy proxy '" + ProxyKindName(kind) + L"' into " + steamDir);
        return 0;
    }

    std::wstring error;
    if (!DeployProxy(kind, steamDir, payload, error)) {
        LogError(error);
        return 1;
    }
    // The proxy loads the payload from its own directory, so the recorded path
    // is the deployed copy, not the one next to the manager.
    PublishRuntimePaths(&options, steamDir, DeployedPayloadPath(kind, steamDir));
    LogInfo(L"installed the " + ProxyKindName(kind) + L" proxy entry point (single entry point enforced)");

    if (!options.restart) {
        if (!FindSteamProcesses().empty()) LogWarn(L"Steam is running: restart it for the proxy to load");
        return 0;
    }
    if (FindSteamProcesses(false).empty()) return 0;

    std::wstring restartError;
    if (!StopSteam(steamDir, options.force, restartError)) {
        LogError(restartError);
        return 1;
    }
    if (!StartSteam(steamDir, restartError)) {
        LogError(restartError);
        return 1;
    }
    return 0;
}

int CmdInstallIfeo(const Options& options, bool rootMode) {
    std::wstring steamDir;
    if (!RequireSteam(options, steamDir)) return 1;

    const std::wstring payload = ResolvePayload(options, steamDir);
    if (!FileExists(payload)) {
        LogError(L"payload DLL not found: " + payload);
        return 1;
    }
    // The IFEO Debugger value can only run the dedicated loader; pointing it at
    // the manager would start the window instead of Steam.
    const std::wstring loader = FindLoaderExe();
    if (loader.empty()) {
        LogError(L"ost-loader.exe is missing next to " + SelfExePath() +
                 L"; rebuild the bundle (cmake --build build --config Release --target ost-manager)");
        return 1;
    }
    LogDetail(L"steam: " + steamDir);
    LogDetail(L"loader: " + loader);
    LogDetail(L"payload: " + payload + L" (" + std::to_wstring(FileSize(payload)) + L" bytes)");
    if (options.dryRun) {
        LogInfo(std::wstring(L"dry run: would install the ") + (rootMode ? L"Steam-root" : L"portable") +
                L" IFEO loader for " + payload);
        return 0;
    }

    bool relaunched = false;
    DWORD elevatedExitCode = 0;
    if (!EnsureAdministrator(options, relaunched, elevatedExitCode)) return 1;
    if (relaunched) return static_cast<int>(elevatedExitCode);

    std::wstring error;
    // One hook at a time: the proxy entry points are removed before the IFEO
    // entry is written.
    if (!RemoveDeployedProxies(steamDir, error)) {
        LogError(L"could not remove the proxy entry points: " + error);
        return 1;
    }
    RemoveRuntimeArtifacts(steamDir, RuntimeDataRoot(options, steamDir), error);

    std::wstring loaderExe = loader;
    std::wstring effectivePayload = payload;
    if (rootMode) {
        const std::wstring targetLoader = steamDir + L"\\ost-loader.exe";
        const std::wstring targetPayload = steamDir + L"\\" + kPayloadFileName;
        if (!CopyFileInto(loader, targetLoader, true, error)) {
            LogError(error);
            return 1;
        }
        TrackDeployedFile(targetLoader);
        if (!IEquals(payload, targetPayload) && !CopyFileInto(payload, targetPayload, true, error)) {
            LogError(error);
            return 1;
        }
        TrackDeployedFile(targetPayload);
        loaderExe = targetLoader;
        effectivePayload = targetPayload;
        LogInfo(L"deployed " + targetLoader);
        LogInfo(L"deployed " + targetPayload);
    }

    if (!InstallIfeo(loaderExe, effectivePayload, rootMode, error)) {
        LogError(error);
        return 1;
    }
    PublishRuntimePaths(&options, steamDir, effectivePayload);
    LogInfo(L"IFEO loader installed (" + std::wstring(rootMode ? L"Steam root" : L"portable") +
            L" mode, single entry point enforced)");
    LogInfo(L"next launch of steam.exe is intercepted before its entry point");
    if (FindSteamProcesses(false).empty()) return 0;
    if (!options.restart) {
        LogWarn(L"Steam is already running: close it so the next start goes through the loader, "
                L"or pass --restart");
        return 0;
    }
    std::wstring restartError;
    if (!StopSteam(steamDir, options.force, restartError) || !StartSteam(steamDir, restartError)) {
        LogError(restartError);
        return 1;
    }
    return 0;
}

int CmdInstall(const Options& options) {
    if (options.positional.empty()) {
        LogError(L"install needs a method: dwmapi | version | xinput | ifeo | ifeo-root");
        return 2;
    }
    const std::wstring method = ToLower(options.positional.front());

    std::wstring steamDir;
    if (method == L"ifeo") return CmdInstallIfeo(options, false);
    if (method == L"ifeo-root" || method == L"ifeo_root") return CmdInstallIfeo(options, true);

    const ProxyKind kind = ParseProxyKind(method);
    if (kind == ProxyKind::None) {
        LogError(L"unknown method '" + options.positional.front() +
                 L"': use dwmapi | version | xinput | ifeo | ifeo-root");
        return 2;
    }
    if (!RequireSteam(options, steamDir)) return 1;
    return InstallProxy(options, kind, steamDir);
}

int CmdUninstallProxies(const Options& options) {
    std::wstring steamDir;
    if (!RequireSteam(options, steamDir)) return 1;
    if (options.dryRun) {
        LogInfo(L"dry run: would remove every proxy entry point and its payload copy from " + steamDir);
        return 0;
    }

    // A payload mapped from inside Steam cannot be deleted; close Steam first.
    bool payloadInSteam = false;
    for (const auto& process : FindSteamProcesses(true)) {
        if (!process.payloadPath.empty() && IStartsWith(process.payloadPath, steamDir)) {
            payloadInSteam = true;
            break;
        }
    }
    bool restartAfter = false;
    if (payloadInSteam) {
        if (!options.restart) {
            LogError(L"Steam has OpenSteamTool loaded; close Steam first or pass --restart");
            return 1;
        }
        std::wstring stopError;
        if (!StopSteam(steamDir, options.force, stopError)) {
            LogError(stopError);
            return 1;
        }
        restartAfter = true;
    }

    std::wstring error;
    if (!RemoveDeployedProxies(steamDir, error)) {
        LogError(error);
        return 1;
    }
    RemoveRuntimeArtifacts(steamDir, RuntimeDataRoot(options, steamDir), error);
    LogInfo(L"proxy entry points removed; the IFEO entry and autostart were left untouched");

    if (restartAfter) {
        std::wstring startError;
        if (!StartSteam(steamDir, startError)) {
            LogError(startError);
            return 1;
        }
    }
    return 0;
}

int CmdUninstall(const Options& options) {
    const IfeoStatus ifeo = QueryIfeo();
    if (options.dryRun) {
        LogInfo(std::wstring(L"dry run: would remove IFEO (") + IfeoStateName(ifeo.state) +
                L"), proxies, autostart and runtime artifacts");
        return 0;
    }

    const bool needsAdmin = ifeo.state == IfeoState::InstalledSelf || ifeo.state == IfeoState::Orphaned;
    bool relaunched = false;
    DWORD elevatedExitCode = 0;
    if (needsAdmin) {
        if (!EnsureAdministrator(options, relaunched, elevatedExitCode)) return 1;
        if (relaunched) return static_cast<int>(elevatedExitCode);
    }

    std::wstring steamDir;
    const auto found = FindSteamInstall(options.steamDir);
    if (found) steamDir = *found;

    // A payload mapped from inside Steam cannot be deleted; close Steam first.
    bool payloadInSteam = false;
    for (const auto& process : FindSteamProcesses(true)) {
        if (!process.payloadPath.empty() && !steamDir.empty() &&
            IStartsWith(process.payloadPath, steamDir)) {
            payloadInSteam = true;
            break;
        }
    }
    bool restartAfter = false;
    if (payloadInSteam) {
        if (!options.restart) {
            LogError(L"Steam has OpenSteamTool loaded; close Steam first or pass --restart");
            return 1;
        }
        std::wstring stopError;
        if (!StopSteam(steamDir, options.force, stopError)) {
            LogError(stopError);
            return 1;
        }
        restartAfter = true;
    }

    if (StopWatcher()) LogInfo(L"stopped the background watcher");
    std::wstring error;
    if (!SetAutostart(false, steamDir, error)) LogWarn(error);

    if (needsAdmin && !UninstallIfeo(error)) {
        LogError(error);
        return 1;
    }
    if (ifeo.state == IfeoState::InstalledOther)
        LogWarn(L"an IFEO entry owned by another tool was left in place");

    if (!RemoveDeployedProxies(steamDir, error)) LogWarn(error);
    if (!RemoveTrackedFiles(steamDir, error)) LogWarn(error);
    RemoveRuntimeArtifacts(steamDir, RuntimeDataRoot(options, steamDir), error);
    RegDeleteValue(HKEY_CURRENT_USER, kAppRegKey, kAppValuePayloadPath);
    RegDeleteValue(HKEY_CURRENT_USER, kAppRegKey, kAppValueConfigDir);
    if (!RegKeyHasValues(HKEY_CURRENT_USER, kAppRegKey)) {
        RegDeleteKeyTree(HKEY_CURRENT_USER, kAppRegKey);
    }

    LogInfo(L"uninstalled: no IFEO entry, proxy DLLs or autostart entries remain");
    if (restartAfter) {
        std::wstring startError;
        if (!StartSteam(steamDir, startError)) {
            LogError(startError);
            return 1;
        }
    }
    return 0;
}

int CmdInject(const Options& options) {
    std::wstring steamDir;
    if (!RequireSteam(options, steamDir)) return 1;
    const std::wstring payload = ResolvePayload(options, steamDir);
    if (!FileExists(payload)) {
        LogError(L"payload DLL not found: " + payload);
        return 1;
    }
    if (options.dryRun) {
        LogInfo(L"dry run: would inject " + payload + L" into Steam");
        return 0;
    }

    PublishRuntimePaths(&options, steamDir, payload);

    if (options.restart && !FindSteamProcesses(false).empty()) {
        std::wstring stopError;
        if (!StopSteam(steamDir, options.force, stopError)) {
            LogError(stopError);
            return 1;
        }
    }

    const InjectOutcome outcome = LaunchSteamAndInject(steamDir, payload);
    if (outcome.ok()) {
        LogInfo(L"payload loaded into steam.exe (pid " + std::to_wstring(outcome.pid) + L")");
        return 0;
    }
    LogError(L"injection failed: " + InjectResultName(outcome.result) +
             (outcome.detail.empty() ? L"" : L" (" + outcome.detail + L")"));
    return 1;
}

int CmdWatch(const Options& options) {
    std::wstring steamDir;
    if (!RequireSteam(options, steamDir)) return 1;
    const std::wstring payload = ResolvePayload(options, steamDir);
    if (!FileExists(payload)) {
        LogError(L"payload DLL not found: " + payload);
        return 1;
    }
    PublishRuntimePaths(&options, steamDir, payload);
    return RunWatcher(steamDir, payload);
}

int CmdRestart(const Options& options) {
    std::wstring steamDir;
    if (!RequireSteam(options, steamDir)) return 1;

    const bool wasRunning = !FindSteamProcesses(false).empty();
    if (!wasRunning) {
        LogInfo(L"Steam is not running, starting it");
        std::wstring startError;
        if (!StartSteam(steamDir, startError)) {
            LogError(startError);
            return 1;
        }
        return 0;
    }
    if (options.dryRun) {
        LogInfo(L"dry run: would close Steam (-shutdown) and start it again");
        return 0;
    }

    std::wstring error;
    if (!StopSteam(steamDir, options.force, error)) {
        LogError(error);
        return 1;
    }
    if (options.noStart) {
        LogInfo(L"Steam closed and left closed (--no-start)");
        return 0;
    }
    if (!StartSteam(steamDir, error)) {
        LogError(error);
        return 1;
    }
    return 0;
}

int CmdAutostart(const Options& options) {
    if (options.positional.empty()) {
        std::wstring command;
        const bool enabled = IsAutostartEnabled(&command);
        LogInfo(enabled ? (L"autostart: enabled (" + command + L")") : std::wstring(L"autostart: disabled"));
        return 0;
    }
    const std::wstring mode = ToLower(options.positional.front());
    if (mode != L"on" && mode != L"off") {
        LogError(L"autostart expects 'on' or 'off'");
        return 2;
    }
    if (options.dryRun) {
        LogInfo(L"dry run: would turn autostart " + mode);
        return 0;
    }
    std::wstring steamDir;
    (void)RequireSteam(options, steamDir);
    std::wstring error;
    if (!SetAutostart(mode == L"on", steamDir, error)) {
        LogError(error);
        return 1;
    }
    LogInfo(mode == L"on" ? L"autostart enabled (HKCU Run, no elevation needed)" : L"autostart disabled");
    return 0;
}

int CmdLog(const Options& options) {
    size_t tail = 60;
    LogLevel minLevel = LogLevel::Detail;
    for (size_t i = 0; i < options.positional.size(); ++i) {
        const std::wstring& arg = options.positional[i];
        if (IEquals(arg, L"--tail") && i + 1 < options.positional.size()) {
            tail = static_cast<size_t>(_wtoi(options.positional[i + 1].c_str()));
            continue;
        }
        if (IEquals(arg, L"--level") && i + 1 < options.positional.size()) {
            const std::wstring level = ToLower(options.positional[++i]);
            if (level == L"debug") minLevel = LogLevel::Detail;
            else if (level == L"info") minLevel = LogLevel::Info;
            else if (level == L"warn" || level == L"warning") minLevel = LogLevel::Warn;
            else if (level == L"error") minLevel = LogLevel::Error;
            else {
                LogWarn(L"unknown level '" + level + L"': use debug | info | warn | error");
                return 2;
            }
        }
    }
    const std::wstring path = LogFilePath().empty() ? SelfDir() + L"\\ost.log" : LogFilePath();
    if (!FileExists(path)) {
        LogInfo(L"no log file at " + path);
        LogDumpHistory(tail);
        return 0;
    }

    std::ifstream file(path, std::ios::binary);
    std::vector<std::wstring> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(WideFromUtf8(line));
    }
    const size_t start = lines.size() > tail ? lines.size() - tail : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        if (minLevel == LogLevel::Detail) {
            EmitRaw(lines[i]);
            continue;
        }
        // Lines from an older log format cannot be classified: keep them, they
        // still belong to the tail the user asked for.
        LogLineParts parts;
        if (!LogParseLine(lines[i], parts) || static_cast<int>(parts.level) >= static_cast<int>(minLevel)) {
            EmitRaw(lines[i]);
        }
    }
    return 0;
}

} // namespace

std::wstring ResolveDataRoot(const std::wstring& dataDirOverride, const std::wstring& steamDir) {
    if (!dataDirOverride.empty()) return dataDirOverride;
    const std::wstring self = SelfDir();
    if (self.empty()) return steamDir;
    if (steamDir.empty() || IEquals(self, steamDir)) return steamDir;
    return self;
}

Options ParseOptions(const std::vector<std::wstring>& args, bool& outHelp) {    Options options;
    options.rawArgs = args;
    outHelp = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (IEquals(arg, L"--help") || IEquals(arg, L"-h") || IEquals(arg, L"/?")) {
            outHelp = true;
            continue;
        }
        if (IEquals(arg, L"--steam-dir") && i + 1 < args.size()) { options.steamDir = args[++i]; continue; }
        if (IEquals(arg, L"--payload") && i + 1 < args.size()) { options.payloadPath = args[++i]; continue; }
        if (IEquals(arg, L"--data-dir") && i + 1 < args.size()) { options.dataDir = args[++i]; continue; }
        if (IEquals(arg, L"--log") && i + 1 < args.size()) { options.logPath = args[++i]; continue; }
        if (IEquals(arg, L"--no-log-file")) { options.noLogFile = true; continue; }
        if (IEquals(arg, L"--quiet") || IEquals(arg, L"/quiet") || IEquals(arg, L"/silent")) { options.quiet = true; continue; }
        if (IEquals(arg, L"--verbose")) { options.verbose = true; continue; }
        if (IEquals(arg, L"--json")) { options.json = true; continue; }
        if (IEquals(arg, L"--dry-run")) { options.dryRun = true; continue; }
        if (IEquals(arg, L"--pause")) { options.pause = true; continue; }
        if (IEquals(arg, L"--elevated")) { options.elevated = true; continue; }
        if (IEquals(arg, L"--restart") || IEquals(arg, L"/restart")) { options.restart = true; continue; }
        if (IEquals(arg, L"--force") || IEquals(arg, L"/force")) { options.force = true; continue; }
        if (IEquals(arg, L"--no-start") || IEquals(arg, L"/no-start")) { options.noStart = true; continue; }
        if (IEquals(arg, L"--ui") || IEquals(arg, L"/ui")) { options.ui = true; continue; }
        if (IEquals(arg, L"--tui") || IEquals(arg, L"/tui")) { options.tui = true; continue; }

        // The switches of the original ost-manager (fyisdy/Traditional_Hook) are
        // kept verbatim so existing shortcuts keep working. They all map onto
        // the explicit commands above.
        if (IEquals(arg, L"/install-portable")) { options.command = L"install"; options.positional.push_back(L"ifeo"); continue; }
        if (IEquals(arg, L"/install-root")) { options.command = L"install"; options.positional.push_back(L"ifeo-root"); continue; }
        if (IEquals(arg, L"/deploy-dll")) { options.command = L"install"; options.positional.push_back(L"dwmapi"); continue; }
        if (IEquals(arg, L"/remove-dll")) { options.command = L"uninstall-proxies"; continue; }
        if (IEquals(arg, L"/enable-autostart")) { options.command = L"autostart"; options.positional.push_back(L"on"); continue; }
        if (IEquals(arg, L"/disable-autostart")) { options.command = L"autostart"; options.positional.push_back(L"off"); continue; }

        if (arg.size() > 1 && (arg[0] == L'/' || arg[0] == L'-') && !IEquals(arg, L"-")) {
            // Unknown switch: keep it as a positional so the command can report it.
            options.positional.push_back(arg);
            continue;
        }
        if (options.command.empty()) {
            options.command = ToLower(arg);
            continue;
        }
        options.positional.push_back(arg);
    }

    // Positional layout inherited from the original tools:
    //   install <method> [steam-dir] [payload]
    // /install-root <dir> <dll>, /install-portable <dll> and /deploy-dll <dir> <dll>
    // all normalise into it. Explicit --steam-dir / --payload always win.
    if (options.command == L"install" && options.positional.size() > 1) {
        size_t index = 1;
        if (options.steamDir.empty() && DirectoryExists(options.positional[index])) {
            options.steamDir = options.positional[index++];
        }
        if (options.payloadPath.empty() && index < options.positional.size()) {
            options.payloadPath = options.positional[index];
        }
    }

    if (options.command.empty() && !options.positional.empty()) {
        std::wstring command = options.positional.front();
        if (!command.empty() && (command.front() == L'/' || command.front() == L'-')) command.erase(0, 1);
        options.command = ToLower(command);
        options.positional.erase(options.positional.begin());
    }
    return options;
}

int RunCommand(const Options& options) {
    const std::wstring& command = options.command;
    if (command == L"status" || command.empty()) return CmdStatus(options);
    if (command == L"install") return CmdInstall(options);
    if (command == L"uninstall" || command == L"remove") return CmdUninstall(options);
    if (command == L"uninstall-proxies" || command == L"remove-dll") return CmdUninstallProxies(options);
    if (command == L"inject" || command == L"launch") return CmdInject(options);
    if (command == L"watch" || command == L"daemon") return CmdWatch(options);
    if (command == L"stop-watch" || command == L"stop") {
        if (StopWatcher()) {
            LogInfo(L"stop signal sent to the watcher");
            return 0;
        }
        LogError(L"no watcher is running");
        return 1;
    }
    if (command == L"restart") return CmdRestart(options);
    if (command == L"autostart") return CmdAutostart(options);
    if (command == L"log" || command == L"logs") return CmdLog(options);
    if (command == L"version" || command == L"--version") {
        PrintVersion();
        return 0;
    }
    if (command == L"help") {
        PrintHelp();
        return 0;
    }

    LogError(L"unknown command '" + command + L"'");
    PrintHelp();
    return 2;
}

void PrintVersion() {
    ConsoleWrite(std::wstring(L"OpenSteamTool manager ") + ToolVersion() + L"\n");
}

void PrintHelp() {
    const std::wstring text =
        L"OpenSteamTool manager " + ToolVersion() + L"\n"
        L"\n"
        L"Usage: ost-manager <command> [options]\n"
        L"\n"
        L"Exactly one entry point can be active. Every install removes the others\n"
        L"first, so a proxy and the IFEO loader never load two payload copies.\n"
        L"\n"
        L"Commands\n"
        L"  status                    Steam, IFEO, proxy, payload and watcher state\n"
        L"  install <method>          Install one entry point (removes all others):\n"
        L"                              dwmapi     bin\\dwmapi.dll proxy (default, anti-cheat friendly)\n"
        L"                              version    version.dll proxy (early isolation, blocked by some AC)\n"
        L"                              xinput     xinput1_4.dll proxy (extra entry point)\n"
        L"                              ifeo       IFEO loader, nothing copied into Steam\n"
        L"                              ifeo-root  IFEO loader with ost-loader.exe deployed into Steam\n"
        L"  uninstall                 Remove IFEO, proxy DLLs, autostart and runtime artifacts\n"
        L"  uninstall-proxies         Remove only the proxy entry points\n"
        L"  inject                    Inject into a running Steam or start Steam with injection\n"
        L"  watch                     Background loop: inject whenever Steam starts\n"
        L"  stop-watch                Signal a running watcher to exit\n"
        L"  restart                   Close Steam (-shutdown) and start it again\n"
        L"  autostart on|off          Register the watcher at user logon\n"
        L"  log [--tail N] [--level warn|error]\n"
        L"                            Print the tail of the action log, optionally only\n"
        L"                            warnings and errors\n"
        L"  version                   Print the manager version\n"
        L"\n"
        L"Compatibility switches (original ost-manager)\n"
        L"  /install-portable <dll>   Same as: install ifeo --payload <dll>\n"
        L"  /install-root <dir> <dll> Same as: install ifeo-root --steam-dir <dir> --payload <dll>\n"
        L"  /deploy-dll <dir> [dll]   Same as: install dwmapi --steam-dir <dir>\n"
        L"  /remove-dll <dir>         Same as: uninstall-proxies --steam-dir <dir>\n"
        L"  /inject  /watch  /uninstall  /status\n"
        L"  /enable-autostart  /disable-autostart  /silent\n"
        L"\n"
        L"Options\n"
        L"  --steam-dir <path>        Override Steam detection\n"
        L"  --payload <path>          Payload DLL (default: OpenSteamTool.dll next to ost-manager.exe)\n"
        L"  --data-dir <path>         Folder that acts as the Steam folder for config\\lua, logs and\n"
        L"                            caches (default: the folder holding ost-manager.exe)\n"
        L"  --log <path>              Log file (default: ost.log next to ost-manager.exe)\n"
        L"  --no-log-file             Write nothing to disk\n"
        L"  --quiet                   Suppress log echo on the console\n"
        L"  --verbose                 Include debug lines\n"
        L"  --json                    Machine-readable status output\n"
        L"  --restart                 Restart Steam around the command when it is running\n"
        L"  --force                   With --restart: terminate Steam if -shutdown hangs\n"
        L"  --no-start                With restart: close Steam but do not start it again\n"
        L"  --dry-run                 Show what an install or uninstall would do\n"
        L"  --ui                      Open the windowed manager\n"
        L"  --tui                     Open the console menu instead of the window\n"
        L"  --pause                   Wait for Enter before exiting\n";
    ConsoleWrite(text);
}

} // namespace ost
