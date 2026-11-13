> **Fork note:** This project is an optimized fork of [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool), adapted for the Steam library manager [huanyuejue/Fluent-Steam-Lua](https://github.com/huanyuejue/Fluent-Steam-Lua).

<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-source Steam unlock tool</strong>
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

## Table of contents

- [How it works](#how-it-works)
- [Feature overview](#feature-overview)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Lua API reference](#lua-api-reference)
- [Features in depth](#features-in-depth)
- [Configuration reference](#configuration-reference)
- [Steam version compatibility](#steam-version-compatibility)
- [Logging and troubleshooting](#logging-and-troubleshooting)
- [Building](#building)
- [Third-party components](#third-party-components)
- [Disclaimer](#disclaimer)

## How it works

1. A **loader DLL** placed next to `steam.exe` forwards every export to the real system DLL and, when the host process is `steam.exe`, loads `OpenSteamTool.dll` from the same folder.
2. `OpenSteamTool.dll` hands all work to a worker thread (never file I/O or detours under the loader lock). It then makes a full copy of `steamclient64.dll` at `<Steam>\bin\diversion64.dll` — the **Diversion shadow module** — and applies every hook to that copy, leaving the original module byte-for-byte untouched so Denuvo/RE Engine integrity checks pass.
3. Steam is redirected onto the copy through two independent paths: the `LoadModuleWithPath` hijack (older builds) and a first-load hijack on `kernel32!LoadLibraryExW` (Steam build 1791 and later). A 15-second watchdog falls back to the original module if Steam never adopts the copy.
4. Hook modules then patch the running client: interface dispatch (`IClientUser`, `IClientUtils`), network messages, package/ownership state, depot decryption keys, KeyValues, manifest request codes, Rich Presence and misc launch behaviour.
5. A pipe-manager handshake identifies the game process attached to Steam; that is where Denuvo authorization and optional library injection happen.
6. Metadata (byte patterns and IPC descriptors) is keyed by the SHA-256 of the DLL on disk, fetched from the upstream mirrors and cached locally.

Only the Windows build is supported.

## Feature overview

| Feature | Summary | Details |
|---|---|---|
| Unlock apps and DLC | Adds apps and all their depots to the owned package | [details](#unlocking-apps-and-dlc) |
| Manifest sources | Six upstreams with ordered failover, or your own Lua endpoint | [details](#manifest-sources-and-failover) |
| Manifest binding | Pin a depot to a specific manifest GID | [details](#manifest-binding) |
| Hot reload | Lua files reload automatically (debounced 500 ms) | [details](#hot-reload) |
| Family Sharing | Play shared games alongside the lender | [details](#family-sharing) |
| Denuvo / SteamStub | Ticket forging for protected titles | [details](#denuvo-and-steamstub) |
| Stats and achievements | Achievements for unowned games via a SteamID override | [details](#stats-and-achievements) |
| Online fix | 480-based lobby matchmaking (`-onlinefix`) | [details](#online-fix) |
| Presence broadcast | How unlocked games appear to friends | [details](#presence-broadcast) |
| Game-process injection | Load your own DLL into the game | [details](#game-process-injection) |
| Cloud saves | Redirect Steam Cloud to Google Drive / OneDrive / a folder | [details](#cloud-save-redirect) |
| Third-party CD keys | Satisfy the "Updating product key" step | [details](#third-party-cd-keys) |

## Installation

### Requirements

- Windows 10/11, 64-bit Steam client
- A Debug or Release build of this project (see [Building](#building))

### Choose a loader

Copy `OpenSteamTool.dll` plus **one** loader DLL to the Steam root directory. Use exactly one entry point: two of them load two copies of `OpenSteamTool.dll` from different folders into the same Steam process and install every hook twice, which crashes Steam. `ost-manager.exe` enforces this for you (see [Manager and IFEO loader](#manager-and-ifeo-loader)).

| Loader | How Steam loads it | Trade-offs |
|---|---|---|
| `dwmapi.dll` | Statically imported by `bin\vgui2_s.dll`, so it enters when the UI starts | Not blocked by most anti-cheat systems (EAC/BE). Some games that verify `steamclient` crash, e.g. Capcom titles such as Onimusha and Dragon's Dogma 2 |
| `version.dll` | Statically imported by `steam.exe`, so it is the earliest entry point | Enables early isolation and fixes the crashes above, but is blocked by many anti-cheat systems, which makes those games unplayable |
| `xinput1_4.dll` | Full XInput wrapper (including undocumented ordinals); loaded by Steam when controller support initialises | Same forwarding and injection rules as `dwmapi.dll`; an optional extra entry point |

The shipped `version.dll` build has a `.disabled` suffix on purpose: `steam.exe` cannot find it and falls back to the system DLL. Rename it to `version.dll` only when you want early isolation, and never enable it together with a game that uses EAC/BE.

### Manager and IFEO loader

`build\tools\<Config>\` holds a complete bundle — `ost-manager.exe`, `ost-loader.exe`, `OpenSteamTool.dll` and the three proxies — so nothing has to be copied by hand.

| File | Role |
|---|---|
| `ost-manager.exe` | CLI, console menu (`--tui`) and windowed manager (double-click or `--ui`) |
| `ost-loader.exe` | Windowless helper that Windows runs instead of `steam.exe` through IFEO |

The window shows one entry-point picker with a one-line explanation per method, colour-coded status (green = healthy, amber = attention, red = two entry points), an icon on every action with a tooltip, a colour-coded action log (green info, amber warning, red error) and a Light/Dark/System theme switch stored per user (`HKCU\Software\OpenSteamTool\GuiTheme`). On Windows 11 the window uses the native chrome — Mica backdrop, rounded corners, DWM border colour, Segoe UI Variable and the accent colour from Settings for hover/pressed/focus — and falls back to the solid palette on Windows 10.

**Exactly one entry point is active at a time.** Every install removes the IFEO key, the other proxy DLLs and their payload copies before deploying its own, and `status` reports how many entry points are live (and warns when more than one is). Leftovers from an earlier install are recognised even when their bookkeeping is gone, so a stale proxy can never keep loading an old payload next to a fresh one.

```bat
ost-manager.exe status
ost-manager.exe install dwmapi        :: bin\dwmapi.dll proxy (default)
ost-manager.exe install version       :: version.dll proxy (early isolation)
ost-manager.exe install xinput        :: xinput1_4.dll proxy
ost-manager.exe install ifeo          :: IFEO loader, nothing copied into Steam
ost-manager.exe install ifeo-root     :: IFEO loader with ost-loader.exe in the Steam root
ost-manager.exe uninstall
```

Options: `--steam-dir <path>`, `--payload <path>`, `--data-dir <path>`, `--restart` (restart Steam around the change, `--force` to terminate a hanging Steam, `--no-start` to close it only), `--dry-run`, `--json`, `--ui`, `--tui`, `--log <path>`. The switches of the original `ost-manager` keep working: `/install-portable <dll>`, `/install-root <dir> <dll>`, `/deploy-dll <dir> [dll]`, `/remove-dll <dir>`, `/inject`, `/watch`, `/uninstall`, `/status`, `/enable-autostart`, `/disable-autostart`, `/silent`.

#### Where the tool writes

Steam's own files (`steamclient64.dll`, `steamui.dll`, `userdata`) always come from the real Steam folder, but everything the tool **writes** follows a data root: `opensteamtool.toml`, `config\lua`, the logs, the metadata caches and `bin\diversion64.dll`.

- Default data root: the folder holding `ost-manager.exe` — so a portable `ost` behaves exactly as if it sat inside Steam without writing into Steam. When the manager is placed inside Steam, the data root is Steam itself.
- `--data-dir <path>` overrides it; `config\lua` is created there on every install.
- Hot injection (`inject`, `watch`) cannot change the environment of an already running Steam, so the manager publishes the data root in `HKCU\Software\OpenSteamTool\ConfigDir` and the payload reads it on attach. The IFEO loader additionally passes `OST_CONFIG_DIR` to the Steam it starts, which takes precedence.

### Steps

1. Run `build.bat` (or build manually, see [Building](#building)).
2. Copy `OpenSteamTool.dll` and the loader you chose into the Steam root directory (next to `steam.exe`).
3. Start Steam. A `main.log` under `<Steam>\opensteamtool\` confirms the loader chain worked (Debug builds only).
4. Add your Lua scripts to `<Steam>\config\lua\`.

### Files created at runtime

| Path | Purpose |
|---|---|
| `<Steam>\opensteamtool.toml` | Optional configuration (see [Configuration reference](#configuration-reference)) |
| `<Steam>\opensteamtool\*.log` | Per-module logs, Debug builds only |
| `<Steam>\opensteamtool\pattern\<component>\<sha256>.toml` | Cached signature files for `steamclient64.dll` and `steamui.dll` |
| `<Steam>\opensteamtool\ipc\steamclient\<sha256>.toml` | Cached IPC descriptors |
| `<Steam>\bin\diversion64.dll` | Shadow copy of `steamclient64.dll` used for isolation |
| `<Steam>\cloud_redirect.dll` | Optional CloudRedirect payload (`[cloud]`) |

When you install with `ost-manager.exe`, the rows above are created under the **data root** instead of `<Steam>` (see [Where the tool writes](#where-the-tool-writes)).

### Uninstall

Close Steam and run `ost-manager.exe uninstall` (it removes the IFEO entry, the proxy DLLs, the payload copies, the autostart entry and the runtime artifacts). Manually: close Steam, delete the loader DLL and `OpenSteamTool.dll`, then delete `<Steam>\bin\diversion64.dll` and the `<Steam>\opensteamtool\` folder.

## Quick start

1. Create the Lua directory (`<Steam>\config\lua`) and drop a script in it:

   ```lua
   addappid(1361510) -- unlock the app with id 1361510
   addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0")
   addtoken(1361510, "2764735786934684318")
   setManifestid(1361511, "5656605350306673283")
   ```

2. Start Steam. Scripts are executed at startup and again whenever a `.lua` file changes.

3. Optionally copy a configuration example to `<Steam>\opensteamtool.toml`:

   | Language | File |
   |---|---|
   | English | [`opensteamtool.example.toml`](opensteamtool.example.toml) |
   | 中文 | [`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml) |
   | Español | [`opensteamtool.example.es.toml`](opensteamtool.example.es.toml) |

   The file is optional; without it the built-in defaults are used.

## Lua API reference

All function names are **case-insensitive**: `setAppTicket`, `setappticket` and `SETAPPTICKET` are equivalent, and the same applies to every function below.

| Function | Signature | Purpose | Notes |
|---|---|---|---|
| `addappid` | `addappid(id [, ignored, depotKey])` | Marks an app (and therefore its depots) as owned | The second argument is accepted for compatibility and ignored. `depotKey`, when given, must be exactly 64 hex characters |
| `addtoken` | `addtoken(appid, token)` | Adds a PICS access token | `token` is a decimal 64-bit value passed as a string |
| `setmanifestid` | `setmanifestid(depotId, gid)` | Pins a depot to a manifest | Extra arguments are ignored; the manifest size is forced to 0 |
| `setAppTicket` | `setAppTicket(appid, hex)` | Writes an ownership ticket to the credential store | On Windows: `HKCU\Software\Valve\Steam\Apps\<appid>\AppTicket` |
| `setETicket` | `setETicket(appid, hex)` | Writes an encrypted ticket to the credential store | Same key path, value name `ETicket` |
| `setStat` | `setStat(appid, "steamid")` | Chooses whose achievement data to use | A decimal SteamID64 as a string |
| `setlegacycdkey` | `setlegacycdkey(appid, "KEY")` | Supplies a third-party product key | Stored verbatim; see [Third-party CD keys](#third-party-cd-keys) |
| `forcedenuvo` | `forcedenuvo(appid)` | Treats an app as Denuvo-protected | Skips protection scanning; only affects detection |
| `http_get` | `body, status = http_get(url [, headers])` | HTTP GET for Lua scripts | Returns `body, status`, or `nil, error` |
| `http_post` | `body, status = http_post(url, body [, headers])` | HTTP POST for Lua scripts | Same return values |

`headers` is an optional table such as `{["Accept"]="application/json"}`.

A script may also define the manifest hooks `fetch_manifest_code(gid)` and `fetch_manifest_code_ex(app_id, depot_id, gid)`; see [Manifest sources and failover](#manifest-sources-and-failover).

## Features in depth

### Unlocking apps and DLC

Adding an app with `addappid` makes Steam treat the app and every depot it references as owned. Depot decryption keys are served from the Lua configuration, so content that needs a key unlocks and downloads normally. Access tokens for protected apps are supplied with `addtoken`.

### Manifest sources and failover

Manifest request codes are fetched from one of six upstreams:

| Name | Endpoint |
|---|---|
| `20770407` (default) | `https://20770407.xyz/manifest/{depotId}/{gid}` |
| `SDM` | `https://steamapi.993499094.xyz/manifest/{depotId}/{gid}` |
| `manifestdex` | `https://manifest.manifestdex.com/{gid}` (sends `User-Agent: ManifestDeX/1.0`) |
| `wudrm` | `http://gmrc.wudrm.com/manifest/{gid}` |
| `steamrun` | `https://manifest.steam.run/api/manifest/{gid}` |
| `opensteamtool` | `https://manifest.opensteamtool.com/{gid}` |

`[manifest] url` selects the preferred upstream. With `failover = true` (the default) a failure moves on to the next upstream in table order; a source that just failed is skipped for 60 seconds, and the configured source always stays first for the next fetch. All attempts of one fetch share a 15-second budget, so a slow source cannot stall depot downloads.

Lua can override the HTTP path entirely. If `<Steam>\config\lua\manifest.lua` (or any loaded script) defines `fetch_manifest_code_ex(app_id, depot_id, gid)` or `fetch_manifest_code(gid)`, those are tried before the upstreams; `_ex` is tried first. `http_get`/`http_post` are available inside them.

### Manifest binding

`setManifestid(depotId, gid)` pins a depot to a specific manifest, which prevents a game from receiving an updated (and possibly incompatible) manifest. It is applied through the KeyValues path.

### Hot reload

The `config\lua` folder and every directory listed in `[lua] paths` are watched. Adding, modifying, renaming or deleting a `.lua` file triggers a reload after a 500 ms debounce — no Steam restart, no offline/online toggle. Files from `[lua] paths` load first; the default folder loads last so user edits win.

### Family Sharing

Shared games can be played directly. For a game that is also listed in Lua, OpenSteamTool takes it off the sharing path completely:

- the local license check is handled like a fake-licence app and the sharing flags are cleared;
- when play state is reported, the AppId is replaced with a value that does not exist on the server, so the server neither counts a sharing slot nor sends a stop-playing notice;
- the trade-off is that purchase state, achievements and playtime are tied to the fake licence, and friends do not see the real activity. With `[presence] display = "spacewar"` these games are reported as Spacewar instead, so friends see what you play, at the cost of playtime being counted on Spacewar.

Shared games that are **not** in Lua keep Steam's native rules: they occupy the lender's slot and can be reclaimed. Games you genuinely own are unaffected even if they are listed in Lua (detected because the licence appears in more than one package).

### Denuvo and SteamStub

SteamStub-protected titles need no ticket configuration: OpenSteamTool reuses Steam's local ConfigStore ticket and forges the requested AppId, with no game-process injection.

Denuvo-protected titles need explicit ticket data, stored in the platform credential store:

- `setAppTicket(appid, "hex")` and `setETicket(appid, "hex")` write the values;
- Denuvo authorization has a 30-minute window; after it expires authorization can fail with error `88500005`, so refresh the tickets and retry;
- explicit tickets (configured or cached) take priority; otherwise OpenSteamTool falls back to a forged ConfigStore ticket;
- forging needs one genuine ticket as a base. The lookup order is: AppId 7 in memory → AppId 228980 (Steamworks common redistributable) in memory → the `apptickets` section of `<Steam>\userdata\<account>\config\localconfig.vdf` → the registry key `HKCU\Software\Valve\Steam\Apps\<AppId>\AppTicket`. Tickets must match the signed-in account, otherwise Steam reports error 54;
- the SteamID comes from the cached credential value, or from an explicit AppTicket when absent. The Windows backend uses `HKCU\Software\Valve\Steam\Apps\<AppId>`; a Linux backend is not implemented;
- when the signed-in account owns the game, OpenSteamTool writes the real SteamID to the credential store at the end of the Denuvo handshake, so a later account can start the game with the cached identity.

Denuvo detection tries the entry-point signature, then a `DENUVO` string in the section, then a structural check (a large writable+executable section with either high entropy or a legacy protector section name such as `.arch`, `.srdata`, `.xpdata`, `.xtls`). If `pipe.log` says `no Denuvo match` for a game that is definitely protected, force it with `forcedenuvo(appid)`.

#### Extracting tickets with `extract_tickets`

`extract_tickets` dumps the `AppTicket` and `ETicket` hex strings used by `setAppTicket` / `setETicket`. Run it on a machine where Steam is running and signed in to an account that **owns** the game.

1. Build it (see [Building](#building)); the binary is `build/tools/<Config>/extract_tickets.exe`.
2. Run it with the AppId (or without arguments and enter the AppId when prompted):

   ```powershell
   extract_tickets.exe 1361510
   ```

3. It reads the Steam install path from the registry, loads `steamclient64.dll` and writes an `<appid>/` folder next to the executable:
   - `appticket.bin` — raw ownership ticket (binary)
   - `eticket.bin` — raw encrypted ticket (binary)
   - `tickets.txt` — plain-text summary with the hex strings

   Tickets that cannot be obtained are reported as `appticket:null` / `eticket:null`.
4. Paste the hex strings into your Lua script:

   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Note:** tickets are only valid when extracted from an account that **genuinely owns** the game.

### Stats and achievements

Achievements and stats work for unowned games:

- `setStat(appid, "steamid")` selects which SteamID's achievement data to pull;
- if no override exists, the stats API is queried when `[stats] enable_api = true` (default): `https://stats.luastools.xyz/{appid}`;
- priority: `setStat` > stats API (when enabled and it returns a valid value) > the hardcoded preset SteamID `76561198028121353`.

### Online fix

- Add `-onlinefix` to the Steam launch options to enable 480-based lobby matchmaking. Only one such game can run at a time. Remove the parameter to return to normal online play.
- The parameter does not require the game to be in the Lua list; genuinely owned games can use it too.
- If a game hangs on the loading screen, shows a black screen or misbehaves after login, add `-realappid` (`-onlinefix -realappid`). The game then receives its real AppId instead of the spoofed 480.
- Both behaviours are needed by different titles and cannot be detected automatically, so the choice is per launch.

### Presence broadcast

`[presence] display` controls what friends see for unlocked games:

- `"spacewar"` (default) — broadcast the real game name;
- `"none"` — skip the name-broadcast patches; unowned games become invisible to friends, and `-onlinefix` games fall back to Spacewar.

Any other value behaves like `"spacewar"`.

### Game-process injection

`[inject]` optionally loads your own library into the game process once the pipe handshake identifies it:

- set `enabled = true` and provide `library_x64` / `library_x86`;
- the library must match the process architecture; an unknown architecture is skipped;
- relative paths resolve from the Steam root directory;
- each process is injected at most once.

There is no bundled injection library — this is a hook for your own tools.

### Cloud save redirect

`[cloud]` integrates [CloudRedirect](https://github.com/Selectively11/CloudRedirect):

- `enabled = true` loads `cloud_redirect.dll` inside Steam and initialises it in cloud-save mode;
- every game added with `addappid` is registered as a redirected app, and Steam Cloud RPCs for those apps are forwarded to CloudRedirect's engine;
- provider sign-in (Google Drive, OneDrive or a local folder) is still done in CloudRedirect's companion app — OpenSteamTool only hosts the DLL;
- `library` may be absolute or relative to the Steam root; the default is `<Steam>\cloud_redirect.dll`;
- if the DLL is missing or lacks the expected exports, the feature disables itself and logs the reason.

### Third-party CD keys

Titles that carry a Steam-issued third-party retail key (Ubisoft Connect, Rockstar and others) request that key during startup. OpenSteamTool answers the request locally so the "Updating product key" step completes for added games:

- `setlegacycdkey(appid, "KEY")` supplies a real key, stored verbatim; it only applies to apps added through Lua;
- with no override, a deterministic synthetic key is generated from the AppId and account, so relaunches are stable;
- a synthetic key only satisfies Steam's local step — titles that validate the key online still need a real one.

## Configuration reference

The configuration file is `<Steam>\opensteamtool.toml`. It is optional, hot-reloaded on change, and unknown keys are ignored. **A file that fails to parse is rejected as a whole** and the built-in defaults are used instead, so check the log after editing.

Localized examples with identical keys and values: [`opensteamtool.example.toml`](opensteamtool.example.toml) (English), [`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml) (中文), [`opensteamtool.example.es.toml`](opensteamtool.example.es.toml) (Español).

### `[log]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `level` | string | `"debug"` | `trace`, `debug`, `info`, `warn`, `error`. Debug builds only; Release builds compile logging out. Logs go to `<Steam>\opensteamtool\*.log` |

### `[manifest]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `url` | string | `"20770407"` | One of the six upstream names; an unknown name logs a warning and falls back to `20770407` |
| `failover` | bool | `true` | `false` queries only `url` |
| `timeout_resolve_ms` | int | `5000` | Clamped to `[1, 15000]`, the whole-fetch budget |
| `timeout_connect_ms` | int | `5000` | Same clamp |
| `timeout_send_ms` | int | `10000` | Same clamp |
| `timeout_recv_ms` | int | `10000` | Same clamp |

### `[stats]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `enable_api` | bool | `true` | Query the stats API when no `setStat` override exists |
| `api_url` | string | `"https://stats.luastools.xyz/{appid}"` | Stats endpoint; must contain `{appid}`. The response body is a SteamID64 |

### `[presence]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `display` | string | `"spacewar"` | `"spacewar"` or `"none"`; any other value behaves like `"spacewar"` |

### `[lua]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `paths` | array of strings | `[]` | Extra Lua directories, loaded before the default `config\lua` folder |

### `[inject]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `false` | Enables game-process injection |
| `library_x64` | string | unset | Required for 64-bit game processes |
| `library_x86` | string | unset | Required for 32-bit game processes |

### `[cloud]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `false` | Enables Steam Cloud redirection through CloudRedirect |
| `library` | string | `<Steam>\cloud_redirect.dll` | Absolute, or relative to the Steam root |

### `[remote]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `url_template` | string | unset | Custom metadata mirror; must contain `{channel}`, `{component}` and `{sha256}` |
| `order` | string | `"gh-proxy-first"` | `gh-proxy-first`, `github-first`, or the legacy alias `jsdelivr-first` (= `gh-proxy-first`) |
| `auto_derive` | bool | `true` | Derive missing pattern/IPC metadata locally from cached earlier builds and a built-in baseline; every derived value is validated against the loaded DLL |

## Steam version compatibility

OpenSteamTool ships no byte-pattern signatures. On each launch it hashes `steamclient64.dll` and `steamui.dll` and looks up a matching descriptor on the `pattern` and `ipc` branches of this repository, falling back to a maintained fork and the original tracker.

Lookup order (first launch after a Steam update, when the cache is cold):

1. **Local cache** — `<Steam>\opensteamtool\<channel>\<component>\<sha256>.toml`. A hit is used directly; the cache is overwritten after every successful remote fetch.
2. **This repository's metadata branches** — the `pattern` and `ipc` branches, fetched through gh-proxy or GitHub raw depending on `[remote] order`.
3. **jsDelivr mirror** — `https://cdn.jsdelivr.net/gh/alex47exe/OST@{channel}/{component}/{sha256}.toml`.
4. **Maintained fork** — the `pattern` and `ipc` branches of `matheeshapathirana/OpenSteamTool`, for builds this repository does not have yet.
5. **Original tracker** — `huanyuejue/steam-monitor` (`main/{channel}/{component}/{sha256}.toml`).
6. **Local derivation** — when no source has the build, pattern and IPC metadata are derived from cached earlier builds plus a built-in baseline, validated against the loaded DLL and written to the cache. Derived files are re-checked against the mirrors on each launch, so a published copy replaces them. Disable with `[remote] auto_derive = false`.

Whatever the source, every pattern entry is checked against the loaded DLL before use: its RVA must be a real function start and its signature must match there. Invalid entries are re-derived or dropped, never called blindly. IPC hashes are cross-checked against the wrappers in `steamclient64.dll` and corrected if they disagree.

Only when every mirror returns **HTTP 404** is the descriptor considered unpublished. Without a cache, a one-shot popup then shows the unmatched DLL name, its SHA-256, the expected cache path and the upstream URL. Only the hooks tied to that DLL are disabled; the rest of OpenSteamTool keeps working.

You can also place a descriptor manually in the cache directory as `<sha256>.toml`. On a cache miss a few short outbound HTTPS requests are made (one per mirror until one succeeds, about 10 KB each) on a worker thread, so Steam's loader is never blocked.

### Using a different mirror

A custom `[remote] url_template` replaces the built-in mirrors and must include `{channel}`, `{component}` and `{sha256}`. Channels currently used are `pattern` and `ipc`.

```toml
[remote]
# "gh-proxy-first" (default), "github-first", or legacy "jsdelivr-first".
# order = "gh-proxy-first"
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

## Logging and troubleshooting

Debug builds write one file per module to `<Steam>\opensteamtool\`; `[log] level` controls the verbosity globally.

| File | Macros | Content |
|---|---|---|
| `main.log` | generic | Initialisation, configuration, Lua loading, tools |
| `ipc.log` | `LOG_IPC_*` | Interface calls, dispatch, spoofing |
| `netpacket.log` | `LOG_NETPACKET_*` | Network messages, eMsg dispatch |
| `manifest.log` | `LOG_MANIFEST_*` | Manifest downloads, `fetch_manifest_code`, binding |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot key injection |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues patches (manifest binding) |
| `misc.log` | `LOG_MISC_*` | Engine pointer capture, AppId rewrites |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | UserStats traffic, SteamID spoofing |
| `pics.log` | `LOG_PICS_*` | PICS access tokens |
| `package.log` | `LOG_PACKAGE_*` | Package injection, file-watcher events |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | Online fix (480 spoofing) |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Rich Presence packet construction and injection |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI hooks and isolation |
| `pipe.log` | `LOG_PIPE_*` | Pipe handshake, process inspection, Denuvo auth, injection |
| `platform.log` | `LOG_PLATFORM_*` | Platform helpers, remote-process operations |

Common situations:

| Symptom | Likely cause | Fix |
|---|---|---|
| Steam does not start, `VGUI_Setup failed` | A loader is not forwarding exports correctly, or two loaders conflict | Remove the extra loader; keep only one, preferably `dwmapi.dll` |
| Game exits with error 54 | Ticket belongs to a different account, or no genuine ticket was found | Extract tickets from an owning account; check `pipe.log` for the donor source |
| Denuvo error `88500005` | Authorization window (30 minutes) expired | Refresh `AppTicket` / `ETicket` and retry |
| Game hangs or black-screens on launch with `-onlinefix` | The game reads its AppId during startup | Launch with `-onlinefix -realappid` |
| Popup listing missing Steam metadata | Upstream has not published this Steam build yet | Wait for an update, or drop the TOML into the cache folder manually |
| Config changes have no effect | The TOML failed to parse and defaults were applied | Check `main.log` for `Config parse error` and validate the file |
| Anti-cheat blocks the game | `version.dll` entry point enabled | Use `dwmapi.dll` (or `xinput1_4.dll`) instead |

## Building

### Requirements

- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 or newer with the MSVC x64 toolchain (C++20)

### Build

```powershell
build.bat
```

The script builds Release and Debug, then builds `extract_tickets` (excluded from the main target). It uses Ninja Multi-Config when Ninja is available; otherwise it requests the `Visual Studio 17 2022` generator, so override `GENERATOR` if you only have a newer Visual Studio. Third-party dependencies (Detours, spdlog, protobuf, toml++, Lua) are fetched automatically into `.deps\`.

### Output

- Debug / Release: `build\<Config>\OpenSteamTool.dll`, `build\<Config>\dwmapi.dll`, `build\<Config>\xinput1_4.dll`, `build\<Config>\version.dll.disabled`
- Tools: `build\tools\<Config>\extract_tickets.exe`, `ost-manager.exe`, `ost-loader.exe` (the manager and the loader are staged together with `OpenSteamTool.dll` and the proxies into `build\tools\<Config>\`, so that folder is the ready-to-use bundle)

## Third-party components

| Component | Use | License |
|---|---|---|
| [Microsoft Detours](https://github.com/microsoft/Detours) | Function hooking | MIT |
| [spdlog](https://github.com/gabime/spdlog) | Logging | MIT |
| [protobuf](https://github.com/protocolbuffers/protobuf) | Steam message serialisation | BSD-3-Clause |
| [toml++](https://github.com/marzer/tomlplusplus) | Configuration and metadata parsing | MIT |
| [Lua 5.5](https://www.lua.org/) | Scripting | MIT |
| [CloudRedirect](https://github.com/Selectively11/CloudRedirect) | Optional cloud-save redirection | See upstream |
| [huanyuejue/steam-monitor](https://github.com/huanyuejue/steam-monitor) | Upstream signature/IPC metadata | See upstream |

## Disclaimer

This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
