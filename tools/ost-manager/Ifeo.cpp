#include "Ifeo.h"

#include "Common.h"
#include "Log.h"

#include <vector>

namespace ost {
namespace {

bool SplitDebugger(const std::wstring& command, std::wstring& outExe, std::wstring& outTail) {
    std::wstring value = Trim(command);
    if (value.empty()) return false;
    if (value.front() == L'"') {
        const size_t closing = value.find(L'"', 1);
        if (closing == std::wstring::npos) return false;
        outExe = value.substr(1, closing - 1);
        outTail = Trim(value.substr(closing + 1));
    } else {
        const size_t space = value.find(L' ');
        outExe = value.substr(0, space);
        outTail = space == std::wstring::npos ? std::wstring() : Trim(value.substr(space + 1));
    }
    return !outExe.empty();
}

bool IsOurDebugger(const std::wstring& command) {
    std::wstring exe;
    std::wstring tail;
    if (!SplitDebugger(command, exe, tail)) return false;

    // Two layouts are ours: ost-loader.exe with no argument (IFEO appends
    // Steam's own command line) and the older single-binary form that needs
    // the --loader switch.
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    const bool loader = IEquals(name, L"ost-loader.exe") || IEquals(name, L"ost-manager.exe") ||
                        IEquals(name, L"ost.exe");
    if (!loader) return false;
    if (!tail.empty() && !IEquals(tail, kLoaderSwitch)) return false;

    // Our install always writes the mode marker in the same operation, so a
    // matching command line without it belongs to somebody else.
    std::wstring mode;
    return RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueMode, mode) && !mode.empty();
}

std::wstring BuildDebuggerCommand(const std::wstring& loaderExe) {
    // Windows appends the original command line, so the loader needs no switch.
    return Quote(loaderExe);
}

} // namespace

std::wstring IfeoStateName(IfeoState state) {
    switch (state) {
        case IfeoState::InstalledSelf:  return L"installed";
        case IfeoState::InstalledOther: return L"owned by another tool";
        case IfeoState::Orphaned:       return L"orphaned";
        case IfeoState::NotInstalled:
        default:                        return L"not installed";
    }
}

IfeoStatus QueryIfeo() {
    IfeoStatus status;
    std::wstring debuggerCmd;
    const bool hasDebugger = RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger, debuggerCmd);
    status.debuggerCommand = debuggerCmd;
    RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePayload, status.payloadPath);
    RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueMode, status.mode);
    std::wstring previous;
    status.previousDebuggerSaved =
        RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePreviousDebugger, previous) && !previous.empty();

    if (hasDebugger) {
        std::wstring tail;
        SplitDebugger(debuggerCmd, status.loaderExe, tail);
    }

    if (!hasDebugger) {
        // Our leftover values without a Debugger value mean a half-finished uninstall.
        status.state = status.payloadPath.empty() ? IfeoState::NotInstalled : IfeoState::Orphaned;
        return status;
    }

    status.loaderExists = FileExists(status.loaderExe);
    status.payloadExists = FileExists(status.payloadPath);

    if (IsOurDebugger(debuggerCmd)) {
        status.state = (status.loaderExists && status.payloadExists) ? IfeoState::InstalledSelf
                                                                    : IfeoState::Orphaned;
    } else {
        status.state = IfeoState::InstalledOther;
    }
    return status;
}

bool InstallIfeo(const std::wstring& loaderExe, const std::wstring& payloadPath, bool rootMode,
                 std::wstring& outError) {
    if (!IsElevated()) {
        outError = L"administrator rights are required to write the IFEO key";
        return false;
    }
    if (!FileExists(loaderExe)) {
        outError = L"loader executable not found: " + loaderExe;
        return false;
    }
    if (!FileExists(payloadPath)) {
        outError = L"payload DLL not found: " + payloadPath;
        return false;
    }

    // Preserve a foreign Debugger value so uninstall can put it back.
    std::wstring existing;
    if (RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger, existing) && !existing.empty() &&
        !IsOurDebugger(existing)) {
        std::wstring previous;
        if (!RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePreviousDebugger, previous) ||
            previous.empty()) {
            if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePreviousDebugger, existing, outError))
                return false;
        }
        LogWarn(L"replacing an existing IFEO Debugger entry: " + existing);
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond);

    if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePayload, payloadPath, outError)) return false;
    if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueMode, rootMode ? L"root" : L"portable",
                        outError))
        return false;
    if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueInstalledAt, stamp, outError)) return false;
    if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueVersion, L"1", outError)) return false;

    // Debugger is written last: a half-written install must not intercept Steam.
    if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger, BuildDebuggerCommand(loaderExe),
                        outError))
        return false;
    return true;
}

bool UninstallIfeo(std::wstring& outError) {
    if (!IsElevated()) {
        outError = L"administrator rights are required to modify the IFEO key";
        return false;
    }

    std::wstring debuggerCmd;
    const bool hasDebugger = RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger, debuggerCmd);
    const bool ours = hasDebugger && IsOurDebugger(debuggerCmd);

    if (hasDebugger && !ours) {
        // Someone else's entry: leave the Debugger value alone, only drop our bookkeeping.
        LogWarn(L"the IFEO Debugger entry belongs to another tool; leaving it untouched");
    } else if (hasDebugger) {
        std::wstring previous;
        if (RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePreviousDebugger, previous) &&
            !previous.empty()) {
            if (!RegWriteString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger, previous, outError))
                return false;
            LogInfo(L"restored the previous IFEO Debugger entry: " + previous);
        } else if (!RegDeleteValue(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValueDebugger)) {
            outError = L"failed to delete the IFEO Debugger value";
            return false;
        }
    }

    const wchar_t* names[] = {kIfeoValuePayload, kIfeoValueMode, kIfeoValueInstalledAt, kIfeoValueVersion,
                              kIfeoValuePreviousDebugger};
    for (const wchar_t* name : names) (void)RegDeleteValue(HKEY_LOCAL_MACHINE, kIfeoSteamKey, name);

    if (!RegKeyHasValues(HKEY_LOCAL_MACHINE, kIfeoSteamKey)) {
        RegDeleteKeyTree(HKEY_LOCAL_MACHINE, kIfeoSteamKey);
    }
    return true;
}

} // namespace ost
