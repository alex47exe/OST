#include "IPCLoader.h"
#include "IPCMessages.gen.h"
#include "OSTPlatform/include/Numbers.h"
#include "OpenSteamToolBuildInfo.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/BuiltinBaseline.h"
#include "Utils/SteamMetadata/MetadataDeriver.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <set>
#include <unordered_map>
#include <utility>

#include <toml++/toml.hpp>

namespace IPCLoader {

namespace {

    struct Registry {
        std::vector<Interface> interfaces;
        std::unordered_map<EIPCInterface, size_t> byID;
        std::unordered_map<std::string, size_t> byName;

        void Clear()
        {
            interfaces.clear();
            byID.clear();
            byName.clear();
        }

        void Add(Interface iface)
        {
            const size_t index = interfaces.size();
            byID[iface.id] = index;
            byName[iface.name] = index;
            interfaces.push_back(std::move(iface));
        }

        const Method* Find(EIPCInterface interfaceID, uint32_t funcHash) const
        {
            const auto it = byID.find(interfaceID);
            if (it == byID.end()) return nullptr;

            for (const auto& method : interfaces[it->second].methods) {
                if (method.funcHash == funcHash) return &method;
            }
            return nullptr;
        }

        const Method* Find(std::string_view interfaceName,
                           std::string_view methodName) const
        {
            const auto it = byName.find(std::string(interfaceName));
            if (it == byName.end()) return nullptr;

            for (const auto& method : interfaces[it->second].methods) {
                if (method.name == methodName) return &method;
            }
            return nullptr;
        }

        size_t MethodCount() const
        {
            size_t count = 0;
            for (const auto& iface : interfaces)
                count += iface.methods.size();
            return count;
        }
    };

    Registry g_registry;

    // ---- TOML helpers ----

    static bool ParseHexU32(std::string_view s, uint32_t& out)
    {
        const auto parsed = OSTPlatform::Numbers::ParseHexUInt32(s);
        if (!parsed) return false;
        out = *parsed;
        return true;
    }

    static bool ParseInterfaceTable(std::string_view name,
                                    const toml::table& tbl,
                                    Interface& out)
    {
        out.name = std::string(name);

        const auto expected = EIPCInterfaceFromName(name);
        if (!expected) {
            LOG_WARN("IPCLoader: [{}] is not declared in EIPCInterface", name);
            return false;
        }
        out.id = *expected;

        // Backward-compatible metadata: EIPCInterface is authoritative.
        if (auto v = tbl["interface_id"].value<int64_t>()) {
            if (*v < 0 || *v > 0xFF) {
                LOG_WARN("IPCLoader: [{}] interface_id out of range ({})", name, *v);
                return false;
            }
            if (static_cast<EIPCInterface>(*v) != out.id) {
                LOG_WARN("IPCLoader: [{}] interface_id {} disagrees with generated EIPCInterface value {}",
                         name, *v, static_cast<uint8_t>(out.id));
                return false;
            }
        }

        if (auto v = tbl["vtable_rva"].value<std::string>())
            ParseHexU32(*v, out.vtableRva);

        // Walk dotted sub-tables — each is a method.
        for (auto& [methodKey, methodVal] : tbl) {
            if (!methodVal.is_table()) continue;
            const auto& mtbl = *methodVal.as_table();

            Method m;
            m.interfaceID = out.id;
            m.name = std::string(methodKey.str());

            if (auto v = mtbl["funcHash"].value<std::string>()) {
                if (!ParseHexU32(*v, m.funcHash)) {
                    LOG_WARN("IPCLoader: [{}.{}] bad funcHash '{}'",
                             name, m.name, *v);
                    continue;
                }
            } else {
                LOG_WARN("IPCLoader: [{}.{}] missing funcHash", name, m.name);
                continue;
            }

            if (auto v = mtbl["fencepost"].value<std::string>())
                ParseHexU32(*v, m.fencepost);
            if (auto v = mtbl["argc"].value<int64_t>())
                m.argc = static_cast<uint32_t>(*v);
            if (auto v = mtbl["wrapper_rva"].value<std::string>())
                ParseHexU32(*v, m.wrapperRva);
            if (auto v = mtbl["method_index"].value<int64_t>())
                m.methodIndex = static_cast<uint32_t>(*v);
            out.methods.push_back(std::move(m));
        }
        return true;
    }

    // Parse a whole IPC TOML into interfaces (invalid tables are skipped).
    static std::vector<Interface> ParseInterfaces(std::string_view body)
    {
        std::vector<Interface> out;
        toml::table root;
        try {
            root = toml::parse(body);
        } catch (const toml::parse_error& e) {
            LOG_WARN("IPCLoader: TOML parse error: {}", e.description());
            return out;
        }
        for (auto& [key, val] : root) {
            if (!val.is_table()) continue;
            Interface iface;
            if (ParseInterfaceTable(key.str(), *val.as_table(), iface))
                out.push_back(std::move(iface));
        }
        return out;
    }

    static std::string SerializeInterfaces(const std::vector<Interface>& ifaces, std::string_view header)
    {
        std::string out(header);
        for (const auto& iface : ifaces) {
            if (!out.empty()) out += '\n';
            out += std::format("[{}]\ninterface_id = {}\n", iface.name, static_cast<int>(iface.id));
            if (iface.vtableRva) out += std::format("vtable_rva = \"0x{:X}\"\n", iface.vtableRva);
            for (const auto& m : iface.methods) {
                out += std::format("\n[{}.{}]\n", iface.name, m.name);
                if (m.methodIndex) out += std::format("method_index = {}\n", m.methodIndex);
                out += std::format("funcHash = \"0x{:08X}\"\n", m.funcHash);
                if (m.wrapperRva) out += std::format("wrapper_rva = \"0x{:X}\"\n", m.wrapperRva);
                out += std::format("fencepost = \"0x{:08X}\"\n", m.fencepost);
                out += std::format("argc = {}\n", m.argc);
            }
        }
        return out;
    }

    // Queue for the single summary popup shown after all loaders have run.
    static void ReportMissing(const std::string& sha256)
    {
        SteamDiagnostics::ReportMissingMetadata({
            "IPC spec for steamclient64.dll",
            "ipc\\steamclient\\" + sha256 + ".toml",
            "https://github.com/OpenSteam001/steam-monitor/tree/ipc/steamclient",
        });
    }

} // namespace

constexpr const char* kIPCChannel = "ipc";

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle steamclient, const std::string& steamclientPath)
{
    g_registry.Clear();

    const RemoteToml::Request request{
        kIPCChannel,
        "steamclient",
        steamclientPath,
    };
    RemoteToml::Result r = RemoteToml::Fetch(request);

    std::vector<Interface> ifaces;
    if (r.ok) ifaces = ParseInterfaces(r.body);

    size_t corrected = 0, added = 0;
    if (Config::GetRemoteAutoDerive()) {
        bool changed = false;

        // Methods we know about: the fetched file, earlier builds, built-in.
        std::vector<std::vector<Interface>> sources;
        sources.push_back(ifaces);
        for (const auto& cached : RemoteToml::ReadOtherCached(request, r.sha256))
            sources.push_back(ParseInterfaces(cached.body));
        sources.push_back(ParseInterfaces(BuiltinBaseline::kSteamclientIpc));

        std::vector<MetadataDeriver::IpcMethodRequest> wanted;
        std::unordered_map<std::string, uint32_t> argcOf;   // "Iface.Method" -> argc
        std::set<std::string> seen;
        for (const auto& src : sources) {
            for (const auto& iface : src) {
                for (const auto& m : iface.methods) {
                    const std::string id = iface.name + "." + m.name;
                    if (!seen.insert(id).second) continue;
                    wanted.push_back({iface.name, m.name});
                    argcOf[id] = m.argc;
                }
            }
        }

        for (const auto& d : MetadataDeriver::DeriveIpcMethods(steamclient, wanted)) {
            auto ifIt = std::find_if(ifaces.begin(), ifaces.end(),
                                     [&](const Interface& i) { return i.name == d.iface; });
            if (ifIt == ifaces.end()) {
                const auto id = EIPCInterfaceFromName(d.iface);
                if (!id) continue;
                Interface fresh;
                fresh.id = *id;
                fresh.name = d.iface;
                ifaces.push_back(std::move(fresh));
                ifIt = std::prev(ifaces.end());
            }
            if (d.vtableRva && ifIt->vtableRva != d.vtableRva) {
                ifIt->vtableRva = d.vtableRva;
                changed = true;
            }

            auto mIt = std::find_if(ifIt->methods.begin(), ifIt->methods.end(),
                                    [&](const Method& m) { return m.name == d.method; });
            if (mIt == ifIt->methods.end()) {
                Method m;
                m.interfaceID = ifIt->id;
                m.name = d.method;
                m.funcHash = d.funcHash;
                m.fencepost = d.fencepost;
                m.argc = argcOf[d.iface + "." + d.method];
                m.wrapperRva = d.wrapperRva;
                m.methodIndex = d.methodIndex;
                ifIt->methods.push_back(std::move(m));
                ++added;
                changed = true;
                continue;
            }
            if (mIt->funcHash != d.funcHash || mIt->fencepost != d.fencepost) {
                LOG_WARN("IPCLoader: {}::{} metadata disagrees with steamclient "
                         "(hash 0x{:08X} -> 0x{:08X}, fencepost 0x{:08X} -> 0x{:08X}); using derived values",
                         d.iface, d.method, mIt->funcHash, d.funcHash, mIt->fencepost, d.fencepost);
                mIt->funcHash = d.funcHash;
                mIt->fencepost = d.fencepost;
                ++corrected;
                changed = true;
            }
            if (mIt->wrapperRva != d.wrapperRva || mIt->methodIndex != d.methodIndex) {
                mIt->wrapperRva = d.wrapperRva;
                mIt->methodIndex = d.methodIndex;
                changed = true;
            }
        }

        if (changed && !r.sha256.empty()) {
            const std::string header = std::format(
                "# Auto-derived by OpenSteamTool {} for steamclient sha256 {}.\n"
                "# funcHash/fencepost were read from the IPC wrappers in this DLL.\n"
                "# This file is replaced automatically once steam-monitor publishes one.\n",
                OPENSTEAMTOOL_VERSION, r.sha256);
            RemoteToml::WriteCache(request, r.sha256, SerializeInterfaces(ifaces, header));
        }
    }

    for (auto& iface : ifaces) g_registry.Add(std::move(iface));

    if (MethodCount() == 0) {
        ReportMissing(r.sha256.empty() ? "(hash failed)" : r.sha256);
        return false;
    }

    LOG_INFO("IPCLoader: loaded {} methods across {} interfaces ({}; {} corrected, {} derived)",
             MethodCount(), InterfaceCount(),
             !r.ok ? "derived" : (r.fromCache ? "cache fallback" : "remote"), corrected, added);
    return true;
}

const Method* Find(EIPCInterface interfaceID, uint32_t funcHash)
{
    return g_registry.Find(interfaceID, funcHash);
}

const Method* Find(std::string_view ifaceName, std::string_view methodName)
{
    return g_registry.Find(ifaceName, methodName);
}

size_t InterfaceCount() 
{ 
    return g_registry.interfaces.size();
}

size_t MethodCount() {
    return g_registry.MethodCount();
}

} // namespace IPCLoader
