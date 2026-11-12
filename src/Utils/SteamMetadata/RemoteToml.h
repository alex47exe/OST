#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
    };

    // Fetch remote TOML first, then fall back to the exact local cache entry.
    Result Fetch(const Request& request);

    struct CachedBody {
        std::string sha256;
        std::string body;
    };

    // Cached TOMLs for *other* builds of this channel/component, newest first
    // (by file time). Used as baselines when deriving metadata locally.
    std::vector<CachedBody> ReadOtherCached(const Request& request,
                                            std::string_view excludeSha256,
                                            size_t maxCount = 8);

    // Write `body` as the cache entry for `sha256` (atomic replace).
    bool WriteCache(const Request& request, std::string_view sha256, std::string_view body);

} // namespace RemoteToml
