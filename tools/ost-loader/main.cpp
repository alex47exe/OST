// ost-loader: the windowless helper Windows starts in place of steam.exe.
//
// It is registered by ost-manager as the IFEO Debugger value for steam.exe:
//     HKLM\...\Image File Execution Options\steam.exe -> Debugger = "<dir>\ost-loader.exe"
// Windows then appends Steam's own command line, so argv looks like
//     ost-loader.exe "C:\Program Files (x86)\Steam\steam.exe" -silent ...
// The loader starts Steam suspended with DEBUG_ONLY_THIS_PROCESS (which stops
// IFEO from being applied to the child a second time), injects the payload,
// detaches and resumes. Nothing is written to the screen: the log file is the
// only output.

#include "Loader.h"

#include "Common.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return ost::RunLoader(ost::CommandLineArgs());
}
