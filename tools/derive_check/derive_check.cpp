// derive_check — offline check and publisher for OpenSteamTool's Steam metadata.
//
// Maps steamclient64.dll / steamui.dll *without running them* (no imports
// resolved, no DllMain) and runs the same MetadataDeriver code the DLL uses
// at startup.
//
//   derive_check <SteamDir> [--no-builtin] [--no-cache]
//       Check mode. Derives metadata for the installed build from the cached
//       TOMLs of *other* builds (what happens after a Steam update) and
//       compares it with <SteamDir>/opensteamtool/... for this build, then
//       validates that cached TOML against the DLLs.
//
//   derive_check --dlls <dir> --metadata <root> --emit [--report <file>]
//                [--issue <file>] [--no-builtin]
//       Publish mode (used by the metadata workflow). <root> holds
//       pattern/<component>/*.toml and ipc/steamclient/*.toml. For each
//       component without a TOML for its current SHA-256, derives one and
//       writes it there. Existing TOMLs are validated, never overwritten.
//       --report gets a Markdown summary; --issue is written only when
//       something could not be derived or failed validation.
//
// Baselines are ordered newest-first by file time (the workflow restores
// file times from git history), capped like the DLL's cache lookup.
//
// Exit code: 0 = ok, 1 = a derived value disagrees with the reference TOML,
// 2 = usage / load error.

#include "OSTPlatform/include/Hash.h"
#include "Utils/SteamMetadata/BuiltinBaseline.h"
#include "Utils/SteamMetadata/MetadataDeriver.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace fs = std::filesystem;
using MetadataDeriver::PatternSet;

namespace {

constexpr size_t kMaxBaselines = 8;   // same cap as RemoteToml::ReadOtherCached

struct Options {
    fs::path dlls;
    fs::path metadata;
    bool emit = false;
    bool useBuiltin = true;
    bool useCache = true;
    fs::path report;
    fs::path issue;
};

struct Component {
    std::string name;      // "steamclient" / "steamui"
    std::string dll;       // file name inside --dlls
    HMODULE module = nullptr;
    std::string sha;
};

int g_wrong = 0;
std::string g_report;                 // Markdown summary
std::vector<std::string> g_problems;  // lines for the issue body

std::string ReadFile(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
}

bool WriteFile(const fs::path& p, const std::string& body)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(ofs);
}

void Out(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    g_report += line + "\n";
}

// Other builds' TOMLs in a directory, newest first.
std::vector<std::string> OtherTomls(const fs::path& dir, const std::string& excludeSha)
{
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (p.extension() != ".toml" || p.stem().string().size() != 64 || p.stem() == excludeSha) continue;
        std::error_code tec;
        files.emplace_back(fs::last_write_time(p, tec), p);
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& [t, p] : files) {
        if (out.size() >= kMaxBaselines) break;
        out.push_back(ReadFile(p));
    }
    return out;
}

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string s;
    for (const auto& n : names) s += (s.empty() ? "" : ", ") + n;
    return s.empty() ? "none" : s;
}

// ---- pattern -------------------------------------------------------------------

void ProcessPatterns(const Options& opt, const Component& c)
{
    const fs::path dir = opt.metadata / "pattern" / c.name;
    const fs::path refPath = dir / (c.sha + ".toml");
    const bool haveRef = fs::exists(refPath);
    const PatternSet ref = haveRef ? MetadataDeriver::ParsePatternToml(ReadFile(refPath)) : PatternSet{};

    std::vector<PatternSet> baselines;
    if (opt.useCache)
        for (const auto& body : OtherTomls(dir, c.sha)) baselines.push_back(MetadataDeriver::ParsePatternToml(body));
    if (opt.useBuiltin) baselines.push_back(MetadataDeriver::ParsePatternToml(BuiltinBaseline::Pattern(c.name)));

    Out(std::format("\n### pattern/{} `{}`", c.name, c.sha));
    Out(std::format("baselines: {}, published: {}", baselines.size(), haveRef ? "yes" : "no"));

    if (haveRef) {
        const auto v = MetadataDeriver::ReconcilePatterns(c.module, ref, {}, false);
        Out(std::format("published TOML validation: {} ok, {} rejected ({})",
                        v.validated, v.rejected.size(), JoinNames(v.rejected)));
        if (!v.rejected.empty())
            g_problems.push_back(std::format("- `pattern/{}/{}.toml` fails validation: {}",
                                             c.name, c.sha, JoinNames(v.rejected)));
        if (opt.emit) return;   // never overwrite published metadata
    }

    const auto out = MetadataDeriver::ReconcilePatterns(c.module, {}, baselines, true);
    for (const auto& [key, spec] : out.entries) {
        const auto r = ref.find(key);
        const char* tag = r == ref.end() ? "new  " : (r->second.rva == spec.rva ? "ok   " : "WRONG");
        if (r != ref.end() && r->second.rva != spec.rva) ++g_wrong;
        Out(std::format("    {} {:<32} 0x{:<8X} {}", tag, spec.name, spec.rva, spec.note));
    }
    for (const auto& n : out.underivable) Out(std::format("    skip  {:<32} (not derivable safely)", n));
    Out(std::format("derived {}, not derived {} ({})", out.derived, out.underivable.size(), JoinNames(out.underivable)));

    if (opt.emit && !out.entries.empty()) {
        const std::string header = std::format(
            "# Derived by the OpenSteamTool metadata workflow for {} sha256 {}.\n"
            "# Entries were carried forward from earlier builds and checked against\n"
            "# function boundaries in this DLL. Not derived: {}.\n",
            c.dll, c.sha, JoinNames(out.underivable));
        WriteFile(refPath, MetadataDeriver::SerializePatternToml(out.entries, header));
        Out(std::format("wrote {}", refPath.generic_string()));
    }
    if (opt.emit && !out.underivable.empty())
        g_problems.push_back(std::format("- `{}` ({}): could not derive {}", c.dll, c.sha, JoinNames(out.underivable)));
}

// ---- IPC -----------------------------------------------------------------------

struct IpcSpec {
    int interfaceId = 0;
    uint32_t argc = 0;
};

void CollectIpc(const std::string& body, std::vector<MetadataDeriver::IpcMethodRequest>& wanted,
                std::map<std::string, int>& ifaceIds, std::map<std::string, uint32_t>& argcOf)
{
    toml::table tbl;
    try { tbl = toml::parse(body); } catch (...) { return; }
    for (auto& [iface, v] : tbl) {
        if (!v.is_table()) continue;
        const std::string ifaceName(iface.str());
        if (auto id = (*v.as_table())["interface_id"].value<int64_t>()) ifaceIds.emplace(ifaceName, static_cast<int>(*id));
        for (auto& [method, m] : *v.as_table()) {
            if (!m.is_table()) continue;
            const std::string key = ifaceName + "." + std::string(method.str());
            if (argcOf.count(key)) continue;
            argcOf[key] = static_cast<uint32_t>((*m.as_table())["argc"].value<int64_t>().value_or(0));
            wanted.push_back({ifaceName, std::string(method.str())});
        }
    }
}

void ProcessIpc(const Options& opt, const Component& client)
{
    const fs::path dir = opt.metadata / "ipc" / "steamclient";
    const fs::path refPath = dir / (client.sha + ".toml");
    const bool haveRef = fs::exists(refPath);

    std::vector<MetadataDeriver::IpcMethodRequest> wanted;
    std::map<std::string, int> ifaceIds;
    std::map<std::string, uint32_t> argcOf;
    if (haveRef) CollectIpc(ReadFile(refPath), wanted, ifaceIds, argcOf);
    if (opt.useCache)
        for (const auto& body : OtherTomls(dir, client.sha)) CollectIpc(body, wanted, ifaceIds, argcOf);
    CollectIpc(std::string(BuiltinBaseline::kSteamclientIpc), wanted, ifaceIds, argcOf);

    toml::table ref;
    if (haveRef) {
        try { ref = toml::parse(ReadFile(refPath)); } catch (...) {}
    }

    Out(std::format("\n### ipc/steamclient `{}`", client.sha));
    Out(std::format("methods wanted: {}, published: {}", wanted.size(), haveRef ? "yes" : "no"));

    const auto found = MetadataDeriver::DeriveIpcMethods(client.module, wanted);
    std::set<std::string> foundKeys;
    for (const auto& f : found) {
        foundKeys.insert(f.iface + "." + f.method);
        const char* tag = "new  ";
        if (auto t = ref[f.iface][f.method].as_table()) {
            const auto hash = (*t)["funcHash"].value<std::string>().value_or("0");
            const auto fp = (*t)["fencepost"].value<std::string>().value_or("0");
            const bool ok = std::stoul(hash, nullptr, 16) == f.funcHash && std::stoul(fp, nullptr, 16) == f.fencepost;
            tag = ok ? "ok   " : "WRONG";
            if (!ok) {
                ++g_wrong;
                g_problems.push_back(std::format("- published IPC {}::{} disagrees with the DLL", f.iface, f.method));
            }
        }
        Out(std::format("    {} {}::{} hash=0x{:08X} fencepost=0x{:08X} wrapper=0x{:X} slot {}", tag, f.iface,
                        f.method, f.funcHash, f.fencepost, f.wrapperRva, f.methodIndex));
    }
    std::vector<std::string> missing;
    for (const auto& w : wanted)
        if (!foundKeys.count(w.iface + "." + w.method)) missing.push_back(w.iface + "::" + w.method);
    Out(std::format("derived {} of {} (not derived: {})", found.size(), wanted.size(), JoinNames(missing)));

    if (!opt.emit || haveRef) return;
    if (!missing.empty())
        g_problems.push_back(std::format("- IPC ({}): could not derive {}", client.sha, JoinNames(missing)));
    if (found.empty()) return;

    // Same layout as IPCLoader's serialiser and steam-monitor's files.
    std::string body = std::format(
        "# Derived by the OpenSteamTool metadata workflow for steamclient64.dll sha256 {}.\n"
        "# funcHash/fencepost were read from the IPC wrappers in this DLL.\n", client.sha);
    std::map<std::string, std::vector<const MetadataDeriver::IpcMethodFound*>> byIface;
    for (const auto& f : found) byIface[f.iface].push_back(&f);
    for (const auto& [iface, methods] : byIface) {
        body += std::format("\n[{}]\ninterface_id = {}\n", iface, ifaceIds.count(iface) ? ifaceIds[iface] : 0);
        if (methods.front()->vtableRva) body += std::format("vtable_rva = \"0x{:X}\"\n", methods.front()->vtableRva);
        for (const auto* m : methods) {
            body += std::format("\n[{}.{}]\nmethod_index = {}\nfuncHash = \"0x{:08X}\"\nwrapper_rva = \"0x{:X}\"\n"
                                "fencepost = \"0x{:08X}\"\nargc = {}\n",
                                iface, m->method, m->methodIndex, m->funcHash, m->wrapperRva, m->fencepost,
                                argcOf[iface + "." + m->method]);
        }
    }
    WriteFile(refPath, body);
    Out(std::format("wrote {}", refPath.generic_string()));
}

bool LoadComponent(const Options& opt, Component& c)
{
    const fs::path path = opt.dlls / c.dll;
    c.module = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!c.module) {
        std::fprintf(stderr, "could not map %s (error %lu)\n", path.string().c_str(), GetLastError());
        return false;
    }
    c.sha = OSTPlatform::Hash::Sha256OfFile(path);
    return !c.sha.empty();
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> fs::path { return i + 1 < argc ? fs::path(argv[++i]) : fs::path(); };
        if (a == "--no-builtin") opt.useBuiltin = false;
        else if (a == "--no-cache") opt.useCache = false;
        else if (a == "--emit") opt.emit = true;
        else if (a == "--dlls") opt.dlls = next();
        else if (a == "--metadata") opt.metadata = next();
        else if (a == "--report") opt.report = next();
        else if (a == "--issue") opt.issue = next();
        else positional.push_back(a);
    }
    if (!positional.empty()) {
        opt.dlls = positional[0];
        opt.metadata = fs::path(positional[0]) / "opensteamtool";
    }
    if (opt.dlls.empty() || opt.metadata.empty()) {
        std::fprintf(stderr,
                     "usage: derive_check <SteamDir> [--no-builtin] [--no-cache]\n"
                     "       derive_check --dlls <dir> --metadata <root> [--emit] [--report <file>] "
                     "[--issue <file>] [--no-builtin]\n");
        return 2;
    }

    Component client{"steamclient", "steamclient64.dll"};
    Component ui{"steamui", "steamui.dll"};
    if (!LoadComponent(opt, client) || !LoadComponent(opt, ui)) return 2;

    Out(std::format("## Steam metadata for steamclient64.dll `{}` / steamui.dll `{}`", client.sha.substr(0, 16),
                    ui.sha.substr(0, 16)));
    ProcessPatterns(opt, client);
    ProcessPatterns(opt, ui);
    ProcessIpc(opt, client);

    Out(std::format("\nresult: {}", g_wrong ? "WRONG VALUES" : "no wrong values"));
    if (!opt.report.empty()) WriteFile(opt.report, g_report);
    if (!opt.issue.empty() && !g_problems.empty()) {
        std::string body = std::format(
            "The metadata workflow could not fully cover Steam build with steamclient64.dll `{}` and "
            "steamui.dll `{}`.\n\n",
            client.sha, ui.sha);
        for (const auto& p : g_problems) body += p + "\n";
        body += "\nUsers on this build get a popup naming these functions; the hooks that need them are "
                "disabled until a signature is added by hand to the `pattern` / `ipc` branch.\n";
        WriteFile(opt.issue, body);
    }
    return g_wrong ? 1 : 0;
}
