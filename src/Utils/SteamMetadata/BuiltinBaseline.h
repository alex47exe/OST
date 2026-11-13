#pragma once

// Built-in metadata baseline: hand-verified TOMLs for Steam client build
// 1794424036 (steamclient64.dll ebf46f84606e4dc4..., steamui.dll 5fdf0f7972ed7f6a...).
//
// Used only as the *oldest* baseline when deriving metadata for an unknown
// Steam build (cached TOMLs for newer builds take precedence). Every entry was
// checked against the real DLLs: signature matches exactly at its RVA, the RVA
// is a real function start, and IPC values were read from the wrapper code.
// CUtlMemoryGrow carries a long signature that covers the element-size bytes,
// because steamclient contains several near-identical instantiations of it.
//
// Regenerate when a newer build has been verified by hand; keep the format
// identical to steam-monitor's TOMLs.

#include <string_view>

namespace BuiltinBaseline {

inline constexpr std::string_view kSteamclientPattern = R"TOML([0x82428E37]
name = "BBuildAndAsyncSendFrame"
rva = "0xD68260"
sig = "48 8B C4 55 48 8D 68 A1 48 81 EC C0 00 00 00 48 89 70 18"

[0xDB78B4AE]
name = "BuildSpawnEnvBlock"
rva = "0xAA8BE0"
sig = "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FB FF FF"

[0x64BF7C45]
name = "CUtlBufferEnsureCapacity"
rva = "0xD1B370"
sig = "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 8D 7A 01"

[0x4B1B1D77]
name = "CheckAppOwnership"
rva = "0xA039F0"
sig = "48 8B C4 89 50 10 48 89 48 08 55 53"

[0x04691B23]
name = "CloseAppCloud"
rva = "0xA672A0"
sig = "48 89 5C 24 10 57 48 83 EC 30 8B FA 48 8B D9 85 D2"

[0x6179E8F9]
name = "ConfigStoreGetBinary"
rva = "0x5EFDA0"
sig = "40 53 55 56 57 48 83 EC 38 48 63 FA 49 8B E9"

[0xAC76B47D]
name = "GetAppDataFromAppInfo"
rva = "0x4DE420"
sig = "40 53 55 56 57 41 56 41 57 48 81 EC 78 01 00 00"

[0xCC79542C]
name = "GetOrAddAppData"
rva = "0x4DF790"
sig = "89 54 24 10 48 83 EC 68 48 8B 05 ?? ?? ?? ?? 48 89 6C 24 60 41 0F B6 E8"

[0x3B3A0F9D]
name = "GetPackageInfo"
rva = "0x4DF9F0"
sig = "48 89 5C 24 18 89 54 24 10 55 56 57 48 83 EC 20 44 8B 49 20"

[0x02DF23BC]
name = "GetPipeClient"
rva = "0x8C5000"
sig = "85 D2 74 ?? 44 0F B7 CA 44 3B 49 60"

[0xC3E20E29]
name = "IPCProcessMessage"
rva = "0x8C4610"
sig = "48 89 5C 24 18 48 89 6C 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 30"

[0xED5ED0C8]
name = "KeyValues_FindOrCreateKey"
rva = "0xD9EB70"
sig = "48 8B C4 4C 89 48 20 57 48 81 EC 60 04 00 00"

[0x2434A8BA]
name = "KeyValues_ReadAsBinary"
rva = "0xDA1790"
sig = "48 8B C4 44 88 48 20 55 48 8D 68 A9"

[0xB13C0C3F]
name = "LoadDepotDecryptionKey"
rva = "0x5EFDA0"
sig = "40 53 55 56 57 48 83 EC 38 48 63 FA 49 8B E9"

[0x31E49927]
name = "LoadPackage"
rva = "0x4DAC90"
sig = "44 89 44 24 18 53 55 56 57 41 55"

[0xC451039D]
name = "MarkLicenseAsChanged"
rva = "0xA10030"
sig = "48 89 5C 24 20 89 54 24 10 55 56 57 48 83 EC 20"

[0x06631030]
name = "OptedInMask"
rva = "0x61B1A0"
sig = "89 54 24 10 55 53 56 57 41 55 41 56 48 8D AC 24 48 FF FF FF"

[0x0F926D0A]
name = "PchMsgNameFromEMsg"
rva = "0xD4C3E0"
sig = "48 89 5C 24 08 57 48 83 EC 20 8B D9 E8 ?? ?? ?? ??"

[0x68211B4D]
name = "SendCallbackToPipe"
rva = "0x9BA880"
sig = "40 53 55 56 57 48 83 EC 38 41 8B F1 41 8B E8 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B C8 48 85 C0"

[0x103B52AA]
name = "ProcessPendingLicenseUpdates"
rva = "0x9FA4C0"
sig = "40 53 41 56 41 57 48 83 EC 40 83 B9 D0 24 00 00 00"

[0x7D1EC415]
name = "SpawnProcess"
rva = "0xA1DFD0"
sig = "48 89 5C 24 18 4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D"

[0xC37F2D8E]
name = "BuildDepotDependency"
rva = "0x4ED110"
sig = "48 8B C4 4C 89 48 20 89 50 10 48 89 48 08 55 48 8D A8"

[0xA185DB47]
name = "GetAppIDForCurrentPipe"
rva = "0x9B55E0"
sig = "8B 81 18 11 00 00 83 F8 FF 74 49 44 8B 89 D8 00"

[0x836FF9F0]
name = "RecvPkt"
rva = "0x5D3030"
sig = "48 8B C4 55 48 8D A8 E8 FB FF FF 48 81 EC 10 05"

[0x2D945919]
name = "CUtlMemoryGrow"
rva = "0xE95C0"
sig = "48 89 5C 24 10 57 48 83 EC 30 8B FA 48 8B D9 8B 51 08 8B 49 10 8D 04 39 3B C2 0F 8E ?? ?? ?? ?? 2B CA 48 89 74 24 40 8D 34 39 85 F6 7F ?? 4C 8D 05 ?? ?? ?? ?? BA E6 01 00 00 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 84 C0 75 ?? FF 15 ?? ?? ?? ?? 84 C0 74 ?? CC 8B 53 0C 8B C2 C1 E8 1F 84 C0 74 ?? 4C 8D 05 ?? ?? ?? ?? BA EB 01 00 00 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 84 C0 74 ?? CC 48 8B 74 24 40 01 7B 10 48 8B 5C 24 48 48 83 C4 30 5F C3 8B 4B 08 41 B9 04 00 00 00 44 8D 04 31 E8 ?? ?? ?? ?? 4C 8B 13 89 43 08 48 63 D0 48 8B 05 ?? ?? ?? ?? 48 C1 E2 02"
)TOML";

inline constexpr std::string_view kSteamuiPattern = R"TOML([0xD05E26A2]
name = "AddProtobufAsBinary"
rva = "0xA41920"
sig = "40 53 55 56 57 48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 8B F2"

[0xE22F74B4]
name = "BuildCompleteAppOverviewChange"
rva = "0x6341F0"
sig = "4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1"

[0x221F0661]
name = "CSteamUIAppControllerRunFrame"
rva = "0x64BA80"
sig = "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 40 0F 29 74 24 30"

[0xB030A061]
name = "FillInAppOverview"
rva = "0x63B0C0"
sig = "48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B8 00 00 00"

[0x3FC68546]
name = "GetAppByID"
rva = "0x63D8F0"
sig = "89 54 24 10 53 48 83 EC 40 48 8B 05 ?? ?? ?? ??"

[0xBDE16BD6]
name = "LoadModuleWithPath"
rva = "0x8DD8C0"
sig = "48 89 5C 24 18 55 56 41 57 48 83 EC 40"

[0xC7D5CACF]
name = "MarkAppChange"
rva = "0x6CF400"
sig = "48 83 EC 78 48 8B 05 ?? ?? ?? ?? 48 89 74 24 70"

[0xD055D6C0]
name = "ShouldShowAppInLibrary"
rva = "0x608DB0"
sig = "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 10 3D D6 0C 09 00"

[0x153479F0]
name = "RepeatedFieldUint32_Add"
rva = "0x72CB40"
sig = "48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 8B 31 48 8B F9 8B 49 04 4C 8D 34 B5"
)TOML";

inline constexpr std::string_view kSteamclientIpc = R"TOML([IClientUser]
interface_id = 1
vtable_rva = "0x1356C58"

[IClientUser.GetSteamID]
method_index = 10
funcHash = "0xD6FC3200"
wrapper_rva = "0x7B8EB0"
fencepost = "0xD7058CA5"
argc = 0

[IClientUser.GetAppOwnershipTicketExtendedData]
method_index = 106
funcHash = "0xC7E71244"
wrapper_rva = "0x7815C0"
fencepost = "0xC844D740"
argc = 2

[IClientUser.RequestEncryptedAppTicket]
method_index = 121
funcHash = "0x25D6BB1C"
wrapper_rva = "0x884ED0"
fencepost = "0x2646F563"
argc = 2

[IClientUser.GetEncryptedAppTicket]
method_index = 122
funcHash = "0xE0468CB5"
wrapper_rva = "0x794150"
fencepost = "0xE0B84102"
argc = 1

[IClientUtils]
interface_id = 4
vtable_rva = "0x135C6E0"

[IClientUtils.GetAppID]
method_index = 19
funcHash = "0x09607EC2"
wrapper_rva = "0x780320"
fencepost = "0x0AFEF352"
argc = 0

[IClientUtils.GetAPICallResult]
method_index = 24
funcHash = "0x2D3D3979"
wrapper_rva = "0x77C160"
fencepost = "0x2EDFDD1A"
argc = 3
)TOML";

inline std::string_view Pattern(std::string_view component)
{
    if (component == "steamclient") return kSteamclientPattern;
    if (component == "steamui") return kSteamuiPattern;
    return {};
}

} // namespace BuiltinBaseline
