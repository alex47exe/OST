<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-Source Steam Unlock Tool</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Windows only">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="United States flag">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Spain flag">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="China flag">
      中文
    </a>
  </p>
</div>

> **Maintained fork.** This is a maintained continuation of [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool), which is no longer updated. Releases, Steam-update metadata and issue tracking now live here: [releases](https://github.com/matheeshapathirana/OpenSteamTool/releases) · [issues](https://github.com/matheeshapathirana/OpenSteamTool/issues).

## Feature

### Core Unlocks
- Unlock an unlimited number of unowned games.
- Unlock all DLCs for unowned games.
- Support auto load depot decryption keys from Lua config.
- Support auto manifest download via `opensteamtool` / `steamrun` / `wudrm` upstream APIs (default is `opensteamtool`, falling back to the others if it fails), or a custom Lua endpoint (see [Manifest via Lua](#manifest-via-lua)).
- Support downloading protected games or DLCs that require an access token.
- Support binding manifest to prevent specific games from being updated.

### Survives Steam Updates
- Per-build Steam metadata is published to this repository automatically when Valve ships a new client build, and OpenSteamTool can derive it locally when nothing is published yet.
- Every signature is validated against the loaded Steam DLLs before use, so stale metadata disables a hook instead of crashing Steam. See [Steam version compatibility](#steam-version-compatibility).

### Hot Reload
- Adding, modifying, deleting, or overwriting `.lua` files in any watched directory automatically triggers a reload. No restart, no offline/online toggle needed.

### Injection
- Add optional game-process library injection through `[inject]` in `opensteamtool.toml`.
- Configure `enabled`, `library_x64`, and `library_x86`; the injected library must match the target process architecture.`library_x64` and `library_x86` may be absolute paths, or relative paths resolved from the Steam root directory.

### Family Sharing and Remote Play
- Bypass Steam Family Sharing restrictions for games that have been added to the library with `addappid` in Lua. All accounts in the Steam Family that participate in sharing must use OpenSteamTool for this to work.

### Compatible with games protected by Denuvo and SteamStub
- SteamStub-only games do not require configuring `AppTicket`. OpenSteamTool can reuse Steam's local ConfigStore ticket and forge the requested AppId through a SteamDRMP off-by-four ticket parsing vulnerability, without injecting into the game process.
- Denuvo-protected games still require explicit ticket data. OpenSteamTool stores `AppTicket` and `ETicket` through the platform credential store.
- Use `setAppTicket(appid, "hex")` and `setETicket(appid, "hex")` in Lua config to write these values to the platform credential store automatically.
- Denuvo verification has a 30-minute validity window. After this window expires, authorization may fail with Denuvo error code `88500005`; refresh the ticket data before retrying.
- AppTicket priority: explicit tickets have the highest priority, including tickets configured by `setAppTicket` and existing cached `AppTicket` credential values. If no explicit AppTicket is available, OpenSteamTool falls back to the forged local ConfigStore ticket path.
- SteamID priority: read cached `SteamID` first; if missing, parse from explicit `AppTicket`. On Windows, the credential store backend currently uses `HKCU\Software\Valve\Steam\Apps\<AppId>`. The Linux backend is not implemented yet.

#### Extracting tickets with `extract_tickets`

The `extract_tickets` tool dumps the `AppTicket` and `ETicket` hex strings you need for `setAppTicket` / `setETicket`. Run it on a machine where Steam is running and logged into an account that **owns** the target game.

1. Build the tools (see [Build](#build)); the binary lands in `build/tools/Release/extract_tickets.exe`.
2. Run it with the target AppId (or run it with no argument and type the AppId when prompted):
   ```powershell
   extract_tickets.exe 1361510
   ```
3. It reads the Steam install path from the registry, loads `steamclient64.dll`, and writes everything into an `<appid>/` folder next to the executable:
   - `appticket.bin` — raw app ownership ticket (binary)
   - `eticket.bin` — raw encrypted app ticket (binary)
   - `tickets.txt` — plain-text summary with the hex strings:
     ```
     appid:1361510
     appticket(184 bytes):14000000...
     eticket(143 bytes):...
     ```
   A ticket that could not be obtained is reported as `appticket:null` / `eticket:null`.
4. Paste the hex strings from `tickets.txt` into your Lua config:
   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Note:** Tickets are only valid when extracted from an account that **genuinely owns** the game.

### Stats and Achievements
- Enable stats and achievements for unowned games.
- Uses `setStat(appid, "steamid")` to configure which SteamID's achievement data to pull.
- If no `setStat` is configured for an app, OpenSteamTool queries the stats API when `[stats] enable_api = true` (default). The endpoint is `https://stats.opensteamtool.com/{appid}` unless `[stats] api_url` points elsewhere.
- Priority: `setStat` > stats API when enabled and valid > hardcoded preset SteamID `76561198028121353`.

### Cloud Saves
- Optional Steam Cloud redirection for unlocked games through [CloudRedirect](https://github.com/Selectively11/CloudRedirect): set `[cloud] enabled = true` and place `cloud_redirect.dll` in the Steam root. Provider sign-in is done in CloudRedirect's companion app.

### Online Fix
- Add `-onlinefix` to the Steam launch parameters to enable 480-based online play in games that use lobby matchmaking. The current limitation is that only one such game can run at a time.To revert, simply remove -onlinefix from the launch parameters — online play returns to normal on the next launch.

## Future
- Native Steam Cloud synchronization (without CloudRedirect).

## Usage
1. Download `OpenSteamTool-<version>-Release.zip` from the [latest release](https://github.com/matheeshapathirana/OpenSteamTool/releases/latest) (or the `-Debug` zip if you want log files), or build it yourself with `build.bat` (see [Build](#build)).
2. Close Steam, then copy `dwmapi.dll`, `xinput1_4.dll` and `OpenSteamTool.dll` to the Steam root directory (e.g. `C:\Program Files (x86)\Steam`).
3. Create Lua directory (for example `C:\steam\config\lua`) and place Lua scripts there. The DLL will automatically load and execute them.
4. Lua example:
```lua
addappid(1361510) -- unlock game with appid 1361510

addappid(1361511, 0,"5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- unlock game with appid 1361511 depotKey is "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0" 

addtoken(1361510,"2764735786934684318") -- add access token ("2764735786934684318") for game with appid 1361510 
-- No Longer Supported:
--pinApp(1361510) -- pin game with appid 1361510 to prevent it from being updated

setManifestid(1361511,"5656605350306673283") -- pin depotid:1361511 manifest_gid:5656605350306673283, size defaults to 0
setManifestid(1361511,"5656605350306673283", 12345678) -- same but with explicit size

setAppTicket(1361510,"0100000000000000...") -- write AppTicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\AppTicket

setETicket(1361510,"0100000000000000...") -- write ETicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\ETicket

setStat(1361510, "76561197960287930") -- use the specified SteamID's achievement data for appid 1361510
-- If not configured, the stats API is used when enabled; otherwise default SteamID 76561198028121353 is used.
```

All function names are **case-insensitive**. `setAppTicket`, `setappticket`, `SetAppticket`, `SETAPPTICKET` etc. are all equivalent. The same applies to every registered function (`addAppId`, `AddToken`, `SETManifestid`, etc.).

### Configuration (optional)

Rename `opensteamtool.example.toml` to `opensteamtool.toml` and place it in the Steam root directory (next to `steam.exe`).
If no config file is found, built-in defaults are used — no auto-creation.
The file is watched while Steam is running; valid changes are hot-reloaded without restarting Steam.

```toml
[log]
# Debug build only.  Level: trace, debug, info, warn, error
level = "info"

[manifest]
# Upstream API for depot manifest request codes.  Options: "opensteamtool", "steamrun", "wudrm"
# If it fails, the other providers are tried in turn.
url = "opensteamtool"

# HTTP timeouts for manifest requests (milliseconds)
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Query the stats API when no Lua setStat override exists.
# Priority: setStat > stats API > hardcoded preset SteamID.
enable_api = true
# Stats API endpoint; must contain {appid}. The response body is a SteamID64.
# api_url = "https://stats.opensteamtool.com/{appid}"

# Additional Lua config directories (optional).
# Files are loaded after the default <Steam>/config/lua folder.
# The default folder is always loaded last so user files take priority.
[lua]
paths = []

[inject]
# Optional library injection into game processes.
# The injected library must match the target process architecture.
enabled = false
# library_x64 = "OpenSteamTool.GameHook.x64.dll"
# library_x86 = "OpenSteamTool.GameHook.x86.dll"

[cloud]
# Optional Steam Cloud redirection via CloudRedirect (see opensteamtool.example.toml).
enabled = false
# library = "cloud_redirect.dll"

# Steam metadata sources. See "Steam version compatibility" below.
[remote]
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# Derive metadata locally when none is published for the installed Steam build.
auto_derive = true
```

### Manifest via Lua

Two manifest code functions are supported:

#### `fetch_manifest_code(gid)`

Basic function that receives only the manifest GID.

#### `fetch_manifest_code_ex(app_id, depot_id, gid)` *(recommended)*

Extended function that receives `app_id`, `depot_id`, and `gid`. Allows constructing API endpoints that require app identification.

The C++ runtime provides two Lua helpers:

| Function | Signature | Returns |
|----------|-----------|---------|
| `http_get`  | `http_get(url [, headers])`       | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` is an optional table: `{["Key"]="Value", ...}`.

### Steam version compatibility

OpenSteamTool needs per-build metadata for the Steam client: byte signatures / RVAs for hooked functions in `steamclient64.dll` and `steamui.dll` (`pattern` channel) and IPC method hashes (`ipc` channel). Every Steam update changes these DLLs, so metadata is keyed by the SHA-256 of each DLL.

**Where metadata comes from (every launch):**

1. **This repository** — the [`pattern`](https://github.com/matheeshapathirana/OpenSteamTool/tree/pattern) and [`ipc`](https://github.com/matheeshapathirana/OpenSteamTool/tree/ipc) branches, via GitHub raw with a jsDelivr fallback (useful where `raw.githubusercontent.com` is blocked).
2. **The original tracker** [`OpenSteam001/steam-monitor`](https://github.com/OpenSteam001/steam-monitor) — last resort, in case it publishes again.
3. **Local cache** — `<Steam>\opensteamtool\{pattern,ipc}\<component>\<sha256>.toml`, used when no remote source has the file.
4. **Local derivation** — if nothing matches the installed build, OpenSteamTool derives the metadata itself from earlier builds' TOMLs (and a baseline built into the DLL) and writes it to the cache. See below.

A 404 from one source skips that source's mirrors and moves on to the next one.

**Validation.** Whatever the source, every pattern entry is checked against the loaded DLL before use: its RVA must be a real function start and its signature must match there. Invalid entries are re-derived or dropped — never called blindly. IPC hashes are cross-checked against the IPC wrappers in `steamclient64.dll` and corrected if they disagree.

**Local derivation** carries signatures forward from previous builds: exact matches (re-anchored when a function's prologue grew), and close matches only where they are unambiguous — near where neighbouring functions moved, or clearly better than every other function in the DLL. Functions it cannot place safely are left out; the hooks that need them are disabled and a single popup names them. Derived files start with an `# Auto-derived ...` header and carry a `# derived: ...` note per entry. Turn it off with:

```toml
[remote]
auto_derive = false
```

**Publishing.** The [Steam metadata workflow](.github/workflows/metadata.yml) checks Valve's client-update feed every few hours. When a new Steam build ships it downloads the DLLs, runs the same derivation (`tools/derive_check`), commits the TOMLs to the `pattern` / `ipc` branches and opens an issue listing anything that needs a signature added by hand. Users on a new build therefore normally get published metadata without deriving anything locally.

You can also drop a TOML into the cache directory yourself; the file name must be `<sha256>.toml`.

#### After a Steam update

Usually nothing is needed: restart Steam and the new build's metadata is downloaded or derived. If a popup lists functions that could not be located, Valve rewrote those functions and they need a signature added by hand; the hooks that depend on them stay disabled until then. Check the [issues](https://github.com/matheeshapathirana/OpenSteamTool/issues) for that Steam build (the workflow opens one automatically) and restart Steam once it is fixed. Popups always include the Steam build ID and DLL hashes to quote in a report.

> A short outbound HTTPS request is performed at every launch per metadata file. The bodies are a few KB and the work runs on a worker thread, so it never blocks Steam's loader.

#### Using a different mirror

To use a private mirror or intranet server, configure a full URL template. A custom mirror replaces the built-in remote sources; the local cache and local derivation remain available.

The template must include `{channel}`, `{component}`, and `{sha256}`. Channels currently used are `pattern` and `ipc`.

```toml
[remote]
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/matheeshapathirana/OpenSteamTool@{channel}/{component}/{sha256}.toml"
```

#### Checking derivation offline

`derive_check` (build with `cmake --build build --config Release --target derive_check`) maps the Steam DLLs without running them and reports what would be derived for the installed build, compared with any cached TOML:

```
build\Release\derive_check.exe "C:\Program Files (x86)\Steam"
```

### Debug logging

Debug builds write per-module log files under `<Steam>/opensteamtool/`:

| File | Source | Content |
|------|--------|---------|
| `main.log`          | General | Init, config loading, Lua parsing, utilities |
| `ipc.log`           | `LOG_IPC_*` | IPC commands, InterfaceCall dispatch, spoofing |
| `netpacket.log`     | `LOG_NETPACKET_*` | Network packet send/recv, eMsg dispatch |
| `manifest.log`      | `LOG_MANIFEST_*` | Manifest download, `fetch_manifest_code`, manifest binding |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot decryption key injection |
| `keyvalue.log`      | `LOG_KEYVALUE_*` | KeyValues patching (manifest binding) |
| `misc.log`          | `LOG_MISC_*` | Engine pointer capture, AppId hints |
| `achievement.log`   | `LOG_ACHIEVEMENT_*` | UserStats requests/responses, steamid spoofing |
| `pics.log`          | `LOG_PICS_*` | PICS access token injection |
| `package.log`       | `LOG_PACKAGE_*` | Package injection, FileWatcher events |
| `onlinefix.log`     | `LOG_ONLINEFIX_*` | Online fix (480 AppId spoofing) |
| `richpresence.log`  | `LOG_RICHPRESENCE_*` | Rich Presence packet construction and injection |
| `steamui.log`       | `LOG_STEAMUI_*` | SteamUI hook diagnostics |
| `pipe.log`          | `LOG_PIPE_*` | Pipe handshakes, process inspection, Denuvo authorization, library injection |
| `platform.log`      | `LOG_PLATFORM_*` | Platform helper diagnostics, including remote-process operations |

The log level is controlled by `[log] level` in `opensteamtool.toml`.

## Build

### Requirements
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 with MSVC (x64 toolchain)

### Runtime requirements
- Outbound HTTPS access to `raw.githubusercontent.com` or `cdn.jsdelivr.net` to fetch Steam metadata (see [Steam version compatibility](#steam-version-compatibility)). Without network access the local cache and local derivation are used.

### Quick build
```powershell
build.bat
```

### Output
- Debug: `build/Debug/OpenSteamTool.dll`, `build/Debug/dwmapi.dll`, `build/Debug/xinput1_4.dll`
- Release: `build/Release/OpenSteamTool.dll`, `build/Release/dwmapi.dll`, `build/Release/xinput1_4.dll`

## Disclaimer
This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
