#include "Ui.h"

#include "Common.h"
#include "Inject.h"
#include "Log.h"

namespace ost {
namespace {

void PrintMenu() {
    ConsoleWrite(
        L"\n"
        L"  1) Install dwmapi proxy        2) Install version proxy\n"
        L"  3) Install xinput proxy        4) Install IFEO (portable)\n"
        L"  5) Install IFEO (Steam root)   6) Inject into Steam now\n"
        L"  7) Start watcher               8) Stop watcher\n"
        L"  9) Toggle autostart            U) Uninstall everything\n"
        L"  S) Show status                 L) Show recent log lines\n"
        L"  Q) Quit\n");
}

Options MakeCommand(const Options& base, const std::wstring& command,
                    const std::vector<std::wstring>& positional = {}) {
    Options options = base;
    options.command = command;
    options.positional = positional;
    options.rawArgs = {SelfExePath(), command};
    for (const auto& arg : positional) options.rawArgs.push_back(arg);
    if (!base.steamDir.empty()) {
        options.rawArgs.push_back(L"--steam-dir");
        options.rawArgs.push_back(base.steamDir);
    }
    return options;
}

int StartWatcherDetached(const Options& base) {
    if (IsWatcherRunning()) {
        LogInfo(L"the watcher is already running");
        return 0;
    }
    std::wstring commandLine = Quote(SelfExePath()) + L" watch --quiet";
    if (!base.steamDir.empty()) commandLine += L" --steam-dir " + Quote(base.steamDir);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(SelfExePath().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, SelfDir().c_str(), &startup, &info)) {
        LogError(L"cannot start the watcher (" + std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    LogInfo(L"watcher started in the background");
    return 0;
}

} // namespace

int RunInteractiveMenu(const Options& baseOptions) {
    PrintVersion();
    (void)RunCommand(MakeCommand(baseOptions, L"status"));
    PrintMenu();

    for (;;) {
        const std::wstring choice = ToLower(ConsoleReadLine(L"\nChoice: "));
        if (choice == L"q") break;
        if (choice.empty()) {
            PrintMenu();
            continue;
        }

        int code = 0;
        if (choice == L"1") code = RunCommand(MakeCommand(baseOptions, L"install", {L"dwmapi"}));
        else if (choice == L"2") code = RunCommand(MakeCommand(baseOptions, L"install", {L"version"}));
        else if (choice == L"3") code = RunCommand(MakeCommand(baseOptions, L"install", {L"xinput"}));
        else if (choice == L"4") code = RunCommand(MakeCommand(baseOptions, L"install", {L"ifeo"}));
        else if (choice == L"5") code = RunCommand(MakeCommand(baseOptions, L"install", {L"ifeo-root"}));
        else if (choice == L"6") code = RunCommand(MakeCommand(baseOptions, L"inject"));
        else if (choice == L"7") code = StartWatcherDetached(baseOptions);
        else if (choice == L"8") {
            code = StopWatcher() ? (LogInfo(L"stop signal sent to the watcher"), 0) : (LogError(L"no watcher is running"), 1);
        } else if (choice == L"9") {
            std::wstring command;
            const bool enabled = IsAutostartEnabled(&command);
            code = RunCommand(MakeCommand(baseOptions, L"autostart", {enabled ? L"off" : L"on"}));
        } else if (choice == L"u") {
            code = RunCommand(MakeCommand(baseOptions, L"uninstall"));
        } else if (choice == L"s") {
            code = RunCommand(MakeCommand(baseOptions, L"status"));
        } else if (choice == L"l") {
            LogDumpHistory(40);
            continue;
        } else {
            LogWarn(L"unknown choice '" + choice + L"'");
            PrintMenu();
            continue;
        }

        if (code != 0) LogWarn(L"the action finished with code " + std::to_wstring(code));
        (void)RunCommand(MakeCommand(baseOptions, L"status"));
        PrintMenu();
    }
    return 0;
}

} // namespace ost
