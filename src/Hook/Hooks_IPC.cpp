#include "dllmain.h"
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Pipe/PipeManager.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/SteamMetadata/IPCLoader.h"

namespace {

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    //  Handler dispatch table
    struct ResolvedHandler {
        EIPCInterface         interfaceID;
        uint32                funcHash;
        std::string           name;       // "IClientUser::GetSteamID" — for logs
        uint32                fencepost;
        uint32                argc;
        IPCHandlerFn          pre;
        IPCHandlerFn          post;

        ResolvedHandler(const IPCHandlerEntry& entry, const IPCLoader::Method& method)
            : interfaceID(method.interfaceID),
              funcHash(method.funcHash),
              name(std::string(entry.interfaceName) + "::" + entry.methodName),
              fencepost(method.fencepost),
              argc(method.argc),
              pre(entry.pre),
              post(entry.post) {}

        std::string DebugString() const {
            return std::format("{} -> hash=0x{:08X} fencepost=0x{:08X} argc={}",
                name, funcHash, fencepost, argc);
        }
    };
    std::vector<ResolvedHandler> g_Handlers;

    // funcHash of IClientUtils::GetAppID, filled in at registration. The onlinefix
    // 480 flip relies on this interface still being dispatched when the appid is
    // not in the Lua list; all other handlers stay gated.
    uint32 g_onlineFixAppIdHash = 0;

    // The packed launcher embeds a legacy function hash that does not match the
    // current Steam client. Treat that value as the ticket method, otherwise the
    // call falls into the unhandled branch and no ticket is obtained.
    uint32 g_legacyTicketHash = 0;
    // Store an index, not a pointer: g_Handlers is a vector and later additions
    // move elements, invalidating pointers. The maximum value means "not registered yet".
    size_t g_legacyTicketIndex = static_cast<size_t>(-1);

    static ResolvedHandler* FindHandler(EIPCInterface iface, uint32 funcHash) {
        for (auto& e : g_Handlers) {
            if (e.interfaceID == iface && e.funcHash == funcHash) return &e;
        }
        // Legacy hash fallback: the packer hardcodes the old value; map it onto
        // the current ticket handler.
        if (funcHash == g_legacyTicketHash && g_legacyTicketIndex < g_Handlers.size()) {
            ResolvedHandler& e = g_Handlers[g_legacyTicketIndex];
            if (e.interfaceID == iface) return &e;
        }
        return nullptr;
    }

    struct IPCDispatch {
        CPipeClient*     pipe = nullptr;
        ResolvedHandler* handler = nullptr;

        bool enabled() const {
            return pipe && handler;
        }

        std::string DebugString() const {
            return std::format("{} {}",pipe ? pipe->DebugString() : "null",
                                handler ? handler->DebugString() : "null");
        }
    };

    static IPCDispatch ResolveDispatch(void* pServer,HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCDispatch dispatch{};
        dispatch.pipe = GetPipe(pServer, hSteamPipe);
        if (!dispatch.pipe) return dispatch;

        // We only care about InterfaceCall messages
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()) return dispatch;
        if (request.command() != EIPCCommand::InterfaceCall) return dispatch;

        // Parse out the interface call header before the appid gate: the
        // onlinefix exemption below needs to know which method this is.
        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return dispatch;

        // Ignore calls when appId is not resolved or not in Lua config.
        // -onlinefix games are exempt, but only for IClientUtils::GetAppID:
        // a genuine copy launched with -onlinefix is not in the Lua list, and
        // gating everything would stop the 480 flip from ever running.
        // Every other handler keeps the gate — letting them through would have
        // the ownership-ticket path forge a pooled ticket over the real one.
        const bool onlineFixAppIdQuery =
            Hooks_Misc::IsOnlineFixActive() &&
            g_onlineFixAppIdHash != 0 &&
            call.interfaceID() == EIPCInterface::IClientUtils &&
            call.funcHash() == g_onlineFixAppIdHash;
        if (!onlineFixAppIdQuery &&
            !LuaConfig::HasDepot(Hooks_Misc::ResolveAppId())) {
            return dispatch;
        }

        // Lookup handler by interface ID + method hash
        dispatch.handler = FindHandler(call.interfaceID(), call.funcHash());
        if (!dispatch.handler) return dispatch;

        LOG_IPC_TRACE("Resolved IPC handler: {}", dispatch.DebugString());
        return dispatch;
    }

    static void HandleHandshake(void* pServer, HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()|| request.command() != EIPCCommand::Handshake) return;
        IPCMessages::IPCHandshakeReq handshake{request.body()};
        if(!handshake.ok()) return;

        CPipeClient* pipe = GetPipe(pServer, hSteamPipe);
        if (!pipe) return;
        // set client PID 
        pipe->m_clientPID = handshake.pid();

        LOG_IPC_DEBUG("Received handshake from {},{}", pipe->DebugString(), handshake.DebugString());
        PipeManager::OnHandshake(pipe);
    }

    // Catch the first time the game uses SteamNetworkingSockets; this decides
    // whether GetAppID flips to 480. The multiplayer session certificate is issued
    // for 480, and switching before P2P would keep the game from loading its real
    // config. With -realappid this only records the fact and leaves the reported
    // value alone.
    static void DetectNetworkingSockets(CUtlBuffer* pRead)
    {
        if (!Hooks_Misc::IsOnlineFixActive() || Hooks_Misc::ShouldReportOnlineFixAppId()) return;

        IPCMessages::IPCRequest request{pRead};
        if (!request.ok() || request.command() != EIPCCommand::InterfaceCall) return;

        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return;

        if (call.interfaceID() == EIPCInterface::IClientNetworkingSocketsSerialized)
            Hooks_Misc::NotifyNetworkingSocketsUsed();
    }

    HOOK_FUNC(IPCProcessMessage, bool,void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        // handle handshake messages
        HandleHandshake(pServer, hSteamPipe, pRead);
        DetectNetworkingSockets(pRead);

        IPCDispatch dispatch = ResolveDispatch(pServer, hSteamPipe, pRead);
        // If we didn't find a handler for this message, just pass through to the original function.
        if(!dispatch.enabled()) return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        // If we did find a handler, run the pre-handler
        if (dispatch.handler->pre)
            dispatch.handler->pre(dispatch.pipe, pRead, pWrite);

        // Then call the original function to let steamclient process the message as normal.
        bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        // Ultimately the post-handler can choose to modify the response.
        if (result && dispatch.handler->post)
            dispatch.handler->post(dispatch.pipe, pRead, pWrite);

        return result;
    }

} // namespace


namespace Hooks_IPC {

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Each module registers a static array. Hash lookup against the
        // IPCLoader metadata happens inside RegisterHandlers.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        LOG_IPC_INFO("Hooks_IPC: {} handlers registered", g_Handlers.size());

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        // The fallback scenario reinstalls the hooks: without clearing the handlers
        // they accumulate (observed 6→12). Dispatch only takes the first match,
        // so behavior is unaffected, but it is a leak and must be cleaned up.
        // Clear the hashes too, otherwise stale values point at entries that no
        // longer exist.
        g_Handlers.clear();
        g_onlineFixAppIdHash = 0;
        g_legacyTicketHash = 0;
        g_legacyTicketIndex = static_cast<size_t>(-1);
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

    void RegisterHandlers(std::span<const IPCHandlerEntry> entries) {
        for (const auto& e : entries) {
            const auto* m = IPCLoader::Find(e.interfaceName, e.methodName);
            if (!m) {
                LOG_IPC_WARN("[Handler Disabled] no IPC spec for {}",e.DebugString());
                continue;
            }
            auto& handler = g_Handlers.emplace_back(e,*m);
            // Record the GetAppID hash so ResolveDispatch can let onlinefix
            // through precisely.
            if (handler.interfaceID == EIPCInterface::IClientUtils &&
                strcmp(e.methodName, "GetAppID") == 0) {
                g_onlineFixAppIdHash = handler.funcHash;
            }
            // Record the ticket method's position and its legacy hash. The packer
            // calls with the old value, so a separate fallback is needed.
            if (handler.interfaceID == EIPCInterface::IClientUser &&
                strcmp(e.methodName, "GetAppOwnershipTicketExtendedData") == 0) {
                g_legacyTicketHash = 0xC7E71244;
                g_legacyTicketIndex = g_Handlers.size() - 1;
                LOG_IPC_INFO("Hooks_IPC: legacy ticket hash 0x{:08X} also accepted",
                             g_legacyTicketHash);
            }
            LOG_IPC_DEBUG("Hooks_IPC: resolved {}", handler.DebugString());
        }
    }

}
