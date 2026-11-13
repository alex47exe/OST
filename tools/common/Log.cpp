#include "Log.h"

#include "Common.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

namespace ost {
namespace {

HANDLE g_consoleOut = INVALID_HANDLE_VALUE;
HANDLE g_consoleIn = INVALID_HANDLE_VALUE;
bool g_ownerOfConsole = false;
bool g_consoleAttached = false;
bool g_vtEnabled = false;

std::mutex g_logMutex;
std::wstring g_logPath;
bool g_echo = true;
bool g_verbose = false;
bool g_fileEnabled = true;
std::wstring g_role = L"manager";
std::deque<std::wstring> g_history;
bool g_sessionOpen = false;
ULONGLONG g_sessionStart = 0;

constexpr size_t kHistoryLimit = 512;
// Keep ost.log small enough to attach to a bug report; one rotation is kept.
constexpr unsigned long long kMaxLogBytes = 1024ull * 1024ull;

const wchar_t* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Detail: return L"DEBUG";
        case LogLevel::Warn:   return L"WARN ";
        case LogLevel::Error:  return L"ERROR";
        case LogLevel::Info:
        default:               return L"INFO ";
    }
}

// ANSI colours, only used when the console accepted virtual terminal mode.
const char* AnsiCode(LogLevel level) {
    switch (level) {
        case LogLevel::Detail: return "\x1b[90m"; // bright black
        case LogLevel::Warn:   return "\x1b[33m"; // yellow
        case LogLevel::Error:  return "\x1b[91m"; // bright red
        case LogLevel::Info:
        default:               return "";
    }
}

const char* kAnsiDim = "\x1b[90m";
const char* kAnsiReset = "\x1b[0m";

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%02u:%02u:%02u.%03u", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

std::wstring Date() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
    return buffer;
}

std::wstring PadTo(const std::wstring& value, size_t width) {
    if (value.size() >= width) return value.substr(0, width);
    return value + std::wstring(width - value.size(), L' ');
}

std::wstring RoleTag() { return PadTo(g_role, kLogRoleWidth); }

std::wstring BuildLine(LogLevel level, const std::wstring& message) {
    return PadTo(Timestamp(), kLogTimestampWidth) + L"  " + LevelTag(level) + L"  " + RoleTag() + L"  " +
           message;
}

std::wstring CommandLineText(const std::vector<std::wstring>& args) {
    std::wstring text;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) text += L' ';
        const std::wstring& arg = args[i];
        const bool needsQuotes = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos;
        text += needsQuotes ? Quote(arg) : arg;
    }
    return text;
}

std::wstring Rule(const std::wstring& title) {
    // Symmetric tails keep the header compact enough to stay on one line next
    // to the 30-character prefix in the window.
    const std::wstring core = L" " + title + L" ";
    return L"----" + core + L"----";
}

void WriteFileUtf8(HANDLE handle, const std::string& text) {
    DWORD written = 0;
    WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void AppendToFile(const std::wstring& line) {
    if (!g_fileEnabled || g_logPath.empty()) return;
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string utf8 = Utf8FromWide(line);
    utf8 += "\r\n";
    WriteFileUtf8(file, utf8);
    CloseHandle(file);
}

// ost.log grows by one line per event; keep the last megabyte and one backup.
void RotateIfNeeded(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) |
                                    info.nFileSizeLow;
    if (size < kMaxLogBytes) return;
    const std::wstring backup = path + L".1";
    MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void WriteConsoleText(const std::wstring& text) {
    HANDLE handle = g_consoleAttached ? g_consoleOut : GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        WriteFileUtf8(handle, Utf8FromWide(text));
    }
}

// Writes one log line to the console, colouring the columns when the console
// understands ANSI sequences (Windows 10 and later terminals).
void EchoLine(const std::wstring& line) {
    LogLineParts parts;
    if (!g_vtEnabled || !LogParseLine(line, parts)) {
        WriteConsoleText(line + L"\n");
        return;
    }

    const std::wstring dim = WideFromUtf8(kAnsiDim);
    const std::wstring reset = WideFromUtf8(kAnsiReset);
    const std::wstring levelColor = WideFromUtf8(AnsiCode(parts.level));
    const std::wstring messageColor = parts.level == LogLevel::Info ? std::wstring() : levelColor;

    std::wstring out;
    out += dim;
    out += line.substr(0, parts.timestampEnd);
    out += L"  ";
    if (!levelColor.empty()) {
        out += levelColor;
        out += line.substr(parts.levelStart, parts.levelEnd - parts.levelStart);
        out += L"  ";
        out += dim;
    } else {
        out += line.substr(parts.levelStart, parts.levelEnd - parts.levelStart);
        out += L"  ";
    }
    out += line.substr(parts.roleStart, parts.roleEnd - parts.roleStart);
    out += reset;
    out += L"  ";
    const std::wstring message = parts.messageStart < line.size() ? line.substr(parts.messageStart) : L"";
    if (!messageColor.empty()) {
        out += messageColor;
        out += message;
        out += reset;
    } else {
        out += message;
    }
    out += L"\n";
    WriteConsoleText(out);
}

} // namespace

std::wstring LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Detail: return L"debug";
        case LogLevel::Warn:   return L"warn";
        case LogLevel::Error:  return L"error";
        case LogLevel::Info:
        default:               return L"info";
    }
}

bool LogParseLine(const std::wstring& line, LogLineParts& out) {
    if (line.size() < kLogMessageColumn) return false;
    // hh:mm:ss.mmm
    for (size_t i = 0; i < kLogTimestampWidth; ++i) {
        const wchar_t ch = line[i];
        const bool ok = (i == 2 || i == 5) ? ch == L':' : (i == 8 ? ch == L'.' : (ch >= L'0' && ch <= L'9'));
        if (!ok) return false;
    }
    if (line[kLogTimestampWidth] != L' ' || line[kLogTimestampWidth + 1] != L' ') return false;

    const size_t levelStart = kLogTimestampWidth + 2;
    const size_t levelEnd = levelStart + kLogLevelWidth;
    const size_t roleStart = levelEnd + 2;
    const size_t roleEnd = roleStart + kLogRoleWidth;
    if (line.size() < roleEnd) return false;

    const std::wstring level = Trim(line.substr(levelStart, kLogLevelWidth));
    out.level = LogLevel::Info;
    if (IEquals(level, L"WARN")) out.level = LogLevel::Warn;
    else if (IEquals(level, L"ERROR")) out.level = LogLevel::Error;
    else if (IEquals(level, L"DEBUG")) out.level = LogLevel::Detail;

    out.timestampEnd = kLogTimestampWidth;
    out.levelStart = levelStart;
    out.levelEnd = levelEnd;
    out.roleStart = roleStart;
    out.roleEnd = roleEnd;
    out.messageStart = (std::min)(line.size(), roleEnd + 2);
    return true;
}

bool ConsoleAttach() {
    if (g_consoleAttached) return true;

    const ConsoleKind kind = ConsoleDetect();
    if (kind == ConsoleKind::Redirected) {
        g_consoleAttached = false;
        g_ownerOfConsole = false;
        return false;
    }
    if (kind == ConsoleKind::None) {
        if (!AllocConsole()) {
            g_consoleAttached = false;
            return false;
        }
        g_ownerOfConsole = true;
        SetConsoleTitleW(L"OpenSteamTool");
    } else {
        g_ownerOfConsole = false;
    }
    g_consoleAttached = true;

    g_consoleOut = CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    g_consoleIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_consoleOut == INVALID_HANDLE_VALUE || g_consoleIn == INVALID_HANDLE_VALUE) {
        g_consoleAttached = false;
        return false;
    }

    // Make the console render wide characters correctly regardless of the shell code page.
    SetConsoleOutputCP(CP_UTF8);

    DWORD mode = 0;
    g_vtEnabled = GetConsoleMode(g_consoleOut, &mode) != 0 &&
                  SetConsoleMode(g_consoleOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    return true;
}

ConsoleKind ConsoleDetect() {
    static ConsoleKind cached = ConsoleKind::None;
    static bool resolved = false;
    if (resolved) return cached;

    if (g_consoleAttached) {
        cached = g_ownerOfConsole ? ConsoleKind::None : ConsoleKind::Parent;
        resolved = true;
        return cached;
    }

    // Respect redirection first: `ost status --json > out.json` or a pipe must
    // receive the text on the inherited handle, not on a fresh console.
    const HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD inheritedType = inherited ? GetFileType(inherited) : FILE_TYPE_UNKNOWN;
    if (inheritedType == FILE_TYPE_PIPE || inheritedType == FILE_TYPE_DISK) {
        cached = ConsoleKind::Redirected;
        resolved = true;
        return cached;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS) || inheritedType == FILE_TYPE_CHAR) {
        cached = ConsoleKind::Parent;
        resolved = true;
        return cached;
    }

    cached = ConsoleKind::None;
    resolved = true;
    return cached;
}

bool ConsoleOwned() { return g_ownerOfConsole; }

bool ConsoleInteractive() {
    if (!g_consoleAttached) return false;
    DWORD mode = 0;
    return GetConsoleMode(g_consoleIn, &mode) != 0;
}

void ConsoleWrite(const std::wstring& text, LogLevel level) {
    (void)level;
    WriteConsoleText(text);
}

std::wstring ConsoleReadLine(const std::wstring& prompt) {
    if (!prompt.empty()) ConsoleWrite(prompt);
    if (!ConsoleInteractive()) return L"";

    std::wstring line;
    wchar_t buffer[512];
    DWORD read = 0;
    if (!ReadConsoleW(g_consoleIn, buffer, static_cast<DWORD>(std::size(buffer)), &read, nullptr)) return L"";
    line.assign(buffer, read);
    return Trim(line);
}

void ConsolePause() {
    if (!g_ownerOfConsole) return;
    ConsoleWrite(L"\nPress Enter to close...");
    wchar_t buffer[8];
    DWORD read = 0;
    ReadConsoleW(g_consoleIn, buffer, static_cast<DWORD>(std::size(buffer)), &read, nullptr);
}

void LogInitialize(const std::wstring& filePath, bool echo, bool verbose, bool fileEnabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_echo = echo;
    g_verbose = verbose;
    g_fileEnabled = fileEnabled;
    g_logPath = filePath;
    if (g_logPath.empty() && fileEnabled) {
        const std::wstring local = SelfDir();
        g_logPath = local.empty() ? L"" : local + L"\\ost.log";
        if (!g_logPath.empty() && !EnsureDirectory(SelfDir())) {
            const std::wstring appData = GetEnvVar(L"LOCALAPPDATA");
            if (!appData.empty()) {
                const std::wstring dir = appData + L"\\OpenSteamTool";
                EnsureDirectory(dir);
                g_logPath = dir + L"\\ost.log";
            }
        }
    }
    if (g_fileEnabled && !g_logPath.empty()) RotateIfNeeded(g_logPath);
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_history.clear();
    g_sessionOpen = false;
}

void LogSetEcho(bool echo) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_echo = echo;
}

void LogSetRole(const wchar_t* role) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (role && role[0] != L'\0') g_role = role;
}

void LogLine(LogLevel level, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (level == LogLevel::Detail && !g_verbose) return;

    const std::wstring line = BuildLine(level, message);
    g_history.push_back(line);
    while (g_history.size() > kHistoryLimit) g_history.pop_front();

    if (g_echo) EchoLine(line);
    AppendToFile(line);
}

void LogInfo(const std::wstring& message) { LogLine(LogLevel::Info, message); }
void LogWarn(const std::wstring& message) { LogLine(LogLevel::Warn, message); }
void LogError(const std::wstring& message) { LogLine(LogLevel::Error, message); }

void LogDetail(const std::wstring& message) {
    // Indented so a detail line sits under the step it belongs to.
    LogLine(LogLevel::Detail, L"  " + message);
}

void LogSection(const std::wstring& title) { LogLine(LogLevel::Info, Rule(title)); }

void LogSessionStart(const std::wstring& version, const std::vector<std::wstring>& args) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_sessionOpen) return;
        g_sessionOpen = true;
        g_sessionStart = GetTickCount64();
    }

    std::wstring header = g_role + L" " + version;
    header += L"  pid " + std::to_wstring(GetCurrentProcessId());
    header += IsElevated() ? L"  elevated" : L"  standard user";

    // A blank line keeps sessions apart when the file already has history.
    LogSection(header);

    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto getVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (getVersion && getVersion(&os) == 0) {
            LogDetail(L"windows " + std::to_wstring(os.dwMajorVersion) + L"." +
                      std::to_wstring(os.dwMinorVersion) + L"." + std::to_wstring(os.dwBuildNumber));
        }
    }

    // Show only the executable name: the full path makes every session line wrap.
    std::vector<std::wstring> shown = args;
    if (!shown.empty()) {
        const std::wstring& exe = shown.front();
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) shown.front() = exe.substr(slash + 1);
    }
    LogInfo(Date() + L"  command: " + CommandLineText(shown));
}

void LogSessionEnd(int exitCode) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_sessionOpen) return;
    }
    const unsigned long long elapsed = GetTickCount64() - g_sessionStart;
    wchar_t duration[32];
    swprintf_s(duration, L"%.2f s", static_cast<double>(elapsed) / 1000.0);
    LogSection(std::wstring(L"exit ") + std::to_wstring(exitCode) + L" - " + duration);
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_sessionOpen = false;
}

const std::vector<std::wstring>& LogHistory() {
    static std::vector<std::wstring> snapshot;
    std::lock_guard<std::mutex> lock(g_logMutex);
    snapshot.assign(g_history.begin(), g_history.end());
    return snapshot;
}

std::wstring LogFilePath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logPath;
}

void LogDumpHistory(size_t tail) {
    auto history = LogHistory();
    if (history.empty()) {
        ConsoleWrite(L"(the action log is empty)\n");
        return;
    }
    const size_t start = history.size() > tail ? history.size() - tail : 0;
    for (size_t i = start; i < history.size(); ++i) EchoLine(history[i]);
}

void LogSeparator() { LogLine(LogLevel::Info, Rule(L"")); }

void PrintFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields) {
    size_t width = 0;
    for (const auto& field : fields) width = (std::max)(width, field.first.size());
    for (const auto& field : fields) {
        std::wstring padded = field.first;
        padded.append(width - field.first.size(), L' ');
        LogInfo(L"  " + padded + L" : " + field.second);
    }
}

} // namespace ost
