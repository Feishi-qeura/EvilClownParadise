# FU Online Session

`FUOnlineSession` is a runtime plugin that exposes provider-agnostic OnlineSubsystem session operations to Blueprint.

## Blueprint API

Get the `FU Online Session Subsystem` from the Game Instance, then bind its delegates before calling:

- `FU_CreateCustomSession(MaxPlayers, RoomName, RoomPassword, bUseLobbiesIfAvailable)`
- `FU_FindCustomSession(RoomName, MaxResults, bUseLobbiesIfAvailable)`
- `FU_JoinCustomSession(RoomPasswordInput)`
- `FU_DestroySession()`

`FU_CheckSessionStatus(WorldContextObject, RefreshTime)` is an async Blueprint node. It reports once on an authoritative controller and periodically reports `APlayerState` Ping on a client. It reports `ClientConnectionOvertime` and stops if the World or player state becomes unavailable.

## LAN default

The project configures the built-in Null provider:

```ini
[OnlineSubsystem]
DefaultPlatformService=Null
```

Create and search calls use LAN mode by default. Test with two separately launched game instances on the same local network. Create a room on the host, search the same room name on the client, then join with the same room password.

## Steam and EOS

The core plugin never links a Steam or EOS private module. To switch provider, enable the applicable engine plugin in `EvilClownParadise.uproject`, add that provider's official project configuration and credentials, then replace `DefaultPlatformService=Null` with the provider's value. Pass `true` for `bUseLobbiesIfAvailable` when the selected provider supports lobby-backed sessions.

Steam and EOS credentials, application identifiers, sandbox/deployment values, and transport settings are provider-specific and must remain in project configuration or secure deployment settings rather than this plugin.

## Password note

`RoomPassword` filters discoverable sessions before a client joins. It is advertised session data, not secure authentication. Enforce real room access through server-side login or gameplay authorization after the client connects.
