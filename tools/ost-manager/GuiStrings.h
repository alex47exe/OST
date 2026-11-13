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
        L"Open log folder",
        L"Ready",
        L"Working...",
        L"Steam is running: restart it after installing",
        L"Steam has OpenSteamTool loaded: close Steam to change the entry point",
        L"Steam is running. Restart it now so the change takes effect?",
        {L"dwmapi proxy (bin)", L"version proxy (root)", L"xinput1_4 proxy (root)",
         L"IFEO loader (portable)", L"IFEO loader (Steam root)"},
    };

    static constexpr GuiStrings kSpanish{
        L"OpenSteamTool",
        L"Instalación de Steam",
        L"Carga útil",
        L" Estado ",
        L" Punto de entrada ",
        L" Acciones ",
        L" Registro ",
        L"Steam:",
        L"Carga útil:",
        L"Proceso:",
        L"Método:",
        L"Inicio automático:",
        L"Watcher:",
        L"Punto de entrada:",
        L"puntos de entrada activos: reinstala para dejar solo uno",
        L"no está en ejecución",
        L"carga útil cargada",
        L"carga útil no cargada",
        L"no encontrado",
        L"ausente",
        L"ninguno",
        L"activado",
        L"desactivado",
        L"en ejecución",
        L"detenido",
        L"Instalar",
        L"Desinstalar todo",
        L"Inyectar ahora",
        L"Iniciar watcher",
        L"Detener watcher",
        L"Autostart on",
        L"Autostart off",
        L"Reiniciar Steam",
        L"Abrir carpeta del registro",
        L"Listo",
        L"Trabajando...",
        L"Steam está en ejecución: reinícialo después de instalar",
        L"Steam tiene OpenSteamTool cargado: ciérralo para cambiar el punto de entrada",
        L"Steam está en ejecución. ¿Reiniciarlo ahora para aplicar el cambio?",
        {L"proxy dwmapi (bin)", L"proxy version (raíz)", L"proxy xinput1_4 (raíz)",
         L"cargador IFEO (portátil)", L"cargador IFEO (raíz de Steam)"},
    };

    static constexpr GuiStrings kChinese{
        L"OpenSteamTool",
        L"Steam 安装",
        L"载荷",
        L" 状态 ",
        L" 注入入口 ",
        L" 操作 ",
        L" 日志 ",
        L"Steam：",
        L"载荷：",
        L"进程：",
        L"方式：",
        L"开机启动：",
        L"监视器：",
        L"注入入口：",
        L"个入口同时激活：请重新安装只保留一个",
        L"未运行",
        L"载荷已加载",
        L"载荷未加载",
        L"未找到",
        L"缺失",
        L"无",
        L"已启用",
        L"已禁用",
        L"运行中",
        L"已停止",
        L"安装",
        L"全部卸载",
        L"立即注入",
        L"启动监视器",
        L"停止监视器",
        L"启用开机启动",
        L"禁用开机启动",
        L"重启 Steam",
        L"打开日志目录",
        L"就绪",
        L"正在处理...",
        L"Steam 正在运行：安装后请重启 Steam",
        L"Steam 已加载 OpenSteamTool：请关闭 Steam 再更改注入入口",
        L"Steam 正在运行。现在重启以应用更改吗？",
        {L"dwmapi 代理（bin）", L"version 代理（根目录）", L"xinput1_4 代理（根目录）",
         L"IFEO 加载器（便携）", L"IFEO 加载器（Steam 根目录）"},
    };

    switch (language) {
        case GuiLanguage::Spanish: return kSpanish;
        case GuiLanguage::Chinese: return kChinese;
        case GuiLanguage::English:
        default: return kEnglish;
    }
}

} // namespace ost
