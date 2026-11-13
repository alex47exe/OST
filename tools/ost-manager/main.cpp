#include "Commands.h"
#include "Common.h"
#include "Gui.h"
#include "Log.h"
#include "Ui.h"

#include <windows.h>

// ost-manager: the CLI, the console menu and the windowed manager.
//   ost-manager.exe <command> ...   CLI, attaches to the parent console
//   ost-manager.exe                 double-click: windowed manager
//   ost-manager.exe --ui            windowed manager
//   ost-manager.exe --tui           console menu in its own window
// The IFEO role belongs to ost-loader.exe (see tools/ost-loader), so a single
// install can never keep two different payload paths in one process.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::vector<std::wstring> args = ost::CommandLineArgs();

    bool help = false;
    const ost::Options options = ost::ParseOptions(args, help);

    const ost::ConsoleKind console = ost::ConsoleDetect();
    // No command and no console to talk to means the user double-clicked the
    // executable: open the window instead of hijacking a console.
    const bool windowed =
        !help && (options.ui || (options.command.empty() && !options.tui && console == ost::ConsoleKind::None));
    if (windowed) {
        // The window tails the log file; keep it enabled so elevated work is visible.
        ost::LogInitialize(options.logPath, false, true, true);
        ost::LogSetRole(L"manager");
        ost::LogSessionStart(ost::ToolVersion(), args);
        const int code = ost::RunGui(options);
        ost::LogSessionEnd(code);
        ost::LogShutdown();
        return code;
    }

    const bool echo = !options.quiet;
    if (echo) ost::ConsoleAttach();
    ost::LogInitialize(options.logPath, echo, options.verbose, !options.noLogFile);
    ost::LogSetRole(L"manager");

    if (help) {
        ost::PrintHelp();
        if (ost::ConsoleOwned()) ost::ConsolePause();
        return 0;
    }

    ost::LogSessionStart(ost::ToolVersion(), args);
    const bool interactive =
        (options.ui || options.command.empty()) && ost::ConsoleOwned() && ost::ConsoleInteractive();
    const int code = interactive ? ost::RunInteractiveMenu(options) : ost::RunCommand(options);
    ost::LogSessionEnd(code);

    if (options.pause || ost::ConsoleOwned()) ost::ConsolePause();
    ost::LogShutdown();
    return code;
}
