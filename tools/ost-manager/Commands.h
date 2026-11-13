#pragma once

#include <string>
#include <vector>

namespace ost {

#ifndef OST_TOOL_VERSION
#define OST_TOOL_VERSION "dev"
#endif

constexpr const wchar_t* kToolName = L"ost";

// The version macro is a narrow literal supplied by CMake.
inline std::wstring ToolVersion() {
    return std::wstring(OST_TOOL_VERSION, OST_TOOL_VERSION + sizeof(OST_TOOL_VERSION) - 1);
}

struct Options {
    std::wstring command;
    std::vector<std::wstring> positional;
    std::vector<std::wstring> rawArgs;
    std::wstring steamDir;
    std::wstring payloadPath;
    std::wstring logPath;
    bool noLogFile = false;
    bool quiet = false;
    bool verbose = false;
    bool json = false;
    bool dryRun = false;
    bool pause = false;
    bool elevated = false;
    bool ui = false;
    bool tui = false;
    bool restart = false;
    bool force = false;
    bool noStart = false;
};

Options ParseOptions(const std::vector<std::wstring>& args, bool& outHelp);
int RunCommand(const Options& options);
void PrintHelp();
void PrintVersion();

} // namespace ost
