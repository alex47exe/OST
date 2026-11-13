#include "Common.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>

namespace ost {
namespace {

std::wstring FormatWin32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = (length && buffer) ? std::wstring(buffer, length) : L"";
    if (buffer) LocalFree(buffer);
    text = Trim(text);
    if (text.empty()) return L"error " + std::to_wstring(code);
    return text + L" (" + std::to_wstring(code) + L")";
}

std::wstring RegLastError(DWORD code) { return FormatWin32Error(code); }

} // namespace

std::wstring ToLower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    });
    return result;
}

bool IEquals(std::wstring_view lhs, std::wstring_view rhs) {
    return ToLower(lhs) == ToLower(rhs);
}

bool IEndsWith(std::wstring_view value, std::wstring_view suffix) {
    if (suffix.size() > value.size()) return false;
    return IEquals(value.substr(value.size() - suffix.size()), suffix);
}

bool IStartsWith(std::wstring_view value, std::wstring_view prefix) {
    if (prefix.size() > value.size()) return false;
    return IEquals(value.substr(0, prefix.size()), prefix);
}

std::wstring Trim(std::wstring_view value) {
    size_t first = 0;
    size_t last = value.size();
    auto isSpace = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    while (first < last && isSpace(value[first])) ++first;
    while (last > first && isSpace(value[last - 1])) --last;
    return std::wstring(value.substr(first, last - first));
}

std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    result += value;
    result += L"\"";
    return result;
}

std::wstring WideFromUtf8(std::string_view value) {
    if (value.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string Utf8FromWide(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::vector<std::wstring> SplitLines(std::wstring_view value) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find_first_of(L"\r\n", start);
        if (end == std::wstring_view::npos) {
            lines.emplace_back(value.substr(start));
            break;
        }
        lines.emplace_back(value.substr(start, end - start));
        start = end + 1;
        while (start < value.size() && (value[start] == L'\r' || value[start] == L'\n')) ++start;
    }
    return lines;
}

std::wstring SelfExePath() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return L"";
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

std::wstring SelfDir() {
    const std::wstring path = SelfExePath();
    if (path.empty()) return L"";
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring SystemDir() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"";
    return std::wstring(buffer, length);
}

std::wstring GetEnvVar(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return L"";
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0) return L"";
    value.resize(written);
    return value;
}

bool SetEnvVar(const wchar_t* name, const std::wstring& value) {
    return SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str()) != 0;
}

std::vector<std::wstring> CommandLineArgs() {
    std::vector<std::wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool RelaunchElevated(const std::vector<std::wstring>& args, std::wstring& outError, DWORD* outExitCode) {
    std::wstring parameters;
    // Skip argv[0]; every other argument is forwarded verbatim (already quoted by CommandLineToArgvW).
    for (size_t i = 1; i < args.size(); ++i) {
        if (IEquals(args[i], L"--elevated")) continue;
        const std::wstring& arg = args[i];
        if (!parameters.empty()) parameters += L' ';
        parameters += (arg.find(L' ') == std::wstring::npos) ? arg : Quote(arg);
    }

    const std::wstring exe = SelfExePath();
    if (exe.empty()) {
        outError = L"cannot resolve the executable path";
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = SelfDir().c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        outError = (error == ERROR_CANCELLED) ? L"elevation was cancelled" : RegLastError(error);
        return false;
    }
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, 120000);
        if (outExitCode) {
            DWORD code = 1;
            GetExitCodeProcess(info.hProcess, &code);
            *outExitCode = code;
        }
        CloseHandle(info.hProcess);
    } else if (outExitCode) {
        *outExitCode = 0;
    }
    return true;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirectoryExists(path)) return true;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    return !ec && DirectoryExists(path);
}

bool CopyFileInto(const std::wstring& source, const std::wstring& destination, bool overwrite,
                  std::wstring& outError) {
    if (!FileExists(source)) {
        outError = L"source missing: " + source;
        return false;
    }
    const size_t slash = destination.find_last_of(L"\\/");
    if (slash != std::wstring::npos) EnsureDirectory(destination.substr(0, slash));

    // A Steam update or an AV scanner can hold the target open; retry briefly.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (CopyFileW(source.c_str(), destination.c_str(), overwrite ? FALSE : TRUE)) return true;
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            outError = L"copy " + source + L" -> " + destination + L" failed: " + RegLastError(error);
            return false;
        }
        Sleep(200);
    }
    outError = L"copy " + source + L" -> " + destination + L" failed: file is locked";
    return false;
}

bool DeleteFileIfExists(const std::wstring& path, std::wstring& outError) {
    if (!FileExists(path)) return true;
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (DeleteFileW(path.c_str())) return true;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return true;
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            outError = L"delete " + path + L" failed: " + RegLastError(error);
            return false;
        }
        Sleep(200);
    }
    outError = L"delete " + path + L" failed: file is locked";
    return false;
}

bool MoveFileInto(const std::wstring& source, const std::wstring& destination, std::wstring& outError) {
    if (!FileExists(source)) {
        outError = L"source missing: " + source;
        return false;
    }
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return true;
    outError = L"move " + source + L" -> " + destination + L" failed: " + RegLastError(GetLastError());
    return false;
}

bool RegReadString(HKEY root, const std::wstring& subkey, const wchar_t* name, std::wstring& outValue) {
    DWORD size = 0;
    DWORD type = 0;
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY,
                     &type, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    if (size == 0) return false;
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY,
                     &type, buffer.data(), &size) != ERROR_SUCCESS) {
        return false;
    }
    buffer.resize(wcslen(buffer.c_str()));
    outValue = buffer;
    return true;
}

bool RegWriteString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                    const std::wstring& value, std::wstring& outError) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        outError = L"open " + subkey + L" failed: " + RegLastError(static_cast<DWORD>(status));
        return false;
    }
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        outError = L"write " + subkey + L"\\" + name + L" failed: " + RegLastError(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

bool RegDeleteValue(HKEY root, const std::wstring& subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool RegReadMultiString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                        std::vector<std::wstring>& outValues) {
    DWORD size = 0;
    DWORD type = 0;
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_MULTI_SZ | RRF_SUBKEY_WOW6464KEY, &type, nullptr,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    if (size == 0) return false;
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_MULTI_SZ | RRF_SUBKEY_WOW6464KEY, &type,
                     buffer.data(), &size) != ERROR_SUCCESS) {
        return false;
    }
    outValues.clear();
    for (size_t i = 0; i < buffer.size() && buffer[i] != L'\0';) {
        std::wstring item(&buffer[i]);
        outValues.push_back(item);
        i += item.size() + 1;
    }
    return true;
}

bool RegWriteMultiString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                         const std::vector<std::wstring>& values, std::wstring& outError) {
    std::wstring packed;
    for (const auto& value : values) {
        packed += value;
        packed.push_back(L'\0');
    }
    packed.push_back(L'\0');

    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        outError = L"open " + subkey + L" failed: " + RegLastError(static_cast<DWORD>(status));
        return false;
    }
    status = RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(packed.c_str()),
                            static_cast<DWORD>(packed.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        outError = L"write " + subkey + L"\\" + name + L" failed: " + RegLastError(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

bool RegDeleteKeyTree(HKEY root, const std::wstring& subkey) {
    LSTATUS status = RegDeleteKeyExW(root, subkey.c_str(), KEY_WOW64_64KEY, 0);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool RegKeyHasValues(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    DWORD subkeys = 0;
    DWORD values = 0;
    const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                            nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && (values > 0 || subkeys > 0);
}

} // namespace ost
