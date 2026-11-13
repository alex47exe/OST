#include "Gui.h"

#include "Common.h"
#include "GuiStrings.h"
#include "Ifeo.h"
#include "Inject.h"
#include "Log.h"
#include "Proxies.h"
#include "Steam.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace ost {
namespace {

enum ControlId : int {
    IdSteamLabel = 100,
    IdSteamValue,
    IdPayloadLabel,
    IdPayloadValue,
    IdLanguage,
    IdGroupStatus,
    IdStatusProcess,
    IdStatusMethod,
    IdStatusAutostart,
    IdStatusWatcher,
    IdStatusEntryPoint,
    IdGroupMethod,
    IdMethodCombo,
    IdBtnInstall,
    IdBtnUninstall,
    IdGroupActions,
    IdBtnInject,
    IdBtnWatch,
    IdBtnAutostart,
    IdBtnRestart,
    IdBtnOpenLog,
    IdGroupLog,
    IdLogView,
    IdStatusBar,
};

constexpr int kTimerRefresh = 1;
constexpr UINT kMsgActionDone = WM_APP + 1;
constexpr int kClientWidth = 640;
constexpr int kClientHeight = 660;

struct GuiState {
    HWND window = nullptr;
    HWND steamValue = nullptr;
    HWND payloadValue = nullptr;
    HWND languageCombo = nullptr;
    HWND statusProcess = nullptr;
    HWND statusMethod = nullptr;
    HWND statusAutostart = nullptr;
    HWND statusWatcher = nullptr;
    HWND statusEntryPoint = nullptr;
    HWND methodCombo = nullptr;
    HWND btnInstall = nullptr;
    HWND btnUninstall = nullptr;
    HWND btnInject = nullptr;
    HWND btnWatch = nullptr;
    HWND btnAutostart = nullptr;
    HWND btnRestart = nullptr;
    HWND btnOpenLog = nullptr;
    HWND logView = nullptr;
    HWND statusBar = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    GuiLanguage language = GuiLanguage::English;
    int dpi = 96;
    std::atomic<bool> busy{false};
    std::wstring steamDir;
    std::wstring payloadPath;
    std::uint64_t logOffset = 0;
    std::wstring lastCommand;
    std::string pendingLogBytes;
    std::deque<std::wstring> pendingLines;
} g;

const GuiStrings& Text() { return GuiText(g.language); }

// Resolved once, before the window exists, so the outer window size and the
// child control layout always use the same scale.
int SystemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto getDpi = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (getDpi) {
            const UINT dpi = getDpi();
            if (dpi != 0) return static_cast<int>(dpi);
        }
    }
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi != 0 ? dpi : 96;
}

int Dpi() { return g.dpi; }

int Px(int value) { return MulDiv(value, Dpi(), 96); }

HFONT CreateUiFont(bool bold) {
    return CreateFontW(-MulDiv(9, Dpi(), 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

void AppendLog(const std::vector<std::wstring>& lines) {
    if (!g.logView || lines.empty()) return;

    std::wstring chunk;
    for (const auto& line : lines) {
        if (chunk.size() > 64 * 1024) break;
        chunk += line;
        chunk += L"\r\n";
    }
    if (chunk.empty()) return;

    // Keep the pane bounded: drop the first quarter when it grows too large.
    const int length = GetWindowTextLengthW(g.logView);
    if (length > 400000) {
        SendMessageW(g.logView, EM_SETSEL, 0, length / 4);
        SendMessageW(g.logView, EM_REPLACESEL, static_cast<WPARAM>(FALSE), reinterpret_cast<LPARAM>(L""));
        SendMessageW(g.logView, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    }

    SendMessageW(g.logView, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g.logView, EM_REPLACESEL, static_cast<WPARAM>(FALSE), reinterpret_cast<LPARAM>(chunk.c_str()));
    SendMessageW(g.logView, EM_SCROLLCARET, 0, 0);
}

// Tails ost.log so work done by an elevated instance (a separate process)
// shows up in the same pane without any IPC.
void DrainLogFile() {
    const std::wstring path = LogFilePath();
    if (path.empty()) return;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
        CloseHandle(file);
        return;
    }
    if (g.logOffset == 0) {
        // First run: start with the tail instead of replaying the whole history.
        g.logOffset = size.QuadPart > 8192 ? static_cast<std::uint64_t>(size.QuadPart) - 8192 : 0;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) <= g.logOffset) {
        CloseHandle(file);
        return;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(g.logOffset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return;
    }

    const DWORD toRead = static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(size.QuadPart) - g.logOffset,
                                                       static_cast<std::uint64_t>(256 * 1024)));
    std::string buffer(toRead, '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer.data(), toRead, &read, nullptr) != 0;
    CloseHandle(file);
    if (!ok || read == 0) return;

    g.logOffset += read;
    g.pendingLogBytes += buffer.substr(0, read);

    size_t start = 0;
    for (;;) {
        const size_t newline = g.pendingLogBytes.find('\n', start);
        if (newline == std::string::npos) break;
        std::string line = g.pendingLogBytes.substr(start, newline - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        g.pendingLines.push_back(WideFromUtf8(line));
        start = newline + 1;
    }
    g.pendingLogBytes.erase(0, start);

    if (g.pendingLines.empty()) return;
    std::vector<std::wstring> lines(g.pendingLines.begin(), g.pendingLines.end());
    g.pendingLines.clear();
    AppendLog(lines);
}

std::wstring DescribeMethod(const IfeoStatus& ifeo, const ProxyStatus& proxies) {
    if (ifeo.state == IfeoState::InstalledSelf) {
        return IEquals(ifeo.mode, L"root") ? Text().methods[4] : Text().methods[3];
    }
    if (proxies.dwmapiInBin) return Text().methods[0];
    if (proxies.versionEnabled) return Text().methods[1];
    if (proxies.xinputInRoot) return Text().methods[2];
    return Text().valueNone;
}

// Two entry points would load two payload copies into one Steam process, which
// crashes Steam -- so the window always states how many are active.
int CountEntryPoints(const IfeoStatus& ifeo, const ProxyStatus& proxies) {
    int count = (ifeo.state == IfeoState::InstalledSelf) ? 1 : 0;
    if (proxies.dwmapiInBin) ++count;
    if (proxies.versionEnabled) ++count;
    if (proxies.xinputInRoot) ++count;
    return count;
}

void RefreshStatus() {
    if (!g.window) return;

    auto steam = FindSteamInstall();
    g.steamDir = steam ? *steam : L"";
    const std::wstring deployed = g.steamDir.empty() ? L"" : g.steamDir + L"\\" + kPayloadFileName;
    g.payloadPath = (!deployed.empty() && FileExists(deployed)) ? deployed : SelfDir() + L"\\" + kPayloadFileName;

    SetText(g.steamValue, g.steamDir.empty() ? Text().valueNotFound : g.steamDir);
    SetText(g.payloadValue,
            FileExists(g.payloadPath) ? g.payloadPath : std::wstring(Text().valueMissing) + L": " + g.payloadPath);

    const auto processes = FindSteamProcesses();
    const IfeoStatus ifeo = QueryIfeo();
    const ProxyStatus proxies = QueryProxies(g.steamDir);
    const bool steamRunning = !processes.empty();

    std::wstring processText = Text().valueNotRunning;
    if (steamRunning) {
        processText = L"pid " + std::to_wstring(processes.front().pid) + L" (" +
                      (processes.front().payloadLoaded ? Text().valuePayloadLoaded
                                                       : Text().valuePayloadNotLoaded) +
                      L")";
    }
    SetText(g.statusProcess, std::wstring(Text().labelProcess) + L" " + processText);
    SetText(g.statusMethod, std::wstring(Text().labelMethod) + L" " + DescribeMethod(ifeo, proxies));
    SetText(g.statusAutostart, std::wstring(Text().labelAutostart) + L" " +
                                   (IsAutostartEnabled() ? Text().valueEnabled : Text().valueDisabled));
    SetText(g.statusWatcher, std::wstring(Text().labelWatcher) + L" " +
                                 (IsWatcherRunning() ? Text().valueRunning : Text().valueStopped));

    const int entryPoints = CountEntryPoints(ifeo, proxies);
    std::wstring entryText;
    if (entryPoints == 0) {
        entryText = L"0 (" + std::wstring(Text().valueNone) + L")";
    } else if (entryPoints == 1) {
        entryText = L"1 (" + DescribeMethod(ifeo, proxies) + L")";
    } else {
        entryText = std::to_wstring(entryPoints) + L" " + Text().valueEntryPointsMany;
    }
    SetText(g.statusEntryPoint, std::wstring(Text().labelEntryPoint) + L" " + entryText);

    const bool busy = g.busy.load();
    // Browsing the entry points is always allowed. Deploying also works while
    // Steam runs (the proxy is picked up on the next start) -- unless the
    // running Steam has a payload mapped from inside the Steam directory:
    // deleting or overwriting a loaded DLL fails and would leave two entry
    // points behind. A payload loaded from anywhere else is untouched by a
    // deploy/uninstall, so it blocks nothing.
    const bool payloadInSteam = steamRunning && !processes.front().payloadPath.empty() &&
                                !g.steamDir.empty() && IStartsWith(processes.front().payloadPath, g.steamDir);
    const bool canDeploy = !busy && !payloadInSteam;
    EnableWindow(g.methodCombo, !busy);
    EnableWindow(g.btnInstall, canDeploy);
    EnableWindow(g.btnUninstall, canDeploy);
    EnableWindow(g.btnInject, !busy);
    EnableWindow(g.btnWatch, !busy);
    EnableWindow(g.btnAutostart, !busy);
    EnableWindow(g.btnRestart, !busy);

    SetText(g.btnWatch, IsWatcherRunning() ? Text().btnWatchStop : Text().btnWatchStart);
    SetText(g.btnAutostart, IsAutostartEnabled() ? Text().btnAutostartOff : Text().btnAutostartOn);

    if (g.statusBar) {
        const wchar_t* hint = busy ? Text().statusBusy
                                   : (payloadInSteam ? Text().statusSteamLocked
                                                    : (steamRunning ? Text().statusSteamRestart
                                                                    : Text().statusReady));
        SendMessageW(g.statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(hint));
    }
}

Options MakeActionOptions(const std::wstring& command, const std::vector<std::wstring>& positional = {}) {
    Options options;
    options.command = command;
    options.positional = positional;
    options.rawArgs = {SelfExePath(), command};
    for (const auto& arg : positional) options.rawArgs.push_back(arg);
    // A self-elevated instance must not open a second window or console: it
    // performs the action headless and the GUI tails the shared log file.
    options.rawArgs.push_back(L"--quiet");
    return options;
}

void StartAction(const std::wstring& command, const std::vector<std::wstring>& positional = {}) {
    if (g.busy.exchange(true)) return;
    g.lastCommand = command;
    RefreshStatus();

    const Options options = MakeActionOptions(command, positional);
    std::thread([options] {
        const int code = RunCommand(options);
        PostMessageW(g.window, kMsgActionDone, static_cast<WPARAM>(code), 0);
    }).detach();
}

void StartWatcherProcess() {
    std::wstring commandLine = Quote(SelfExePath()) + L" watch --quiet";
    if (!g.steamDir.empty()) commandLine += L" --steam-dir " + Quote(g.steamDir);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(SelfExePath().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, SelfDir().c_str(), &startup, &info)) {
        return;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
}

void ApplyStrings() {
    const GuiStrings& text = Text();
    SetWindowTextW(g.window, (std::wstring(text.appTitle) + L" " + ToolVersion()).c_str());
    SetText(GetDlgItem(g.window, IdSteamLabel), text.steamInstall);
    SetText(GetDlgItem(g.window, IdPayloadLabel), text.payload);
    SetText(GetDlgItem(g.window, IdGroupStatus), text.groupStatus);
    SetText(GetDlgItem(g.window, IdGroupMethod), text.groupMethod);
    SetText(GetDlgItem(g.window, IdGroupActions), text.groupActions);
    SetText(GetDlgItem(g.window, IdGroupLog), text.groupLog);
    SetText(g.btnInstall, text.btnInstall);
    SetText(g.btnUninstall, text.btnUninstall);
    SetText(g.btnInject, text.btnInject);
    SetText(g.btnRestart, text.btnRestart);
    SetText(g.btnOpenLog, text.btnOpenLog);

    const int selected = static_cast<int>(SendMessageW(g.methodCombo, CB_GETCURSEL, 0, 0));
    SendMessageW(g.methodCombo, CB_RESETCONTENT, 0, 0);
    for (const wchar_t* method : text.methods) {
        SendMessageW(g.methodCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(method));
    }
    SendMessageW(g.methodCombo, CB_SETCURSEL, selected < 0 ? 0 : selected, 0);

    RefreshStatus();
}

HWND CreateChild(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width,
                 int height, int id) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(width),
                                   Px(height), g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
    if (control && g.font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

void BuildLayout() {
    const GuiStrings& text = Text();

    g.font = CreateUiFont(false);
    g.fontBold = CreateUiFont(true);

    CreateChild(L"STATIC", text.steamInstall, SS_LEFT, 12, 10, 108, 18, IdSteamLabel);
    g.steamValue = CreateChild(L"EDIT", L"", ES_LEFT | ES_READONLY | WS_BORDER, 124, 8, 388, 22, IdSteamValue);
    g.languageCombo = CreateChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 524, 8, 104, 200, IdLanguage);
    SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Espa\u00f1ol"));
    SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\u4e2d\u6587"));
    SendMessageW(g.languageCombo, CB_SETCURSEL, 0, 0);

    CreateChild(L"STATIC", text.payload, SS_LEFT, 12, 36, 108, 18, IdPayloadLabel);
    g.payloadValue = CreateChild(L"EDIT", L"", ES_LEFT | ES_READONLY | WS_BORDER, 124, 34, 504, 22, IdPayloadValue);

    CreateChild(L"BUTTON", text.groupStatus, BS_GROUPBOX, 12, 64, 616, 128, IdGroupStatus);
    g.statusProcess = CreateChild(L"STATIC", L"", SS_LEFT, 24, 84, 592, 18, IdStatusProcess);
    g.statusMethod = CreateChild(L"STATIC", L"", SS_LEFT, 24, 104, 592, 18, IdStatusMethod);
    g.statusAutostart = CreateChild(L"STATIC", L"", SS_LEFT, 24, 124, 592, 18, IdStatusAutostart);
    g.statusWatcher = CreateChild(L"STATIC", L"", SS_LEFT, 24, 144, 592, 18, IdStatusWatcher);
    g.statusEntryPoint = CreateChild(L"STATIC", L"", SS_LEFT, 24, 164, 592, 18, IdStatusEntryPoint);

    CreateChild(L"BUTTON", text.groupMethod, BS_GROUPBOX, 12, 200, 616, 68, IdGroupMethod);
    g.methodCombo = CreateChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 222, 300, 200,
                                IdMethodCombo);
    g.btnInstall = CreateChild(L"BUTTON", text.btnInstall, BS_PUSHBUTTON | WS_TABSTOP, 334, 220, 110, 26,
                               IdBtnInstall);
    g.btnUninstall = CreateChild(L"BUTTON", text.btnUninstall, BS_PUSHBUTTON | WS_TABSTOP, 452, 220, 140, 26,
                                 IdBtnUninstall);

    CreateChild(L"BUTTON", text.groupActions, BS_GROUPBOX, 12, 276, 616, 68, IdGroupActions);
    g.btnInject = CreateChild(L"BUTTON", text.btnInject, BS_PUSHBUTTON | WS_TABSTOP, 24, 298, 100, 26, IdBtnInject);
    g.btnWatch = CreateChild(L"BUTTON", text.btnWatchStart, BS_PUSHBUTTON | WS_TABSTOP, 130, 298, 112, 26,
                             IdBtnWatch);
    g.btnAutostart = CreateChild(L"BUTTON", text.btnAutostartOn, BS_PUSHBUTTON | WS_TABSTOP, 248, 298, 112, 26,
                                 IdBtnAutostart);
    g.btnRestart = CreateChild(L"BUTTON", text.btnRestart, BS_PUSHBUTTON | WS_TABSTOP, 366, 298, 116, 26,
                               IdBtnRestart);
    g.btnOpenLog = CreateChild(L"BUTTON", text.btnOpenLog, BS_PUSHBUTTON | WS_TABSTOP, 488, 298, 116, 26,
                               IdBtnOpenLog);

    CreateChild(L"BUTTON", text.groupLog, BS_GROUPBOX, 12, 352, 616, 268, IdGroupLog);
    g.logView = CreateChild(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                            24, 372, 592, 236, IdLogView);

    g.statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                  0, g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStatusBar)), nullptr,
                                  nullptr);
    if (g.statusBar && g.font) SendMessageW(g.statusBar, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);

    SendMessageW(g.steamValue, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.payloadValue, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.logView, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}

void OnCommand(int id, int notification) {
    switch (id) {
        case IdBtnInstall: {
            const int index = static_cast<int>(SendMessageW(g.methodCombo, CB_GETCURSEL, 0, 0));
            static const wchar_t* kMethods[] = {L"dwmapi", L"version", L"xinput", L"ifeo", L"ifeo-root"};
            if (index < 0 || index >= 5) return;
            StartAction(L"install", {kMethods[index]});
            break;
        }
        case IdBtnUninstall:
            StartAction(L"uninstall");
            break;
        case IdBtnInject:
            StartAction(L"inject");
            break;
        case IdBtnWatch:
            if (IsWatcherRunning()) {
                if (StopWatcher()) LogInfo(L"stop signal sent to the watcher");
            } else {
                StartWatcherProcess();
            }
            RefreshStatus();
            break;
        case IdBtnAutostart: {
            const bool enabled = IsAutostartEnabled();
            std::wstring error;
            if (!SetAutostart(!enabled, g.steamDir, error)) LogWarn(error);
            RefreshStatus();
            break;
        }
        case IdBtnRestart:
            StartAction(L"restart");
            break;
        case IdBtnOpenLog:
            ShellExecuteW(g.window, L"open", SelfDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdLanguage:
            if (notification == CBN_SELCHANGE) {
                const int selected = static_cast<int>(SendMessageW(g.languageCombo, CB_GETCURSEL, 0, 0));
                if (selected >= 0 && selected < static_cast<int>(GuiLanguage::Count)) {
                    g.language = static_cast<GuiLanguage>(selected);
                    ApplyStrings();
                }
            }
            break;
        default:
            break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g.window = window;
            BuildLayout();
            ApplyStrings();
            DrainLogFile();
            SetTimer(window, kTimerRefresh, 700, nullptr);
            if (!GetEnvVar(L"OST_GUI_SELFTEST").empty()) SetTimer(window, 2, 2500, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerRefresh) {
                RefreshStatus();
                DrainLogFile();
            } else if (wParam == 2) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case kMsgActionDone: {
            g.busy.store(false);
            const int code = static_cast<int>(wParam);
            if (code != 0) LogWarn(L"the action finished with code " + std::to_wstring(code));
            DrainLogFile();
            RefreshStatus();

            const bool entryPointChanged = g.lastCommand == L"install" || g.lastCommand == L"uninstall";
            g.lastCommand.clear();
            if (code == 0 && entryPointChanged && !FindSteamProcesses(false).empty()) {
                if (MessageBoxW(window, Text().promptRestart, Text().appTitle,
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES) {
                    StartAction(L"restart");
                }
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTimerRefresh);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int RunGui(const Options& options) {
    (void)options;

    // System-DPI awareness keeps the fixed layout readable on scaled displays.
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) setContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    }

    g.dpi = SystemDpi();

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"OpenSteamToolManagerWindow";
    if (!RegisterClassExW(&windowClass)) return 1;

    RECT desired{0, 0, Px(kClientWidth), Px(kClientHeight)};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;

    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"OpenSteamTool",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                  CW_USEDEFAULT, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (g.font) DeleteObject(g.font);
    if (g.fontBold) DeleteObject(g.fontBold);
    return 0;
}

} // namespace ost
