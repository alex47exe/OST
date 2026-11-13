> **Nota sobre el fork:** este proyecto es una bifurcación optimizada de [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool), adaptada para el gestor de biblioteca de Steam [huanyuejue/Fluent-Steam-Lua](https://github.com/huanyuejue/Fluent-Steam-Lua).

<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Herramienta de código abierto para desbloquear Steam</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Solo Windows">
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

## Índice

- [Cómo funciona](#cómo-funciona)
- [Resumen de funciones](#resumen-de-funciones)
- [Instalación](#instalación)
- [Inicio rápido](#inicio-rápido)
- [Referencia de la API Lua](#referencia-de-la-api-lua)
- [Funciones en detalle](#funciones-en-detalle)
- [Referencia de configuración](#referencia-de-configuración)
- [Compatibilidad con versiones de Steam](#compatibilidad-con-versiones-de-steam)
- [Registro y solución de problemas](#registro-y-solución-de-problemas)
- [Compilación](#compilación)
- [Componentes de terceros](#componentes-de-terceros)
- [Descargo de responsabilidad](#descargo-de-responsabilidad)

## Cómo funciona

1. Una **DLL cargadora** colocada junto a `steam.exe` reenvía cada exportación a la DLL real del sistema y, cuando el proceso anfitrión es `steam.exe`, carga `OpenSteamTool.dll` desde la misma carpeta.
2. `OpenSteamTool.dll` delega todo el trabajo a un hilo de trabajo (nunca E/S de archivos ni detours bajo el loader lock). Después crea una copia completa de `steamclient64.dll` en `<Steam>\bin\diversion64.dll` —el **módulo sombra de Diversion**— y aplica todos los hooks a esa copia, dejando el módulo original intacto byte a byte para que pasen las comprobaciones de integridad de Denuvo/RE Engine.
3. Steam se redirige a la copia por dos vías independientes: la interceptación de `LoadModuleWithPath` (compilaciones antiguas) y la interceptación de la primera carga en `kernel32!LoadLibraryExW` (Steam 1791 y posteriores). Un watchdog de 15 segundos vuelve al módulo original si Steam nunca adopta la copia.
4. Los módulos de hook parchean entonces el cliente en ejecución: despacho de interfaces (`IClientUser`, `IClientUtils`), mensajes de red, estado de paquetes/propiedad, claves de descifrado de depósitos, KeyValues, códigos de solicitud de manifiestos, Rich Presence y comportamiento diverso al iniciar.
5. El handshake del gestor de pipes identifica el proceso del juego conectado a Steam; ahí ocurren la autorización de Denuvo y la inyección opcional de bibliotecas.
6. Los metadatos (patrones de bytes y descriptores IPC) se indexan por el SHA-256 de la DLL en disco, se descargan de los espejos ascendentes y se guardan en caché local.

Solo se admite la compilación para Windows.

## Resumen de funciones

| Función | Resumen | Detalles |
|---|---|---|
| Desbloqueo de juegos y DLC | Añade los juegos y todos sus depósitos al paquete de propiedad | [detalles](#desbloqueo-de-juegos-y-dlc) |
| Fuentes de manifiestos | Seis upstreams con failover ordenado, o tu propio endpoint Lua | [detalles](#fuentes-de-manifiestos-y-failover) |
| Vinculación de manifiestos | Fija un depósito a un GID de manifiesto concreto | [detalles](#vinculación-de-manifiestos) |
| Recarga en caliente | Los archivos Lua se recargan solos (500 ms de debounce) | [detalles](#recarga-en-caliente) |
| Préstamo familiar | Juega juegos compartidos junto al prestador | [detalles](#préstamo-familiar) |
| Denuvo / SteamStub | Falsificación de tickets para títulos protegidos | [detalles](#denuvo-y-steamstub) |
| Estadísticas y logros | Logros para juegos no poseídos mediante un SteamID alternativo | [detalles](#estadísticas-y-logros) |
| Online fix | Matchmaking de lobbies basado en 480 (`-onlinefix`) | [detalles](#online-fix) |
| Difusión de presencia | Cómo ven tus amigos los juegos desbloqueados | [detalles](#difusión-de-presencia) |
| Inyección en el proceso del juego | Carga tu propia DLL dentro del juego | [detalles](#inyección-en-el-proceso-del-juego) |
| Guardados en la nube | Redirige Steam Cloud a Google Drive / OneDrive / una carpeta | [detalles](#redirección-de-guardados-en-la-nube) |
| Claves de CD de terceros | Completa el paso «Actualizando clave de producto» | [detalles](#claves-de-cd-de-terceros) |

## Instalación

### Requisitos

- Windows 10/11, cliente de Steam de 64 bits
- Una compilación Debug o Release de este proyecto (ver [Compilación](#compilación))

### Elige un cargador

Copia `OpenSteamTool.dll` y **un solo** cargador al directorio raíz de Steam. Usa exactamente un punto de entrada: dos de ellos cargan dos copias de `OpenSteamTool.dll` desde carpetas distintas en el mismo proceso de Steam e instalan cada hook dos veces, lo que cierra Steam. `ost-manager.exe` lo garantiza por ti (ver [Gestor y cargador IFEO](#gestor-y-cargador-ifeo)).

| Cargador | Cómo lo carga Steam | Ventajas y desventajas |
|---|---|---|
| `dwmapi.dll` | Lo importa estáticamente `bin\vgui2_s.dll`, así que entra al iniciar la interfaz | No lo bloquean la mayoría de los anti-cheats (EAC/BE). Algunos juegos que verifican `steamclient` se cierran, p. ej. títulos de Capcom como Onimusha y Dragon's Dogma 2 |
| `version.dll` | Lo importa estáticamente `steam.exe`, es el punto de entrada más temprano | Permite aislamiento temprano y corrige los cierres anteriores, pero lo bloquean muchos anti-cheats y esos juegos dejan de ser jugables |
| `xinput1_4.dll` | Envoltorio completo de XInput (incluidas las ordinales no documentadas); Steam lo carga al inicializar el soporte de mandos | Mismas reglas de reenvío e inyección que `dwmapi.dll`; punto de entrada adicional opcional |

La compilación de `version.dll` se publica a propósito con el sufijo `.disabled`: `steam.exe` no la encuentra y usa la DLL del sistema. Cambia el nombre a `version.dll` solo si necesitas aislamiento temprano, y nunca la actives junto con un juego que use EAC/BE.

### Gestor y cargador IFEO

`build\tools\<Config>\` contiene un paquete completo — `ost-manager.exe`, `ost-loader.exe`, `OpenSteamTool.dll` y los tres proxies — así que no hay que copiar nada a mano.

| Archivo | Función |
|---|---|
| `ost-manager.exe` | CLI, menú de consola (`--tui`) y gestor con ventana (doble clic o `--ui`) |
| `ost-loader.exe` | Ayudante sin ventana que Windows ejecuta en lugar de `steam.exe` a través de IFEO |

La ventana ofrece un selector de punto de entrada con una explicación por método, estado con colores (verde = correcto, ámbar = atención, rojo = dos puntos de entrada), un icono y un tooltip en cada acción, un registro de acciones con colores (verde información, ámbar aviso, rojo error) y un conmutador de tema Claro/Oscuro/Sistema guardado por usuario (`HKCU\Software\OpenSteamTool\GuiTheme`). En Windows 11 usa el marco nativo: esquinas redondeadas, color de borde de DWM, barra de título oscura, Segoe UI Variable y el color de acento de Configuración para hover/pulsado/foco; en Windows 10 recurre a una paleta simple.

**Siempre hay exactamente un punto de entrada activo.** Cada instalación elimina la clave IFEO, los demás proxies y sus copias del payload antes de desplegar el suyo, y `status` informa cuántos puntos de entrada están activos (y avisa si hay más de uno). Los restos de una instalación anterior se reconocen aunque se haya perdido su registro, así que un proxy antiguo nunca puede seguir cargando un payload viejo junto a uno nuevo.

```bat
ost-manager.exe status
ost-manager.exe install dwmapi        :: proxy bin\dwmapi.dll (predeterminado)
ost-manager.exe install version       :: proxy version.dll (aislamiento temprano)
ost-manager.exe install xinput        :: proxy xinput1_4.dll
ost-manager.exe install ifeo          :: cargador IFEO, no copia nada dentro de Steam
ost-manager.exe install ifeo-root     :: cargador IFEO con ost-loader.exe en la raíz de Steam
ost-manager.exe uninstall
```

Opciones: `--steam-dir <ruta>`, `--payload <ruta>`, `--data-dir <ruta>`, `--restart` (reinicia Steam alrededor del cambio, `--force` para terminar un Steam colgado, `--no-start` para solo cerrarlo), `--dry-run`, `--json`, `--ui`, `--tui`, `--log <ruta>`. Los conmutadores del `ost-manager` original siguen funcionando: `/install-portable <dll>`, `/install-root <dir> <dll>`, `/deploy-dll <dir> [dll]`, `/remove-dll <dir>`, `/inject`, `/watch`, `/uninstall`, `/status`, `/enable-autostart`, `/disable-autostart`, `/silent`.

#### Dónde escribe la herramienta

Los archivos propios de Steam (`steamclient64.dll`, `steamui.dll`, `userdata`) siempre salen de la carpeta real de Steam, pero todo lo que la herramienta **escribe** sigue una raíz de datos: `opensteamtool.toml`, `config\lua`, los registros, las cachés de metadatos y `bin\diversion64.dll`.

- Raíz de datos por defecto: la carpeta donde está `ost-manager.exe`, así un `ost` portátil se comporta como si estuviera dentro de Steam sin escribir en Steam. Si el gestor se coloca dentro de Steam, la raíz es Steam.
- `--data-dir <ruta>` la sobrescribe; `config\lua` se crea ahí en cada instalación.
- La inyección en caliente (`inject`, `watch`) no puede cambiar el entorno de un Steam ya en ejecución, así que el gestor publica la raíz de datos en `HKCU\Software\OpenSteamTool\ConfigDir` y el payload la lee al cargarse. El cargador IFEO además pasa `OST_CONFIG_DIR` al Steam que inicia, y ese valor tiene prioridad.

### Pasos

1. Ejecuta `build.bat` (o compila manualmente, ver [Compilación](#compilación)).
2. Copia `OpenSteamTool.dll` y el cargador elegido al directorio raíz de Steam (junto a `steam.exe`).
3. Inicia Steam. Un archivo `main.log` en `<Steam>\opensteamtool\` confirma que la cadena de carga funcionó (solo compilaciones Debug).
4. Añade tus scripts Lua a `<Steam>\config\lua\`.

### Archivos creados en tiempo de ejecución

| Ruta | Propósito |
|---|---|
| `<Steam>\opensteamtool.toml` | Configuración opcional (ver [Referencia de configuración](#referencia-de-configuración)) |
| `<Steam>\opensteamtool\*.log` | Registros por módulo, solo compilaciones Debug |
| `<Steam>\opensteamtool\pattern\<component>\<sha256>.toml` | Caché de firmas de `steamclient64.dll` y `steamui.dll` |
| `<Steam>\opensteamtool\ipc\steamclient\<sha256>.toml` | Caché de descriptores IPC |
| `<Steam>\bin\diversion64.dll` | Copia sombra de `steamclient64.dll` usada para el aislamiento |
| `<Steam>\cloud_redirect.dll` | Carga opcional de CloudRedirect (`[cloud]`) |

Si instalas con `ost-manager.exe`, las filas anteriores se crean bajo la **raíz de datos** en lugar de `<Steam>` (ver [Dónde escribe la herramienta](#dónde-escribe-la-herramienta)).

### Desinstalación

Cierra Steam y ejecuta `ost-manager.exe uninstall` (elimina la entrada IFEO, los proxies, las copias del payload, el inicio automático y los artefactos de ejecución). Manualmente: cierra Steam, elimina el cargador y `OpenSteamTool.dll`, después elimina `<Steam>\bin\diversion64.dll` y la carpeta `<Steam>\opensteamtool\`.

## Inicio rápido

1. Crea el directorio Lua (`<Steam>\config\lua`) y coloca un script:

   ```lua
   addappid(1361510) -- desbloquea el juego con id 1361510
   addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0")
   addtoken(1361510, "2764735786934684318")
   setManifestid(1361511, "5656605350306673283")
   ```

2. Inicia Steam. Los scripts se ejecutan al arrancar y de nuevo cada vez que cambia un archivo `.lua`.

3. Opcionalmente copia un ejemplo de configuración a `<Steam>\opensteamtool.toml`:

   | Idioma | Archivo |
   |---|---|
   | English | [`opensteamtool.example.toml`](opensteamtool.example.toml) |
   | 中文 | [`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml) |
   | Español | [`opensteamtool.example.es.toml`](opensteamtool.example.es.toml) |

   El archivo es opcional; sin él se usan los valores predeterminados.

## Referencia de la API Lua

Todos los nombres de función **distinguen mayúsculas de minúsculas**: `setAppTicket`, `setappticket` y `SETAPPTICKET` son equivalentes, igual que el resto de funciones de la tabla.

| Función | Firma | Propósito | Notas |
|---|---|---|---|
| `addappid` | `addappid(id [, ignorado, depotKey])` | Marca un juego (y por tanto sus depósitos) como poseído | El segundo argumento se acepta por compatibilidad y se ignora. `depotKey`, si se indica, debe tener exactamente 64 caracteres hexadecimales |
| `addtoken` | `addtoken(appid, token)` | Añade un token de acceso PICS | `token` es un valor decimal de 64 bits pasado como cadena |
| `setmanifestid` | `setmanifestid(depotId, gid)` | Fija un depósito a un manifiesto | Los argumentos extra se ignoran; el tamaño del manifiesto se fuerza a 0 |
| `setAppTicket` | `setAppTicket(appid, hex)` | Escribe un ticket de propiedad en el almacén de credenciales | En Windows: `HKCU\Software\Valve\Steam\Apps\<appid>\AppTicket` |
| `setETicket` | `setETicket(appid, hex)` | Escribe un ticket cifrado en el almacén de credenciales | Misma ruta, nombre de valor `ETicket` |
| `setStat` | `setStat(appid, "steamid")` | Elige de qué cuenta se usan los datos de logros | SteamID64 decimal como cadena |
| `setlegacycdkey` | `setlegacycdkey(appid, "KEY")` | Proporciona una clave de producto de terceros | Se guarda tal cual; ver [Claves de CD de terceros](#claves-de-cd-de-terceros) |
| `forcedenuvo` | `forcedenuvo(appid)` | Trata un juego como protegido por Denuvo | Omite el escaneo; solo afecta a la detección |
| `http_get` | `body, status = http_get(url [, headers])` | HTTP GET para scripts Lua | Devuelve `body, status`, o `nil, error` |
| `http_post` | `body, status = http_post(url, body [, headers])` | HTTP POST para scripts Lua | Mismos valores de retorno |

`headers` es una tabla opcional como `{["Accept"]="application/json"}`.

Un script también puede definir los hooks de manifiesto `fetch_manifest_code(gid)` y `fetch_manifest_code_ex(app_id, depot_id, gid)`; ver [Fuentes de manifiestos y failover](#fuentes-de-manifiestos-y-failover).

## Funciones en detalle

### Desbloqueo de juegos y DLC

Añadir un juego con `addappid` hace que Steam trate el juego y todos sus depósitos como poseídos. Las claves de descifrado se sirven desde la configuración Lua, así que el contenido que necesita clave se desbloquea y descarga con normalidad. Los tokens de acceso de juegos protegidos se aportan con `addtoken`.

### Fuentes de manifiestos y failover

Los códigos de solicitud de manifiestos se obtienen de uno de seis upstreams:

| Nombre | Endpoint |
|---|---|
| `20770407` (predeterminado) | `https://20770407.xyz/manifest/{depotId}/{gid}` |
| `SDM` | `https://steamapi.993499094.xyz/manifest/{depotId}/{gid}` |
| `manifestdex` | `https://manifest.manifestdex.com/{gid}` (envía `User-Agent: ManifestDeX/1.0`) |
| `wudrm` | `http://gmrc.wudrm.com/manifest/{gid}` |
| `steamrun` | `https://manifest.steam.run/api/manifest/{gid}` |
| `opensteamtool` | `https://manifest.opensteamtool.com/{gid}` |

`[manifest] url` selecciona el upstream preferido. Con `failover = true` (predeterminado), un fallo pasa al siguiente upstream en el orden de la tabla; una fuente que acaba de fallar se omite durante 60 segundos y la fuente configurada sigue siendo la primera en la siguiente obtención. Todos los intentos de una obtención comparten un presupuesto de 15 segundos, así que una fuente lenta no puede bloquear las descargas de depósitos.

Lua puede sustituir por completo la vía HTTP. Si `<Steam>\config\lua\manifest.lua` (o cualquier script cargado) define `fetch_manifest_code_ex(app_id, depot_id, gid)` o `fetch_manifest_code(gid)`, se intentan antes que los upstreams, con `_ex` primero. Dentro de ellas están disponibles `http_get` / `http_post`.

### Vinculación de manifiestos

`setManifestid(depotId, gid)` fija un depósito a un manifiesto concreto, lo que evita que el juego reciba un manifiesto actualizado (y posiblemente incompatible). Se aplica a través de la vía KeyValues.

### Recarga en caliente

Se vigilan la carpeta `config\lua` y cada directorio de `[lua] paths`. Añadir, modificar, renombrar o borrar un archivo `.lua` provoca una recarga tras 500 ms de debounce: sin reiniciar Steam ni alternar modo offline/online. Los archivos de `[lua] paths` se cargan primero; la carpeta predeterminada se carga al final para que tus cambios tengan prioridad.

### Préstamo familiar

Los juegos compartidos se pueden jugar directamente. Si el juego también está en Lua, OpenSteamTool lo saca por completo de la lógica de préstamo:

- la comprobación de licencia local se trata como una licencia falsa y se borran las marcas de compartición;
- al informar del estado de juego, el AppId se sustituye por un valor que no existe en el servidor, así que el servidor ni cuenta una plaza ni envía el aviso de fin de partida;
- el precio es que el estado de compra, los logros y el tiempo de juego quedan ligados a la licencia falsa, y tus amigos no ven la actividad real. Con `[presence] display = "spacewar"` estos juegos se informan como Spacewar, de modo que tus amigos ven qué juegas, a cambio de que el tiempo se cuente en Spacewar.

Los juegos compartidos que **no** están en Lua conservan las reglas nativas de Steam: ocupan la plaza del prestador y pueden reclamarse. Los juegos que realmente posees no se ven afectados aunque estén en Lua (se detecta porque la licencia aparece en más de un paquete).

### Denuvo y SteamStub

Los títulos protegidos solo con SteamStub no necesitan configurar tickets: OpenSteamTool reutiliza el ticket local del ConfigStore de Steam y falsifica el AppId solicitado, sin inyectar nada en el proceso del juego.

Los títulos con Denuvo necesitan datos de ticket explícitos, guardados en el almacén de credenciales de la plataforma:

- `setAppTicket(appid, "hex")` y `setETicket(appid, "hex")` escriben los valores;
- la autorización de Denuvo tiene una ventana de 30 minutos; al expirar puede fallar con el error `88500005`, así que refresca los tickets y reintenta;
- los tickets explícitos (configurados o en caché) tienen prioridad; si no, OpenSteamTool recurre a un ticket falsificado del ConfigStore;
- falsificar requiere un ticket real como base. El orden de búsqueda es: AppId 7 en memoria → AppId 228980 (redistribuible común de Steamworks) en memoria → la sección `apptickets` de `<Steam>\userdata\<cuenta>\config\localconfig.vdf` → la clave de registro `HKCU\Software\Valve\Steam\Apps\<AppId>\AppTicket`. Los tickets deben coincidir con la cuenta conectada, o Steam devuelve el error 54;
- el SteamID proviene del valor en caché o, si falta, de un AppTicket explícito. El backend de Windows usa `HKCU\Software\Valve\Steam\Apps\<AppId>`; el de Linux no está implementado;
- cuando la cuenta conectada posee el juego, OpenSteamTool escribe el SteamID real en el almacén de credenciales al final del handshake de Denuvo, de modo que otra cuenta puede iniciar el juego con esa identidad en caché.

La detección de Denuvo prueba la firma del punto de entrada, luego la cadena `DENUVO` en la sección y, por último, una comprobación estructural (una sección grande de escritura+ejecución con entropía alta o con un nombre de sección de protector heredado como `.arch`, `.srdata`, `.xpdata`, `.xtls`). Si `pipe.log` dice `no Denuvo match` para un juego que claramente está protegido, fuerza el modo con `forcedenuvo(appid)`.

#### Extracción de tickets con `extract_tickets`

`extract_tickets` vuelca las cadenas hexadecimales de `AppTicket` y `ETicket` que usan `setAppTicket` / `setETicket`. Ejecútalo en una máquina con Steam en marcha e iniciada con una cuenta que **posea** el juego.

1. Compílalo (ver [Compilación](#compilación)); el binario está en `build/tools/<Config>/extract_tickets.exe`.
2. Ejecútalo con el AppId (o sin argumentos e introduce el AppId cuando lo pida):

   ```powershell
   extract_tickets.exe 1361510
   ```

3. Lee la ruta de instalación de Steam del registro, carga `steamclient64.dll` y crea una carpeta `<appid>/` junto al ejecutable:
   - `appticket.bin` — ticket de propiedad sin procesar (binario)
   - `eticket.bin` — ticket cifrado sin procesar (binario)
   - `tickets.txt` — resumen de texto con las cadenas hexadecimales

   Los tickets que no se pueden obtener aparecen como `appticket:null` / `eticket:null`.
4. Pega las cadenas hexadecimales en tu script Lua:

   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Nota:** los tickets solo son válidos cuando se extraen de una cuenta que **realmente posee** el juego.

### Estadísticas y logros

Los logros y las estadísticas funcionan para juegos no poseídos:

- `setStat(appid, "steamid")` elige de qué SteamID se extraen los datos de logros;
- si no hay anulación, se consulta la API de estadísticas cuando `[stats] enable_api = true` (predeterminado): `https://stats.luastools.xyz/{appid}`;
- prioridad: `setStat` > API de estadísticas (activada y con valor válido) > el SteamID predefinido `76561198028121353`.

### Online fix

- Añade `-onlinefix` a las opciones de lanzamiento de Steam para habilitar el matchmaking de lobbies basado en 480. Solo puede ejecutarse un juego así a la vez. Quita el parámetro para volver al juego en línea normal.
- El parámetro no exige que el juego esté en la lista Lua; los juegos que posees de forma legítima también pueden usarlo.
- Si un juego se cuelga en la pantalla de carga, muestra una pantalla negra o falla tras el inicio de sesión, añade `-realappid` (`-onlinefix -realappid`). El juego recibe entonces su AppId real en lugar del 480 suplantado.
- Cada comportamiento lo necesitan títulos distintos y no se pueden distinguir automáticamente, así que la elección es por partida.

### Difusión de presencia

`[presence] display` controla lo que ven tus amigos de los juegos desbloqueados:

- `"spacewar"` (predeterminado) — difundir el nombre real del juego;
- `"none"` — omitir los parches de difusión del nombre; los juegos no poseídos se vuelven invisibles para los amigos y los juegos con `-onlinefix` vuelven a Spacewar.

Cualquier otro valor se comporta como `"spacewar"`.

### Inyección en el proceso del juego

`[inject]` carga opcionalmente tu propia biblioteca en el proceso del juego cuando el handshake del pipe lo identifica:

- pon `enabled = true` y proporciona `library_x64` / `library_x86`;
- la biblioteca debe coincidir con la arquitectura del proceso; si se desconoce, se omite;
- las rutas relativas se resuelven desde el directorio raíz de Steam;
- cada proceso se inyecta como máximo una vez.

No se incluye ninguna biblioteca de inyección: esto es un punto de enganche para tus propias herramientas.

### Redirección de guardados en la nube

`[cloud]` integra [CloudRedirect](https://github.com/Selectively11/CloudRedirect):

- `enabled = true` carga `cloud_redirect.dll` dentro de Steam y la inicializa en modo de guardado en la nube;
- cada juego añadido con `addappid` se registra como aplicación redirigida, y las RPC de Steam Cloud de esas aplicaciones se reenvían al motor de CloudRedirect;
- el inicio de sesión del proveedor (Google Drive, OneDrive o una carpeta local) se sigue haciendo en la aplicación complementaria de CloudRedirect; OpenSteamTool solo aloja la DLL;
- `library` puede ser absoluta o relativa a la raíz de Steam; por defecto `<Steam>\cloud_redirect.dll`;
- si falta la DLL o no tiene las exportaciones esperadas, la función se desactiva y registra el motivo.

### Claves de CD de terceros

Los títulos con una clave minorista de terceros emitida por Steam (Ubisoft Connect, Rockstar y otros) piden esa clave al iniciar. OpenSteamTool responde localmente para que el paso «Actualizando clave de producto» se complete en los juegos añadidos:

- `setlegacycdkey(appid, "KEY")` aporta una clave real, guardada tal cual; solo se aplica a juegos añadidos por Lua;
- sin anulación se genera una clave sintética determinista a partir del AppId y la cuenta, de modo que los relanzamientos son estables;
- una clave sintética solo satisface el paso local de Steam: los títulos que validan la clave en línea siguen necesitando una real.

## Referencia de configuración

El archivo de configuración es `<Steam>\opensteamtool.toml`. Es opcional, se recarga en caliente al cambiar y las claves desconocidas se ignoran. **Un archivo que no se puede analizar se rechaza por completo** y se usan los valores predeterminados, así que revisa el registro después de editarlo.

Ejemplos localizados con claves y valores idénticos: [`opensteamtool.example.toml`](opensteamtool.example.toml) (English), [`opensteamtool.example.zh.toml`](opensteamtool.example.zh.toml) (中文), [`opensteamtool.example.es.toml`](opensteamtool.example.es.toml) (Español).

### `[log]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `level` | string | `"debug"` | `trace`, `debug`, `info`, `warn`, `error`. Solo compilaciones Debug; Release elimina el registro. Los archivos van a `<Steam>\opensteamtool\*.log` |

### `[manifest]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `url` | string | `"20770407"` | Uno de los seis nombres de upstream; un nombre desconocido registra una advertencia y usa `20770407` |
| `failover` | bool | `true` | `false` consulta solo `url` |
| `timeout_resolve_ms` | int | `5000` | Ajustado a `[1, 15000]`, el presupuesto de toda la obtención |
| `timeout_connect_ms` | int | `5000` | Mismo ajuste |
| `timeout_send_ms` | int | `10000` | Mismo ajuste |
| `timeout_recv_ms` | int | `10000` | Mismo ajuste |

### `[stats]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `enable_api` | bool | `true` | Consulta la API de estadísticas cuando no hay anulación `setStat` |
| `api_url` | string | `"https://stats.luastools.xyz/{appid}"` | Endpoint de estadísticas; debe contener `{appid}`. El cuerpo es un SteamID64 |

### `[presence]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `display` | string | `"spacewar"` | `"spacewar"` o `"none"`; cualquier otro valor se comporta como `"spacewar"` |

### `[lua]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `paths` | array de cadenas | `[]` | Directorios Lua adicionales, cargados antes de la carpeta `config\lua` |

### `[inject]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `enabled` | bool | `false` | Activa la inyección en procesos de juego |
| `library_x64` | string | sin definir | Necesaria para procesos de juego de 64 bits |
| `library_x86` | string | sin definir | Necesaria para procesos de juego de 32 bits |

### `[cloud]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `enabled` | bool | `false` | Activa la redirección de Steam Cloud mediante CloudRedirect |
| `library` | string | `<Steam>\cloud_redirect.dll` | Absoluta o relativa a la raíz de Steam |

### `[remote]`

| Clave | Tipo | Predeterminado | Notas |
|---|---|---|---|
| `url_template` | string | sin definir | Espejo de metadatos personalizado; debe contener `{channel}`, `{component}` y `{sha256}` |
| `order` | string | `"gh-proxy-first"` | `gh-proxy-first`, `github-first`, o el alias heredado `jsdelivr-first` (= `gh-proxy-first`) |
| `auto_derive` | bool | `true` | Deriva localmente los metadatos de pattern/IPC que falten a partir de compilaciones anteriores en caché y una base integrada; cada valor derivado se valida contra la DLL cargada |

## Compatibilidad con versiones de Steam

OpenSteamTool no incluye firmas de patrones de bytes. En cada inicio calcula el SHA-256 de `steamclient64.dll` y `steamui.dll` y busca un descriptor coincidente en las ramas `pattern` e `ipc` de este repositorio, con un fork mantenido y el rastreador original como respaldo.

Orden de búsqueda (primer inicio tras una actualización de Steam, con la caché vacía):

1. **Caché local** — `<Steam>\opensteamtool\<channel>\<component>\<sha256>.toml`. Si hay acierto se usa directamente; la caché se sobrescribe tras cada descarga remota correcta.
2. **Ramas de metadatos de este repositorio** — `pattern` e `ipc`, obtenidas por gh-proxy o GitHub raw según `[remote] order`.
3. **Espejo jsDelivr** — `https://cdn.jsdelivr.net/gh/alex47exe/OST@{channel}/{component}/{sha256}.toml`.
4. **Fork mantenido** — las ramas `pattern` e `ipc` de `matheeshapathirana/OpenSteamTool`, para compilaciones que este repositorio aún no tiene.
5. **Rastreador original** — `huanyuejue/steam-monitor` (`main/{channel}/{component}/{sha256}.toml`).
6. **Derivación local** — cuando ninguna fuente tiene la compilación, los metadatos de pattern e IPC se derivan de compilaciones anteriores en caché más una base integrada, se validan contra la DLL cargada y se escriben en la caché. Los archivos derivados se vuelven a comprobar contra los espejos en cada inicio, de modo que una copia publicada los reemplaza. Se desactiva con `[remote] auto_derive = false`.

Sea cual sea la fuente, cada entrada de pattern se comprueba contra la DLL cargada antes de usarse: su RVA debe ser un inicio de función real y su firma debe coincidir allí. Las entradas inválidas se vuelven a derivar o se descartan, nunca se llaman a ciegas. Los hashes de IPC se contrastan con los wrappers de `steamclient64.dll` y se corrigen si no coinciden.

Solo cuando todos los espejos devuelven **HTTP 404** se considera que el descriptor no está publicado. Sin caché, aparece un aviso único con el nombre de la DLL no emparejada, su SHA-256, la ruta de caché esperada y la URL de origen. Solo se desactivan los hooks ligados a esa DLL; el resto de OpenSteamTool sigue funcionando.

También puedes colocar el descriptor manualmente en la caché como `<sha256>.toml`. Cuando la caché falla se hacen unas pocas solicitudes HTTPS salientes breves (una por espejo hasta que una responde, unos 10 KB cada una) en un hilo de trabajo, así que el cargador de Steam nunca se bloquea.

### Uso de un espejo diferente

Un `[remote] url_template` personalizado reemplaza los espejos integrados y debe incluir `{channel}`, `{component}` y `{sha256}`. Los canales usados actualmente son `pattern` e `ipc`.

```toml
[remote]
# "gh-proxy-first" (predeterminado), "github-first", o el heredado "jsdelivr-first".
# order = "gh-proxy-first"
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

## Registro y solución de problemas

Las compilaciones Debug escriben un archivo por módulo en `<Steam>\opensteamtool\`; `[log] level` controla la verbosidad de forma global.

| Archivo | Macros | Contenido |
|---|---|---|
| `main.log` | genérico | Inicialización, configuración, carga de Lua, herramientas |
| `ipc.log` | `LOG_IPC_*` | Llamadas a interfaces, despacho, suplantación |
| `netpacket.log` | `LOG_NETPACKET_*` | Mensajes de red, despacho de eMsg |
| `manifest.log` | `LOG_MANIFEST_*` | Descargas de manifiestos, `fetch_manifest_code`, vinculación |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Inyección de claves de depósito |
| `keyvalue.log` | `LOG_KEYVALUE_*` | Parches KeyValues (vinculación de manifiestos) |
| `misc.log` | `LOG_MISC_*` | Captura de punteros del motor, reescritura de AppId |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | Tráfico UserStats, suplantación de SteamID |
| `pics.log` | `LOG_PICS_*` | Tokens de acceso PICS |
| `package.log` | `LOG_PACKAGE_*` | Inyección de paquetes, eventos del vigilante de archivos |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | Online fix (suplantación 480) |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Construcción e inyección de paquetes Rich Presence |
| `steamui.log` | `LOG_STEAMUI_*` | Hooks de SteamUI y aislamiento |
| `pipe.log` | `LOG_PIPE_*` | Handshake del pipe, inspección de procesos, autorización Denuvo, inyección |
| `platform.log` | `LOG_PLATFORM_*` | Asistentes de plataforma, operaciones en procesos remotos |

Situaciones habituales:

| Síntoma | Causa probable | Solución |
|---|---|---|
| Steam no arranca, `VGUI_Setup failed` | Un cargador no reenvía bien las exportaciones, o dos cargadores entran en conflicto | Elimina el cargador sobrante; deja solo uno, preferiblemente `dwmapi.dll` |
| El juego sale con error 54 | El ticket pertenece a otra cuenta o no se encontró un ticket real | Extrae tickets de una cuenta propietaria; revisa `pipe.log` para ver el origen del donor |
| Error `88500005` de Denuvo | La ventana de autorización (30 minutos) expiró | Refresca `AppTicket` / `ETicket` y reintenta |
| El juego se cuelga o muestra pantalla negra con `-onlinefix` | El juego lee su AppId durante el arranque | Inicia con `-onlinefix -realappid` |
| Aviso que lista metadatos de Steam ausentes | El upstream aún no publicó esta compilación de Steam | Espera una actualización o coloca el TOML en la carpeta de caché |
| Los cambios de configuración no surten efecto | El TOML no se pudo analizar y se aplicaron los valores predeterminados | Busca `Config parse error` en `main.log` y valida el archivo |
| El anti-cheat bloquea el juego | Está activo el punto de entrada `version.dll` | Usa `dwmapi.dll` (o `xinput1_4.dll`) |

## Compilación

### Requisitos

- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 o superior con la cadena de herramientas MSVC x64 (C++20)

### Compilar

```powershell
build.bat
```

El script compila Release y Debug y luego `extract_tickets` (excluido del objetivo principal). Usa Ninja Multi-Config cuando Ninja está disponible; si no, solicita el generador `Visual Studio 17 2022`, así que sobrescribe `GENERATOR` si solo tienes un Visual Studio más reciente. Las dependencias de terceros (Detours, spdlog, protobuf, toml++, Lua) se descargan automáticamente en `.deps\`.

### Salida

- Debug / Release: `build\<Config>\OpenSteamTool.dll`, `build\<Config>\dwmapi.dll`, `build\<Config>\xinput1_4.dll`, `build\<Config>\version.dll.disabled`
- Herramienta: `build\tools\<Config>\extract_tickets.exe`

## Componentes de terceros

| Componente | Uso | Licencia |
|---|---|---|
| [Microsoft Detours](https://github.com/microsoft/Detours) | Hooking de funciones | MIT |
| [spdlog](https://github.com/gabime/spdlog) | Registro | MIT |
| [protobuf](https://github.com/protocolbuffers/protobuf) | Serialización de mensajes de Steam | BSD-3-Clause |
| [toml++](https://github.com/marzer/tomlplusplus) | Análisis de configuración y metadatos | MIT |
| [Lua 5.5](https://www.lua.org/) | Scripts | MIT |
| [CloudRedirect](https://github.com/Selectively11/CloudRedirect) | Redirección opcional de guardados en la nube | Ver el upstream |
| [huanyuejue/steam-monitor](https://github.com/huanyuejue/steam-monitor) | Metadatos de firmas/IPC | Ver el upstream |

## Descargo de responsabilidad

Este proyecto se ofrece únicamente con fines de investigación y educativos. Eres responsable de cumplir las leyes locales, los términos de servicio de las plataformas y las licencias de software.
