#include "Gui.h"

#include "Commands.h"
#include "Common.h"
#include "GuiStrings.h"
#include "Ifeo.h"
#include "Inject.h"
#include "Log.h"
#include "Proxies.h"
#include "Steam.h"

#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

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
    IdTheme,
    IdGroupStatus,
    IdStatusProcessLabel,
    IdStatusProcess,
    IdStatusMethodLabel,
    IdStatusMethod,
    IdStatusAutostartLabel,
    IdStatusAutostart,
    IdStatusWatcherLabel,
    IdStatusWatcher,
    IdStatusEntryPointLabel,
    IdStatusEntryPoint,
    IdGroupMethod,
    IdMethodCombo,
    IdMethodHint,
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
    IdFooter,
};

constexpr int kTimerRefresh = 1;
constexpr int kTimerSelftest = 2;
constexpr UINT kMsgActionDone = WM_APP + 1;
constexpr int kClientWidth = 640;
constexpr int kClientHeight = 662;
constexpr int kAppIconId = 101;

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
enum class ThemeMode { System, Light, Dark };

struct Theme {
    bool dark = false;
    COLORREF window = 0;
    COLORREF card = 0;
    COLORREF border = 0;
    COLORREF text = 0;
    COLORREF subtle = 0;
    COLORREF accent = 0;
    COLORREF accentText = 0;
    COLORREF button = 0;
    COLORREF buttonHover = 0;
    COLORREF buttonPressed = 0;
    COLORREF buttonBorder = 0;
    COLORREF edit = 0;
    COLORREF log = 0;
    COLORREF info = 0;
    COLORREF warn = 0;
    COLORREF error = 0;
    COLORREF debug = 0;
};

Theme MakeLightTheme() {
    Theme t;
    t.dark = false;
    t.window = RGB(0xF3, 0xF3, 0xF3);
    t.card = RGB(0xFF, 0xFF, 0xFF);
    t.border = RGB(0xDD, 0xDD, 0xDD);
    t.text = RGB(0x1A, 0x1A, 0x1A);
    t.subtle = RGB(0x5C, 0x5C, 0x5C);
    t.accent = RGB(0x0F, 0x6C, 0xBD);
    t.accentText = RGB(0xFF, 0xFF, 0xFF);
    t.button = RGB(0xFA, 0xFA, 0xFA);
    t.buttonHover = RGB(0xEF, 0xEF, 0xEF);
    t.buttonPressed = RGB(0xE2, 0xE2, 0xE2);
    t.buttonBorder = RGB(0xD0, 0xD0, 0xD0);
    t.edit = RGB(0xFF, 0xFF, 0xFF);
    t.log = RGB(0xFF, 0xFF, 0xFF);
    t.info = RGB(0x10, 0x7C, 0x10);
    t.warn = RGB(0x9A, 0x67, 0x00);
    t.error = RGB(0xC4, 0x2B, 0x1C);
    t.debug = RGB(0x6E, 0x6E, 0x6E);
    return t;
}

Theme MakeDarkTheme() {
    Theme t;
    t.dark = true;
    t.window = RGB(0x1F, 0x1F, 0x1F);
    t.card = RGB(0x2A, 0x2A, 0x2A);
    t.border = RGB(0x3A, 0x3A, 0x3A);
    t.text = RGB(0xF2, 0xF2, 0xF2);
    t.subtle = RGB(0xB4, 0xB4, 0xB4);
    t.accent = RGB(0x4C, 0xC2, 0xFF);
    t.accentText = RGB(0x08, 0x13, 0x1C);
    t.button = RGB(0x33, 0x33, 0x33);
    t.buttonHover = RGB(0x3D, 0x3D, 0x3D);
    t.buttonPressed = RGB(0x48, 0x48, 0x48);
    t.buttonBorder = RGB(0x46, 0x46, 0x46);
    t.edit = RGB(0x1A, 0x1A, 0x1A);
    t.log = RGB(0x18, 0x18, 0x18);
    t.info = RGB(0x6C, 0xCB, 0x5F);
    t.warn = RGB(0xF2, 0xC7, 0x44);
    t.error = RGB(0xFF, 0x8A, 0x80);
    t.debug = RGB(0x9A, 0x9A, 0x9A);
    return t;
}

enum class Glyph { None, Install, Uninstall, Inject, Play, Stop, Power, Restart, Folder, Contrast };

struct Card {
    RECT rect{};
    const wchar_t* caption = L"";
};

struct StatusRow {
    HWND label = nullptr;
    HWND value = nullptr;
    COLORREF color = 0;
};

struct TipBinding {
    HWND control = nullptr;
    const wchar_t* text = L"";
};

// The tooltip control only wraps text that already contains line breaks, so
// long explanations are broken at word boundaries (or by length for scripts
// without spaces) before they are handed over.
std::wstring WrapTipText(const wchar_t* text, size_t maxChars = 60) {
    std::wstring out;
    std::wstring line;
    size_t lineLength = 0;
    const std::wstring source = text ? text : L"";
    size_t index = 0;
    while (index < source.size()) {
        while (index < source.size() && source[index] == L' ') ++index;
        const size_t start = index;
        while (index < source.size() && source[index] != L' ') ++index;
        if (start == index) break;
        const std::wstring word = source.substr(start, index - start);
        // Scripts without spaces (Chinese) arrive as one long word: hard-break it.
        size_t consumed = 0;
        while (word.size() - consumed > maxChars) {
            if (!line.empty()) {
                if (!out.empty()) out += L"\r\n";
                out += line;
                line.clear();
                lineLength = 0;
            }
            if (!out.empty()) out += L"\r\n";
            out += word.substr(consumed, maxChars);
            consumed += maxChars;
        }
        const std::wstring remainder = word.substr(consumed);
        if (remainder.empty()) continue;
        if (lineLength != 0 && lineLength + 1 + remainder.size() > maxChars) {
            if (!out.empty()) out += L"\r\n";
            out += line;
            line.clear();
            lineLength = 0;
        }
        if (lineLength != 0) {
            line += L' ';
            ++lineLength;
        }
        line += remainder;
        lineLength += remainder.size();
    }
    if (!line.empty()) {
        if (!out.empty()) out += L"\r\n";
        out += line;
    }
    return out.empty() ? source : out;
}

struct GuiState {
    HWND window = nullptr;
    HWND steamValue = nullptr;
    HWND payloadValue = nullptr;
    HWND languageCombo = nullptr;
    HWND themeButton = nullptr;
    HWND methodCombo = nullptr;
    HWND methodHint = nullptr;
    HWND btnInstall = nullptr;
    HWND btnUninstall = nullptr;
    HWND btnInject = nullptr;
    HWND btnWatch = nullptr;
    HWND btnAutostart = nullptr;
    HWND btnRestart = nullptr;
    HWND btnOpenLog = nullptr;
    HWND logView = nullptr;
    HWND footer = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    HFONT fontSmall = nullptr;
    GuiLanguage language = GuiLanguage::English;
    ThemeMode themeMode = ThemeMode::System;
    Theme theme = MakeLightTheme();
    HBRUSH windowBrush = nullptr;
    HBRUSH cardBrush = nullptr;
    HBRUSH editBrush = nullptr;
    HWND tooltip = nullptr;
    std::vector<Card> cards;
    std::vector<StatusRow> rows;
    std::vector<TipBinding> tips;
    // Owns the wrapped tooltip strings; deque keeps the pointers stable.
    std::deque<std::wstring> tipTexts;
    int hoverId = 0;
    int dpi = 96;
    bool richLog = false;
    std::atomic<bool> busy{false};
    std::wstring steamDir;
    std::wstring payloadPath;
    // Empty: the folder holding ost-manager.exe acts as the data root.
    std::wstring dataDir;
    std::uint64_t logOffset = 0;
    std::wstring lastCommand;
    std::string pendingLogBytes;
    std::deque<std::wstring> pendingLines;
} g;

const GuiStrings& Text() { return GuiText(g.language); }

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

HFONT CreateUiFont(int points, bool bold) {
    return CreateFontW(-MulDiv(points, Dpi(), 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

const wchar_t* ThemeModeName() { return Text().themeNames[static_cast<int>(g.themeMode)]; }

bool SystemUsesDarkTheme() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
        return false;
    }
    return light == 0;
}

Theme ResolveTheme() {
    switch (g.themeMode) {
        case ThemeMode::Light: return MakeLightTheme();
        case ThemeMode::Dark: return MakeDarkTheme();
        case ThemeMode::System:
        default: return SystemUsesDarkTheme() ? MakeDarkTheme() : MakeLightTheme();
    }
}

void LoadThemePreference() {
    std::wstring value;
    if (!RegReadString(HKEY_CURRENT_USER, kAppRegKey, L"GuiTheme", value)) return;
    if (IEquals(value, L"light")) g.themeMode = ThemeMode::Light;
    else if (IEquals(value, L"dark")) g.themeMode = ThemeMode::Dark;
    else g.themeMode = ThemeMode::System;
}

void SaveThemePreference() {
    static const wchar_t* kNames[] = {L"system", L"light", L"dark"};
    std::wstring error;
    RegWriteString(HKEY_CURRENT_USER, kAppRegKey, L"GuiTheme", kNames[static_cast<int>(g.themeMode)], error);
}

void ApplyTitleBar(HWND window, bool dark) {
    using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    // Load the system copy on purpose: this bundle also contains our dwmapi
    // proxy, which must never be pulled into the manager.
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    auto setAttribute = reinterpret_cast<SetAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!setAttribute) return;
    BOOL value = dark ? TRUE : FALSE;
    setAttribute(window, 20, &value, sizeof(value)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    setAttribute(window, 19, &value, sizeof(value)); // pre-20H1 builds
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void RecreateBrush(HBRUSH& brush, COLORREF color) {
    if (brush) DeleteObject(brush);
    brush = CreateSolidBrush(color);
}

void InvalidateChildTree(HWND parent) {
    InvalidateRect(parent, nullptr, TRUE);
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        InvalidateRect(child, nullptr, TRUE);
    }
}

void ApplyTheme(HWND window, bool recreateBrushes) {
    g.theme = ResolveTheme();
    if (recreateBrushes) {
        RecreateBrush(g.windowBrush, g.theme.window);
        RecreateBrush(g.cardBrush, g.theme.card);
        RecreateBrush(g.editBrush, g.theme.edit);
    }
    if (window) ApplyTitleBar(window, g.theme.dark);

    // Themed scrollbars/combos: harmless where the OS does not support them.
    const wchar_t* scrollTheme = g.theme.dark ? L"DarkMode_Explorer" : L"Explorer";
    const wchar_t* comboTheme = g.theme.dark ? L"DarkMode_CFD" : L"Explorer";
    if (g.logView) SetWindowTheme(g.logView, scrollTheme, nullptr);
    if (g.languageCombo) SetWindowTheme(g.languageCombo, comboTheme, nullptr);
    if (g.methodCombo) SetWindowTheme(g.methodCombo, comboTheme, nullptr);

    if (g.logView && g.richLog) {
        SendMessageW(g.logView, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(g.theme.log));
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR;
        format.crTextColor = g.theme.text;
        SendMessageW(g.logView, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    }
    if (g.tooltip) {
        SendMessageW(g.tooltip, TTM_SETTIPBKCOLOR, 0, static_cast<LPARAM>(g.theme.card));
        SendMessageW(g.tooltip, TTM_SETTIPTEXTCOLOR, 0, static_cast<LPARAM>(g.theme.text));
    }
    if (window) InvalidateChildTree(window);
}

void SetStatusRowColor(size_t index, COLORREF color) {
    if (index >= g.rows.size()) return;
    if (g.rows[index].color == color) return;
    g.rows[index].color = color;
    InvalidateRect(g.rows[index].value, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Log pane
// ---------------------------------------------------------------------------
COLORREF ColorForLine(const std::wstring& line) {
    if (line.find(L"[WARN ]") != std::wstring::npos) return g.theme.warn;
    if (line.find(L"[ERROR]") != std::wstring::npos) return g.theme.error;
    if (line.find(L"[DEBUG]") != std::wstring::npos) return g.theme.debug;
    if (line.find(L"[INFO ]") != std::wstring::npos) return g.theme.info;
    return g.theme.text;
}

void AppendLog(const std::vector<std::wstring>& lines) {
    if (!g.logView || lines.empty()) return;

    // Keep the pane bounded: drop the first quarter when it grows too large.
    const int length = GetWindowTextLengthW(g.logView);
    if (length > 400000) {
        SendMessageW(g.logView, EM_SETSEL, 0, length / 4);
        SendMessageW(g.logView, EM_REPLACESEL, static_cast<WPARAM>(FALSE), reinterpret_cast<LPARAM>(L""));
    }

    SendMessageW(g.logView, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    for (const auto& line : lines) {
        if (g.richLog) {
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR;
            format.crTextColor = ColorForLine(line);
            SendMessageW(g.logView, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        }
        std::wstring padded = line + L"\r\n";
        SendMessageW(g.logView, EM_REPLACESEL, static_cast<WPARAM>(FALSE),
                     reinterpret_cast<LPARAM>(padded.c_str()));
    }
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

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
void FillRounded(HDC dc, const RECT& rect, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(6), Px(6));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawArrowHead(HDC dc, int tipX, int tipY, int size, bool pointRight) {
    POINT points[3]{};
    if (pointRight) {
        points[0] = {tipX, tipY};
        points[1] = {tipX - size, tipY - size};
        points[2] = {tipX - size, tipY + size};
    } else {
        points[0] = {tipX, tipY};
        points[1] = {tipX - size, tipY - size};
        points[2] = {tipX + size, tipY - size};
    }
    Polygon(dc, points, 3);
}

void DrawGlyph(HDC dc, const RECT& box, Glyph glyph, COLORREF color) {
    if (glyph == Glyph::None) return;

    const int left = box.left;
    const int top = box.top;
    const int width = box.right - box.left;
    const int height = box.bottom - box.top;
    const int cx = left + width / 2;
    const int cy = top + height / 2;

    HPEN pen = CreatePen(PS_SOLID, (std::max)(1, MulDiv(2, Dpi(), 96)), color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);

    switch (glyph) {
        case Glyph::Install: {
            const int lineTop = top + height / 8;
            const int lineBottom = top + height * 5 / 8;
            MoveToEx(dc, cx, lineTop, nullptr);
            LineTo(dc, cx, lineBottom);
            DrawArrowHead(dc, cx, lineBottom + height / 6, width / 5, false);
            const int baseY = top + height - height / 8;
            MoveToEx(dc, left + width / 5, baseY, nullptr);
            LineTo(dc, left + width - width / 5, baseY);
            break;
        }
        case Glyph::Uninstall: {
            const int inset = width / 4;
            MoveToEx(dc, left + inset, top + inset, nullptr);
            LineTo(dc, left + width - inset, top + height - inset);
            MoveToEx(dc, left + width - inset, top + inset, nullptr);
            LineTo(dc, left + inset, top + height - inset);
            break;
        }
        case Glyph::Inject: {
            MoveToEx(dc, left + width / 8, cy, nullptr);
            LineTo(dc, left + width * 2 / 3, cy);
            DrawArrowHead(dc, left + width * 3 / 4, cy, height / 4, true);
            MoveToEx(dc, left + width - width / 8, top + height / 5, nullptr);
            LineTo(dc, left + width - width / 8, top + height - height / 5);
            break;
        }
        case Glyph::Play: {
            POINT points[3] = {{left + width / 3, top + height / 5},
                               {left + width - width / 5, cy},
                               {left + width / 3, top + height - height / 5}};
            Polygon(dc, points, 3);
            break;
        }
        case Glyph::Stop: {
            RECT square{left + width / 4, top + height / 4, left + width - width / 4,
                        top + height - height / 4};
            FillRect(dc, &square, brush);
            break;
        }
        case Glyph::Power: {
            // Circle with the gap at the top, plus the vertical stem.
            const int radius = width / 3;
            const double diagonal = 0.70710678;
            const int gapX = static_cast<int>(radius * diagonal);
            const int gapY = static_cast<int>(radius * diagonal);
            Arc(dc, cx - radius, cy - radius, cx + radius, cy + radius, cx + gapX, cy - gapY, cx - gapX,
                cy - gapY);
            MoveToEx(dc, cx, top + height / 10, nullptr);
            LineTo(dc, cx, cy + height / 10);
            break;
        }
        case Glyph::Restart: {
            const int radius = width / 3;
            const double diagonal = 0.70710678;
            const int gapX = static_cast<int>(radius * diagonal);
            const int gapY = static_cast<int>(radius * diagonal);
            // Three quarters of a circle, opening at the top right, with the
            // arrow head sitting in the opening.
            Arc(dc, cx - radius, cy - radius, cx + radius, cy + radius, cx + gapX, cy - gapY, cx + gapX,
                cy + gapY);
            DrawArrowHead(dc, cx + radius, cy - radius / 2, height / 5, true);
            break;
        }
        case Glyph::Folder: {
            const int tabY = top + height / 4;
            MoveToEx(dc, left + width / 8, top + height * 3 / 4, nullptr);
            LineTo(dc, left + width / 8, tabY);
            LineTo(dc, cx, tabY);
            LineTo(dc, cx + width / 8, tabY + height / 8);
            LineTo(dc, left + width - width / 8, tabY + height / 8);
            LineTo(dc, left + width - width / 8, top + height * 3 / 4);
            LineTo(dc, left + width / 8, top + height * 3 / 4);
            break;
        }
        case Glyph::Contrast: {
            Ellipse(dc, left + width / 8, top + height / 8, left + width - width / 8,
                    top + height - height / 8);
            const int radius = width / 2 - width / 8;
            Pie(dc, cx - radius, cy - radius, cx + radius, cy + radius, cx, top, cx, top + height);
            break;
        }
        case Glyph::None:
        default:
            break;
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

Glyph GlyphForButton(int id) {
    switch (id) {
        case IdBtnInstall: return Glyph::Install;
        case IdBtnUninstall: return Glyph::Uninstall;
        case IdBtnInject: return Glyph::Inject;
        case IdBtnWatch: return IsWatcherRunning() ? Glyph::Stop : Glyph::Play;
        case IdBtnAutostart: return Glyph::Power;
        case IdBtnRestart: return Glyph::Restart;
        case IdBtnOpenLog: return Glyph::Folder;
        case IdTheme: return Glyph::Contrast;
        default: return Glyph::None;
    }
}

void DrawButton(const DRAWITEMSTRUCT& item) {
    const Theme& theme = g.theme;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool hover = g.hoverId == static_cast<int>(item.CtlID);
    const int radius = Px(4);

    COLORREF fill = theme.button;
    if (disabled) fill = theme.dark ? theme.card : RGB(0xF0, 0xF0, 0xF0);
    else if (pressed) fill = theme.buttonPressed;
    else if (hover) fill = theme.buttonHover;

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, theme.buttonBorder);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, radius * 2,
              radius * 2);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t label[128] = {};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));

    const Glyph glyph = GlyphForButton(static_cast<int>(item.CtlID));
    const COLORREF textColor = disabled ? theme.subtle : theme.text;
    const COLORREF iconColor = disabled ? theme.subtle : theme.text;

    SetBkMode(item.hDC, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(item.hDC, g.font);
    SIZE textSize{};
    GetTextExtentPoint32W(item.hDC, label, lstrlenW(label), &textSize);

    const int iconSide = Px(14);
    const int gap = glyph == Glyph::None ? 0 : Px(6);
    const int total = (glyph == Glyph::None ? 0 : iconSide + gap) + textSize.cx;
    int x = item.rcItem.left + ((item.rcItem.right - item.rcItem.left) - total) / 2;
    const int textTop = item.rcItem.top + ((item.rcItem.bottom - item.rcItem.top) - textSize.cy) / 2;

    if (glyph != Glyph::None) {
        RECT iconBox{x, textTop + (textSize.cy - iconSide) / 2, x + iconSide,
                     textTop + (textSize.cy - iconSide) / 2 + iconSide};
        DrawGlyph(item.hDC, iconBox, glyph, iconColor);
        x += iconSide + gap;
    }

    RECT textRect{x, textTop, item.rcItem.right - Px(4), textTop + textSize.cy + Px(2)};
    SetTextColor(item.hDC, textColor);
    DrawTextW(item.hDC, label, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    if (focused) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Px(3), -Px(3));
        DrawFocusRect(item.hDC, &focus);
    }
    SelectObject(item.hDC, oldFont);
}

void DrawComboItem(const DRAWITEMSTRUCT& item) {
    const Theme& theme = g.theme;
    const bool selectionField = item.itemID == static_cast<UINT>(-1);
    const bool highlighted = (item.itemState & ODS_SELECTED) != 0;

    COLORREF fill = selectionField ? theme.edit : theme.card;
    COLORREF textColor = theme.text;
    if (highlighted) {
        fill = theme.accent;
        textColor = theme.accentText;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);

    wchar_t buffer[256] = {};
    if (selectionField) {
        const int selected = static_cast<int>(SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0));
        if (selected < 0) return;
        SendMessageW(item.hwndItem, CB_GETLBTEXT, static_cast<WPARAM>(selected),
                     reinterpret_cast<LPARAM>(buffer));
    } else {
        SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(buffer));
    }
    if (buffer[0] == L'\0') return;

    HGDIOBJ oldFont = SelectObject(item.hDC, g.font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    RECT textRect = item.rcItem;
    textRect.left += Px(8);
    textRect.right -= Px(6);
    DrawTextW(item.hDC, buffer, -1, &textRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, oldFont);
}

void PaintCards(HDC dc) {
    for (const auto& card : g.cards) {
        FillRounded(dc, card.rect, g.theme.card, g.theme.border);
        if (!card.caption || card.caption[0] == L'\0') continue;
        HGDIOBJ oldFont = SelectObject(dc, g.fontBold);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g.theme.text);
        RECT caption{card.rect.left + Px(14), card.rect.top + Px(8), card.rect.right - Px(12),
                     card.rect.top + Px(28)};
        DrawTextW(dc, card.caption, -1, &caption, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }
}

// ---------------------------------------------------------------------------
// Status / actions
// ---------------------------------------------------------------------------
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

void UpdateMethodHint() {
    if (!g.methodHint) return;
    const int index = static_cast<int>(SendMessageW(g.methodCombo, CB_GETCURSEL, 0, 0));
    SetText(g.methodHint, (index >= 0 && index < 5) ? Text().methodHints[index] : L"");
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
    if (g.rows.size() >= 5) {
        const int entryPoints = CountEntryPoints(ifeo, proxies);
        SetText(g.rows[0].value, processText);
        SetStatusRowColor(0, steamRunning ? g.theme.info : g.theme.subtle);

        SetText(g.rows[1].value, DescribeMethod(ifeo, proxies));
        SetStatusRowColor(1, entryPoints == 0 ? g.theme.subtle : g.theme.text);

        SetText(g.rows[2].value, IsAutostartEnabled() ? Text().valueEnabled : Text().valueDisabled);
        SetStatusRowColor(2, IsAutostartEnabled() ? g.theme.text : g.theme.subtle);

        SetText(g.rows[3].value, IsWatcherRunning() ? Text().valueRunning : Text().valueStopped);
        SetStatusRowColor(3, IsWatcherRunning() ? g.theme.info : g.theme.subtle);

        std::wstring entryText;
        if (entryPoints == 0) {
            entryText = L"0 (" + std::wstring(Text().valueNone) + L")";
        } else if (entryPoints == 1) {
            entryText = L"1 (" + DescribeMethod(ifeo, proxies) + L")";
        } else {
            entryText = std::to_wstring(entryPoints) + L" " + Text().valueEntryPointsMany;
        }
        SetText(g.rows[4].value, entryText);
        SetStatusRowColor(4, entryPoints > 1 ? g.theme.error : (entryPoints == 1 ? g.theme.info : g.theme.subtle));
    }

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
    InvalidateRect(g.btnWatch, nullptr, TRUE);
    InvalidateRect(g.btnAutostart, nullptr, TRUE);

    if (g.footer) {
        const wchar_t* hint = busy ? Text().statusBusy
                                   : (payloadInSteam ? Text().statusSteamLocked
                                                    : (steamRunning ? Text().statusSteamRestart
                                                                    : Text().statusReady));
        // The payload keeps config\lua, logs and caches in the data root, so it
        // is always one glance away (default: this folder).
        const std::wstring dataRoot = ResolveDataRoot(g.dataDir, g.steamDir);
        SetText(g.footer, std::wstring(hint) + (dataRoot.empty() ? L"" : L"  |  " + dataRoot));
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

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
HWND CreateChild(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width,
                 int height, int id, HFONT font = nullptr) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(width),
                                   Px(height), g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
    if (control) {
        HFONT use = font ? font : g.font;
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
    }
    return control;
}

void AddTip(HWND control, const wchar_t* text) {
    if (!g.tooltip || !control) return;
    g.tipTexts.push_back(WrapTipText(text));
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = g.window;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<LPWSTR>(g.tipTexts.back().c_str());
    SendMessageW(g.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    TipBinding binding;
    binding.control = control;
    binding.text = g.tipTexts.back().c_str();
    g.tips.push_back(binding);
}

void ClearTips() {
    if (!g.tooltip) return;
    for (const auto& tip : g.tips) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = g.window;
        tool.uId = reinterpret_cast<UINT_PTR>(tip.control);
        SendMessageW(g.tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    g.tips.clear();
    g.tipTexts.clear();
}

void BindTips() {
    const GuiStrings& text = Text();
    ClearTips();
    if (!g.tooltip) return;
    AddTip(g.steamValue, text.tipSteam);
    AddTip(g.payloadValue, text.tipPayload);
    AddTip(g.languageCombo, text.tipLanguage);
    AddTip(g.themeButton, text.tipTheme);
    AddTip(g.methodCombo, text.tipMethod);
    AddTip(g.btnInstall, text.tipInstall);
    AddTip(g.btnUninstall, text.tipUninstall);
    AddTip(g.btnInject, text.tipInject);
    AddTip(g.btnWatch, text.tipWatch);
    AddTip(g.btnAutostart, text.tipAutostart);
    AddTip(g.btnRestart, text.tipRestart);
    AddTip(g.btnOpenLog, text.tipOpenLog);
    AddTip(g.logView, text.tipLog);
    for (const auto& row : g.rows) AddTip(row.value, text.tipStatus);
}

void BuildLayout() {
    const GuiStrings& text = Text();

    g.font = CreateUiFont(9, false);
    g.fontBold = CreateUiFont(9, true);
    g.fontSmall = CreateUiFont(8, false);

    // Row 1: Steam folder, language, theme.
    CreateChild(L"STATIC", text.steamInstall, SS_LEFT, 12, 12, 124, 18, IdSteamLabel);
    g.steamValue = CreateChild(L"EDIT", L"", ES_LEFT | ES_READONLY | ES_AUTOHSCROLL, 140, 10, 268, 22,
                               IdSteamValue);
    g.languageCombo = CreateChild(L"COMBOBOX", L"",
                                  CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                                      WS_TABSTOP,
                                  416, 10, 110, 200, IdLanguage);
    g.themeButton = CreateChild(L"BUTTON", L"", BS_OWNERDRAW | BS_PUSHBUTTON | WS_TABSTOP, 534, 9, 94, 24,
                                IdTheme);

    // Row 2: payload.
    CreateChild(L"STATIC", text.payload, SS_LEFT, 12, 40, 124, 18, IdPayloadLabel);
    g.payloadValue = CreateChild(L"EDIT", L"", ES_LEFT | ES_READONLY | ES_AUTOHSCROLL, 140, 38, 488, 22,
                                 IdPayloadValue);

    // Status card.
    const int statusTop = 70;
    Card statusCard{RECT{Px(12), Px(statusTop), Px(628), Px(statusTop + 128)}, text.groupStatus};
    g.cards.push_back(statusCard);
    const wchar_t* labels[] = {text.labelProcess, text.labelMethod, text.labelAutostart, text.labelWatcher,
                               text.labelEntryPoint};
    const int labelIds[] = {IdStatusProcessLabel, IdStatusMethodLabel, IdStatusAutostartLabel,
                            IdStatusWatcherLabel, IdStatusEntryPointLabel};
    const int valueIds[] = {IdStatusProcess, IdStatusMethod, IdStatusAutostart, IdStatusWatcher,
                            IdStatusEntryPoint};
    for (int i = 0; i < 5; ++i) {
        const int y = statusTop + 24 + i * 20;
        HWND label = CreateChild(L"STATIC", labels[i], SS_LEFT | SS_ENDELLIPSIS, 26, y, 104, 18, labelIds[i]);
        HWND value = CreateChild(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 132, y, 484, 18, valueIds[i]);
        StatusRow row;
        row.label = label;
        row.value = value;
        row.color = g.theme.subtle;
        g.rows.push_back(row);
    }

    // Entry point card.
    const int methodTop = 206;
    Card methodCard{RECT{Px(12), Px(methodTop), Px(628), Px(methodTop + 98)}, text.groupMethod};
    g.cards.push_back(methodCard);
    g.methodCombo = CreateChild(L"COMBOBOX", L"",
                                CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                                    WS_TABSTOP,
                                26, methodTop + 22, 288, 220, IdMethodCombo);
    g.btnInstall = CreateChild(L"BUTTON", text.btnInstall, BS_OWNERDRAW | WS_TABSTOP, 326, methodTop + 21, 138, 28,
                               IdBtnInstall);
    g.btnUninstall = CreateChild(L"BUTTON", text.btnUninstall, BS_OWNERDRAW | WS_TABSTOP, 472, methodTop + 21,
                                 142, 28, IdBtnUninstall);
    g.methodHint = CreateChild(L"STATIC", L"", SS_LEFT, 26, methodTop + 58, 590, 34, IdMethodHint, g.fontSmall);

    // Actions card.
    const int actionsTop = 312;
    Card actionsCard{RECT{Px(12), Px(actionsTop), Px(628), Px(actionsTop + 66)}, text.groupActions};
    g.cards.push_back(actionsCard);
    g.btnInject = CreateChild(L"BUTTON", text.btnInject, BS_OWNERDRAW | WS_TABSTOP, 26, actionsTop + 22, 132, 28,
                              IdBtnInject);
    g.btnWatch = CreateChild(L"BUTTON", text.btnWatchStart, BS_OWNERDRAW | WS_TABSTOP, 178, actionsTop + 22, 132,
                             28, IdBtnWatch);
    g.btnAutostart = CreateChild(L"BUTTON", text.btnAutostartOn, BS_OWNERDRAW | WS_TABSTOP, 330, actionsTop + 22,
                                 132, 28, IdBtnAutostart);
    g.btnRestart = CreateChild(L"BUTTON", text.btnRestart, BS_OWNERDRAW | WS_TABSTOP, 482, actionsTop + 22, 134,
                               28, IdBtnRestart);

    // Log card, with the folder shortcut in its header.
    const int logTop = 386;
    Card logCard{RECT{Px(12), Px(logTop), Px(628), Px(logTop + 244)}, text.groupLog};
    g.cards.push_back(logCard);
    g.btnOpenLog = CreateChild(L"BUTTON", text.btnOpenLog, BS_OWNERDRAW | WS_TABSTOP, 480, logTop + 6, 136, 24,
                               IdBtnOpenLog);
    HMODULE rich = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (rich) {
        g.logView = CreateChild(L"RICHEDIT50W", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                26, logTop + 38, 588, 194, IdLogView);
        g.richLog = g.logView != nullptr;
    }
    if (!g.logView) {
        g.logView = CreateChild(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 26,
                                logTop + 38, 588, 194, IdLogView);
    }
    if (g.richLog) {
        SendMessageW(g.logView, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(g.theme.log));
        SendMessageW(g.logView, EM_SETUNDOLIMIT, 0, 0);
    }

    g.footer = CreateChild(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 14, 638, 612, 18, IdFooter, g.fontSmall);

    g.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, g.window, nullptr, nullptr, nullptr);
    if (g.tooltip) {
        SendMessageW(g.tooltip, TTM_SETMAXTIPWIDTH, 0, Px(380));
        SendMessageW(g.tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, Px(350));
        SendMessageW(g.tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(g.fontSmall), TRUE);
    }
}

void ApplyStrings() {
    const GuiStrings& text = Text();

    if (g.window) SetWindowTextW(g.window, (std::wstring(text.appTitle) + L" " + ToolVersion()).c_str());
    SetText(GetDlgItem(g.window, IdSteamLabel), text.steamInstall);
    SetText(GetDlgItem(g.window, IdPayloadLabel), text.payload);
    SetText(g.btnInstall, text.btnInstall);
    SetText(g.btnUninstall, text.btnUninstall);
    SetText(g.btnInject, text.btnInject);
    SetText(g.btnRestart, text.btnRestart);
    SetText(g.btnOpenLog, text.btnOpenLog);
    SetText(g.btnWatch, IsWatcherRunning() ? text.btnWatchStop : text.btnWatchStart);
    SetText(g.btnAutostart, IsAutostartEnabled() ? text.btnAutostartOff : text.btnAutostartOn);
    SetText(g.themeButton, ThemeModeName());

    if (g.cards.size() >= 4) {
        g.cards[0].caption = text.groupStatus;
        g.cards[1].caption = text.groupMethod;
        g.cards[2].caption = text.groupActions;
        g.cards[3].caption = text.groupLog;
    }
    const wchar_t* labels[] = {text.labelProcess, text.labelMethod, text.labelAutostart, text.labelWatcher,
                               text.labelEntryPoint};
    for (size_t i = 0; i < g.rows.size() && i < 5; ++i) SetText(g.rows[i].label, labels[i]);

    if (g.methodCombo) {
        const int selected = static_cast<int>(SendMessageW(g.methodCombo, CB_GETCURSEL, 0, 0));
        SendMessageW(g.methodCombo, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < 5; ++i) {
            const LRESULT index = SendMessageW(g.methodCombo, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(text.methods[i]));
            SendMessageW(g.methodCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
        }
        SendMessageW(g.methodCombo, CB_SETCURSEL, selected < 0 ? 0 : selected, 0);
    }

    if (g.languageCombo) {
        const int language = static_cast<int>(SendMessageW(g.languageCombo, CB_GETCURSEL, 0, 0));
        SendMessageW(g.languageCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Espa\u00f1ol"));
        SendMessageW(g.languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\u4e2d\u6587"));
        SendMessageW(g.languageCombo, CB_SETCURSEL, language < 0 ? 0 : language, 0);
    }

    UpdateMethodHint();
    BindTips();
    if (g.window) InvalidateRect(g.window, nullptr, TRUE);
    RefreshStatus();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
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
        case IdTheme:
            g.themeMode = static_cast<ThemeMode>((static_cast<int>(g.themeMode) + 1) % 3);
            SaveThemePreference();
            ApplyTheme(g.window, true);
            SetText(g.themeButton, ThemeModeName());
            InvalidateRect(g.themeButton, nullptr, TRUE);
            break;
        case IdMethodCombo:
            if (notification == CBN_SELCHANGE) UpdateMethodHint();
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

// Hover state for the owner-drawn buttons: they do not report mouse moves to
// the parent, so each one is subclassed.
LRESULT CALLBACK ButtonSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                DWORD_PTR) {
    switch (message) {
        case WM_MOUSEMOVE: {
            const int id = GetDlgCtrlID(window);
            if (g.hoverId != id) {
                const int previous = g.hoverId;
                g.hoverId = id;
                InvalidateRect(window, nullptr, TRUE);
                if (previous != 0) {
                    HWND old = GetDlgItem(g.window, previous);
                    if (old) InvalidateRect(old, nullptr, TRUE);
                }
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            break;
        }
        case WM_MOUSELEAVE:
            if (g.hoverId == GetDlgCtrlID(window)) {
                g.hoverId = 0;
                InvalidateRect(window, nullptr, TRUE);
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool IsCardChild(HWND child) {
    if (!child) return false;
    POINT origin{0, 0};
    MapWindowPoints(child, g.window, &origin, 1);
    for (const auto& card : g.cards) {
        if (PtInRect(&card.rect, origin)) return true;
    }
    return false;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g.window = window;
            g.dpi = SystemDpi();
            LoadThemePreference();
            g.theme = ResolveTheme();
            g.windowBrush = CreateSolidBrush(g.theme.window);
            g.cardBrush = CreateSolidBrush(g.theme.card);
            g.editBrush = CreateSolidBrush(g.theme.edit);
            BuildLayout();
            if (g.steamValue) SendMessageW(g.steamValue, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
            if (g.payloadValue) SendMessageW(g.payloadValue, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
            if (g.logView) SendMessageW(g.logView, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
            if (g.languageCombo) {
                SendMessageW(g.languageCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(20));
                SendMessageW(g.languageCombo, CB_SETITEMHEIGHT, 0, Px(20));
            }
            if (g.methodCombo) {
                SendMessageW(g.methodCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(22));
                SendMessageW(g.methodCombo, CB_SETITEMHEIGHT, 0, Px(22));
            }
            BindTips();
            ApplyTheme(window, false);
            ApplyStrings();
            for (HWND button : {g.btnInstall, g.btnUninstall, g.btnInject, g.btnWatch, g.btnAutostart,
                                g.btnRestart, g.btnOpenLog, g.themeButton}) {
                if (button) SetWindowSubclass(button, ButtonSubclass, 1, 0);
            }
            DrainLogFile();
            SetTimer(window, kTimerRefresh, 700, nullptr);
            if (!GetEnvVar(L"OST_GUI_SELFTEST").empty()) SetTimer(window, kTimerSelftest, 2500, nullptr);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(window, &ps);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, g.windowBrush);
            PaintCards(dc);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND child = reinterpret_cast<HWND>(lParam);
            wchar_t className[32] = {};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (IEquals(className, L"Edit") || IEquals(className, L"RichEdit50W")) {
                SetTextColor(dc, g.theme.text);
                SetBkColor(dc, g.theme.edit);
                return reinterpret_cast<LRESULT>(g.editBrush);
            }
            SetBkMode(dc, TRANSPARENT);
            COLORREF color = IsCardChild(child) ? g.theme.text : g.theme.subtle;
            const int id = GetDlgCtrlID(child);
            for (const auto& row : g.rows) {
                if (row.value == child) {
                    color = row.color;
                    break;
                }
                if (row.label == child) {
                    color = g.theme.subtle;
                    break;
                }
            }
            if (id == IdMethodHint || id == IdFooter) color = g.theme.subtle;
            SetTextColor(dc, color);
            return reinterpret_cast<LRESULT>(IsCardChild(child) ? g.cardBrush : g.windowBrush);
        }

        case WM_CTLCOLORLISTBOX: {
            // Drop-down list of the two combo boxes (a separate popup window).
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, g.theme.text);
            SetBkColor(dc, g.theme.card);
            return reinterpret_cast<LRESULT>(g.cardBrush);
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item) return 0;
            if (item->CtlType == ODT_BUTTON) DrawButton(*item);
            else if (item->CtlType == ODT_COMBOBOX) DrawComboItem(*item);
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == kTimerRefresh) {
                RefreshStatus();
                DrainLogFile();
                // Follow Windows when the user switches theme while we run.
                if (g.themeMode == ThemeMode::System) {
                    const bool dark = SystemUsesDarkTheme();
                    if (dark != g.theme.dark) ApplyTheme(window, true);
                }
            } else if (wParam == kTimerSelftest) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;

        case WM_SETTINGCHANGE:
            if (g.themeMode == ThemeMode::System) {
                const bool dark = SystemUsesDarkTheme();
                if (dark != g.theme.dark) ApplyTheme(window, true);
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
    g.dataDir = options.dataDir;

    // System-DPI awareness keeps the fixed layout readable on scaled displays.
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) setContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    }

    g.dpi = SystemDpi();

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = L"OpenSteamToolManager";
    windowClass.hbrBackground = nullptr;
    if (!RegisterClassExW(&windowClass)) return 1;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT desired{0, 0, Px(kClientWidth), Px(kClientHeight)};
    AdjustWindowRectEx(&desired, style, FALSE, WS_EX_APPWINDOW);

    HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName,
                                  (std::wstring(Text().appTitle) + L" " + ToolVersion()).c_str(), style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left,
                                  desired.bottom - desired.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

} // namespace ost
