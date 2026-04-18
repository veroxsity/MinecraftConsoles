# Minecraft Consoles

A community-maintained source port and modernization effort for Minecraft: Legacy Console Edition (TU19 codebase).

The goal is simple: preserve the original LCE gameplay feel while making it practical, stable, and reliable on modern desktop setups.

## Project Goals

- Keep LCE identity and behavior intact
- Improve runtime stability and crash resistance
- Improve desktop UX (controller + keyboard/mouse)
- Improve multiplayer reliability (LAN, WAN, relay)
- Provide a solid base for future extensions/modding

## Feature Snapshot

| Area | Status |
|---|---|
| Windows Client | Available |
| Windows Dedicated Server | Available |
| LAN Discovery/Join | Available |
| WAN/IP Join | Available |
| LCELive Invites/Signaling/Relay | Available |
| Keyboard + Mouse | Available |
| Controller Support | Available |
| Split-screen (where applicable) | Available |

## Platform Compatibility

| Platform | Build Support | Runtime Support | Notes |
|---|---|---|---|
| Windows | Yes | Yes | Primary platform |
| Linux | Cross-compile + Wine | Community-tested | Unofficial runtime path |
| macOS | No native build | Community-tested via Wine/CrossOver | Unofficial runtime path |
| Consoles | Code remains in tree | Not currently validated by maintainers | Desktop priority |

## Quick Start (Visual Studio)

1. Install Visual Studio 2022 (or newer) with C++ workloads.
2. Open this `GAME` directory in Visual Studio.
3. Wait for CMake configure/generation to complete.
4. Select a configuration such as `Windows64 - Debug`.
5. Build and run `Minecraft.Client.exe` or `Minecraft.Server.exe`.

## Build With CMake

Configure:

```powershell
cmake --preset windows64
```

Build client (Debug):

```powershell
cmake --build --preset windows64-debug --target Minecraft.Client
```

Build dedicated server (Debug):

```powershell
cmake --build --preset windows64-debug --target Minecraft.Server
```

Build release binaries:

```powershell
cmake --build --preset windows64-release --target Minecraft.Client
cmake --build --preset windows64-release --target Minecraft.Server
```

For cross-compile and Linux/Nix details, see [COMPILE.md](COMPILE.md).

## Running

Client:

```powershell
cd .\build\windows64\Minecraft.Client\Debug
.\Minecraft.Client.exe
```

Dedicated server:

```powershell
cd .\build\windows64\Minecraft.Server\Debug
.\Minecraft.Server.exe -port 25565 -bind 0.0.0.0 -name DedicatedServer
```

Important: launch from the output directory so relative asset paths resolve correctly.

## Client Launch Arguments

| Argument | Description |
|---|---|
| `-name <username>` | Override in-game username |
| `-fullscreen` | Launch in fullscreen |

Example:

```text
Minecraft.Client.exe -name Steve -fullscreen
```

## Multiplayer Defaults

- Game port: TCP `25565`
- LAN discovery: UDP `25566`
- Relay/signaling fallback is used when direct paths are unavailable

## Logging

Logs are written next to the executable in `logs`:

- `logs/game.log`
- `logs/game.previous.log`
- `logs/lcelive.log`
- `logs/lcelive.previous.log`

If networking fails, inspect `logs/lcelive.log` first.

## Troubleshooting

### Startup fails under Wine

Try Wine built-in D3D11 path instead of DXVK:

```bash
WINEDLLOVERRIDES="d3d11,dxgi=b" wine ./Minecraft.Client.exe
```

### Invite/join reliability issues

- Verify both players are on matching builds
- Verify `logs/lcelive.log` on both host and joiner
- Check for `peer known`, `forwarding active`, and session `closed` timing

### Missing media/SWF errors

Ensure game is launched from the correct output folder with `Common/Media` present.

## Dedicated Server

`Minecraft.Server` loads `server.properties` from its working directory.
CLI arguments override properties.

Common flags:

- `-port <1-65535>`
- `-ip <addr>`
- `-bind <addr>`
- `-name <name>`
- `-maxplayers <1-8>`
- `-seed <int64>`
- `-loglevel <debug|info|warn|error>`

## Docker (Dedicated Server)

This repo includes Wine-based Docker flows for dedicated server hosting.
Use the compose/start scripts in this `GAME` directory.

## Contributing

Before opening PRs, read [CONTRIBUTING.md](CONTRIBUTING.md).

Current review priority:

- Stability and crash fixes
- Networking reliability
- Controller and desktop usability
- Faithful LCE behavior/presentation
