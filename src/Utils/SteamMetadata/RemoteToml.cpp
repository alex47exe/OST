#include "RemoteToml.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/ProjectInfo.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

namespace RemoteToml {

namespace {
    // Metadata source layout: the `pattern` and `ipc` branches of ProjectInfo::kRepoUrl,
    // laid out as {component}/{sha256}.toml, with a maintained fork and the original
    // tracker as fallbacks. See ProjectInfo.h.

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

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (!remoteUrlTemplate.empty()) {
            if (!IsValidTemplate(remoteUrlTemplate)) {
                LOG_WARN("RemoteToml: remote.url_template must contain "
                         "{channel}, {component}, and {sha256}; remote fetch disabled");
                return {};
            }

            return { remoteUrlTemplate };
        }

        // Built-in order: this repository's metadata branches first (gh-proxy or raw
        // depending on [remote] order), then the jsDelivr mirror, the maintained fork,
        // and the original tracker as the last resort.
        std::vector<std::string> urls;
        if (Config::GetRemoteOrder() == "github-first") {
            urls.push_back(ProjectInfo::kLocalRawTemplate);
            urls.push_back(ProjectInfo::kLocalGhProxyTemplate);
        } else {
            urls.push_back(ProjectInfo::kLocalGhProxyTemplate);
            urls.push_back(ProjectInfo::kLocalRawTemplate);
        }
        urls.push_back(ProjectInfo::kLocalJsdelivrTemplate);
        urls.push_back(ProjectInfo::kForkRawTemplate);
        urls.push_back(ProjectInfo::kUpstreamRawTemplate);
        return urls;
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

    // Cache first: pattern/IPC files are named by DLL SHA-256 and their content is immutable; a hit returns immediately,
    // so remote timeouts cannot block hook installation.
    // Exception: locally derived entries ("# Auto-derived ...") are re-checked against the mirrors so an upstream
    // publication replaces them; they still serve as the fallback when every mirror is unavailable.
    std::string derivedCacheBody;
    {
        std::error_code ec;
        if (fs::exists(cachePath, ec) && !ec) {
            const auto sz = fs::file_size(cachePath, ec);
            if (!ec && sz > 0) {
                std::ifstream ifs(cachePath, std::ios::binary);
                if (ifs) {
                    std::string buf((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                    if (!buf.empty()) {
                        if (buf.rfind("# Auto-derived", 0) == 0) {
                            LOG_INFO("RemoteToml({}/{}): found locally derived cache {}; checking mirrors for a published copy",
                                     request.channel, request.component, cachePathText);
                            derivedCacheBody = std::move(buf);
                        } else {
                            LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                                     request.channel, request.component, cachePathText);
                            out.body = std::move(buf);
                            out.ok = true;
                            out.fromCache = true;
                            return out;
                        }
                    }
                }
                if (derivedCacheBody.empty()) {
                    LOG_WARN("RemoteToml({}/{}): cache file exists but failed to read or empty: {}",
                             request.channel, request.component, cachePathText);
                }
            }
        }
    }

    // 3. Cache miss -> try remote (poll all mirrors in order).
    // Try every mirror before declaring failure: CDN sync is delayed, so when the first mirror 404s the next may already have the file.
    // On a transport failure (non-404) each mirror is hit once more: cross-border links jitter often, and one retry absorbs the jitter;
    // 404 is not retried (refetching finds nothing either). Capped at one retry, so we do not burn time on the startup path.
    const std::vector<std::string> urlTemplates = BuildUrlTemplates();
    OSTPlatform::Http::Result http;
    std::string lastUrl;
    size_t notFoundCount = 0;

    for (size_t i = 0; i < urlTemplates.size(); ++i) {
        lastUrl = ExpandTemplate(urlTemplates[i], request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                          nullptr, 0, nullptr);

        const bool is404 = http.ok && http.status == 404;
        const bool gotBody = http.ok && http.status == 200 && !http.body.empty();
        if (!gotBody && !is404) {
            LOG_INFO("RemoteToml({}/{}): transport failed ({} ok={} HTTP={}), retrying once",
                     request.channel, request.component, lastUrl, http.ok, http.status);
            http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                              nullptr, 0, nullptr);
        }

        if (http.ok && http.status == 200 && !http.body.empty()) break;

        const bool hasNext = (i + 1 < urlTemplates.size());
        if (http.ok && http.status == 200) {
            LOG_WARN("RemoteToml({}/{}): mirror returned empty body: {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (http.ok && http.status == 404) {
            ++notFoundCount;
            LOG_WARN("RemoteToml({}/{}): mirror has no such file (HTTP 404): {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (hasNext) {
            LOG_WARN("RemoteToml({}/{}): mirror failed ({} ok={} HTTP={}), falling back",
                     request.channel, request.component, lastUrl, http.ok, http.status);
        }
    }

    // 4. Remote OK → write cache, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        // Write a temporary file first, then replace, so a crash cannot leave the cache file truncated
        fs::path tmpPath = cachePath;
        tmpPath += ".tmp";
        std::error_code writeEc;
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(http.body.data(),
                          static_cast<std::streamsize>(http.body.size()));
                ofs.close();
            }
            if (!ofs)
                writeEc = std::make_error_code(std::errc::io_error);
        }
        if (!writeEc) {
            fs::remove(cachePath, writeEc);
            if (!writeEc)
                fs::rename(tmpPath, cachePath, writeEc);
        }
        if (writeEc) {
            LOG_WARN("RemoteToml({}/{}): could not write cache {} ({})",
                     request.channel, request.component, cachePathText, writeEc.message());
            std::error_code cleanupEc;
            fs::remove(tmpPath, cleanupEc);
        } else {
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePathText);
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Total failure — caller handles popup / degraded mode.
    // A derived cache still serves this launch; it is retried against the mirrors next time.
    if (!derivedCacheBody.empty()) {
        LOG_INFO("RemoteToml({}/{}): no mirror has a published copy, using the locally derived cache {}",
                 request.channel, request.component, cachePathText);
        out.body = std::move(derivedCacheBody);
        out.ok = true;
        out.fromCache = true;
        return out;
    }

    // Distinguish all-404 from network failure: the former means upstream has not published, the latter is a local network problem.
    // allNotFound is tightened: at least one mirror must have been tried and all must be 404 (zero attempts
    // from a misconfigured template do not count as "unpublished" but as misconfiguration, which also skips the popup).
    out.allNotFound = !urlTemplates.empty() && notFoundCount == urlTemplates.size();
    if (out.allNotFound) {
        LOG_WARN("RemoteToml({}/{}): upstream has not published (all {} mirrors 404, last URL: {})",
                 request.channel, request.component, urlTemplates.size(),
                 lastUrl.empty() ? "<none>" : lastUrl);
    } else {
        LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
                 request.channel, request.component,
                 lastUrl.empty() ? "<none>" : lastUrl, http.status);
    }
    return out;
}

bool Invalidate(const Request& request)
{
    namespace fs = std::filesystem;
    const std::string sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    if (sha256.empty()) return false;
    fs::path cachePath = CacheDirFor(request) / (sha256 + ".toml");
    std::error_code ec;
    const bool removed = fs::remove(cachePath, ec);
    if (ec) {
        LOG_WARN("RemoteToml({}/{}): failed to remove stale cache {} ({})",
                 request.channel, request.component, cachePath.string(), ec.message());
        return false;
    }
    if (removed) {
        LOG_WARN("RemoteToml({}/{}): removed stale cache {}",
                 request.channel, request.component, cachePath.string());
    }
    return removed;
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
        if (!ofs) {
            std::error_code cleanupEc;
            fs::remove(tmpPath, cleanupEc);
            return false;
        }
    }

    fs::remove(finalPath, ec);
    ec.clear();
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        LOG_WARN("RemoteToml({}/{}): could not replace {} ({})",
                 request.channel, request.component, finalPath.string(), ec.message());
        std::error_code cleanupEc;
        fs::remove(tmpPath, cleanupEc);
        return false;
    }
    LOG_INFO("RemoteToml({}/{}): wrote {}", request.channel, request.component, finalPath.string());
    return true;
}

} // namespace RemoteToml
