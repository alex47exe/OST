// dwmapi.dll HiJack Project - loader-stage agent (anti-cheat friendly entry point).
//
// Same duty as the version.dll proxy: bring OpenSteamTool.dll into the steam.exe process.
// The difference is the load path — version is a static import of steam.exe and enters first
// with the loader; dwmapi comes in through bin\vgui2_s.dll's static import when Steam starts its UI.
//
// This module sits on Steam's critical load path: every exported function must be forwarded
// verbatim to the system dwmapi, otherwise modules that depend on it (vgui2_s calls
// DwmExtendFrameIntoClientArea / DwmSetWindowAttribute) hit missing symbols, the whole module
// fails to load, and Steam won't start at all (manifesting as VGUI_Setup failed).
//
// Why not use MSVC's /EXPORT:Name=DWMAPI.Name forwarders: the loader resolves the recorded target
// module name "DWMAPI" to the module dwmapi.dll, which is this very module — it hits itself in the
// application directory and forms a self-loop, so vgui2_s always fails to load. Hence we resolve
// the real system dwmapi addresses at runtime via GetProcAddress and forward each one by hand.

#include <windows.h>

// The real system module must be bound by absolute path: LoadLibrary("dwmapi.dll") by name hits this module itself.
static HMODULE g_hRealDwmapi = nullptr;

// Real addresses of each system dwmapi export. A missing entry only makes the call site fall back
// to failure; the forwarding path must never throw or crash the host — this module is a link in
// Steam's static import chain. DllCanUnloadNow / DllGetClassObject are deliberately not exported:
// dwmapi's COM objects resolve via the registry to the System32 original and never reach this copy,
// so exporting them would only clash with the existing combaseapi.h declarations (REFCLSID/REFIID).
static FARPROC p_DwmAttachMilContent = nullptr;
static FARPROC p_DwmDefWindowProc = nullptr;
static FARPROC p_DwmDetachMilContent = nullptr;
static FARPROC p_DwmEnableBlurBehindWindow = nullptr;
static FARPROC p_DwmEnableComposition = nullptr;
static FARPROC p_DwmEnableMMCSS = nullptr;
static FARPROC p_DwmExtendFrameIntoClientArea = nullptr;
static FARPROC p_DwmFlush = nullptr;
static FARPROC p_DwmGetColorizationColor = nullptr;
static FARPROC p_DwmGetCompositionTimingInfo = nullptr;
static FARPROC p_DwmGetGraphicsStreamClient = nullptr;
static FARPROC p_DwmGetGraphicsStreamTransformHint = nullptr;
static FARPROC p_DwmGetTransportAttributes = nullptr;
static FARPROC p_DwmGetUnmetTabRequirements = nullptr;
static FARPROC p_DwmGetWindowAttribute = nullptr;
static FARPROC p_DwmInvalidateIconicBitmaps = nullptr;
static FARPROC p_DwmIsCompositionEnabled = nullptr;
static FARPROC p_DwmModifyPreviousDxFrameDuration = nullptr;
static FARPROC p_DwmQueryThumbnailSourceSize = nullptr;
static FARPROC p_DwmRegisterThumbnail = nullptr;
static FARPROC p_DwmRenderGesture = nullptr;
static FARPROC p_DwmSetDxFrameDuration = nullptr;
static FARPROC p_DwmSetIconicLivePreviewBitmap = nullptr;
static FARPROC p_DwmSetIconicThumbnail = nullptr;
static FARPROC p_DwmSetPresentParameters = nullptr;
static FARPROC p_DwmSetWindowAttribute = nullptr;
static FARPROC p_DwmShowContact = nullptr;
static FARPROC p_DwmTetherContact = nullptr;
static FARPROC p_DwmTransitionOwnedWindow = nullptr;
static FARPROC p_DwmUnregisterThumbnail = nullptr;
static FARPROC p_DwmUpdateThumbnailProperties = nullptr;

static void EnsureRealDwmapi()
{
    // magic static: initialized exactly once and thread-safe; concurrent first calls from
    // several threads still load the real module only once.
    static const bool ready = [] {
        char realPath[MAX_PATH + 16] = {};
        if (!GetSystemDirectoryA(realPath, MAX_PATH))
            return false;
        lstrcatA(realPath, "\\dwmapi.dll");
        g_hRealDwmapi = LoadLibraryA(realPath);
        if (!g_hRealDwmapi)
            return false;
        const struct { const char* Name; FARPROC* Slot; } entries[] = {
            { "DwmAttachMilContent", &p_DwmAttachMilContent },
            { "DwmDefWindowProc", &p_DwmDefWindowProc },
            { "DwmDetachMilContent", &p_DwmDetachMilContent },
            { "DwmEnableBlurBehindWindow", &p_DwmEnableBlurBehindWindow },
            { "DwmEnableComposition", &p_DwmEnableComposition },
            { "DwmEnableMMCSS", &p_DwmEnableMMCSS },
            { "DwmExtendFrameIntoClientArea", &p_DwmExtendFrameIntoClientArea },
            { "DwmFlush", &p_DwmFlush },
            { "DwmGetColorizationColor", &p_DwmGetColorizationColor },
            { "DwmGetCompositionTimingInfo", &p_DwmGetCompositionTimingInfo },
            { "DwmGetGraphicsStreamClient", &p_DwmGetGraphicsStreamClient },
            { "DwmGetGraphicsStreamTransformHint", &p_DwmGetGraphicsStreamTransformHint },
            { "DwmGetTransportAttributes", &p_DwmGetTransportAttributes },
            { "DwmGetUnmetTabRequirements", &p_DwmGetUnmetTabRequirements },
            { "DwmGetWindowAttribute", &p_DwmGetWindowAttribute },
            { "DwmInvalidateIconicBitmaps", &p_DwmInvalidateIconicBitmaps },
            { "DwmIsCompositionEnabled", &p_DwmIsCompositionEnabled },
            { "DwmModifyPreviousDxFrameDuration", &p_DwmModifyPreviousDxFrameDuration },
            { "DwmQueryThumbnailSourceSize", &p_DwmQueryThumbnailSourceSize },
            { "DwmRegisterThumbnail", &p_DwmRegisterThumbnail },
            { "DwmRenderGesture", &p_DwmRenderGesture },
            { "DwmSetDxFrameDuration", &p_DwmSetDxFrameDuration },
            { "DwmSetIconicLivePreviewBitmap", &p_DwmSetIconicLivePreviewBitmap },
            { "DwmSetIconicThumbnail", &p_DwmSetIconicThumbnail },
            { "DwmSetPresentParameters", &p_DwmSetPresentParameters },
            { "DwmSetWindowAttribute", &p_DwmSetWindowAttribute },
            { "DwmShowContact", &p_DwmShowContact },
            { "DwmTetherContact", &p_DwmTetherContact },
            { "DwmTransitionOwnedWindow", &p_DwmTransitionOwnedWindow },
            { "DwmUnregisterThumbnail", &p_DwmUnregisterThumbnail },
            { "DwmUpdateThumbnailProperties", &p_DwmUpdateThumbnailProperties },
        };
        for (const auto& e : entries)
            *e.Slot = GetProcAddress(g_hRealDwmapi, e.Name);
        return true;
    }();
    (void)ready;
}

// Forwarding macro: declares the export with the system dwmapi's original signature and casts to a
// same-signature function pointer on call. Struct pointers are always void*/const void*: on x64
// arguments travel purely at pointer width, so the ABI is identical and Dwmapi's internal struct
// definitions need not be pulled in. Export names and ordinals are in dwmapi.def.
#define OST_FORWARD_DWM(Ret, Name, Params, Args, FailValue)               \
    extern "C" Ret WINAPI Name Params                                     \
    {                                                                     \
        EnsureRealDwmapi();                                               \
        if (!p_##Name) return FailValue;                                  \
        return reinterpret_cast<Ret(WINAPI*)Params>(p_##Name) Args;       \
    }

OST_FORWARD_DWM(HRESULT, DwmAttachMilContent, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(BOOL, DwmDefWindowProc,
                (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* plResult),
                (hwnd, msg, wParam, lParam, plResult), FALSE)
OST_FORWARD_DWM(HRESULT, DwmDetachMilContent, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableBlurBehindWindow, (HWND hwnd, const void* pBlurBehind),
                (hwnd, pBlurBehind), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableComposition, (UINT uCompositionAction), (uCompositionAction), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableMMCSS, (BOOL fEnableMMCSS), (fEnableMMCSS), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmExtendFrameIntoClientArea, (HWND hwnd, const void* pMarInset),
                (hwnd, pMarInset), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmFlush, (), (), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetColorizationColor, (DWORD* pcrColorization, BOOL* pfOpaqueBlend),
                (pcrColorization, pfOpaqueBlend), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetCompositionTimingInfo, (HWND hwnd, void* pTimingInfo),
                (hwnd, pTimingInfo), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetGraphicsStreamClient, (UINT uIndex, void* pClientUuid),
                (uIndex, pClientUuid), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetGraphicsStreamTransformHint, (UINT uIndex, void* pTransform),
                (uIndex, pTransform), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetTransportAttributes,
                (BOOL* pfIsRemoting, BOOL* pfIsConnected, DWORD* pDwGeneration),
                (pfIsRemoting, pfIsConnected, pDwGeneration), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetUnmetTabRequirements, (HWND appWindow, void* value),
                (appWindow, value), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetWindowAttribute,
                (HWND hwnd, DWORD dwAttribute, void* pvAttribute, DWORD cbAttribute),
                (hwnd, dwAttribute, pvAttribute, cbAttribute), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmInvalidateIconicBitmaps, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmIsCompositionEnabled, (BOOL* pfEnabled), (pfEnabled), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmModifyPreviousDxFrameDuration,
                (HWND hwnd, INT cRefreshes, BOOL fRelative), (hwnd, cRefreshes, fRelative), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmQueryThumbnailSourceSize, (HANDLE hThumbnail, void* pSize),
                (hThumbnail, pSize), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmRegisterThumbnail,
                (HWND hwndDestination, HWND hwndSource, HANDLE* phThumbnailId),
                (hwndDestination, hwndSource, phThumbnailId), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmRenderGesture,
                (int gt, UINT cContacts, const DWORD* pdwPointerID, const POINT* pPoints),
                (gt, cContacts, pdwPointerID, pPoints), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetDxFrameDuration, (HWND hwnd, INT cRefreshes),
                (hwnd, cRefreshes), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetIconicLivePreviewBitmap,
                (HWND hwnd, HBITMAP hbmp, void* pptClient, DWORD dwSITFlags),
                (hwnd, hbmp, pptClient, dwSITFlags), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetIconicThumbnail, (HWND hwnd, HBITMAP hbmp, DWORD dwSITFlags),
                (hwnd, hbmp, dwSITFlags), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetPresentParameters, (HWND hwnd, void* pPresentParams),
                (hwnd, pPresentParams), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetWindowAttribute,
                (HWND hwnd, DWORD dwAttribute, const void* pvAttribute, DWORD cbAttribute),
                (hwnd, dwAttribute, pvAttribute, cbAttribute), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmShowContact, (DWORD dwPointerID, DWORD eShowContact),
                (dwPointerID, eShowContact), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmTetherContact, (DWORD dwPointerID, BOOL fEnable, POINT ptTether),
                (dwPointerID, fEnable, ptTether), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmTransitionOwnedWindow, (HWND hwnd, int target), (hwnd, target), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmUnregisterThumbnail, (HANDLE hThumbnailId), (hThumbnailId), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmUpdateThumbnailProperties, (HANDLE hThumbnailId, const void* ptnProperties),
                (hThumbnailId, ptnProperties), E_FAIL)

#undef OST_FORWARD_DWM

// ─── OpenSteamTool injection ──────────────────────────────────────
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
        // This module is a link in Steam's load chain: returning FALSE here drags the calling
        // module's load down with it. If OpenSteamTool can't load (blocked by AV, corrupted copy)
        // we only skip injection and let Steam run.
        OpenSteamToolLoad(hModule);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
