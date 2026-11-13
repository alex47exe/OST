#pragma once

// GUI labels in the three languages the README set uses. The action log is
// always English because it is the same text the DLL and the CLI produce.

namespace ost {

enum class GuiLanguage { English, Spanish, Chinese, Count = 3 };

struct GuiStrings {
    const wchar_t* appTitle;
    const wchar_t* steamInstall;
    const wchar_t* payload;
    const wchar_t* groupStatus;
    const wchar_t* groupMethod;
    const wchar_t* groupActions;
    const wchar_t* groupLog;

    const wchar_t* labelSteam;
    const wchar_t* labelPayload;
    const wchar_t* labelProcess;
    const wchar_t* labelMethod;
    const wchar_t* labelAutostart;
    const wchar_t* labelWatcher;
    const wchar_t* labelEntryPoint;
    const wchar_t* valueEntryPointsMany;

    const wchar_t* valueNotRunning;
    const wchar_t* valuePayloadLoaded;
    const wchar_t* valuePayloadNotLoaded;
    const wchar_t* valueNotFound;
    const wchar_t* valueMissing;
    const wchar_t* valueNone;
    const wchar_t* valueEnabled;
    const wchar_t* valueDisabled;
    const wchar_t* valueRunning;
    const wchar_t* valueStopped;

    const wchar_t* btnInstall;
    const wchar_t* btnUninstall;
    const wchar_t* btnInject;
    const wchar_t* btnWatchStart;
    const wchar_t* btnWatchStop;
    const wchar_t* btnAutostartOn;
    const wchar_t* btnAutostartOff;
    const wchar_t* btnRestart;
    const wchar_t* btnOpenLog;

    const wchar_t* statusReady;
    const wchar_t* statusBusy;
    const wchar_t* statusSteamRestart;
    const wchar_t* statusSteamLocked;
    const wchar_t* promptRestart;

    // Theme cycle button: "System", "Light", "Dark" (in this order).
    const wchar_t* themeNames[3];

    const wchar_t* tipSteam;
    const wchar_t* tipPayload;
    const wchar_t* tipLanguage;
    const wchar_t* tipTheme;
    const wchar_t* tipMethod;
    const wchar_t* tipInstall;
    const wchar_t* tipUninstall;
    const wchar_t* tipInject;
    const wchar_t* tipWatch;
    const wchar_t* tipAutostart;
    const wchar_t* tipRestart;
    const wchar_t* tipOpenLog;
    const wchar_t* tipLog;
    const wchar_t* tipStatus;

    // One-line explanation per entry point, shown under the picker.
    const wchar_t* methodHints[5];

    const wchar_t* methods[5];
};

inline const GuiStrings& GuiText(GuiLanguage language) {
    static constexpr GuiStrings kEnglish{
        L"OpenSteamTool",
        L"Steam install",
        L"Payload",
        L" Status ",
        L" Entry point ",
        L" Actions ",
        L" Log ",
        L"Steam install:",
        L"Payload:",
        L"Process:",
        L"Method:",
        L"Autostart:",
        L"Watcher:",
        L"Entry point:",
        L"entry points active: install again to keep exactly one",
        L"not running",
        L"payload loaded",
        L"payload not loaded",
        L"not found",
        L"missing",
        L"none",
        L"enabled",
        L"disabled",
        L"running",
        L"stopped",
        L"Install",
        L"Uninstall all",
        L"Inject now",
        L"Start watcher",
        L"Stop watcher",
        L"Autostart on",
        L"Autostart off",
        L"Restart Steam",
        L"Open folder",
        L"Ready",
        L"Working...",
        L"Steam is running: restart it after installing",
        L"Steam has OpenSteamTool loaded: close Steam to change the entry point",
        L"Steam is running. Restart it now so the change takes effect?",
        {L"System", L"Light", L"Dark"},
        L"Folder that contains steam.exe (detected automatically, override with --steam-dir).",
        L"OpenSteamTool.dll that gets loaded. Default: the copy next to ost-manager.exe.",
        L"Interface language.",
        L"Switch between following Windows (System), Light and Dark.",
        L"Only one entry point may be active: installing the selected one removes the others.",
        L"Remove every other entry point, then deploy the selected one.",
        L"Remove the IFEO entry, proxy DLLs, payload copies and autostart.",
        L"Load OpenSteamTool.dll into a running Steam, or start Steam with it.",
        L"Background watcher: inject whenever Steam starts.",
        L"Start the watcher automatically at user logon.",
        L"Close Steam (-shutdown) and start it again.",
        L"Open the folder that holds ost.log and the tool data root.",
        L"Action log. Green = info, yellow = warning, red = error.",
        L"State of Steam, the entry points and the payload.",
        {L"Loaded by bin\\vgui2_s.dll. Works with most anti-cheat; a few games that verify steamclient may crash.",
         L"Loaded by steam.exe first. Enables early isolation, but many anti-cheat systems block it.",
         L"Loaded when the controller layer starts. Same rules as dwmapi, one more entry point.",
         L"Windows starts ost-loader.exe instead of steam.exe; nothing is copied into Steam.",
         L"Like IFEO, with ost-loader.exe and the payload copied into the Steam folder."},
        {L"dwmapi proxy (bin)", L"version proxy (root)", L"xinput1_4 proxy (root)",
         L"IFEO loader (portable)", L"IFEO loader (Steam root)"},
    };

    static constexpr GuiStrings kSpanish{
        L"OpenSteamTool",
        L"Instalaci\u00f3n de Steam",
        L"Carga \u00fatil",
        L" Estado ",
        L" Punto de entrada ",
        L" Acciones ",
        L" Registro ",
        L"Steam:",
        L"Carga \u00fatil:",
        L"Proceso:",
        L"M\u00e9todo:",
        L"Inicio autom\u00e1tico:",
        L"Watcher:",
        L"Punto de entrada:",
        L"puntos de entrada activos: reinstala para dejar solo uno",
        L"no est\u00e1 en ejecuci\u00f3n",
        L"carga \u00fatil cargada",
        L"carga \u00fatil no cargada",
        L"no encontrado",
        L"ausente",
        L"ninguno",
        L"activado",
        L"desactivado",
        L"en ejecuci\u00f3n",
        L"detenido",
        L"Instalar",
        L"Desinstalar todo",
        L"Inyectar ahora",
        L"Iniciar watcher",
        L"Detener watcher",
        L"Autostart on",
        L"Autostart off",
        L"Reiniciar Steam",
        L"Abrir carpeta",
        L"Listo",
        L"Trabajando...",
        L"Steam est\u00e1 en ejecuci\u00f3n: rein\u00edcialo despu\u00e9s de instalar",
        L"Steam tiene OpenSteamTool cargado: ci\u00e9rralo para cambiar el punto de entrada",
        L"Steam est\u00e1 en ejecuci\u00f3n. \u00bfReiniciarlo ahora para aplicar el cambio?",
        {L"Sistema", L"Claro", L"Oscuro"},
        L"Carpeta que contiene steam.exe (se detecta autom\u00e1ticamente; usa --steam-dir para cambiarla).",
        L"OpenSteamTool.dll que se carga. Por defecto: la copia junto a ost-manager.exe.",
        L"Idioma de la interfaz.",
        L"Alterna entre seguir a Windows (Sistema), Claro y Oscuro.",
        L"Solo puede haber un punto de entrada activo: instalar el elegido elimina los dem\u00e1s.",
        L"Elimina los dem\u00e1s puntos de entrada y despliega el elegido.",
        L"Elimina la entrada IFEO, los proxies, las copias del payload y el autostart.",
        L"Carga OpenSteamTool.dll en un Steam en ejecuci\u00f3n, o inicia Steam con ella.",
        L"Watcher en segundo plano: inyecta cada vez que arranca Steam.",
        L"Inicia el watcher autom\u00e1ticamente al iniciar sesi\u00f3n.",
        L"Cierra Steam (-shutdown) y lo vuelve a iniciar.",
        L"Abre la carpeta que contiene ost.log y la ra\u00edz de datos.",
        L"Registro de acciones. Verde = info, amarillo = aviso, rojo = error.",
        L"Estado de Steam, los puntos de entrada y el payload.",
        {L"Lo carga bin\\vgui2_s.dll. Compatible con casi todos los anti-cheat; algunos juegos que verifican steamclient pueden cerrarse.",
         L"Lo carga steam.exe primero. Permite aislamiento temprano, pero muchos anti-cheat lo bloquean.",
         L"Se carga al iniciar la capa de mandos. Mismas reglas que dwmapi, un punto de entrada m\u00e1s.",
         L"Windows inicia ost-loader.exe en lugar de steam.exe; no se copia nada dentro de Steam.",
         L"Como IFEO, copiando ost-loader.exe y el payload a la carpeta de Steam."},
        {L"proxy dwmapi (bin)", L"proxy version (ra\u00edz)", L"proxy xinput1_4 (ra\u00edz)",
         L"cargador IFEO (port\u00e1til)", L"cargador IFEO (ra\u00edz de Steam)"},
    };

    static constexpr GuiStrings kChinese{
        L"OpenSteamTool",
        L"Steam 安装",
        L"载荷",
        L" 状态 ",
        L" 注入入口 ",
        L" 操作 ",
        L" 日志 ",
        L"Steam\uff1a",
        L"载荷\uff1a",
        L"进程\uff1a",
        L"\u65b9\u5f0f\uff1a",
        L"\u5f00\u673a\u542f\u52a8\uff1a",
        L"监视器\uff1a",
        L"\u6ce8\u5165\u5165\u53e3\uff1a",
        L"个入口同时激活：请重新安装只保留一个",
        L"\u672a\u8fd0\u884c",
        L"\u8f7d\u8377\u5df2\u52a0\u8f7d",
        L"\u8f7d\u8377\u672a\u52a0\u8f7d",
        L"\u672a\u627e\u5230",
        L"\u7f3a\u5931",
        L"\u65e0",
        L"\u5df2\u542f\u7528",
        L"\u5df2\u7981\u7528",
        L"\u8fd0\u884c\u4e2d",
        L"\u5df2\u505c\u6b62",
        L"\u5b89\u88c5",
        L"\u5168\u90e8\u5378\u8f7d",
        L"\u7acb\u5373\u6ce8\u5165",
        L"\u542f\u52a8\u76d1\u89c6\u5668",
        L"\u505c\u6b62\u76d1\u89c6\u5668",
        L"\u542f\u7528\u5f00\u673a\u542f\u52a8",
        L"\u7981\u7528\u5f00\u673a\u542f\u52a8",
        L"\u91cd\u542f Steam",
        L"\u6253\u5f00\u76ee\u5f55",
        L"\u5c31\u7eea",
        L"\u6b63\u5728\u5904\u7406...",
        L"Steam \u6b63\u5728\u8fd0\u884c\uff1a\u5b89\u88c5\u540e\u8bf7\u91cd\u542f Steam",
        L"Steam \u5df2\u52a0\u8f7d OpenSteamTool\uff1a\u8bf7\u5173\u95ed Steam \u518d\u66f4\u6539\u6ce8\u5165\u5165\u53e3",
        L"Steam \u6b63\u5728\u8fd0\u884c\u3002\u73b0\u5728\u91cd\u542f\u4ee5\u5e94\u7528\u66f4\u6539\u5417\uff1f",
        {L"\u8ddf\u968f\u7cfb\u7edf", L"\u6d45\u8272", L"\u6df1\u8272"},
        L"\u5305\u542b steam.exe \u7684\u76ee\u5f55\uff08\u81ea\u52a8\u68c0\u6d4b\uff0c\u53ef\u7528 --steam-dir \u6307\u5b9a\uff09\u3002",
        L"\u5c06\u88ab\u52a0\u8f7d\u7684 OpenSteamTool.dll\u3002\u9ed8\u8ba4\uff1aost-manager.exe \u65c1\u7684\u526f\u672c\u3002",
        L"\u754c\u9762\u8bed\u8a00\u3002",
        L"\u5728\u8ddf\u968f\u7cfb\u7edf\uff08\u7cfb\u7edf\uff09\u3001\u6d45\u8272\u548c\u6df1\u8272\u4e4b\u95f4\u5207\u6362\u3002",
        L"\u540c\u4e00\u65f6\u95f4\u53ea\u80fd\u6709\u4e00\u4e2a\u5165\u53e3\uff1a\u5b89\u88c5\u6240\u9009\u5165\u53e3\u4f1a\u79fb\u9664\u5176\u4ed6\u5165\u53e3\u3002",
        L"\u5148\u79fb\u9664\u5176\u4ed6\u6240\u6709\u5165\u53e3\uff0c\u518d\u90e8\u7f72\u6240\u9009\u5165\u53e3\u3002",
        L"\u6e05\u9664 IFEO \u9879\u3001\u4ee3\u7406 DLL\u3001\u8f7d\u8377\u526f\u672c\u548c\u5f00\u673a\u542f\u52a8\u3002",
        L"\u5c06 OpenSteamTool.dll \u6ce8\u5165\u6b63\u5728\u8fd0\u884c\u7684 Steam\uff0c\u6216\u5e26\u7740\u5b83\u542f\u52a8 Steam\u3002",
        L"\u540e\u53f0\u76d1\u89c6\u5668\uff1aSteam \u6bcf\u6b21\u542f\u52a8\u90fd\u6ce8\u5165\u3002",
        L"\u7528\u6237\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8\u76d1\u89c6\u5668\u3002",
        L"\u5173\u95ed Steam\uff08-shutdown\uff09\u5e76\u91cd\u65b0\u542f\u52a8\u3002",
        L"\u6253\u5f00\u5305\u542b ost.log \u548c\u6570\u636e\u6839\u76ee\u5f55\u7684\u6587\u4ef6\u5939\u3002",
        L"\u64cd\u4f5c\u65e5\u5fd7\u3002\u7eff\u8272 = \u4fe1\u606f\uff0c\u9ec4\u8272 = \u8b66\u544a\uff0c\u7ea2\u8272 = \u9519\u8bef\u3002",
        L"Steam\u3001\u5165\u53e3\u548c\u8f7d\u8377\u7684\u72b6\u6001\u3002",
        {L"\u7531 bin\\vgui2_s.dll \u52a0\u8f7d\u3002\u5927\u591a\u6570\u53cd\u4f5c\u5f0a\u4e0d\u62e6\u622a\uff1b\u5c11\u6570\u6821\u9a8c steamclient \u7684\u6e38\u620f\u4f1a\u5d29\u6e83\u3002",
         L"\u7531 steam.exe \u6700\u5148\u52a0\u8f7d\u3002\u53ef\u5b9e\u73b0\u65e9\u6ce8\u9694\u79bb\uff0c\u4f46\u8bb8\u591a\u53cd\u4f5c\u5f0a\u4f1a\u62e6\u622a\u3002",
         L"\u624b\u67c4\u5c42\u521d\u59cb\u5316\u65f6\u52a0\u8f7d\u3002\u89c4\u5219\u540c dwmapi\uff0c\u591a\u4e00\u4e2a\u5165\u53e3\u3002",
         L"Windows \u7528 ost-loader.exe \u4ee3\u66ff steam.exe \u542f\u52a8\uff1b\u4e0d\u5411 Steam \u5185\u590d\u5236\u4efb\u4f55\u6587\u4ef6\u3002",
         L"\u540c IFEO\uff0c\u4f46\u628a ost-loader.exe \u548c\u8f7d\u8377\u590d\u5236\u5230 Steam \u76ee\u5f55\u3002"},
        {L"dwmapi \u4ee3\u7406\uff08bin\uff09", L"version \u4ee3\u7406\uff08\u6839\u76ee\u5f55\uff09", L"xinput1_4 \u4ee3\u7406\uff08\u6839\u76ee\u5f55\uff09",
         L"IFEO \u52a0\u8f7d\u5668\uff08\u4fbf\u643a\uff09", L"IFEO \u52a0\u8f7d\u5668\uff08Steam \u6839\u76ee\u5f55\uff09"},
    };

    switch (language) {
        case GuiLanguage::Spanish: return kSpanish;
        case GuiLanguage::Chinese: return kChinese;
        case GuiLanguage::English:
        default: return kEnglish;
    }
}

} // namespace ost