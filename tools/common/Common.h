#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ost {

// ---------------------------------------------------------------------------
// Strings. The tool is UNICODE internally; conversions only happen when
// talking to ANSI-only APIs or when writing the log file.
// ---------------------------------------------------------------------------
std::wstring ToLower(std::wstring_view value);
bool IEquals(std::wstring_view lhs, std::wstring_view rhs);
bool IEndsWith(std::wstring_view value, std::wstring_view suffix);
bool IStartsWith(std::wstring_view value, std::wstring_view prefix);
std::wstring Trim(std::wstring_view value);
std::wstring Quote(const std::wstring& value);
std::wstring WideFromUtf8(std::string_view value);
std::string Utf8FromWide(std::wstring_view value);
std::vector<std::wstring> SplitLines(std::wstring_view value);

// ---------------------------------------------------------------------------
// Process / environment.
// ---------------------------------------------------------------------------
std::wstring SelfExePath();
std::wstring SelfDir();
std::wstring SystemDir();
std::wstring GetEnvVar(const wchar_t* name);
bool SetEnvVar(const wchar_t* name, const std::wstring& value);
std::vector<std::wstring> CommandLineArgs();
bool IsElevated();

// Relaunch this executable with the same arguments requesting administrator
// rights. Returns true when a new elevated instance was started (the caller
// must then exit); `outExitCode` receives that instance's exit code.
bool RelaunchElevated(const std::vector<std::wstring>& args, std::wstring& outError,
                      DWORD* outExitCode = nullptr);

// ---------------------------------------------------------------------------
// Files.
// ---------------------------------------------------------------------------
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
std::uint64_t FileSize(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
bool CopyFileInto(const std::wstring& source, const std::wstring& destination, bool overwrite,
                  std::wstring& outError);
bool DeleteFileIfExists(const std::wstring& path, std::wstring& outError);
bool MoveFileInto(const std::wstring& source, const std::wstring& destination, std::wstring& outError);

// ---------------------------------------------------------------------------
// Registry. All access uses the 64-bit view so a 32-bit shell cannot write to
// Wow6432Node by accident.
// ---------------------------------------------------------------------------
bool RegReadString(HKEY root, const std::wstring& subkey, const wchar_t* name, std::wstring& outValue);
bool RegWriteString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                    const std::wstring& value, std::wstring& outError);
bool RegDeleteValue(HKEY root, const std::wstring& subkey, const wchar_t* name);
bool RegReadMultiString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                        std::vector<std::wstring>& outValues);
bool RegWriteMultiString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                         const std::vector<std::wstring>& values, std::wstring& outError);
bool RegDeleteKeyTree(HKEY root, const std::wstring& subkey);
bool RegKeyHasValues(HKEY root, const std::wstring& subkey);

// ---------------------------------------------------------------------------
// Well-known locations and names.
// ---------------------------------------------------------------------------
constexpr const wchar_t* kIfeoSteamKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\steam.exe";
constexpr const wchar_t* kAppRegKey = L"Software\\OpenSteamTool";
constexpr const wchar_t* kPayloadFileName = L"OpenSteamTool.dll";
constexpr const wchar_t* kLoaderSwitch = L"--loader";
constexpr const wchar_t* kSteamDirEnv = L"OST_STEAM_DIR";
// Data root handed to the Steam the loader starts: the folder the loader lives
// in, so the payload writes config\lua, logs and the diversion copy there
// instead of into Steam. Hot injection uses HKCU ConfigDir instead.
constexpr const wchar_t* kConfigDirEnv = L"OST_CONFIG_DIR";
constexpr const wchar_t* kIfeoChildEnv = L"OST_IFEO_CHILD";

// Registry value names under kIfeoSteamKey.
constexpr const wchar_t* kIfeoValueDebugger = L"Debugger";
constexpr const wchar_t* kIfeoValuePayload = L"OSTPayloadPath";
constexpr const wchar_t* kIfeoValueMode = L"OSTInstallMode";
constexpr const wchar_t* kIfeoValueInstalledAt = L"OSTInstalledAt";
constexpr const wchar_t* kIfeoValueVersion = L"OSTVersion";
constexpr const wchar_t* kIfeoValuePreviousDebugger = L"OSTPreviousDebugger";

// Registry value names under kAppRegKey (HKCU). The proxies read
// PayloadPath as their last fallback when the payload is not next to them or
// next to steam.exe; the manager writes it on every install.
constexpr const wchar_t* kAppValuePayloadPath = L"PayloadPath";
// Data root published for hot-injected payloads (they cannot inherit the
// loader's environment, so the registry carries the path).
constexpr const wchar_t* kAppValueConfigDir = L"ConfigDir";

} // namespace ost
