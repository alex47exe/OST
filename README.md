# OpenSteamTool metadata: `pattern`

Per-build Steam client metadata consumed by [OpenSteamTool](https://github.com/alex47exe/OST),
laid out as `<component>/<sha256 of the DLL>.toml`.

New Steam builds are published automatically by the
[Steam metadata workflow](https://github.com/alex47exe/OST/blob/main/.github/workflows/metadata.yml),
which derives and validates them against the official client binaries.

History is mirrored from
[matheeshapathirana/OpenSteamTool](https://github.com/matheeshapathirana/OpenSteamTool) (`pattern` branch),
which continues the data from
[OpenSteam001/steam-monitor](https://github.com/OpenSteam001/steam-monitor).

OpenSteamTool fetches files from this branch first and falls back to the fork and the
original tracker when a build is not present here yet.