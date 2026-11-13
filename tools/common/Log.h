#pragma once

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

// Action log: every operation the tool performs goes through here, so the
// console output and the log file always show the same lines.
void LogInitialize(const std::wstring& filePath, bool echo, bool verbose, bool fileEnabled);
void LogShutdown();
void LogSetEcho(bool echo);
void LogLine(LogLevel level, const std::wstring& message);
void LogInfo(const std::wstring& message);
void LogWarn(const std::wstring& message);
void LogError(const std::wstring& message);
void LogDetail(const std::wstring& message);
const std::vector<std::wstring>& LogHistory();
std::wstring LogFilePath();
void LogDumpHistory(size_t tail);
void LogSeparator();

// Aligned key/value report block used by `status` and by every action result.
void PrintFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields);

} // namespace ost
