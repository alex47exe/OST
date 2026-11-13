#include "AppTicket.h"
#include "Hook/Hooks_Decryption.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include "dllmain.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace AppTicket {
    constexpr AppId_t kLocalAppTicketSourceAppId = 7;
    // Steamworks common redistributable: free auto-install, very wide coverage. Second-choice donor;
    // measured to hold a valid ticket in memory (178-byte hit). Use it if present and log the source.
    constexpr AppId_t kSysVerifyAppId = 228980;
    constexpr size_t kDonorMaxSize = 65536;
    constexpr size_t kSteamIdTicketMinimumSize = 16;
    constexpr const wchar_t* kRegAppsPath = L"Software\\Valve\\Steam\\Apps";

    // A donor lends only its structure + real signature: big enough, not a placeholder, first field non-zero means usable.
    // 16B placeholders and garbage oversized values are dropped outright; whether it is genuine is up to Denuvo.
    static bool IsUsableDonor(const std::vector<uint8_t>& t) {
        if (t.size() <= kAppTicketSignatureSize || t.size() > kDonorMaxSize) return false;
        uint32_t head = 0;
        memcpy(&head, t.data(), sizeof(head));
        return head != 0;
    }

    // The low 32 bits of the SteamID in the ticket are the account ID; use them to tell whose ticket this is.
    // Layout matches GetSpoofSteamID: [Size][Version][SteamID].
    static uint32_t DonorAccountId(const std::vector<uint8_t>& t) {
        if (t.size() < kSteamIdTicketMinimumSize) return 0;
        uint64_t steamId = 0;
        memcpy(&steamId, t.data() + 8, sizeof(steamId));
        return static_cast<uint32_t>(steamId & 0xFFFFFFFFull);
    }

    // Currently signed-in account. Returns 0 when unavailable, and the caller falls back to structure-only validation.
    // Switching accounts always restarts Steam, so there is never a second value in-process; no cache invalidation needed.
    static uint32_t ActiveAccountId() {
        uint32_t accountId = 0;
        std::wstring universe;
        if (OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universe) !=
            OSTPlatform::SteamCredentialStore::Status::Ok) {
            return 0;
        }
        return accountId;
    }

    // pin: remember the donor source so the next call validates the pin first (single IPC thread, lock-free, as before).
    enum class DonorKind { None, Memory, Registry, LocalConfig };
    static DonorKind g_donorKind = DonorKind::None;
    static AppId_t g_donorAppId = 0;

    // The apptickets section in localconfig.vdf is Steam's own repository of real tickets; OST never writes it.
    // The registry side holds mostly OST's own activation records, orders of magnitude fewer, so this is the live ticket source.
    // Tickets are Valve-signed; forging only borrows the trailing signature, and since the source appid is unrelated to the target, any one will do.
    //
    // The file is text VDF with the fixed structure "apptickets" { "<appid>" "<hex>" ... }.
    // The section is found by brace matching rather than by seeking the closing brace, so future nesting cannot truncate it.
    static bool ParseLocalConfigAppTickets(const std::string& data, uint32_t wantAccount,
                                           AppId_t excludeAppId,
                                           std::vector<uint8_t>& out, AppId_t& appId) {
        const size_t marker = data.find("\"apptickets\"");
        if (marker == std::string::npos) return false;

        const size_t open = data.find('{', marker);
        if (open == std::string::npos) return false;

        size_t close = std::string::npos;
        int depth = 0;
        for (size_t i = open; i < data.size(); ++i) {
            if (data[i] == '{') {
                ++depth;
            } else if (data[i] == '}') {
                if (--depth == 0) { close = i; break; }
            }
        }
        if (close == std::string::npos) return false;

        // Read "<appid>" "<hex>" pairs one at a time, stopping at the first qualifying ticket.
        size_t pos = open;
        auto NextQuoted = [&](std::string& value) -> bool {
            const size_t a = data.find('"', pos);
            if (a == std::string::npos || a >= close) return false;
            const size_t b = data.find('"', a + 1);
            if (b == std::string::npos || b >= close) return false;
            value.assign(data, a + 1, b - a - 1);
            pos = b + 1;
            return true;
        };
        auto HexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };

        std::string key;
        std::string hex;
        while (NextQuoted(key) && NextQuoted(hex)) {
            if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
            if (hex.size() < 2 || (hex.size() % 2) != 0) continue;

            bool allHex = true;
            for (char c : hex) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) { allHex = false; break; }
            }
            if (!allHex) continue;
            // Must be large enough to hold the trailing signature, otherwise splicing would consume the whole ticket.
            if (hex.size() / 2 <= kAppTicketSignatureSize) continue;

            std::vector<uint8_t> bytes;
            bytes.reserve(hex.size() / 2);
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                bytes.push_back(static_cast<uint8_t>((HexNibble(hex[i]) << 4) | HexNibble(hex[i + 1])));
            }
            if (!IsUsableDonor(bytes)) continue;

            // A ticket from Family Sharing carries the lender's SteamID; splicing it always fails with 54.
            // The caller filters by wantAccount, where wantAccount == 0 means no restriction.
            const AppId_t parsedApp = static_cast<AppId_t>(strtoul(key.c_str(), nullptr, 10));
            // Skip the target game itself: if it had a ticket we would never reach forge, so using it as the base would be self-defeating.
            if (parsedApp == excludeAppId) continue;
            if (wantAccount != 0) {
                const uint32_t ticketAccount = DonorAccountId(bytes);
                if (ticketAccount != 0 && ticketAccount != wantAccount) {
                    LOG_DEBUG("LocalConfigDonor: skip appid {} base (account {} != {})",
                              parsedApp, ticketAccount, wantAccount);
                    continue;
                }
            }

            out = std::move(bytes);
            appId = parsedApp;
            return true;
        }
        return false;
    }

    // accountId == 0 reads the currently signed-in account; non-zero reads that account's directory.
    static bool ReadLocalConfigDonor(uint32_t accountId, AppId_t excludeAppId,
                                     std::vector<uint8_t>& out, AppId_t& appId) {
        out.clear();
        appId = 0;
        if (accountId == 0 || SteamInstallPath[0] == '\0') return false;

        const std::string path = std::string(SteamInstallPath) + "\\userdata\\" +
                                 std::to_string(accountId) + "\\config\\localconfig.vdf";
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            LOG_DEBUG("LocalConfigDonor: cannot open {}", path);
            return false;
        }

        const std::string data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        if (data.empty()) return false;

        if (!ParseLocalConfigAppTickets(data, accountId, excludeAppId, out, appId)) {
            LOG_DEBUG("LocalConfigDonor: no usable ticket in {} ({} bytes)", path, data.size());
            return false;
        }
        LOG_DEBUG("LocalConfigDonor: picked appid {} ({} bytes) from account {}",
                  appId, out.size(), accountId);
        return true;
    }

    // excludeAppId only matters for LocalConfig: that path re-parses file contents and needs to know
    // which game is the target in order to skip it. Memory/Registry look up by appid directly and never hit the target.
    static bool ReadDonor(DonorKind kind, AppId_t donorAppId, AppId_t excludeAppId,
                          std::vector<uint8_t>& out) {
        out.clear();
        if (donorAppId == 0) return false;
        if (kind == DonorKind::Memory) {
            out = Hooks_Decryption::GetCacheAppOwnershipTicket(donorAppId);
        } else if (kind == DonorKind::Registry) {
            std::vector<uint8_t> t;
            if (OSTPlatform::SteamCredentialStore::GetAppTicket(donorAppId, t) ==
                OSTPlatform::SteamCredentialStore::Status::Ok) {
                out = std::move(t);
            }
        } else if (kind == DonorKind::LocalConfig) {
            // Accept only this one: if the pinned appid changed it is invalidated and the caller reruns the full scan.
            std::vector<uint8_t> t;
            AppId_t appId = 0;
            if (!ReadLocalConfigDonor(ActiveAccountId(), excludeAppId, t, appId)) return false;
            if (appId != donorAppId) return false;
            out = std::move(t);
        }
        return IsUsableDonor(out);
    }

    // Scan order: pin → memory 7 → memory 228980 (verification slot) → localconfig → full registry enumeration.
    // The target game itself is skipped (it would never reach forge if it had a ticket). Donor bytes never touch disk,
    // because landing under the target name would pollute that game's spoof identity.
    //
    // localconfig.vdf is the main source of real tickets for this account, and the ticket carries this account's SteamID,
    // so using it as the base needs no further identity alignment. The registry is the final fallback: it usually
    // holds only OST's own activation records, and pool accounts with no real ticket at all can only rely on it.
    //
    // Two passes: the first accepts only tickets under the signed-in account, the second relaxes to any usable ticket.
    // In the lenient pass localconfig still reads only this account's directory, because cross-account tickets have
    // mismatched identities and using them as the base would only surface 54 sooner.
    static bool FindDonorAppTicket(AppId_t excludeAppId, std::vector<uint8_t>& out, AppId_t& donorAppId) {
        const uint32_t activeAccount = ActiveAccountId();

        auto TryAccept = [&](DonorKind kind, AppId_t cand) -> bool {
            std::vector<uint8_t> ticket;
            if (!ReadDonor(kind, cand, excludeAppId, ticket)) return false;
            if (activeAccount != 0) {
                const uint32_t donorAccount = DonorAccountId(ticket);
                if (donorAccount != 0 && donorAccount != activeAccount) return false;
            }
            out = std::move(ticket);
            donorAppId = cand;
            return true;
        };

        auto TryMemCandidates = [&](bool requireOwn) -> bool {
            const AppId_t memCandidates[] = { kLocalAppTicketSourceAppId, kSysVerifyAppId };
            for (AppId_t cand : memCandidates) {
                if (cand == excludeAppId) continue;
                if (!requireOwn) {
                    if (TryAccept(DonorKind::Memory, cand)) {
                        g_donorKind = DonorKind::Memory;
                        g_donorAppId = cand;
                        LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket={})",
                                 cand, out.size(), activeAccount != 0 && DonorAccountId(out) == activeAccount);
                        return true;
                    }
                    continue;
                }
                std::vector<uint8_t> ticket;
                if (!ReadDonor(DonorKind::Memory, cand, excludeAppId, ticket)) continue;
                if (activeAccount == 0 || DonorAccountId(ticket) != activeAccount) continue;
                out = std::move(ticket);
                donorAppId = cand;
                g_donorKind = DonorKind::Memory;
                g_donorAppId = cand;
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket=true)",
                         cand, out.size());
                return true;
            }
            return false;
        };

        // localconfig can only read this account's directory: the directories are split per account, and cross-account
        // tickets carry someone else's SteamID, so using them as the base would only surface the identity conflict sooner.
        // It therefore stays out of the lenient pass; a miss in the first pass needs no retry and saves one whole-file read.
        auto TryLocalConfig = [&]() -> bool {
            if (activeAccount == 0) return false;
            std::vector<uint8_t> ticket;
            AppId_t cand = 0;
            if (!ReadLocalConfigDonor(activeAccount, excludeAppId, ticket, cand)) return false;
            out = std::move(ticket);
            donorAppId = cand;
            g_donorKind = DonorKind::LocalConfig;
            g_donorAppId = cand;
            LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from localconfig appid={} ({} bytes, account={})",
                     cand, out.size(), activeAccount);
            return true;
        };

        auto TryRegistry = [&](bool requireOwn) -> bool {
            HKEY hApps = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegAppsPath, 0, KEY_ENUMERATE_SUB_KEYS,
                              &hApps) != ERROR_SUCCESS) {
                return false;
            }
            wchar_t nameBuf[32];
            for (DWORD index = 0; ; ++index) {
                DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
                if (RegEnumKeyExW(hApps, index, nameBuf, &nameLen, nullptr, nullptr,
                                  nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                const unsigned long id = wcstoul(nameBuf, nullptr, 10);
                if (id == 0 || id > UINT32_MAX || static_cast<AppId_t>(id) == excludeAppId) continue;
                const AppId_t cand = static_cast<AppId_t>(id);
                std::vector<uint8_t> ticket;
                if (!ReadDonor(DonorKind::Registry, cand, excludeAppId, ticket)) continue;
                if (requireOwn && (activeAccount == 0 || DonorAccountId(ticket) != activeAccount)) continue;
                out = std::move(ticket);
                donorAppId = cand;
                RegCloseKey(hApps);
                g_donorKind = DonorKind::Registry;
                g_donorAppId = cand;
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor ticket found: appid={} ({} bytes, own_ticket={})",
                         cand, out.size(), activeAccount != 0 && DonorAccountId(out) == activeAccount);
                return true;
            }
            RegCloseKey(hApps);
            return false;
        };

        out.clear();
        donorAppId = 0;

        // Validate the pin first: prefer the source picked last time and skip a repeated full scan.
        if (g_donorKind != DonorKind::None && g_donorAppId != excludeAppId) {
            if (TryAccept(g_donorKind, g_donorAppId)) {
                return true;
            }
            g_donorKind = DonorKind::None;
            g_donorAppId = 0;
        }

        if (activeAccount != 0) {
            if (TryMemCandidates(true)) return true;
            if (TryLocalConfig()) return true;
            if (TryRegistry(true)) return true;
        }
        if (TryMemCandidates(false)) return true;
        if (TryRegistry(false)) return true;

        out.clear();
        donorAppId = 0;
        return false;
    }

    static uint64_t GetSteamIDFromCredentialStore(AppId_t appId) {
        uint64_t steamId = 0;
        const auto status = OSTPlatform::SteamCredentialStore::GetSteamId(appId, steamId);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("GetSpoofSteamID for AppId {}: SteamID unavailable in credential store ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return 0;
        }

        LOG_DEBUG("GetSpoofSteamID for AppId {}: SteamID credential -> 0x{:X}({})", appId, steamId, steamId);
        return steamId;
    }

    std::vector<uint8_t> GetAppOwnershipTicketFromCredentialStore(AppId_t appId) {
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetAppOwnershipTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetAppTicket(appId, ticket);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("Read App Ownership Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved App Ownership Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    static std::vector<uint8_t> ForgeLocalAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> source;
        AppId_t donorAppId = 0;
        if (!FindDonorAppTicket(appId, source, donorAppId)) {
            LOG_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no source appticket (no usable donor)", appId);
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin(), source.begin() + signedSize);

        const uint8_t* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
        ticket.insert(ticket.end(), appIdBytes, appIdBytes + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin() + signedSize, source.end());

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Total Size: {}",
                 appId, donorAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        
        if (source == AppTicketSource::CredentialStoreOnly || source == AppTicketSource::CredentialStoreThenForge) {
            ticket.data = GetAppOwnershipTicketFromCredentialStore(appId);
            if (!ticket.data.empty() && ticket.data.size() >= sizeof(uint32)) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                ticket.signatureOffset = *reinterpret_cast<const uint32*>(ticket.data.data());
                ticket.signatureSize = kAppTicketSignatureSize;
                return true;
            }
        }

        if (source == AppTicketSource::CredentialStoreOnly) return false;

        ticket.data = ForgeLocalAppOwnershipTicket(appId);
        if (ticket.data.empty()) return false;

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        return true;
    }

    std::vector<uint8_t> GetEncryptedTicketFromCredentialStore(AppId_t appId) {
        LOG_DEBUG("appid={}", appId);    
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetEncryptedTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetETicket(appId, ticket);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("Read Encrypted App Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved Encrypted App Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        const auto status = OSTPlatform::SteamCredentialStore::WriteAppTicket(appId, data);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write AppTicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        const auto status = OSTPlatform::SteamCredentialStore::WriteETicket(appId, data);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write ETicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        const auto status = OSTPlatform::SteamCredentialStore::WriteSteamId(appId, steamId);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write SteamID for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote SteamID for AppId {} ({})", appId, steamId);
        return true;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetSpoofSteamID for AppId {}: not in addappid, skip spoofing", appId);
            return 0;
        }
        const uint64_t credentialSteamID = GetSteamIDFromCredentialStore(appId);
        if (credentialSteamID != 0) {
            return credentialSteamID;
        }

        // The SteamID baked into the cached AppOwnershipTicket is the same
        // one Steam itself uses for this app — pull it straight out of the
        // ticket so spoofed responses match what the DRM layer expects.
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        std::vector<uint8_t> ticket = GetAppOwnershipTicketFromCredentialStore(appId);
        if (ticket.size() >= kSteamIdTicketMinimumSize) {
            const uint64_t steamID = reinterpret_cast<const uint64_t*>(ticket.data())[1];
            LOG_DEBUG("GetSpoofSteamID for AppId {}: -> 0x{:X}({})", appId, steamID, steamID);
            return steamID;
        }
        return 0;
    }
}
