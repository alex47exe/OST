#pragma once

#include <string>

namespace ost {

enum class IfeoState {
    NotInstalled,   // no Debugger value, nothing of ours left behind
    InstalledSelf,  // Debugger points at this executable with --loader
    InstalledOther, // Debugger belongs to another tool (or a manual entry)
    Orphaned,       // our values exist but the loader executable or payload is gone
};

struct IfeoStatus {
    IfeoState state = IfeoState::NotInstalled;
    std::wstring debuggerCommand;
    std::wstring loaderExe;
    std::wstring payloadPath;
    std::wstring mode; // "portable" or "root"
    bool loaderExists = false;
    bool payloadExists = false;
    bool previousDebuggerSaved = false;
};

IfeoStatus QueryIfeo();
bool InstallIfeo(const std::wstring& loaderExe, const std::wstring& payloadPath, bool rootMode,
                 std::wstring& outError);
bool UninstallIfeo(std::wstring& outError);

// "portable" / "root" helpers, shared with status rendering.
std::wstring IfeoStateName(IfeoState state);

} // namespace ost
