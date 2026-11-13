#pragma once

#include <string>
#include <vector>

namespace ost {

// The loader is the executable Windows starts in place of steam.exe through the
// IFEO Debugger value, so every invocation of ost-loader.exe is a loader
// invocation. Both spellings are accepted because two callers exist:
//   ost-loader.exe "C:\...\steam.exe" [steam arguments]   (IFEO, upstream layout)
//   ost.exe --loader "C:\...\steam.exe" [steam arguments] (single-binary layout)
bool IsLoaderInvocation(const std::vector<std::wstring>& args);

// Runs the loader role. Never allocates a console and never shows UI: it is
// started by Windows in place of steam.exe through the IFEO Debugger value.
int RunLoader(const std::vector<std::wstring>& args);

} // namespace ost
