// derive_check — offline check of OpenSteamTool's local metadata derivation.
//
// Maps steamclient64.dll / steamui.dll from a Steam install *without running
// them* (no imports resolved, no DllMain), then runs the same MetadataDeriver
// code the DLL uses at startup:
//
//   * derive:   pattern + IPC metadata for the installed build, using only the
//               cached TOMLs of *other* builds (and optionally the built-in
//               baseline) — i.e. exactly what happens after a Steam update.
//   * compare:  results against <Steam>/opensteamtool/{pattern,ipc}/.../<sha>.toml
//               for the installed build, when one exists (e.g. hand-verified).
//   * validate: that same cached TOML against the mapped DLLs.
//
// Usage: derive_check <SteamDir> [--no-builtin] [--no-cache]
// Exit code: 0 = no wrong values, 1 = a derived value disagrees with the
// reference TOML, 2 = usage / load error.

#include "OSTPlatform/include/Hash.h"
#include "Utils/SteamMetadata/BuiltinBaseline.h"
#include "Utils/SteamMetadata/MetadataDeriver.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace fs = std::filesystem;
using MetadataDeriver::PatternSet;

namespace {

std::string ReadFile(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
}

// Other builds' TOMLs in a cache dir, newest first.
std::vector<std::string> OtherCached(const fs::path& dir, const std::string& excludeSha)
{
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (p.extension() != ".toml" || p.stem().string().size() != 64 || p.stem() == excludeSha) continue;
        files.emplace_back(fs::last_write_time(p, ec), p);
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& [t, p] : files) out.push_back(ReadFile(p));
    return out;
}

int g_wrong = 0;

void CheckPatterns(HMODULE module, const fs::path& steam, const std::string& component,
                   const std::string& sha, bool useCache, bool useBuiltin)
{
    const fs::path dir = steam / "opensteamtool" / "pattern" / component;
    std::vector<PatternSet> baselines;
    if (useCache)
        for (const auto& body : OtherCached(dir, sha)) baselines.push_back(MetadataDeriver::ParsePatternToml(body));
    if (useBuiltin) baselines.push_back(MetadataDeriver::ParsePatternToml(BuiltinBaseline::Pattern(component)));

    const fs::path refPath = dir / (sha + ".toml");
    const PatternSet ref = fs::exists(refPath) ? MetadataDeriver::ParsePatternToml(ReadFile(refPath)) : PatternSet{};

    std::printf("\n== %s (%s...)  baselines=%zu  reference=%s\n", component.c_str(), sha.substr(0, 12).c_str(),
                baselines.size(), ref.empty() ? "none" : "cached TOML");

    const auto out = MetadataDeriver::ReconcilePatterns(module, {}, baselines, true);
    for (const auto& [key, spec] : out.entries) {
        const auto r = ref.find(key);
        const char* tag = r == ref.end() ? "NEW  " : (r->second.rva == spec.rva ? "OK   " : "WRONG");
        if (r != ref.end() && r->second.rva != spec.rva) ++g_wrong;
        std::printf("  %s %-32s 0x%-8X %s\n", tag, spec.name.c_str(), spec.rva, spec.note.c_str());
    }
    for (const auto& n : out.underivable) std::printf("  SKIP  %-32s (not derivable safely)\n", n.c_str());
    std::printf("  derived %zu, skipped %zu\n", out.derived, out.underivable.size());

    if (!ref.empty()) {
        const auto v = MetadataDeriver::ReconcilePatterns(module, ref, {}, false);
        std::printf("  reference TOML validation: %zu ok, %zu rejected", v.validated, v.rejected.size());
        for (const auto& n : v.rejected) std::printf(" [%s]", n.c_str());
        std::printf("\n");
    }
}

void CheckIpc(HMODULE module, const fs::path& steam, const std::string& sha)
{
    std::vector<MetadataDeriver::IpcMethodRequest> wanted;
    const auto builtin = toml::parse(BuiltinBaseline::kSteamclientIpc);
    for (auto& [iface, v] : builtin) {
        if (!v.is_table()) continue;
        for (auto& [method, m] : *v.as_table())
            if (m.is_table()) wanted.push_back({std::string(iface.str()), std::string(method.str())});
    }

    const fs::path refPath = steam / "opensteamtool" / "ipc" / "steamclient" / (sha + ".toml");
    toml::table ref;
    if (fs::exists(refPath)) {
        try { ref = toml::parse(ReadFile(refPath)); } catch (...) {}
    }

    std::printf("\n== ipc  reference=%s\n", ref.empty() ? "none" : "cached TOML");
    const auto found = MetadataDeriver::DeriveIpcMethods(module, wanted);
    for (const auto& f : found) {
        const char* tag = "NEW  ";
        if (auto t = ref[f.iface][f.method].as_table()) {
            const auto hash = (*t)["funcHash"].value<std::string>().value_or("");
            const auto fp = (*t)["fencepost"].value<std::string>().value_or("");
            const bool ok = std::stoul(hash, nullptr, 16) == f.funcHash && std::stoul(fp, nullptr, 16) == f.fencepost;
            tag = ok ? "OK   " : "WRONG";
            if (!ok) ++g_wrong;
        }
        std::printf("  %s %s::%s hash=0x%08X fencepost=0x%08X wrapper=0x%X vtable=0x%X slot=%u\n", tag,
                    f.iface.c_str(), f.method.c_str(), f.funcHash, f.fencepost, f.wrapperRva, f.vtableRva,
                    f.methodIndex);
    }
    std::printf("  derived %zu of %zu\n", found.size(), wanted.size());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: derive_check <SteamDir> [--no-builtin] [--no-cache]\n");
        return 2;
    }
    const fs::path steam = argv[1];
    bool useBuiltin = true, useCache = true;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-builtin") useBuiltin = false;
        if (std::string(argv[i]) == "--no-cache") useCache = false;
    }

    const fs::path clientPath = steam / "steamclient64.dll";
    const fs::path uiPath = steam / "steamui.dll";
    HMODULE client = LoadLibraryExW(clientPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE ui = LoadLibraryExW(uiPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!client || !ui) {
        std::fprintf(stderr, "could not map Steam DLLs from %s (error %lu)\n", steam.string().c_str(), GetLastError());
        return 2;
    }

    const std::string clientSha = OSTPlatform::Hash::Sha256OfFile(clientPath);
    const std::string uiSha = OSTPlatform::Hash::Sha256OfFile(uiPath);

    CheckPatterns(client, steam, "steamclient", clientSha, useCache, useBuiltin);
    CheckPatterns(ui, steam, "steamui", uiSha, useCache, useBuiltin);
    CheckIpc(client, steam, clientSha);

    std::printf("\n%s\n", g_wrong ? "RESULT: WRONG VALUES DERIVED" : "RESULT: no wrong values");
    return g_wrong ? 1 : 0;
}
