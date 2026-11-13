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

std::mutex g_logMutex;
std::wstring g_logPath;
bool g_echo = true;
bool g_verbose = false;
bool g_fileEnabled = true;
std::deque<std::wstring> g_history;

constexpr size_t kHistoryLimit = 512;

const wchar_t* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Detail: return L"DEBUG";
        case LogLevel::Warn:   return L"WARN ";
        case LogLevel::Error:  return L"ERROR";
        case LogLevel::Info:
        default:               return L"INFO ";
    }
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return buffer;
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

} // namespace

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
    HANDLE handle = g_consoleAttached ? g_consoleOut : GetStdHandle(STD_OUTPUT_HANDLE);
    (void)level;
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        WriteFileUtf8(handle, Utf8FromWide(text));
    }
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
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_history.clear();
}

void LogSetEcho(bool echo) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_echo = echo;
}

void LogLine(LogLevel level, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (level == LogLevel::Detail && !g_verbose) return;

    const std::wstring line = L"[" + Timestamp() + L"] [" + LevelTag(level) + L"] " + message;
    g_history.push_back(line);
    while (g_history.size() > kHistoryLimit) g_history.pop_front();

    if (g_echo) ConsoleWrite(line + L"\n", level);
    AppendToFile(line);
}

void LogInfo(const std::wstring& message) { LogLine(LogLevel::Info, message); }
void LogWarn(const std::wstring& message) { LogLine(LogLevel::Warn, message); }
void LogError(const std::wstring& message) { LogLine(LogLevel::Error, message); }
void LogDetail(const std::wstring& message) { LogLine(LogLevel::Detail, message); }

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
        LogInfo(L"(the action log is empty)");
        return;
    }
    const size_t start = history.size() > tail ? history.size() - tail : 0;
    for (size_t i = start; i < history.size(); ++i) LogInfo(history[i]);
}

void LogSeparator() {
    LogInfo(L"------------------------------------------------------------");
}

void PrintFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields) {
    size_t width = 0;
    for (const auto& field : fields) width = (std::max)(width, field.first.size());
    for (const auto& field : fields) {
        std::wstring padded = field.first;
        padded.append(width - field.first.size(), L' ');
        LogInfo(padded + L" : " + field.second);
    }
}

} // namespace ost
