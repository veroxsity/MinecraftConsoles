# Minecraft Consoles

A community-maintained source port and modernization effort for Minecraft: Legacy Console Edition (TU19 codebase).

This project focuses on keeping the original LCE feel while improving stability, desktop usability, and multiplayer reliability.

## What This Project Is

- A playable desktop-oriented LCE codebase
- A base for bug fixes, platform work, and careful backporting
- A long-term foundation for modding and extension

## Current Highlights

- Windows client and dedicated server targets
- Keyboard and mouse support
- Controller-first compatibility
- LAN discovery and multiplayer
- WAN/IP multiplayer support
- LCELive invite flow, signaling, and relay support
- Split-screen support (where applicable)
- Better diagnostics and runtime logs

## Platform Notes

- Windows: primary supported build and runtime platform
- Linux/macOS: commonly run through Wine/CrossOver by community users (unofficial)
- Console code remains in-tree, but desktop stability and networking are current priorities

## Quick Start (Windows)

1. Install Visual Studio 2022 or newer with C++ tools.
2. Open this GAME directory in Visual Studio.
3. Let CMake configure.
4. Choose a target/configuration, for example Windows64 - Debug.
5. Build and run Minecraft.Client or Minecraft.Server.

## Build With CMake

Configure:

```powershell
cmake --preset windows64
```

Build client:

```powershell
cmake --build --preset windows64-debug --target Minecraft.Client
```

Build dedicated server:

```powershell
cmake --build --preset windows64-debug --target Minecraft.Server
```

Release builds:

```powershell
cmake --build --preset windows64-release --target Minecraft.Client
cmake --build --preset windows64-release --target Minecraft.Server
```

For full compiler/platform details, see COMPILE.md.

## Running

Client (example):

```powershell
cd .\build\windows64\Minecraft.Client\Debug
.\Minecraft.Client.exe
```

Server (example):

```powershell
cd .\build\windows64\Minecraft.Server\Debug
.\Minecraft.Server.exe -port 25565 -bind 0.0.0.0 -name DedicatedServer
```

Important: run from the output directory so relative asset paths resolve correctly.

## Client Launch Arguments

| Argument | Description |
|---|---|
| -name <username> | Override in-game username |
| -fullscreen | Start in fullscreen |

Example:

```text
Minecraft.Client.exe -name Steve -fullscreen
```

## Logging

Client logs are written next to the executable in:

- logs/game.log
- logs/game.previous.log
- logs/lcelive.log
- logs/lcelive.previous.log

Use these first when diagnosing startup, invite, signaling, relay, or join issues.

## Multiplayer Notes

- Default game port: TCP 25565
- LAN discovery: UDP 25566
- Relay/signaling paths are used when direct connectivity is blocked
- If direct joins fail, logs/lcelive.log typically shows whether fallback was used

## Dedicated Server

Minecraft.Server reads server.properties from its working directory.
CLI arguments override server.properties values.

Common flags:

- -port <1-65535>
- -ip <addr>
- -bind <addr>
- -name <name>
- -maxplayers <1-8>
- -seed <int64>
- -loglevel <debug|info|warn|error>

## Docker (Dedicated Server)

A Wine-based container flow is included.
Use the provided compose/start scripts in this GAME directory.

## Contribution Direction

The project prioritizes:

- Stability and crash reduction
- Networking reliability
- Controller and desktop UX quality
- Faithful LCE presentation and behavior

For contribution standards and scope, see CONTRIBUTING.md.
