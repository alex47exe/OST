#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        uint64_t code = 0;
        auto [_, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{}) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.
    // Table order is the ordered-failover order; the first row must be the default preferred source.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // full literal with one %llu (gid-only)
                                        // or two %llu (depotId, gid) when needsDepot
        Parser           parse;
        bool             needsDepot;
        const wchar_t*   headers;        // extra request headers (e.g. UA gate), or nullptr
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse,
                            bool needsDepot = false, const wchar_t* headers = nullptr) {
        return {name, url, parse, needsDepot, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("20770407",    "https://20770407.xyz/manifest/%llu/%llu",   ParsePlainUint, true),
        Make("SDM",           "https://steamapi.993499094.xyz/manifest/%llu/%llu", ParsePlainUint, true),
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",   ParsePlainUint, false,
             L"User-Agent: ManifestDeX/1.0"),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",     ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu", ParseSteamRunJson),
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu", ParsePlainUint),
    };

    static constexpr size_t kProviderCount = std::size(kProviders);

    // ── provider state ────────────────────────────────────────────
    //
    // This lock only guards in-memory state such as the active index and the cooldown
    // table; no HTTP or Lua calls are made while holding it, so a dead upstream cannot
    // block concurrent depots.

    static std::mutex g_stateMutex;

    // Index of the configured preferred source; failover always starts here and success never rewrites it.
    static size_t g_active = 0;                            // 20770407

    // Per-source failure deadline; before it passes the source is skipped, so no timeout is paid again.
    static std::chrono::steady_clock::time_point g_deadUntil[kProviderCount];

    // Per-source success counter: with concurrent fetches a failure can arrive after a success,
    // so comparing against it detects stale failures and keeps a just-proven-live source out of cooldown.
    static uint64_t g_okSeq[kProviderCount] = {};

    // Master ordered-failover switch, maps to [manifest] failover.
    static bool g_failoverEnabled = true;

    static size_t ActiveIndex() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_active;
    }

    static bool FailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    static bool IsCoolingDown(size_t i, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_deadUntil[i] > now;
    }

    // Snapshot before the request is sent; compared when a failure is written back.
    static uint64_t OkSeq(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_okSeq[i];
    }

    static void MarkFailed(size_t i, std::chrono::steady_clock::time_point now, uint64_t seq) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        // A success landed after the snapshot, so this failure describes stale state; discard it.
        if (g_okSeq[i] != seq) {
            LOG_MANIFEST_DEBUG("Manifest {} failure ignored (superseded by a success)",
                               kProviders[i].name);
            return;
        }
        g_deadUntil[i] = now + std::chrono::milliseconds(kProviderCooldownMs);
    }

    // Record that the source is alive and clear its cooldown; the preferred index is not rewritten.
    static void RecordSuccess(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ++g_okSeq[i];
        g_deadUntil[i] = {};
    }

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (size_t i = 0; i < kProviderCount; ++i)
            if (kProviders[i].name == name) {
                // Re-selecting the same name is a no-op: any toml change triggers a full reload, and
                // clearing cooldowns here would send a known-dead source back to the front of the queue.
                if (i == g_active) return true;
                g_active = i;
                // Only the newly selected source has its cooldown cleared and sequence bumped; other sources keep their
                // cooldown verdicts, and in-flight failures do not override this switch.
                g_deadUntil[i] = {};
                ++g_okSeq[i];
                return true;
            }
        return false;
    }

    void SetFailoverEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_failoverEnabled == enabled) return;
        g_failoverEnabled = enabled;
        // After the switch flips, old cooldown verdicts are no longer trustworthy; clear them and start clean.
        for (auto& t : g_deadUntil) t = {};
    }

    bool IsFailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return kProviders[g_active].name.data();
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    // Issue one request to a single source; the caller clamps the timeouts to the budget before passing them in.
    static bool FetchFrom(const Provider& p, uint64_t gid, AppId_t depotId, uint64_t* outCode,
                          uint32_t resolveMs, uint32_t connectMs, uint32_t sendMs, uint32_t recvMs) {
        char urlLog[256];
        if (p.needsDepot) {
            if (!depotId) return false;
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        } else {
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(gid));
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            urlLog,
            nullptr,
            0,
            p.headers,
            resolveMs,
            connectMs,
            sendMs,
            recvMs);

        LOG_MANIFEST_INFO("Manifest {} status={} depot={} gid={}", p.name, r.status, depotId, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // With the switch off, only the preferred source is requested; no failover.
    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Provider& p = kProviders[ActiveIndex()];
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        return FetchFrom(p, gid, depotId, outCode,
                         timeouts.resolve, timeouts.connect, timeouts.send, timeouts.recv);
    }

    // Walk the whole table in a ring starting at the preferred source: skip sources in cooldown, skip
    // needsDepot sources when depotId is missing (without recording a failure), and stop once the budget is
    // gone; if the whole table is cooling down, return immediately.
    static bool FetchWithFailover(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kFetchBudgetMs);

        // Snapshot the start index once; a concurrent preferred-source change does not affect this traversal order.
        const size_t start = ActiveIndex();

        size_t tried = 0;
        for (size_t k = 0; k < kProviderCount; ++k) {
            const size_t i = (start + k) % kProviderCount;
            const Provider& p = kProviders[i];

            const auto now = std::chrono::steady_clock::now();
            if (IsCoolingDown(i, now)) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (cooldown)", p.name);
                continue;
            }
            if (p.needsDepot && !depotId) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (no depot id)", p.name);
                continue;
            }

            // Snapshot the success counter before sending and compare it when a failure is written back, filtering stale failures.
            const uint64_t seq = OkSeq(i);

            // A single attempt must not exceed the remaining budget: the hook only waits budget+1s, and
            // a slower reply is useless to Steam while it holds up the next depot.
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                LOG_MANIFEST_WARN("Manifest gid={}: {}ms budget exhausted after {} attempt(s)",
                                  gid, kFetchBudgetMs, tried);
                break;
            }
            // Clamp both ends: never above the remaining budget and never below 1ms — WinHTTP reads
            // a zero timeout as an infinite wait, so one attempt would blow through the hook's wait window.
            const auto cap = [left](uint32_t ms) {
                return static_cast<uint32_t>(
                    std::max<int64_t>(1, std::min<int64_t>(ms, left)));
            };

            const bool ok = FetchFrom(p, gid, depotId, outCode,
                                      cap(timeouts.resolve), cap(timeouts.connect),
                                      cap(timeouts.send), cap(timeouts.recv));
            ++tried;

            if (ok) {
                RecordSuccess(i);
                return true;
            }

            MarkFailed(i, std::chrono::steady_clock::now(), seq);
        }

        if (tried == 0)
            LOG_MANIFEST_WARN("Manifest gid={}: every provider is in cooldown", gid);
        return false;
    }

    // ── public ────────────────────────────────────────────────────

    // LuaConfig exposes a single shared lua_State and does not lock internally, so concurrent fetches must be
    // serialized here; the lock covers only the Lua section and is released before network I/O starts.
    static std::mutex g_luaMutex;

    // If Lua yields a code, return true and the caller does not fall through to HTTP; this is a separate
    // function so the lock is released before the request goes out.
    static bool TryLua(uint64_t manifestGid, uint64_t* outRequestCode,
                       AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_luaMutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return false;
    }

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (TryLua(manifestGid, outRequestCode, appId, depotId))
            return true;

        if (FailoverEnabled())
            return FetchWithFailover(manifestGid, outRequestCode, depotId);

        return FetchActive(manifestGid, outRequestCode, depotId);
    }
}
