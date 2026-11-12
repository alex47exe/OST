#include "RemoteToml.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/ProjectInfo.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace RemoteToml {

namespace {
    struct Source {
        std::string origin;
        std::string urlTemplate;
    };

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<Source> BuildSources()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (remoteUrlTemplate.empty()) {
            std::vector<Source> sources;
            for (const auto& s : ProjectInfo::kMetadataSources)
                sources.push_back({s.origin, s.urlTemplate});
            return sources;
        }

        if (!IsValidTemplate(remoteUrlTemplate)) {
            LOG_WARN("RemoteToml: remote.url_template must contain "
                     "{channel}, {component}, and {sha256}; remote fetch disabled");
            return {};
        }

        return { {"custom", remoteUrlTemplate} };
    }

    static std::filesystem::path CacheDirFor(const Request& request)
    {
        const std::filesystem::path steamRoot = std::filesystem::path(request.dllPath).parent_path();
        return steamRoot / "opensteamtool" / request.channel / request.component;
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache path & dir.
    fs::path cacheDir  = CacheDirFor(request);
    fs::path cachePath = cacheDir / (out.sha256 + ".toml");
    const std::string cachePathText = cachePath.string();

    std::error_code mkdirEc;
    fs::create_directories(cacheDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("RemoteToml({}/{}): could not create cache dir {} ({})",
                 request.channel, request.component, cacheDir.string(), mkdirEc.message());
    }

    // 3. Try remote sources in order. Mirrors of one origin serve the same
    //    data, so a 404 skips the rest of that origin and moves to the next.
    const std::vector<Source> sources = BuildSources();
    OSTPlatform::Http::Result http;
    std::string lastUrl;
    std::string skipOrigin;

    for (const auto& source : sources) {
        if (source.origin == skipOrigin) continue;
        lastUrl = ExpandTemplate(source.urlTemplate, request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                          nullptr, 0, nullptr);

        if (http.ok && http.status == 200) break;

        if (http.ok && http.status == 404) {
            LOG_WARN("RemoteToml({}/{}): {} has no such file (HTTP 404): {}",
                     request.channel, request.component, source.origin, lastUrl);
            skipOrigin = source.origin;
            continue;
        }

        LOG_WARN("RemoteToml({}/{}): source failed ({} ok={} HTTP={}), falling back",
                 request.channel, request.component, lastUrl, http.ok, http.status);
    }

    // 4. Remote OK → write cache, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        std::ofstream ofs(cachePath, std::ios::binary);
        if (ofs) {
            ofs.write(http.body.data(),
                      static_cast<std::streamsize>(http.body.size()));
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePathText);
        } else {
            LOG_WARN("RemoteToml({}/{}): could not open {} for writing",
                     request.channel, request.component, cachePathText);
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Remote failed → fall back to whatever is cached for this exact SHA.
    if (fs::exists(cachePath)) {
        LOG_WARN("RemoteToml({}/{}): remote failed (last URL {} HTTP {}); "
                 "falling back to local cache {}",
                 request.channel, request.component,
                 lastUrl.empty() ? "<none>" : lastUrl, http.status, cachePathText);

        std::ifstream ifs(cachePath, std::ios::binary);
        if (ifs) {
            std::string buf((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
            if (!buf.empty()) {
                out.body = std::move(buf);
                out.ok = true;
                out.fromCache = true;
                return out;
            }
            LOG_WARN("RemoteToml({}/{}): cache file empty: {}",
                     request.channel, request.component, cachePathText);
        } else {
            LOG_WARN("RemoteToml({}/{}): could not open cache file: {}",
                     request.channel, request.component, cachePathText);
        }
    }

    // 6. Total failure — caller handles popup / degraded mode.
    LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
             request.channel, request.component,
             lastUrl.empty() ? "<none>" : lastUrl, http.status);
    return out;
}

std::vector<CachedBody> ReadOtherCached(const Request& request,
                                        std::string_view excludeSha256,
                                        size_t maxCount)
{
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;

    std::error_code ec;
    for (fs::directory_iterator it(CacheDirFor(request), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".toml") continue;
        const std::string stem = p.stem().string();
        if (stem.size() != 64 || stem == excludeSha256) continue;
        std::error_code tec;
        const auto t = fs::last_write_time(p, tec);
        if (tec) continue;
        files.emplace_back(t, p);
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<CachedBody> out;
    for (const auto& [t, p] : files) {
        if (out.size() >= maxCount) break;
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) continue;
        std::string buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (buf.empty()) continue;
        out.push_back({p.stem().string(), std::move(buf)});
    }
    return out;
}

bool WriteCache(const Request& request, std::string_view sha256, std::string_view body)
{
    namespace fs = std::filesystem;
    if (sha256.empty() || body.empty()) return false;

    const fs::path dir = CacheDirFor(request);
    std::error_code ec;
    fs::create_directories(dir, ec);

    const fs::path finalPath = dir / (std::string(sha256) + ".toml");
    const fs::path tmpPath   = dir / (std::string(sha256) + ".toml.tmp");
    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            LOG_WARN("RemoteToml({}/{}): could not open {} for writing",
                     request.channel, request.component, tmpPath.string());
            return false;
        }
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!ofs) return false;
    }
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        LOG_WARN("RemoteToml({}/{}): could not replace {} ({})",
                 request.channel, request.component, finalPath.string(), ec.message());
        fs::remove(tmpPath, ec);
        return false;
    }
    LOG_INFO("RemoteToml({}/{}): wrote {}", request.channel, request.component, finalPath.string());
    return true;
}

} // namespace RemoteToml
