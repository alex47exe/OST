#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ost {

enum class LogLevel { Detail, Info, Warn, Error };

// Console plumbing. The executable is built as a GUI subsystem binary so that
// the IFEO loader role never flashes a window; the manager role attaches to
// the parent console (CLI), allocates one (--tui), or opens its window (GUI).
enum class ConsoleKind { None, Parent, Redirected };

// Classifies the inherited handles without allocating anything. When a parent
// console exists it is attached as a side effect (that is what the CLI wants).
ConsoleKind ConsoleDetect();
bool ConsoleAttach();
bool ConsoleOwned();
bool ConsoleInteractive();
void ConsoleWrite(const std::wstring& text, LogLevel level = LogLevel::Info);
std::wstring ConsoleReadLine(const std::wstring& prompt);
void ConsolePause();

// ---------------------------------------------------------------------------
// Action log
//
// Every line has the same shape, so the console, the file and the window can
// style and parse it identically:
//
//   03:58:12.123  WARN   manager  could not remove ...
//   ^ timestamp   ^level ^role    ^message
//
// The loader and the manager share one log file, so the role column is what
// keeps a tail readable when both are writing (Steam starts while a command
// runs, for example). Columns are fixed: 12 + 2 + 5 + 2 + 7 + 2 = 30
// characters before the message.
// ---------------------------------------------------------------------------
constexpr size_t kLogTimestampWidth = 12; // hh:mm:ss.mmm
constexpr size_t kLogLevelWidth = 5;      // DEBUG / INFO / WARN / ERROR
constexpr size_t kLogRoleWidth = 7;       // manager / loader
constexpr size_t kLogMessageColumn =
    kLogTimestampWidth + 2 + kLogLevelWidth + 2 + kLogRoleWidth + 2;

struct LogLineParts {
    LogLevel level = LogLevel::Info;
    size_t timestampEnd = 0;
    size_t levelStart = 0;
    size_t levelEnd = 0;
    size_t roleStart = 0;
    size_t roleEnd = 0;
    size_t messageStart = 0;
};

// Splits a line produced by this logger. Returns false for anything else (a
// blank line, a stack trace pasted into the file, an older log format).
bool LogParseLine(const std::wstring& line, LogLineParts& out);
std::wstring LogLevelName(LogLevel level);

void LogInitialize(const std::wstring& filePath, bool echo, bool verbose, bool fileEnabled);
void LogShutdown();
void LogSetEcho(bool echo);
// "manager" or "loader"; shown in the role column.
void LogSetRole(const wchar_t* role);
void LogLine(LogLevel level, const std::wstring& message);
void LogInfo(const std::wstring& message);
void LogWarn(const std::wstring& message);
void LogError(const std::wstring& message);
// Detail lines are only written with --verbose and are indented to sit under
// the step they belong to.
void LogDetail(const std::wstring& message);
// A ruled line: blank line, then "---- title ----".
void LogSection(const std::wstring& title);
// Header (version, pid, elevation, Windows build) and command line.
void LogSessionStart(const std::wstring& version, const std::vector<std::wstring>& args);
void LogSessionEnd(int exitCode);
const std::vector<std::wstring>& LogHistory();
std::wstring LogFilePath();
void LogDumpHistory(size_t tail);
void LogSeparator();

// Aligned key/value report block used by `status` and by every action result.
void PrintFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields);

} // namespace ost
