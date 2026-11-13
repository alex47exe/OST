#pragma once
#include <cstddef>
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
        // Only meaningful when the download fails: all mirrors 404 = upstream really has not published (show a popup);
        // false = network failure/timeout (stay silent, retry next time). Always false on success.
        bool        allNotFound = false;
    };

    struct CachedBody {
        std::string sha256;
        std::string body;
    };

    // Prefer the local cache named by SHA; on a miss poll every remote mirror in order and return empty only if all fail
    Result Fetch(const Request& request);

    // Cached TOMLs for *other* builds of this channel/component, newest first
    // (by file time). Used as baselines when deriving metadata locally.
    std::vector<CachedBody> ReadOtherCached(const Request& request,
                                            std::string_view excludeSha256,
                                            size_t maxCount = 8);

    // Write `body` as the cache entry for `sha256` (atomic replace).
    bool WriteCache(const Request& request, std::string_view sha256, std::string_view body);

    // Delete the cache file for this request (for cross-wired/stale content); returns whether a file was removed.
    // The next Fetch goes to the remote again. Only deletes the file, never touches the directory.
    bool Invalidate(const Request& request);

} // namespace RemoteToml
