> **Fork 说明：** 本项目是 [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) 的优化分支，为适配 Steam 入库工具 [huanyuejue/Fluent-Steam-Lua](https://github.com/huanyuejue/Fluent-Steam-Lua) 而开发。

<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>开源 Steam 解锁工具</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="仅 Windows">
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

## 目录

- [工作原理](#工作原理)
- [功能概览](#功能概览)
- [安装](#安装)
- [快速开始](#快速开始)
- [Lua API 参考](#lua-api-参考)
- [功能详解](#功能详解)
- [配置参考](#配置参考)
- [Steam 版本兼容性](#steam-版本兼容性)
- [日志与故障排查](#日志与故障排查)
- [构建](#构建)
- [第三方组件](#第三方组件)
- [免责声明](#免责声明)

## 工作原理

1. 放在 `steam.exe` 同级的**加载器 DLL**会把自身的每个导出函数原样转发给系统 DLL，并在宿主进程是 `steam.exe` 时从同一目录加载 `OpenSteamTool.dll`。
2. `OpenSteamTool.dll` 把所有工作交给工作线程（绝不在 loader lock 下做文件 I/O 或装 Detour）。随后它把 `steamclient64.dll` 完整复制到 `<Steam>\bin\diversion64.dll`（**Diversion 影子模块**），所有 hook 只打在这份副本上，原版模块保持逐字节干净，Denuvo/RE 引擎的完整性检查才能通过。
3. Steam 通过两条独立路径被换到副本上：`LoadModuleWithPath` 劫持（较老版本）与 `kernel32!LoadLibraryExW` 首载劫持（Steam 1791 及以后）。若 Steam 始终没换过去，15 秒看门狗会回退到原版模块。
4. 各 hook 模块随后对运行中的客户端打补丁：接口分发（`IClientUser`、`IClientUtils`）、网络消息、包/所有权状态、仓库解密密钥、KeyValues、manifest 请求码、Rich Presence 以及启动期杂项行为。
5. 管道握手指明当前接入 Steam 的游戏进程，Denuvo 授权与可选的库注入都在这一步完成。
6. 元数据（字节特征与 IPC 描述文件）以磁盘上 DLL 的 SHA-256 为键，从上游镜像获取并缓存在本地。

仅支持 Windows 构建。

## 功能概览

| 功能 | 简述 | 详情 |
|---|---|---|
| 解锁游戏与 DLC | 把游戏及其全部仓库加入已拥有的包 | [详情](#解锁游戏与-dlc) |
| Manifest 来源 | 六个上游，支持顺位回退，也可用自定义 Lua 端点 | [详情](#manifest-来源与顺位回退) |
| Manifest 绑定 | 把仓库固定到指定的 manifest GID | [详情](#manifest-绑定) |
| 热重载 | Lua 文件改动后自动重载（500 毫秒防抖） | [详情](#热重载) |
| 家庭共享 | 与出借方同时游玩共享游戏 | [详情](#家庭共享) |
| Denuvo / SteamStub | 为受保护游戏伪造票据 | [详情](#denuvo-与-steamstub) |
| 统计与成就 | 通过 SteamID 覆盖为未拥有游戏启用成就 | [详情](#统计与成就) |
| 在线修复 | 基于 480 的大厅联机（`-onlinefix`） | [详情](#在线修复) |
| 好友播报 | 已解锁游戏在好友侧的显示方式 | [详情](#好友播报) |
| 游戏进程注入 | 把自己的 DLL 注入游戏进程 | [详情](#游戏进程注入) |
| 云存档 | 把 Steam Cloud 重定向到 Google Drive / OneDrive / 本地文件夹 | [详情](#云存档重定向) |
| 第三方 CD Key | 完成「正在更新产品密钥」步骤 | [详情](#第三方-cd-key) |

## 安装

### 要求

- Windows 10/11，64 位 Steam 客户端
- 本项目的 Debug 或 Release 构建（见 [构建](#构建)）

### 选择加载器

把 `OpenSteamTool.dll` 和 **一个** 加载器 DLL 复制到 Steam 根目录。只使用一个入口：两个入口会从不同目录把两份 `OpenSteamTool.dll` 载入同一个 Steam 进程，所有 hook 被安装两次，导致 Steam 崩溃。`ost-manager.exe` 会为你保证这一点（见 [管理器与 IFEO 加载器](#管理器与-ifeo-加载器)）。

| 加载器 | Steam 如何加载它 | 取舍 |
|---|---|---|
| `dwmapi.dll` | 由 `bin\vgui2_s.dll` 静态导入，界面启动时进入进程 | 不会被大多数反作弊系统拦截（EAC/BE）。部分校验 `steamclient` 的游戏会崩溃，例如卡普空的鬼武者、龙信2 等 |
| `version.dll` | 由 `steam.exe` 静态导入，是最早的入口 | 能做早注隔离，解决上面的崩溃问题；但会被许多反作弊系统拦截，导致这些游戏无法游玩 |
| `xinput1_4.dll` | 完整的 XInput 包装（含未文档化的序数），Steam 初始化手柄支持时加载 | 转发与注入规则同 `dwmapi.dll`，可作为额外的可选入口 |

发布的 `version.dll` 构建故意带 `.disabled` 后缀：`steam.exe` 找不到它，会回退到系统 DLL。只有需要早注隔离时才改名为 `version.dll`，并且不要与使用 EAC/BE 的游戏同时启用。

### 管理器与 IFEO 加载器

`build\tools\<Config>\` 里是一套完整的文件——`ost-manager.exe`、`ost-loader.exe`、`OpenSteamTool.dll` 和三个代理——无需手工复制任何东西。

| 文件 | 作用 |
|---|---|
| `ost-manager.exe` | 命令行、控制台菜单（`--tui`）和窗口管理器（双击或 `--ui`） |
| `ost-loader.exe` | 无窗口的辅助程序，Windows 通过 IFEO 用它代替 `steam.exe` 启动 |

**任何时刻只有一个入口处于激活状态。** 每次安装都会先删除 IFEO 键、其他代理 DLL 及其载荷副本，然后再部署自己；`status` 会显示当前激活的入口数量（多于一个时给出警告）。即使记录已丢失，旧安装残留也能被识别，因此过期的代理绝不会和新载荷同时加载。

```bat
ost-manager.exe status
ost-manager.exe install dwmapi        :: bin\dwmapi.dll 代理（默认）
ost-manager.exe install version       :: version.dll 代理（早注隔离）
ost-manager.exe install xinput        :: xinput1_4.dll 代理
ost-manager.exe install ifeo          :: IFEO 加载器，不向 Steam 内复制任何文件
ost-manager.exe install ifeo-root     :: IFEO 加载器，并把 ost-loader.exe 部署到 Steam 根目录
ost-manager.exe uninstall
```

选项：`--steam-dir <路径>`、`--payload <路径>`、`--restart`（安装前后重启 Steam，`--force` 强制结束卡住的 Steam，`--no-start` 只关闭不启动）、`--dry-run`、`--json`、`--ui`、`--tui`、`--log <路径>`。原版 `ost-manager` 的开关继续可用：`/install-portable <dll>`、`/install-root <dir> <dll>`、`/deploy-dll <dir> [dll]`、`/remove-dll <dir>`、`/inject`、`/watch`、`/uninstall`、`/status`、`/enable-autostart`、`/disable-autostart`、`/silent`。

### 步骤

1. 运行 `build.bat`（或手动构建，见 [构建](#构建)）。
2. 把 `OpenSteamTool.dll` 和你选定的加载器复制到 Steam 根目录（与 `steam.exe` 同级）。
3. 启动 Steam。`<Steam>\opensteamtool\` 下的 `main.log` 能确认加载链是否生效（仅 Debug 构建）。
4. 把 Lua 脚本放入 `<Steam>\config\lua\`。

### 运行期生成的文件

| 路径 | 用途 |
|---|---|
| `<Steam>\opensteamtool.toml` | 可选配置（见 [配置参考](#配置参考)） |
| `<Steam>\opensteamtool\*.log` | 各模块日志，仅 Debug 构建 |
| `<Steam>\opensteamtool\pattern\<component>\<sha256>.toml` | `steamclient64.dll` 与 `steamui.dll` 的特征文件缓存 |
| `<Steam>\opensteamtool\ipc\steamclient\<sha256>.toml` | IPC 描述文件缓存 |
| `<Steam>\bin\diversion64.dll` | 用于隔离的 `steamclient64.dll` 副本 |
| `<Steam>\cloud_redirect.dll` | 可选的 CloudRedirect 载荷（`[cloud]`） |

### 卸载

关闭 Steam 后运行 `ost-manager.exe uninstall`（会清除 IFEO 项、代理 DLL、载荷副本、开机启动项和运行期产物）。手工方式：关闭 Steam，删除加载器 DLL 与 `OpenSteamTool.dll`，再删除 `<Steam>\bin\diversion64.dll` 和 `<Steam>\opensteamtool\` 目录。

## 快速开始

1. 创建 Lua 目录（`<Steam>\config\lua`），放入一个脚本：

   ```lua
   addappid(1361510) -- 解锁 id 为 1361510 的游戏
   addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0")
   addtoken(1361510, "2764735786934684318")
   setManifestid(1361511, "5656605350306673283")
   ```

2. 启动 Steam。脚本在启动时执行，之后任何 `.lua` 文件发生改动都会重新执行。

3. 可选：把配置示例复制为 `<Steam>\opensteamtool.toml`：

   | 语言 | 文件 |
   |---|---|
   | English | [`opensteamtool.example.toml`](opensteamtool.example.toml) |
   | 中文 | [`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml) |
   | Español | [`opensteamtool.example.es.toml`](opensteamtool.example.es.toml) |

   配置文件是可选的；没有它时使用内置默认值。

## Lua API 参考

所有函数名**不区分大小写**：`setAppTicket`、`setappticket`、`SETAPPTICKET` 等价，下表所有函数同理。

| 函数 | 签名 | 用途 | 说明 |
|---|---|---|---|
| `addappid` | `addappid(id [, 忽略, depotKey])` | 把游戏（及其全部仓库）标记为已拥有 | 第二个参数仅为兼容而接受，会被忽略。`depotKey` 若填写必须是 64 个十六进制字符 |
| `addtoken` | `addtoken(appid, token)` | 添加 PICS 访问令牌 | `token` 是十进制 64 位值，以字符串传入 |
| `setmanifestid` | `setmanifestid(depotId, gid)` | 把仓库固定到某个 manifest | 多余参数被忽略；manifest 大小强制为 0 |
| `setAppTicket` | `setAppTicket(appid, hex)` | 把所有权票据写入凭据存储 | Windows 下位于 `HKCU\Software\Valve\Steam\Apps\<appid>\AppTicket` |
| `setETicket` | `setETicket(appid, hex)` | 把加密票据写入凭据存储 | 同一路径，值名为 `ETicket` |
| `setStat` | `setStat(appid, "steamid")` | 指定使用哪个账号的成就数据 | 十进制 SteamID64 字符串 |
| `setlegacycdkey` | `setlegacycdkey(appid, "KEY")` | 提供第三方产品密钥 | 原样保存；见 [第三方 CD Key](#第三方-cd-key) |
| `forcedenuvo` | `forcedenuvo(appid)` | 强制把游戏按 Denuvo 处理 | 跳过保护扫描；只影响识别 |
| `http_get` | `body, status = http_get(url [, headers])` | 供 Lua 使用的 HTTP GET | 成功返回 `body, status`，失败返回 `nil, error` |
| `http_post` | `body, status = http_post(url, body [, headers])` | 供 Lua 使用的 HTTP POST | 返回值同上 |

`headers` 是可选表，例如 `{["Accept"]="application/json"}`。

脚本还可以定义 manifest 钩子 `fetch_manifest_code(gid)` 与 `fetch_manifest_code_ex(app_id, depot_id, gid)`，见 [Manifest 来源与顺位回退](#manifest-来源与顺位回退)。

## 功能详解

### 解锁游戏与 DLC

用 `addappid` 加入游戏后，Steam 会把该游戏及其引用的每个仓库都视为已拥有。仓库解密密钥由 Lua 配置提供，需要密钥的内容可以正常解锁与下载。受保护游戏所需的访问令牌通过 `addtoken` 提供。

### Manifest 来源与顺位回退

Manifest 请求码来自六个上游之一：

| 名称 | 端点 |
|---|---|
| `20770407`（默认） | `https://20770407.xyz/manifest/{depotId}/{gid}` |
| `SDM` | `https://steamapi.993499094.xyz/manifest/{depotId}/{gid}` |
| `manifestdex` | `https://manifest.manifestdex.com/{gid}`（会发送 `User-Agent: ManifestDeX/1.0`） |
| `wudrm` | `http://gmrc.wudrm.com/manifest/{gid}` |
| `steamrun` | `https://manifest.steam.run/api/manifest/{gid}` |
| `opensteamtool` | `https://manifest.opensteamtool.com/{gid}` |

`[manifest] url` 选择首选上游。开启 `failover = true`（默认）后，失败会按表顺序尝试下一个上游；刚失败的上游在 60 秒内被跳过，而配置的来源在下次取码时始终排第一。单次取码的所有尝试共享 15 秒预算，因此慢速上游不会拖住仓库下载。

Lua 可以完全接管 HTTP 取码。若 `<Steam>\config\lua\manifest.lua`（或任何已加载脚本）定义了 `fetch_manifest_code_ex(app_id, depot_id, gid)` 或 `fetch_manifest_code(gid)`，会先于上游尝试，其中 `_ex` 优先。函数内部可以使用 `http_get` / `http_post`。

### Manifest 绑定

`setManifestid(depotId, gid)` 把某个仓库固定到指定 manifest，避免游戏拿到更新后的（可能不兼容的）manifest。该功能通过 KeyValues 路径生效。

### 热重载

`config\lua` 目录以及 `[lua] paths` 中列出的每个目录都会被监视。新增、修改、重命名或删除 `.lua` 文件会在 500 毫秒防抖后触发重载——无需重启 Steam，也无需切换离线/在线模式。`[lua] paths` 中的文件先加载，默认目录最后加载，因此用户改动优先级最高。

### 家庭共享

共享游戏可以直接游玩。对于同时写进 Lua 的游戏，OpenSteamTool 会让它彻底脱离共享逻辑：

- 本地许可判定按假入库处理，共享标记全部清掉；
- 上报游玩状态时把 AppId 换成服务器上不存在的值，服务器既不计入共享名额，也不会下发停玩通知；
- 代价是购买状态、成就与时长都挂在假许可上，好友侧看不到真实游玩状态。开启 `[presence] display = "spacewar"` 后这类游戏会按 Spacewar 上报，好友能看到你在玩什么，代价是时长记到 Spacewar 上。

**没有**写进 Lua 的共享游戏仍按 Steam 原生规则运行：占用出借方的运行名额，出借方需要时会被收回。自己购买的游戏即使写进 Lua 也不受影响（判据是这条许可出现在多个包中）。

### Denuvo 与 SteamStub

仅 SteamStub 保护的游戏不需要配置票据：OpenSteamTool 复用 Steam 本地 ConfigStore 票据并伪造请求的 AppId，无需注入游戏进程。

Denuvo 保护的游戏需要显式票据数据，保存在平台凭据存储中：

- `setAppTicket(appid, "hex")` 与 `setETicket(appid, "hex")` 负责写入；
- Denuvo 授权有 30 分钟有效窗口，过期后授权可能失败并显示错误 `88500005`，刷新票据后重试；
- 显式票据（配置或缓存的）优先级最高，否则回退到伪造的 ConfigStore 票据；
- 伪造需要一张真实票据作基底，取票顺序为：内存中的 AppId 7 → 内存中的 AppId 228980（Steamworks 公共运行库）→ `<Steam>\userdata\<账号>\config\localconfig.vdf` 的 `apptickets` 段 → 注册表 `HKCU\Software\Valve\Steam\Apps\<AppId>\AppTicket`。票据必须与当前登录账号一致，否则 Steam 报错误 54；
- SteamID 优先取自缓存的凭据值，缺失时从显式 AppTicket 解析。Windows 后端使用 `HKCU\Software\Valve\Steam\Apps\<AppId>`；Linux 后端尚未实现；
- 当前登录账号拥有该游戏时，OpenSteamTool 会在 Denuvo 握手结束时把真实 SteamID 写入凭据存储，之后换号也能凭已缓存的身份启动。

Denuvo 识别依次尝试：入口点特征、段内 `DENUVO` 字样，最后回退到结构特征（存在足够大的可写可执行段，且满足「熵值够高」或「带 legacy 保护壳段名（`.arch`、`.srdata`、`.xpdata`、`.xtls`）」其一）。若 `pipe.log` 对确定受保护的游戏报 `no Denuvo match`，用 `forcedenuvo(appid)` 强制指定。

#### 使用 `extract_tickets` 提取票据

`extract_tickets` 会转储 `setAppTicket` / `setETicket` 所需的 `AppTicket` 与 `ETicket` 十六进制字符串。请在 Steam 正在运行、且登录到**拥有**该游戏账号的机器上执行。

1. 构建工具（见 [构建](#构建)）；二进制位于 `build/tools/<Config>/extract_tickets.exe`。
2. 带 AppId 运行（或不带参数运行，在提示时输入 AppId）：

   ```powershell
   extract_tickets.exe 1361510
   ```

3. 它从注册表读取 Steam 安装路径，加载 `steamclient64.dll`，并在可执行文件旁生成 `<appid>/` 目录：
   - `appticket.bin` — 原始所有权票据（二进制）
   - `eticket.bin` — 原始加密票据（二进制）
   - `tickets.txt` — 含十六进制字符串的纯文本摘要

   无法获取的票据会记为 `appticket:null` / `eticket:null`。
4. 把十六进制字符串粘贴到 Lua 脚本中：

   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **注意：** 票据仅在从**真正拥有**游戏的账号提取时才有效。

### 统计与成就

未拥有的游戏同样可以启用成就与统计：

- `setStat(appid, "steamid")` 指定拉取哪个 SteamID 的成就数据；
- 没有覆盖值时，若 `[stats] enable_api = true`（默认）会查询 stats API：`https://stats.luastools.xyz/{appid}`；
- 优先级：`setStat` > stats API（启用且返回有效值时）> 硬编码预设 SteamID `76561198028121353`。

### 在线修复

- 在 Steam 启动参数中加入 `-onlinefix`，为使用大厅匹配的游戏启用基于 480 的联机。同一时间只能运行一个这样的游戏；移除该参数即可恢复正常联机。
- 该参数不要求游戏在 Lua 解锁名单里，正版拥有的游戏同样可以使用。
- 若游戏卡在加载界面、黑屏或登录后行为异常，再加上 `-realappid`（`-onlinefix -realappid`）。此时游戏询问自身 appid 会得到真实值而不是伪装的 480。
- 两种行为由不同游戏各自需要，且无法自动判断，因此按局指定。

### 好友播报

`[presence] display` 控制已解锁游戏在好友侧的显示方式：

- `"spacewar"`（默认）— 广播真实游戏名；
- `"none"` — 跳过名称广播补丁；未拥有的游戏对好友不可见，`-onlinefix` 游戏回退为 Spacewar。

其他取值按 `"spacewar"` 处理。

### 游戏进程注入

`[inject]` 可在管道握手识别出游戏进程后，把你自己提供的库注入进去：

- 设置 `enabled = true`，并提供 `library_x64` / `library_x86`；
- 库必须与进程架构匹配，架构未知时跳过；
- 相对路径以 Steam 根目录为基准解析；
- 每个进程最多注入一次。

本项目不附带注入库——这是给你自己的工具用的挂点。

### 云存档重定向

`[cloud]` 集成 [CloudRedirect](https://github.com/Selectively11/CloudRedirect)：

- `enabled = true` 会在 Steam 进程内加载 `cloud_redirect.dll`，并以云存档模式初始化；
- 每个用 `addappid` 添加的游戏都会注册为可重定向应用，这些应用的 Steam Cloud RPC 会转发到 CloudRedirect 的引擎；
- 提供方登录（Google Drive、OneDrive 或本地文件夹）仍由 CloudRedirect 的配套程序完成——OpenSteamTool 只负责宿主 DLL；
- `library` 可以是绝对路径或相对于 Steam 根目录的路径，默认 `<Steam>\cloud_redirect.dll`；
- 若 DLL 缺失或缺少所需导出，该功能会自行禁用并记录原因。

### 第三方 CD Key

带有 Steam 第三方零售密钥的游戏（Ubisoft Connect、Rockstar 等）会在启动时请求密钥。OpenSteamTool 在本地应答该请求，使入库游戏的「正在更新产品密钥」步骤能够完成：

- `setlegacycdkey(appid, "KEY")` 提供真实密钥，原样保存；仅对通过 Lua 添加的游戏生效；
- 没有覆盖值时，会根据 AppId 与账号生成确定性的合成密钥，重开游戏也不会变化；
- 合成密钥只满足 Steam 的本地步骤——需要联网校验密钥的游戏仍需要真实密钥。

## 配置参考

配置文件为 `<Steam>\opensteamtool.toml`。它是可选的，改动后热重载，未知键会被忽略。**解析失败的文件会被整份拒绝**并回退到内置默认值，因此修改后请检查日志。

键与值完全一致的多语言示例：[`opensteamtool.example.toml`](opensteamtool.example.toml)（English）、[`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml)（中文）、[`opensteamtool.example.es.toml`](opensteamtool.example.es.toml)（Español）。

### `[log]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `level` | string | `"debug"` | `trace`、`debug`、`info`、`warn`、`error`。仅 Debug 构建生效；Release 构建会把日志编译掉。日志写入 `<Steam>\opensteamtool\*.log` |

### `[manifest]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `url` | string | `"20770407"` | 六个上游名称之一；无法识别的名称会记录警告并回退到 `20770407` |
| `failover` | bool | `true` | 为 `false` 时只请求 `url` |
| `timeout_resolve_ms` | int | `5000` | 钳制到 `[1, 15000]`（整次取码预算） |
| `timeout_connect_ms` | int | `5000` | 同上 |
| `timeout_send_ms` | int | `10000` | 同上 |
| `timeout_recv_ms` | int | `10000` | 同上 |

### `[stats]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `enable_api` | bool | `true` | 没有 `setStat` 覆盖时查询 stats API |
| `api_url` | string | `"https://stats.luastools.xyz/{appid}"` | stats 端点，必须包含 `{appid}`；响应体为 SteamID64 |

### `[presence]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `display` | string | `"spacewar"` | `"spacewar"` 或 `"none"`；其他取值按 `"spacewar"` 处理 |

### `[lua]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `paths` | 字符串数组 | `[]` | 额外的 Lua 目录，先于默认 `config\lua` 目录加载 |

### `[inject]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `enabled` | bool | `false` | 启用游戏进程注入 |
| `library_x64` | string | 未设置 | 64 位游戏进程使用 |
| `library_x86` | string | 未设置 | 32 位游戏进程使用 |

### `[cloud]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `enabled` | bool | `false` | 启用基于 CloudRedirect 的 Steam Cloud 重定向 |
| `library` | string | `<Steam>\cloud_redirect.dll` | 绝对路径，或相对于 Steam 根目录 |

### `[remote]`

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `url_template` | string | 未设置 | 自定义元数据镜像，必须包含 `{channel}`、`{component}` 与 `{sha256}` |
| `order` | string | `"gh-proxy-first"` | `gh-proxy-first`、`github-first`，或旧名 `jsdelivr-first`（等同 `gh-proxy-first`） |
| `auto_derive` | bool | `true` | 无已发布元数据时，基于缓存的旧版本与内置基线在本地推导 pattern/IPC 元数据；每个推导值都会针对已加载的 DLL 校验 |

## Steam 版本兼容性

OpenSteamTool 不再内置字节特征签名。每次启动时它会计算 `steamclient64.dll` 与 `steamui.dll` 的 SHA-256，并在本仓库的 `pattern` 与 `ipc` 分支查找匹配的描述文件，找不到时依次回退到维护中的 fork 与原始跟踪器。

查找顺序（Steam 更新后首次启动、本地无缓存时）：

1. **本地缓存** — `<Steam>\opensteamtool\<channel>\<component>\<sha256>.toml`。命中直接使用；每次成功远程获取后会覆盖缓存。
2. **本仓库的元数据分支** — `pattern` 与 `ipc` 分支，按 `[remote] order` 通过 gh-proxy 或 GitHub raw 获取。
3. **jsDelivr 镜像** — `https://cdn.jsdelivr.net/gh/alex47exe/OST@{channel}/{component}/{sha256}.toml`。
4. **维护中的 fork** — `matheeshapathirana/OpenSteamTool` 的 `pattern` / `ipc` 分支，用于本仓库尚未收录的构建。
5. **原始跟踪器** — `huanyuejue/steam-monitor`（`main/{channel}/{component}/{sha256}.toml`）。
6. **本地推导** — 所有来源都没有该构建时，会基于缓存的旧版本加内置基线推导 pattern 与 IPC 元数据，针对已加载的 DLL 校验后写入缓存。推导出的文件每次启动都会重新向镜像确认，一旦上游发布就会被替换。可用 `[remote] auto_derive = false` 关闭。

无论来源如何，每个 pattern 条目在使用前都会针对已加载的 DLL 校验：RVA 必须是真实的函数起点，且签名必须在该处匹配。无效条目会被重新推导或丢弃，绝不会被盲目调用。IPC 的 funcHash 会与 `steamclient64.dll` 中的包装函数交叉核对，不一致时以推导值为准。

只有所有镜像都返回 **HTTP 404** 时才判定上游尚未发布。此时若无缓存，会弹一次性提示，显示不匹配的 DLL 名称、其 SHA-256、预期缓存路径与上游 URL。只停用与该 DLL 相关的 hook，其余功能继续工作。

你也可以手动把描述文件按 `<sha256>.toml` 放进缓存目录。缓存未命中时会在工作线程上发起若干次简短的出站 HTTPS 请求（按镜像顺序逐个尝试直到成功，每个约 10 KB），因此绝不会阻塞 Steam 加载器。

### 使用不同的镜像

自定义 `[remote] url_template` 会替换内置镜像，且必须包含 `{channel}`、`{component}` 与 `{sha256}`。当前使用的通道是 `pattern` 与 `ipc`。

```toml
[remote]
# "gh-proxy-first"（默认）、"github-first"，或旧名 "jsdelivr-first"。
# order = "gh-proxy-first"
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

## 日志与故障排查

Debug 构建会为每个模块在 `<Steam>\opensteamtool\` 下写一个日志文件；`[log] level` 全局控制详细程度。

| 文件 | 宏 | 内容 |
|---|---|---|
| `main.log` | 通用 | 初始化、配置加载、Lua 加载、工具 |
| `ipc.log` | `LOG_IPC_*` | 接口调用、分发、欺骗 |
| `netpacket.log` | `LOG_NETPACKET_*` | 网络消息、eMsg 分发 |
| `manifest.log` | `LOG_MANIFEST_*` | manifest 下载、`fetch_manifest_code`、绑定 |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | 仓库密钥注入 |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues 补丁（manifest 绑定） |
| `misc.log` | `LOG_MISC_*` | 引擎指针捕获、AppId 改写 |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | UserStats 流量、SteamID 欺骗 |
| `pics.log` | `LOG_PICS_*` | PICS 访问令牌 |
| `package.log` | `LOG_PACKAGE_*` | 包注入、文件监视事件 |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 在线修复（480 欺骗） |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Rich Presence 包构造与注入 |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI hook 与隔离 |
| `pipe.log` | `LOG_PIPE_*` | 管道握手、进程检查、Denuvo 授权、库注入 |
| `platform.log` | `LOG_PLATFORM_*` | 平台助手、远程进程操作 |

常见情况：

| 现象 | 可能原因 | 处理 |
|---|---|---|
| Steam 起不来，`VGUI_Setup failed` | 加载器转发异常，或两个加载器冲突 | 去掉多余的加载器，只保留一个，优先 `dwmapi.dll` |
| 游戏报错误 54 退出 | 票据属于其他账号，或没找到真实票据 | 从拥有游戏的账号提取票据；查看 `pipe.log` 中的 donor 来源 |
| Denuvo 报错 `88500005` | 授权窗口（30 分钟）已过期 | 刷新 `AppTicket` / `ETicket` 后重试 |
| 加 `-onlinefix` 后卡加载或黑屏 | 游戏在启动期读取自身 AppId | 改用 `-onlinefix -realappid` 启动 |
| 弹出「缺少 Steam 元数据」提示 | 上游尚未发布该 Steam 版本 | 等待上游更新，或手动把 TOML 放入缓存目录 |
| 改了配置没有生效 | TOML 解析失败，已回退默认值 | 在 `main.log` 中查找 `Config parse error` 并校验文件 |
| 反作弊拦截游戏 | 启用了 `version.dll` 入口 | 改用 `dwmapi.dll`（或 `xinput1_4.dll`） |

## 构建

### 要求

- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 或更新版本，带 MSVC x64 工具链（C++20）

### 构建

```powershell
build.bat
```

脚本会构建 Release 与 Debug，然后单独构建 `extract_tickets`（它不在主目标中）。有 Ninja 时使用 Ninja Multi-Config，否则请求 `Visual Studio 17 2022` 生成器；若你只安装了更新的 Visual Studio，请覆盖 `GENERATOR`。第三方依赖（Detours、spdlog、protobuf、toml++、Lua）会自动拉取到 `.deps\`。

### 输出

- Debug / Release：`build\<Config>\OpenSteamTool.dll`、`build\<Config>\dwmapi.dll`、`build\<Config>\xinput1_4.dll`、`build\<Config>\version.dll.disabled`
- 工具：`build\tools\<Config>\extract_tickets.exe`

## 第三方组件

| 组件 | 用途 | 许可证 |
|---|---|---|
| [Microsoft Detours](https://github.com/microsoft/Detours) | 函数 hook | MIT |
| [spdlog](https://github.com/gabime/spdlog) | 日志 | MIT |
| [protobuf](https://github.com/protocolbuffers/protobuf) | Steam 消息序列化 | BSD-3-Clause |
| [toml++](https://github.com/marzer/tomlplusplus) | 配置与元数据解析 | MIT |
| [Lua 5.5](https://www.lua.org/) | 脚本 | MIT |
| [CloudRedirect](https://github.com/Selectively11/CloudRedirect) | 可选的云存档重定向 | 见上游 |
| [huanyuejue/steam-monitor](https://github.com/huanyuejue/steam-monitor) | 上游特征/IPC 元数据 | 见上游 |

## 免责声明

本项目仅供研究和教育目的使用。你负责遵守当地法律、平台服务条款和软件许可证。
