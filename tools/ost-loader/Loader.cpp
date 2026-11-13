#include "Loader.h"

#include "Common.h"
#include "Log.h"

#include <filesystem>
#include <thread>

namespace ost {
namespace {

constexpr const wchar_t* kLoaderArg = L"loader";

bool LooksLikeExe(const std::wstring& value) {
    return IEndsWith(value, L".exe") && FileExists(value);
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ResolvePayload(const std::wstring& steamDir, const std::wstring& explicitPath) {
    if (!explicitPath.empty()) return explicitPath;

    const std::wstring nextToExe = SelfDir() + L"\\" + kPayloadFileName;
    if (FileExists(nextToExe)) return nextToExe;

    const std::wstring inSteam = steamDir + L"\\" + kPayloadFileName;
    if (FileExists(inSteam)) return inSteam;

    std::wstring registryValue;
    if (RegReadString(HKEY_LOCAL_MACHINE, kIfeoSteamKey, kIfeoValuePayload, registryValue) &&
        FileExists(registryValue)) {
        return registryValue;
    }
    if (RegReadString(HKEY_CURRENT_USER, kAppRegKey, L"PayloadPath", registryValue) &&
        FileExists(registryValue)) {
        return registryValue;
    }
    return L"";
}

bool InjectWithHandle(HANDLE process, const std::wstring& payloadPath, std::wstring& outError) {
    const std::wstring nativePath = payloadPath;
    const SIZE_T byteSize = (nativePath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        outError = L"VirtualAllocEx failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, nativePath.c_str(), byteSize, &written) || written != byteSize) {
        outError = L"WriteProcessMemory failed (" + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
                                : nullptr;
    if (!loadLibrary) {
        outError = L"LoadLibraryW not resolvable";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        outError = L"CreateRemoteThread failed (" + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    if (wait != WAIT_OBJECT_0) {
        outError = L"remote LoadLibrary timed out";
        return false;
    }
    if (exitCode == 0) {
        outError = L"remote LoadLibrary returned 0 (payload blocked or missing dependency)";
        return false;
    }
    return true;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& forwarded) {
    std::wstring commandLine = Quote(exe);
    for (const auto& arg : forwarded) {
        commandLine += L' ';
        commandLine += (arg.find(L' ') == std::wstring::npos && arg.find(L'\t') == std::wstring::npos)
                           ? arg
                           : Quote(arg);
    }
    return commandLine;
}

} // namespace

bool IsLoaderInvocation(const std::vector<std::wstring>& args) {
    if (args.size() < 2) return false;
    if (IEquals(args[1], kLoaderSwitch) || IEquals(args[1], kLoaderArg)) return true;
    // ost-loader.exe is only ever started by Windows in place of steam.exe, so a
    // plain "steam.exe <arguments>" command line is a loader invocation too.
    return LooksLikeExe(args[1]);
}

int RunLoader(const std::vector<std::wstring>& args) {
    // The loader runs without a console; the log file is the only diagnostic.
    std::wstring logPath;
    bool noInject = false;
    std::wstring explicitPayload;
    std::vector<std::wstring> forwarded;

    // argv[0] is the loader; the optional switch is skipped so both the IFEO
    // layout ("ost-loader.exe steam.exe ...") and the single-binary layout
    // ("ost.exe --loader steam.exe ...") parse identically.
    size_t index = 1;
    if (index < args.size() && (IEquals(args[index], kLoaderSwitch) || IEquals(args[index], kLoaderArg))) ++index;
    std::wstring targetExe;
    for (; index < args.size(); ++index) {
        const std::wstring& arg = args[index];
        if (IEquals(arg, L"--no-inject")) {
            noInject = true;
            continue;
        }
        if (IEquals(arg, L"--payload") && index + 1 < args.size()) {
            explicitPayload = args[++index];
            continue;
        }
        if (IEquals(arg, L"--log") && index + 1 < args.size()) {
            logPath = args[++index];
            continue;
        }
        if (targetExe.empty() && LooksLikeExe(arg)) {
            targetExe = arg;
            continue;
        }
        forwarded.push_back(arg);
    }

    LogInitialize(logPath, false, false, true);
    LogInfo(L"loader: starting (target '" + targetExe + L"')");

    if (GetEnvVar(kIfeoChildEnv) == L"1") {
        // Windows is re-entering us because IFEO still applies to the child; a
        // second launch would fork-bomb steam.exe, so stop here.
        LogError(L"loader: recursion detected, refusing to relaunch Steam (run 'ost-manager uninstall')");
        return 4;
    }

    if (targetExe.empty() || !IEquals(BaseName(targetExe), L"steam.exe")) {
        LogError(L"loader: target is not steam.exe, refusing");
        return 2;
    }

    const std::wstring steamDir = std::filesystem::path(targetExe).parent_path().wstring();
    if (steamDir.empty()) {
        LogError(L"loader: cannot resolve the Steam directory");
        return 2;
    }

    const std::wstring payload = ResolvePayload(steamDir, explicitPayload);
    if (payload.empty()) {
        LogWarn(L"loader: no payload DLL found; Steam will start without OpenSteamTool");
    } else {
        LogInfo(L"loader: payload " + payload);
    }

    // The child inherits our environment block: the recursion guard keeps a
    // re-entered loader from forking steam.exe, and OST_STEAM_DIR lets a manager
    // started from that process find the installation it was launched for.
    SetEnvVar(kIfeoChildEnv, L"1");
    SetEnvVar(kSteamDirEnv, steamDir);

    std::wstring commandLine = BuildCommandLine(targetExe, forwarded);
    LogInfo(L"loader: command line " + commandLine);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const DWORD flags = DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED;
    if (!CreateProcessW(targetExe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        steamDir.c_str(), &startup, &info)) {
        const DWORD error = GetLastError();
        LogError(L"loader: CreateProcess (debug flag) failed (" + std::to_wstring(error) +
                 L"); Steam was not started. Run 'ost-manager uninstall' if IFEO cannot be used on this system.");
        return 3;
    }

    // Detach immediately: Steam must run as an ordinary, independent process.
    DebugSetProcessKillOnExit(FALSE);
    DebugActiveProcessStop(info.dwProcessId);

    if (!noInject && !payload.empty()) {
        std::wstring injectionError;
        if (InjectWithHandle(info.hProcess, payload, injectionError)) {
            LogInfo(L"loader: payload injected before the entry point");
        } else {
            LogError(L"loader: injection failed: " + injectionError);
        }
    }

    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    LogInfo(L"loader: Steam resumed");
    return 0;
}

} // namespace ost
