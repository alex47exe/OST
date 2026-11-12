#include "PatternLoader.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "OpenSteamToolBuildInfo.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/BuiltinBaseline.h"
#include "Utils/SteamMetadata/MetadataDeriver.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


// ---- compile-time sanity checks for FNV-1a table keys ----
// If the steam-monitor bot uses the same algorithm these must hold.
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u,
              "FNV-1a mismatch for BBuildAndAsyncSendFrame");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu,
              "FNV-1a mismatch for BuildDepotDependency");

namespace {

// ---- per-function pattern record ----
struct PatternEntry {
    std::string name;
    uintptr_t   rva = 0;   // 0 = not present in file
    std::string sig;        // empty = not present in file
};

// key = Fnv1aHash(funcName)
using PatternMap = std::unordered_map<uint32_t, PatternEntry>;

// module → its pattern map
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternMap> g_moduleMaps;

// Modules whose Load() call failed (popup already shown). FindPattern
// silently returns nullptr for these — without re-logging or adding the
// function to g_missingFunctions — so we don't follow one "TOML missing"
// popup with a second popup listing every dependent hook.
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_failedModules;

// functions whose names were not found during FindPattern
static std::vector<std::string> g_missingFunctions;

// ---- byte-pattern scanner ----

static bool ParseSig(const std::string& str,
                     std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& mask)
{
    bytes.clear();
    mask.clear();
    for (const char* p = str.c_str(); *p; ) {
        if (*p == ' ' || *p == '\t' || *p == ',') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0); mask.push_back(0); p += 2; continue;
        }
        char hi = p[0], lo = p[1];
        if (!hi || !lo) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

static void* ScanModule(OSTPlatform::DynamicLibrary::ModuleHandle module,
                        const std::vector<uint8_t>& bytes,
                        const std::vector<uint8_t>& mask)
{
    const auto image = OSTPlatform::Memory::GetModuleImage(module);
    if (!image) return nullptr;

    auto* base = image->base;
    size_t size = image->size;
    size_t patLen = bytes.size();
    if (size < patLen) return nullptr;

    for (size_t i = 0; i <= size - patLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && base[i + j] != bytes[j]) { found = false; break; }
        }
        if (found) return base + i;
    }
    return nullptr;
}

} // namespace

// ---- public API ----

namespace PatternLoader {

    constexpr const char* kPatternChannel = "pattern";

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component)
{
    namespace fs = std::filesystem;

    // Delegate fetch + cache + mirror fallback to RemoteToml.
    const RemoteToml::Request request{
        kPatternChannel,
        component,
        dllPath,
    };
    RemoteToml::Result r = RemoteToml::Fetch(request);

    MetadataDeriver::PatternSet current;
    if (r.ok) {
        std::string parseErr;
        current = MetadataDeriver::ParsePatternToml(r.body, &parseErr);
        if (current.empty()) {
            LOG_WARN("PatternLoader: TOML for {} parsed empty ({})",
                     component, parseErr.empty() ? "no entries" : parseErr);
        }
    }

    // Validate every entry against the loaded module (a stale or mistyped RVA
    // would otherwise be called/detoured blindly), and derive what is missing
    // or invalid from earlier builds' metadata.
    const bool derive = Config::GetRemoteAutoDerive();
    std::vector<MetadataDeriver::PatternSet> baselines;
    if (derive) {
        for (const auto& cached : RemoteToml::ReadOtherCached(request, r.sha256)) {
            auto set = MetadataDeriver::ParsePatternToml(cached.body);
            if (!set.empty()) baselines.push_back(std::move(set));
        }
        auto builtin = MetadataDeriver::ParsePatternToml(BuiltinBaseline::Pattern(component));
        if (!builtin.empty()) baselines.push_back(std::move(builtin));
    }

    auto outcome = MetadataDeriver::ReconcilePatterns(module, current, baselines, derive);
    LOG_INFO("PatternLoader: {}: {} validated, {} derived, {} rejected, {} underivable",
             component, outcome.validated, outcome.derived, outcome.rejected.size(),
             outcome.underivable.size());

    if (derive && outcome.changed && outcome.derived > 0 && !r.sha256.empty()) {
        const std::string header = std::format(
            "# Auto-derived by OpenSteamTool {} for {} sha256 {}.\n"
            "# Entries were carried forward from earlier builds and checked against\n"
            "# function boundaries in this DLL. Not derived: {}.\n"
            "# This file is replaced automatically once steam-monitor publishes one.\n",
            OPENSTEAMTOOL_VERSION, component, r.sha256,
            outcome.underivable.empty() ? std::string("none") : [&] {
                std::string s;
                for (const auto& n : outcome.underivable) s += (s.empty() ? "" : ", ") + n;
                return s;
            }());
        RemoteToml::WriteCache(request, r.sha256,
                               MetadataDeriver::SerializePatternToml(outcome.entries, header));
    }

    if (!outcome.entries.empty()) {
        PatternMap map;
        for (auto& [key, spec] : outcome.entries) {
            PatternEntry e;
            e.name = std::move(spec.name);
            e.rva  = spec.rva;
            e.sig  = std::move(spec.sig);
            map[key] = std::move(e);
        }
        LOG_INFO("PatternLoader: loaded {} patterns for {} ({})",
                 map.size(), component,
                 outcome.derived ? "locally derived" : (r.fromCache ? "cache fallback" : "remote"));
        g_moduleMaps[module] = std::move(map);
        return true;
    }

    // Total failure — queue for the single summary popup (shown by the caller
    // once every loader has run) + disable module's hooks.
    std::string dllName = fs::path(dllPath).filename().string();
    std::string sha     = r.sha256.empty() ? "(hash failed)" : r.sha256;
    SteamDiagnostics::ReportMissingMetadata({
        dllName + " signatures",
        std::string(kPatternChannel) + "\\" + component + "\\" + sha + ".toml",
        "https://github.com/OpenSteam001/steam-monitor/tree/" +
            std::string(kPatternChannel) + "/" + component,
    });
    g_failedModules.insert(module);
    return false;
}

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    // If the whole module's pattern file failed to load, stay quiet — the
    // user already gets the missing-metadata popup and main.log has the warning.
    // No point amplifying that into one log line per hook plus a second
    // "missing functions" popup later.
    if (g_failedModules.count(module)) {
        return nullptr;
    }

    uint32_t key = Fnv1aHash(funcName);

    auto mapIt = g_moduleMaps.find(module);
    if (mapIt == g_moduleMaps.end()) {
        // Load() was never called for this module.
        LOG_WARN("PatternLoader: FindPattern called for module that was never loaded "
                 "('{}')", funcName);
        g_missingFunctions.emplace_back(funcName);
        return nullptr;
    }

    auto& map = mapIt->second;
    auto entryIt = map.find(key);
    if (entryIt == map.end()) {
        LOG_WARN("PatternLoader: no entry for '{}' (key=0x{:08X})", funcName, key);
        g_missingFunctions.emplace_back(funcName);
        return nullptr;
    }

    const PatternEntry& entry = entryIt->second;

    // Priority 1: RVA direct offset
    if (entry.rva != 0) {
        void* addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(module) + entry.rva);
        LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X}", funcName, entry.rva);
        return addr;
    }

    // Priority 2: byte-signature scan
    if (!entry.sig.empty()) {
        std::vector<uint8_t> bytes, mask;
        if (ParseSig(entry.sig, bytes, mask)) {
            void* addr = ScanModule(module, bytes, mask);
            if (addr) {
                uintptr_t rva = reinterpret_cast<uintptr_t>(addr) -
                                reinterpret_cast<uintptr_t>(module);
                LOG_DEBUG("PatternLoader: {} resolved via sig @ RVA 0x{:X}",
                          funcName, rva);
                return addr;
            }
            LOG_WARN("PatternLoader: sig scan miss for '{}' (pattern parsed OK, "
                     "no match in module image)", funcName);
        } else {
            LOG_WARN("PatternLoader: malformed sig for '{}': '{}'",
                     funcName, entry.sig);
        }
    } else {
        LOG_WARN("PatternLoader: entry for '{}' has neither rva nor sig", funcName);
    }

    g_missingFunctions.emplace_back(funcName);
    return nullptr;
}

void ReportMissingFunctions()
{
    if (g_missingFunctions.empty()) return;

    // Build the list
    std::string list;
    for (const auto& name : g_missingFunctions)
        list += "  - " + name + "\n";
    g_missingFunctions.clear();

    SteamDiagnostics::ShowWarning(
        "OpenSteamTool - Missing Signatures",
        "OpenSteamTool: some functions could not be located.\n\n"
        "The following functions were not found in the signature file:\n" +
        list +
        "\nHooks for these functions are disabled for this session.\n\n"
        "Please report this at:\n"
        "https://github.com/OpenSteam001/OpenSteamTool/issues");
}

} // namespace PatternLoader
