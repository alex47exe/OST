#include "IPCLoader.h"
#include "IPCMessages.gen.h"
#include "OpenSteamToolBuildInfo.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/BuiltinBaseline.h"
#include "Utils/SteamMetadata/MetadataDeriver.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/ProjectInfo.h"

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

        bool HasInterface(std::string_view interfaceName) const
        {
            const auto it = byName.find(std::string(interfaceName));
            return it != byName.end() && !interfaces[it->second].methods.empty();
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
    static std::vector<Interface> ParseInterfaces(std::string_view body, std::string* outError = nullptr)
    {
        std::vector<Interface> out;
        toml::table root;
        try {
            root = toml::parse(body);
        } catch (const toml::parse_error& e) {
            if (outError) *outError = e.description();
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

    // Queue the miss for the single summary popup shown after all loaders ran.
    static void ReportMissing(const std::string& sha256)
    {
        SteamDiagnostics::ReportMissingMetadata({
            "IPC description for steamclient64.dll",
            "ipc\\steamclient\\" + sha256 + ".toml",
            ProjectInfo::MetadataBrowseUrl("ipc", "steamclient"),
        });
    }

} // namespace

constexpr const char* kIPCChannel = "ipc";

// Required interface tables: all our handlers live under these two. Missing either one means the whole
// IPC ticket layer is gone, so it must be treated as a failure and never papered over as success
// (observed: a cross-wired pattern file produces 0 methods + a silent 54).
constexpr const char* kRequiredInterfaces[] = { "IClientUser", "IClientUtils" };

static bool HasRequiredInterfaces()
{
    for (const char* name : kRequiredInterfaces) {
        if (!g_registry.HasInterface(name)) return false;
    }
    return true;
}

// One fetch+parse pass. A cached file that parses but lacks the required
// interfaces is worth dropping and refetching once; a download failure or bad
// remote content is not, so it fails loudly without paying a second timeout.
struct FetchOutcome {
    RemoteToml::Result result;
    std::vector<Interface> ifaces;
    std::string parseErr;
};

static FetchOutcome FetchOnce(const RemoteToml::Request& request)
{
    FetchOutcome out;
    out.result = RemoteToml::Fetch(request);
    if (out.result.ok)
        out.ifaces = ParseInterfaces(out.result.body, &out.parseErr);
    return out;
}

static bool HasRequiredInterfacesIn(const std::vector<Interface>& ifaces)
{
    for (const char* name : kRequiredInterfaces) {
        const bool found = std::any_of(ifaces.begin(), ifaces.end(),
                                       [name](const Interface& i) { return i.name == name; });
        if (!found) return false;
    }
    return true;
}

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle steamclient, const std::string& steamclientPath)
{
    g_registry.Clear();

    const RemoteToml::Request request{ kIPCChannel, "steamclient", steamclientPath };
    FetchOutcome fetched = FetchOnce(request);

    // Self-heal only when the cache is cross-wired: drop it and refetch once (upstream currently serves the right format, curing it in one go).
    // Download failure / bad remote content will not improve by refetching, so fail loudly instead of wasting startup time.
    // Retry only once, to avoid a download loop.
    if (fetched.result.ok && fetched.result.fromCache && !HasRequiredInterfacesIn(fetched.ifaces)) {
        LOG_WARN("IPCLoader: parsed spec missing required interfaces, dropping stale cache and refetching once");
        RemoteToml::Invalidate(request);
        fetched = FetchOnce(request);
    }

    size_t corrected = 0;
    size_t added = 0;
    if (Config::GetRemoteAutoDerive()) {
        bool changed = false;

        // Methods we know about: the fetched file, earlier builds, built-in.
        std::vector<std::vector<Interface>> sources;
        sources.push_back(fetched.ifaces);
        for (const auto& cached : RemoteToml::ReadOtherCached(request, fetched.result.sha256))
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
            auto ifIt = std::find_if(fetched.ifaces.begin(), fetched.ifaces.end(),
                                     [&](const Interface& i) { return i.name == d.iface; });
            if (ifIt == fetched.ifaces.end()) {
                const auto id = EIPCInterfaceFromName(d.iface);
                if (!id) continue;
                Interface fresh;
                fresh.id = *id;
                fresh.name = d.iface;
                fetched.ifaces.push_back(std::move(fresh));
                ifIt = std::prev(fetched.ifaces.end());
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

        if (changed && !fetched.result.sha256.empty()) {
            const std::string header = std::format(
                "# Auto-derived by OpenSteamTool {} for steamclient sha256 {}.\n"
                "# funcHash/fencepost were read from the IPC wrappers in this DLL.\n"
                "# This file is replaced automatically once upstream publishes one.\n",
                OPENSTEAMTOOL_VERSION, fetched.result.sha256);
            RemoteToml::WriteCache(request, fetched.result.sha256,
                                   SerializeInterfaces(fetched.ifaces, header));
        }
    }

    for (auto& iface : fetched.ifaces) g_registry.Add(std::move(iface));

    if (MethodCount() == 0 || !HasRequiredInterfaces()) {
        // Fail loudly: show a popup when the file is missing, never stay silent.
        // Treating 0 methods as success is the root cause of the mysterious 54; never stay silent again.
        if (!fetched.result.ok) {
            if (fetched.result.allNotFound) {
                LOG_ERROR("IPCLoader: IPC spec not published upstream, IPC interception disabled for this session");
            } else {
                LOG_ERROR("IPCLoader: IPC spec download failed (network or bad upstream content), "
                          "IPC interception disabled for this session; will retry next launch");
            }
        } else {
            LOG_ERROR("IPCLoader: IPC spec is missing required interfaces, IPC interception disabled for this session");
        }
        ReportMissing(fetched.result.sha256.empty() ? "(hash failed)" : fetched.result.sha256);
        return false;
    }

    const char* source = !fetched.result.ok ? "locally derived"
                       : fetched.result.fromCache ? "cache" : "remote";
    std::string extra;
    if (corrected) extra += std::format(", {} corrected", corrected);
    if (added) extra += std::format(", {} derived", added);
    LOG_INFO("IPCLoader: loaded {} methods across {} interfaces ({}{})",
             MethodCount(), InterfaceCount(), source, extra);
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
