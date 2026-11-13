// version.dll HiJack Project - loader-stage agent.
//
// steam.exe's static import table lists VERSION.dll, so once this proxy is placed in the Steam
// directory it enters the process during loader initialization — earlier than Steam's first
// load of steamclient64.dll. OpenSteamTool can therefore set up its redirect view ahead of that
// first load (first-load hijack + handle redirection). This module runs on steam.exe's startup
// critical path: all exports must be forwarded and DllMain must never fail, otherwise static
// import resolution fails and Steam simply won't start.

#include <windows.h>

// ─── 1. Real system version.dll binding ──────────────────────────
// Must bind the real module by its full System32 path: LoadLibrary("version.dll") by name hits this module.
static HMODULE g_hRealVersion = nullptr;

static FARPROC g_pGetFileVersionInfoA = nullptr;
static FARPROC g_pGetFileVersionInfoByHandle = nullptr;
static FARPROC g_pGetFileVersionInfoExA = nullptr;
static FARPROC g_pGetFileVersionInfoExW = nullptr;
static FARPROC g_pGetFileVersionInfoSizeA = nullptr;
static FARPROC g_pGetFileVersionInfoSizeExA = nullptr;
static FARPROC g_pGetFileVersionInfoSizeExW = nullptr;
static FARPROC g_pGetFileVersionInfoSizeW = nullptr;
static FARPROC g_pGetFileVersionInfoW = nullptr;
static FARPROC g_pVerFindFileA = nullptr;
static FARPROC g_pVerFindFileW = nullptr;
static FARPROC g_pVerInstallFileA = nullptr;
static FARPROC g_pVerInstallFileW = nullptr;
static FARPROC g_pVerLanguageNameA = nullptr;
static FARPROC g_pVerLanguageNameW = nullptr;
static FARPROC g_pVerQueryValueA = nullptr;
static FARPROC g_pVerQueryValueW = nullptr;

static void EnsureRealVersion()
{
    // magic static: initialized exactly once and thread-safe; concurrent first calls from
    // several threads still load the real module only once.
    static const bool ready = [] {
        char realPath[MAX_PATH + 16] = {};
        if (!GetSystemDirectoryA(realPath, MAX_PATH))
            return false;
        lstrcatA(realPath, "\\version.dll");
        g_hRealVersion = LoadLibraryA(realPath);
        if (!g_hRealVersion)
            return false;
        const struct { const char* Name; FARPROC* Slot; } entries[] = {
            { "GetFileVersionInfoA", &g_pGetFileVersionInfoA },
            { "GetFileVersionInfoByHandle", &g_pGetFileVersionInfoByHandle },
            { "GetFileVersionInfoExA", &g_pGetFileVersionInfoExA },
            { "GetFileVersionInfoExW", &g_pGetFileVersionInfoExW },
            { "GetFileVersionInfoSizeA", &g_pGetFileVersionInfoSizeA },
            { "GetFileVersionInfoSizeExA", &g_pGetFileVersionInfoSizeExA },
            { "GetFileVersionInfoSizeExW", &g_pGetFileVersionInfoSizeExW },
            { "GetFileVersionInfoSizeW", &g_pGetFileVersionInfoSizeW },
            { "GetFileVersionInfoW", &g_pGetFileVersionInfoW },
            { "VerFindFileA", &g_pVerFindFileA },
            { "VerFindFileW", &g_pVerFindFileW },
            { "VerInstallFileA", &g_pVerInstallFileA },
            { "VerInstallFileW", &g_pVerInstallFileW },
            { "VerLanguageNameA", &g_pVerLanguageNameA },
            { "VerLanguageNameW", &g_pVerLanguageNameW },
            { "VerQueryValueA", &g_pVerQueryValueA },
            { "VerQueryValueW", &g_pVerQueryValueW },
        };
        for (const auto& e : entries)
            *e.Slot = GetProcAddress(g_hRealVersion, e.Name);
        return true;
    }();
    (void)ready;
}

// ─── 2. Forwarded exports (ordinals match the system version.dll, names identical) ─────
#pragma comment(linker, "/EXPORT:GetFileVersionInfoA,@1")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle,@2")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA,@3")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW,@4")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA,@5")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA,@6")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW,@7")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW,@8")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW,@9")
#pragma comment(linker, "/EXPORT:VerFindFileA,@10")
#pragma comment(linker, "/EXPORT:VerFindFileW,@11")
#pragma comment(linker, "/EXPORT:VerInstallFileA,@12")
#pragma comment(linker, "/EXPORT:VerInstallFileW,@13")
#pragma comment(linker, "/EXPORT:VerLanguageNameA,@14")
#pragma comment(linker, "/EXPORT:VerLanguageNameW,@15")
#pragma comment(linker, "/EXPORT:VerQueryValueA,@16")
#pragma comment(linker, "/EXPORT:VerQueryValueW,@17")

extern "C" BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoA) return FALSE;
    return reinterpret_cast<decltype(&GetFileVersionInfoA)>(g_pGetFileVersionInfoA)(lptstrFilename, dwHandle, dwLen, lpData);
}

extern "C" BOOL WINAPI GetFileVersionInfoByHandle(DWORD dwFlags, HANDLE hFile, LPVOID* lplpData, PDWORD pdwLen)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoByHandle) return FALSE;
    return reinterpret_cast<decltype(&GetFileVersionInfoByHandle)>(g_pGetFileVersionInfoByHandle)(dwFlags, hFile, lplpData, pdwLen);
}

extern "C" BOOL WINAPI GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoExA) return FALSE;
    return reinterpret_cast<decltype(&GetFileVersionInfoExA)>(g_pGetFileVersionInfoExA)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

extern "C" BOOL WINAPI GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoExW) return FALSE;
    return reinterpret_cast<decltype(&GetFileVersionInfoExW)>(g_pGetFileVersionInfoExW)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoSizeA) return 0;
    return reinterpret_cast<decltype(&GetFileVersionInfoSizeA)>(g_pGetFileVersionInfoSizeA)(lptstrFilename, lpdwHandle);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoSizeExA) return 0;
    return reinterpret_cast<decltype(&GetFileVersionInfoSizeExA)>(g_pGetFileVersionInfoSizeExA)(dwFlags, lpwstrFilename, lpdwHandle);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoSizeExW) return 0;
    return reinterpret_cast<decltype(&GetFileVersionInfoSizeExW)>(g_pGetFileVersionInfoSizeExW)(dwFlags, lpwstrFilename, lpdwHandle);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoSizeW) return 0;
    return reinterpret_cast<decltype(&GetFileVersionInfoSizeW)>(g_pGetFileVersionInfoSizeW)(lptstrFilename, lpdwHandle);
}

extern "C" BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    EnsureRealVersion();
    if (!g_pGetFileVersionInfoW) return FALSE;
    return reinterpret_cast<decltype(&GetFileVersionInfoW)>(g_pGetFileVersionInfoW)(lptstrFilename, dwHandle, dwLen, lpData);
}

extern "C" DWORD WINAPI VerFindFileA(DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir,
                                     LPSTR szCurDir, PUINT puCurDirLen, LPSTR szDestDir, PUINT puDestDirLen)
{
    EnsureRealVersion();
    if (!g_pVerFindFileA) return 0;
    return reinterpret_cast<decltype(&VerFindFileA)>(g_pVerFindFileA)(uFlags, szFileName, szWinDir, szAppDir,
                                                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

extern "C" DWORD WINAPI VerFindFileW(DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir,
                                     LPWSTR szCurDir, PUINT puCurDirLen, LPWSTR szDestDir, PUINT puDestDirLen)
{
    EnsureRealVersion();
    if (!g_pVerFindFileW) return 0;
    return reinterpret_cast<decltype(&VerFindFileW)>(g_pVerFindFileW)(uFlags, szFileName, szWinDir, szAppDir,
                                                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

extern "C" DWORD WINAPI VerInstallFileA(DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName,
                                        LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir,
                                        LPSTR szTmpFile, PUINT puTmpFileLen)
{
    EnsureRealVersion();
    if (!g_pVerInstallFileA) return 0;
    return reinterpret_cast<decltype(&VerInstallFileA)>(g_pVerInstallFileA)(uFlags, szSrcFileName, szDestFileName,
                                                                            szSrcDir, szDestDir, szCurDir,
                                                                            szTmpFile, puTmpFileLen);
}

extern "C" DWORD WINAPI VerInstallFileW(DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName,
                                        LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir,
                                        LPWSTR szTmpFile, PUINT puTmpFileLen)
{
    EnsureRealVersion();
    if (!g_pVerInstallFileW) return 0;
    return reinterpret_cast<decltype(&VerInstallFileW)>(g_pVerInstallFileW)(uFlags, szSrcFileName, szDestFileName,
                                                                            szSrcDir, szDestDir, szCurDir,
                                                                            szTmpFile, puTmpFileLen);
}

extern "C" DWORD WINAPI VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang)
{
    EnsureRealVersion();
    if (!g_pVerLanguageNameA) return 0;
    return reinterpret_cast<decltype(&VerLanguageNameA)>(g_pVerLanguageNameA)(wLang, szLang, cchLang);
}

extern "C" DWORD WINAPI VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang)
{
    EnsureRealVersion();
    if (!g_pVerLanguageNameW) return 0;
    return reinterpret_cast<decltype(&VerLanguageNameW)>(g_pVerLanguageNameW)(wLang, szLang, cchLang);
}

extern "C" BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    EnsureRealVersion();
    if (!g_pVerQueryValueA) return FALSE;
    return reinterpret_cast<decltype(&VerQueryValueA)>(g_pVerQueryValueA)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

extern "C" BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    EnsureRealVersion();
    if (!g_pVerQueryValueW) return FALSE;
    return reinterpret_cast<decltype(&VerQueryValueW)>(g_pVerQueryValueW)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

// ─── 3. OpenSteamTool injection ───────────────────────────────────
// Inject only when the host process is steam.exe (case-insensitive); other processes pass through.
static void OpenSteamToolLoad(HMODULE selfModule)
{
    char exePath[MAX_PATH] = {};
    const DWORD exeLen = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    // If the host name is unavailable or the path was truncated (unreliable filename), skip
    // injection: better to miss an injection than to kill the wrong process.
    if (exeLen == 0 || exeLen >= MAX_PATH)
        return;
    const char* exeName = strrchr(exePath, '\\');
    exeName = exeName ? exeName + 1 : exePath;
    if (_stricmp(exeName, "steam.exe") != 0)
        return;

    // Locate the payload via this module's own directory: no reliance on DLL search order, and
    // same-named files planted elsewhere (CWD/PATH) can't hijack it. Payload follows the proxy.
    char selfPath[MAX_PATH + 16] = {};
    const DWORD selfLen = GetModuleFileNameA(selfModule, selfPath, MAX_PATH);
    if (selfLen == 0 || selfLen >= MAX_PATH)
        return;
    char* slash = strrchr(selfPath, '\\');
    if (!slash)
        return;
    lstrcpyA(slash + 1, "OpenSteamTool.dll");
    LoadLibraryA(selfPath);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // This module is a static import of steam.exe: returning FALSE here fails process init outright.
        // If OpenSteamTool can't load (blocked by AV, corrupted copy) we only skip injection and let Steam run.
        OpenSteamToolLoad(hModule);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
