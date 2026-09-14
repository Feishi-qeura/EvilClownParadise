# FUOnlineSession UE 5.8 Win64 Upgrade Design

## Status

Approved for implementation on 2026-09-11. The direction was approved section by section in conversation and the consolidated specification passed independent safety review before coding began.

## Goal

Upgrade `FUOnlineSession` for Unreal Engine 5.8 on Win64 so one packaged build can use either NULL/LAN sessions or Steam lobby sessions. Enabling the plugin supplies a complete OnlineSubsystem environment without manual Engine ini editing, while disabling it removes that effective configuration after restart. Every online operation always enters structured history and is broadcast to Blueprint; configurable sinks additionally send it to `UE_LOG` and to an in-game diagnostic overlay.

## Current State

The plugin already contains two modules:

- `FUOnlineSession`, a Runtime module with Blueprint-facing session operations.
- `FUOnlineSessionEditor`, an Editor module that currently writes a marked block directly into the project's `DefaultEngine.ini`.

The runtime implementation already uses `TFU_OnlineSessionProviderTraits<Provider>` specializations and `if constexpr` to share Create, Find, Join, and Destroy behavior between Steam and NULL. It also keeps separate provider state and explicitly requests the `STEAM` or `NULL` subsystem.

The current implementation is incomplete for this goal because:

- Its Editor module physically rewrites `DefaultEngine.ini` and deliberately leaves generated configuration behind when automatic configuration is disabled.
- Its runtime changes the process-global `GEngine->NetDriverDefinitions` without coordinating multiple PIE worlds or restoring the prior definition.
- Logging is a mixture of a private log category, `LogTemp`, editor notifications, and a small connection-failure delegate. There is no correlated operation history, timeout watchdog, Blueprint report, or consistent screen output.
- The checked-in `DefaultEngine.ini` still contains an older marked FU configuration block that differs from the current generator.
- Build artifacts and prior logs do not prove that the latest source passes all tests or that both providers are present in a packaged Win64 build.

## Scope

### In scope

- Unreal Engine 5.8, verified against the installed 5.8.2 engine.
- Win64 x64 Editor plus packaged Development and Shipping builds.
- A single packaged executable contains both OnlineSubsystemNull and OnlineSubsystemSteam; NULL is fully verified in Development and Shipping, while Steam Shipping acceptance is conditional on the consuming project's Steam distribution prerequisites.
- Steam lobby sessions transported through SteamSockets in packaged Development and in Shipping builds launched through Steam or compiled with the project's required Steam shipping metadata.
- NULL sessions transported through the native IP net driver and LAN broadcast.
- Zero-manual-ini development setup using Steam test AppID 480.
- Project Settings fields for the development AppID and the expected real Shipping AppID; the plugin applies only the development override at runtime and validates the real Shipping AppID rather than pretending to replace Steam/TargetRules distribution metadata.
- Structured runtime diagnostics, operation watchdogs and recovery, an asset-free runtime screen overlay, Blueprint events/history/reporting, and Unreal log output.
- Safe one-time removal of legacy FU-managed blocks from `DefaultEngine.ini`.
- Existing Blueprint API compatibility.

### Out of scope

- macOS, Linux, Win64 ARM64, consoles, mobile, or non-Win64 packaging.
- EOS or the newer Online Services API.
- Dedicated-server Steam configuration and Steamworks partner backend setup.
- Generating or rewriting the consuming game's `Target.cs`/Steamworks partner metadata. A Shipping client launched outside Steam must supply `UE_PROJECT_STEAMSHIPPINGID` through the consuming project as required by Epic's Steam documentation.
- Secure password authentication. The current advertised room password remains discovery metadata, not authorization.
- Editing the existing `LAN.uasset`, `Room_Steam.uasset`, or `Steam.uasset` widgets.
- Solving or committing the unrelated, incorrectly nested staged empty `FUOnlineSessionEditor.Build.cs` entry.
- Claiming a real Steam end-to-end result without two machines or environments logged into different Steam accounts.

## Design Principles

1. Preserve the template/metaprogramming architecture. Provider-specific facts belong in compile-time traits; the session state machine remains shared.
2. Use UE 5.8 configuration layers instead of rewriting project configuration for steady-state operation.
3. Preserve existing Blueprint entry points and completion delegates. Diagnostics are additive.
4. Never silently reject an operation. Every rejection must produce both the existing completion result and a structured diagnostic event.
5. Never log room passwords, authentication tickets, tokens, secrets, or other sensitive values.
6. Treat process-global engine data as a leased resource with explicit ownership and restoration.
7. Verify packaged contents and effective configuration; an Editor-only test is not sufficient evidence for packaged support.

## Architecture

### Module responsibilities

`FUOnlineSession` remains the Runtime module and owns:

- Provider traits and the shared templated session state machine.
- `PreDefault` application of the configured Steam development AppID before any FU-owned Steam subsystem lookup.
- NetDriver lease coordination.
- Runtime environment checks, structured history/Blueprint dispatch, `UE_LOG`, and the viewport diagnostic overlay.
- Blueprint diagnostic APIs and history.

`FUOnlineSessionEditor` remains an Editor-only module and owns:

- Project Settings change notifications that explain when an editor restart is required.
- Validation of the effective UE 5.8/Win64 configuration.
- Safe one-time migration of old FU-managed `DefaultEngine.ini` blocks.
- Editor automation tests for configuration and migration behavior.

It no longer generates or continuously synchronizes OnlineSubsystem configuration into `DefaultEngine.ini`.

### Proposed file boundaries

- `Plugins/FUOnlineSession/Config/Engine.ini`: reversible Engine configuration modification layer.
- `Plugins/FUOnlineSession/Config/DefaultFUOnlineSession.ini`: defaults for FU-owned settings.
- `Source/FUOnlineSession/Public/FU_OnlineDiagnosticTypes.h`: Blueprint enums and diagnostic structs.
- `Source/FUOnlineSession/Private/Diagnostics/FU_OnlineSessionDiagnostics.h/.cpp`: formatting, sanitization, sink dispatch, history, and report generation helpers.
- `Source/FUOnlineSession/Private/Diagnostics/SFU_OnlineDiagnosticOverlay.h/.cpp`: asset-free Slate/viewport presentation used by packaged Development and Shipping clients instead of development-only print helpers.
- `Source/FUOnlineSession/Private/NetDriver/FU_OnlineSessionNetDriverLease.h/.cpp`: process-global GameNetDriver ownership and restoration.
- `Source/FUOnlineSession/Private/FU_SteamSocketsReadiness.h/.cpp`: concrete module/socket-subsystem checks behind an injectable Steam readiness snapshot.
- `Source/FUOnlineSession/Private/ProviderTraits/FU_OnlineSessionProviderTraits.h`: compile-time provider selection, retained and extended only with provider facts needed by diagnostics.
- `Source/FUOnlineSession/Public/FU_OnlineSessionSettings.h` and its implementation: plugin-specific AppID, timeout, history, and viewport-overlay settings.
- `Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h` and its implementation: existing public operations plus additive diagnostic APIs.
- `Source/FUOnlineSessionEditor/Private/FUOnlineSessionLegacyConfigMigration.h/.cpp`: pure legacy-block analysis/transformation and guarded file migration.
- Existing Runtime and Editor test files, split only if their size becomes difficult to review.

Each new non-trivial type or function must have a comment above it explaining its purpose, invariant, and failure behavior. Comments must explain why a boundary or guard exists rather than merely paraphrasing the next line.

## Reversible UE 5.8 Configuration

### Plugin Engine modification layer

UE 5.8 supports plugin modification layers such as `Plugin/Config/Engine.ini`. The plugin manager adds the layer only for an enabled plugin and removes it from the effective hierarchy when the plugin is disabled and the process restarts. UE 5.8 staging also includes enabled plugin ini files.

`Plugins/FUOnlineSession/Config/Engine.ini` will provide these reversible transport defaults. The development AppID deliberately stays out of this static layer so a packaged Shipping client cannot inherit the test AppID merely because the plugin is enabled:

```ini
[OnlineSubsystem]
DefaultPlatformService=Null

[OnlineSubsystemSteam]
bEnabled=true
bUseSteamNetworking=false
bAllowP2PPacketRelay=true

[/Script/SteamSockets.SteamSocketsNetDriver]
NetConnectionClassName="/Script/SteamSockets.SteamSocketsNetConnection"
```

`DefaultPlatformService=Null` is intentional. It lets the process start without Steam and ensures the base OSS initializes a safe offline/LAN default. Steam operations still explicitly request `STEAM`; NULL operations explicitly request `NULL`.

`bUseSteamNetworking=false` is intentional for a dual-provider process, but it does not disable the separate SteamSockets plugin. It disables OnlineSubsystemSteam's legacy SteamNetworking transport; SteamSockets remains a separately selected named transport. NULL therefore keeps its native UDP `IpNetDriver`, while Steam operations explicitly install `SteamSocketsNetDriver` through the provider trait with `SteamSocketsNetConnection`.

The plugin layer will not clear or replace `NetDriverDefinitions`. Clearing the array from a plugin layer could destroy a project's Beacon, Demo, custom, or third-party drivers. Runtime selection is instead handled by the guarded NetDriver lease.

### FU-owned settings and AppID bootstrap

`UFU_OnlineSessionSettings` will move from the generic Engine config branch to the plugin-owned `FUOnlineSession` config branch. Defaults live in `DefaultFUOnlineSession.ini` and include:

- `SteamDevAppId=480`, used only by the non-Shipping in-memory bootstrap.
- `ExpectedShippingSteamAppId=0`, a Project Settings validation target for the real app. Steam operations in Shipping require a value greater than zero but this field is never injected into Engine config at runtime.
- `OperationTimeoutSeconds=30.0`, shared by Create, Find, Join, and Destroy and clamped to `[5.0, 300.0]` seconds when read.
- `DiagnosticHistoryLimit=256`.
- Output-log enablement.
- Viewport-overlay enablement, minimum overlay severity, display duration, and maximum visible rows.

The Runtime module entry in `FUOnlineSession.uplugin` will use `"LoadingPhase": "PreDefault"`. In non-Shipping builds, `StartupModule` first checks `IOnlineSubsystem::DoesInstanceExist(STEAM)` without auto-loading Steam. If an instance already exists, another earlier module has made the development AppID immutable for this process; FU records `FU.Steam.InitializedBeforeBootstrap` and keeps Steam operations fail-closed.

Otherwise, only in non-Shipping builds, the module reads `UFU_OnlineSessionSettings` and applies a nonzero development AppID through UE 5.8's in-memory `FConfigBranch::AddDynamicLayerStringToHierarchy`, using a process-unique FU synthetic filename, an FU ownership tag, and `DynamicLayerPriority::ProjectPluginOverrides`. The tiny layer contains only `[OnlineSubsystemSteam] SteamDevAppId=<validated value>`, participates in the effective `GEngineIni` hierarchy for the process lifetime, and is removed by the stored exact synthetic filename during orderly module shutdown; the tag is retained for ownership/invariant checks. It does not use `GConfig->SetString`, does not enter `RuntimeChanges`, and is never flushed to a project or Saved ini file.

The non-Shipping bootstrap succeeds only when the Engine branch exists, no conflicting FU ownership tag is present, `AddDynamicLayerStringToHierarchy` returns `true`, tag lookup resolves that same branch, and readback equals the requested value. Any failed prerequisite is fail-closed even when an underlying project value coincidentally already equals 480; the milestone is set only after the owned layer itself is proven active. `ShutdownModule` calls `RemoveDynamicLayerFromHierarchy` only when this module instance successfully added the layer, checks its return value, and reports a safe shutdown Error if removal fails.

After the non-Shipping bootstrap, or at Shipping startup without injection, FU reads the effective transport keys. If an equal- or higher-priority layer produced a conflicting final value, the affected provider fails closed with `FU.Config.EffectiveValueConflict` rather than running in an ambiguous transport environment. After Steam is first instantiated, FU compares the provider's public `IOnlineSubsystem::GetAppId()` result with `SteamDevAppId` outside Shipping and `ExpectedShippingSteamAppId` in Shipping. A mismatch, a missing expected Shipping ID, or failed Steam initialization/subscription emits a precise `FU.Steam.RuntimeAppIdMismatch`/`FU.Steam.ShippingPrerequisiteMissing` diagnostic and prevents Steam session operations without affecting NULL. The diagnostic names Steam launch and the consuming project's `UE_PROJECT_STEAMSHIPPINGID` as checks; it does not claim to infer the exact missing external build setting at runtime. Startup and every provider preflight log the sanitized effective `DefaultPlatformService`, Steam enabled flag, `bUseSteamNetworking`, configured expectation, NetConnection class, and initialized provider AppID when available. The README warns that another project plugin must not override these FU-owned transport keys at the same dynamic priority.

OnlineSubsystem itself loads at `PostConfigInit`, where the static plugin layer's `DefaultPlatformService=Null` makes NULL the safe default. OnlineSubsystemSteam and SteamSockets remain `Default`-phase modules and consume the in-memory development AppID later. `PreDefault` guarantees ordering only for FU-owned non-Shipping lookups; a third-party `PostConfigInit` or earlier `PreDefault` plugin that instantiates Steam first is explicitly unsupported and detected as described above.

No FU static initializer, subsystem constructor, settings validation path, or diagnostic preflight may resolve Steam before the non-Shipping bootstrap milestone. A descriptor/config contract test locks the Runtime module to `PreDefault`; branch tests prove the tagged override changes only the in-memory effective value and is removable without a disk write; and a startup-order integration test records the bootstrap milestone before the first instrumented FU call to `IOnlineSubsystem::Get(STEAM)`. Any non-Shipping FU lookup that does not observe that milestone fails closed with `FU.Steam.AppIdBootstrapOrderInvalid`.

Changing either AppID setting in Project Settings persists only FU-owned settings and requires an editor restart. AppID 480 is accepted only as a development value and produces a clear warning. A value less than one for the active build mode is rejected and leaves Steam operations unavailable with a precise diagnostic. Shipping acceptance additionally requires the game's real Steam AppID, a subscribed logged-in user, and Steam launch or the consuming project's `UE_PROJECT_STEAMSHIPPINGID`; the plugin detects these conditions but does not generate TargetRules or Steamworks backend data.

When the plugin is disabled, its Engine modification layer is absent on the next run. FU-specific settings may remain as inert project preferences, but they no longer alter OnlineSubsystem behavior, so the effective Engine configuration returns to the project's underlying state.

### Legacy configuration migration

Older plugin versions wrote a block delimited by:

```text
; BEGIN FUONLINESESSION AUTO CONFIG
; END FUONLINESESSION AUTO CONFIG
```

The Editor module will analyze the project `DefaultEngine.ini` once per startup:

- No markers: no migration is needed.
- Exactly one correctly ordered pair: remove only the marked block while preserving every surrounding byte, the original UTF BOM policy, and the original CRLF/LF convention.
- Missing, reversed, duplicated, or overlapping markers: do not write the file; emit an Error diagnostic and editor notification with a manual recovery instruction.
- A valid migration first writes an exact timestamped byte-for-byte backup under `Saved/FUOnlineSession/Backups`. It then writes transformed bytes to a uniquely named temporary file in the same directory as `DefaultEngine.ini`, flushes and closes it, rereads it, and validates both the preserved prefix/suffix and the absence of the managed block.
- Only after backup and temporary-file validation succeed does the Win64 implementation call `ReplaceFileW` with supported flags `0` and a second, unique same-directory rollback-backup path; the temporary file is made durable beforehand with `FlushFileBuffers` and close. `IFileManager::Move` is not accepted as proof of atomic replacement. `ReplaceFileW` can report failure after file names have moved, so the post-call classifier compares the target bytes with precomputed original and transformed hashes: original hash means unchanged failure; transformed hash means published-with-warning despite the API failure; missing or any third hash means `ManualRecoveryRequired`. In the last case FU retains the temporary file when present, the same-directory rollback backup, and the Saved byte-exact backup, performs no automatic cleanup or speculative rollback, and gives the exact paths and recovery instruction.

The repository's currently checked-in legacy block will be removed as part of the upgrade so the new configuration layer is the only FU-owned Engine configuration source.

## Explicit Dependencies and Platform Boundary

`FUOnlineSession.uplugin` will use the exact descriptor boundary below:

- Root `"EngineVersion": "5.8.0"`.
- Root `"SupportedTargetPlatforms": ["Win64"]`.
- Both Runtime and Editor module entries use `"PlatformAllowList": ["Win64"]` and `"PlatformArchitectureDenyList": ["Win64:arm64"]`.
- The Runtime module changes from `Default` to `"LoadingPhase": "PreDefault"`; the Editor module remains `PostEngineInit`.

The descriptor will declare these direct plugin dependencies as enabled:

- `OnlineSubsystem`
- `OnlineSubsystemUtils`
- `OnlineSubsystemNull`
- `OnlineSubsystemSteam`
- `SteamSockets`

`SteamShared` is not duplicated as a direct FU dependency because both Epic's `OnlineSubsystemSteam` and `SteamSockets` descriptors already declare it. The README will make that transitive dependency explicit.

Runtime build rules retain the OnlineSubsystem interfaces they compile against, add `Slate` and `SlateCore` for the asset-free packaged overlay, and retain the dynamic references required to include Steam, NULL, and SteamSockets in a packaged build. Platform allow-lists prevent the plugin from claiming unsupported targets. Automated descriptor assertions, BuildPlugin, and staged-receipt checks must prove these boundaries and dependencies are present in Win64 output.

### SteamSockets runtime readiness

Steam preflight does not treat a loadable `USteamSocketsNetDriver` class as proof that the transport can actually run. It loads/checks `FSteamSocketsModule`, requires `IsSteamSocketsEnabled()`, and then requires `ISocketSubsystem::Get(STEAM_SOCKETS_SUBSYSTEM)` to return the named SteamSockets subsystem. Each failure maps to a distinct provider-status/diagnostic code (`FU.SteamSockets.ModuleUnavailable`, `FU.SteamSockets.Disabled`, or `FU.SteamSockets.SocketSubsystemUnavailable`) and prevents only Steam operations; NULL remains available.

The Runtime module therefore adds a direct private `SteamSockets` build dependency for its public module/readiness API, while Steam and NULL remain dynamically loaded OnlineSubsystem implementations. Tests use an injectable readiness snapshot rather than relying on whether the Editor happens to load the SteamSockets DLL. A PIE class-load result is never treated as Steam transport readiness; actual SteamSockets end-to-end acceptance remains a packaged-client check.

## Provider Traits and Runtime Selection

The public Blueprint API remains provider-specific while the implementation remains templated:

```text
CreateSteamSession / FindSteamSessions / JoinSteamSession / DestroySteamSession
CreateLanSession   / FindLanSessions   / JoinLanSession   / DestroyLanSession
```

Each wrapper selects `TFU_OnlineSessionProviderTraits<Steam>` or `TFU_OnlineSessionProviderTraits<Lan>` at compile time. Traits continue to define:

- Subsystem name.
- Human-readable diagnostic name.
- Whether the search is LAN.
- Create and search settings.
- Required NetDriver and NetConnection classes.
- Steam-only lobby keyword publication and exact search filtering.

Steam and NULL retain independent provider state, cached search results, delegate handles, and watchdogs. A single GameInstance may search either provider, but only one provider may own an active gameplay session.

### NetDriver lease

`GEngine->NetDriverDefinitions` is process-global, including across PIE worlds. In UE 5.8, active `FNamedNetDriver` records can retain a pointer to an element of that array. Adding, removing, sorting, or otherwise structurally changing the array could reallocate it and invalidate those pointers, so the lease is intentionally stricter than a general-purpose configuration editor:

1. Acquisition is allowed only on the game thread with valid Engine, GameInstance, and World objects.
2. The coordinator requires exactly one existing `GameNetDriver` definition. A missing or duplicate definition is a hard readiness error; FU never inserts, removes, or deduplicates array elements.
3. Before a first acquisition or any restoration, the coordinator scans every entry returned by `GEngine->GetWorldContexts()`. It rejects first mutation for any `FNamedNetDriver` whose `NetDriverDef` is the target definition or whose live `UNetDriver::GetNetDriverDefinition()` is `GameNetDriver`, and for every non-null `PendingNetGame` (recording its `GetNetDriver()` and, when present, that driver's definition). A pending object with no driver can still create `GameNetDriver` later, so it is unsafe as well. **Any** such use rejects the first mutation, even if its current class appears compatible, and identifies its World/PIE instance.
4. First acquisition snapshots the complete existing `FNetDriverDefinition`, the exact element pointer and index, the array `Num`, an ordered fingerprint of every element's `DefName`, the provider, and a weak GameInstance owner. Ownership uses GameInstance rather than a single World so legitimate travel does not look like a different owner.
5. The installed definition starts as a complete copy of that snapshot and changes only `DriverClassName` and `DriverClassNameFallback`. Installation assigns the copy only into the existing element, preserving fields such as channel overrides and parallel-connection-tick policy. Both changed class fields use the provider trait's class, so Steam can never silently fall back from a `steam.<id>` address to `IpNetDriver` and LAN can never inherit the Steam transport.
6. Re-acquisition is allowed only when the same GameInstance/provider already owns the lease, the target definition still equals the FU-installed value, and the structural fingerprint is unchanged. That repeat acquisition is idempotent even when the owner's expected travel driver is active. Another GameInstance/PIE instance, another provider, or any mismatch is rejected.
7. Switching providers is allowed only after the prior named session is gone and a full all-World scan finds no active or pending driver using the leased definition.
8. Release is a request, not an assumption that a completed `DestroySession` has already torn down network travel. If any World still uses or may use the definition, the process-global coordinator keeps the lease and a game-thread ticker retries after driver/world teardown.
9. Immediately before restoration, the coordinator verifies the exact element pointer and index, array `Num`, ordered `DefName` fingerprint, exactly-one-`GameNetDriver` invariant, and complete current installed value. Full-definition equality is explicit over UE 5.8's five fields: `DefName`, `DriverClassName`, `DriverClassNameFallback`, `MaxChannelsOverride`, and `bRunParallelConnectionTick`. A mismatch transitions the coordinator to `Poisoned/ExternallyModified`: FU never overwrites the third-party state, never grants another lease in that process, and emits an invariant Error containing the safe structural facts and both non-secret definitions.
10. When restoration is safe, it assigns the complete snapshot back into the same element in place. No acquire or release path performs `Add`, `Remove`, `Empty`, `Reset`, or any other structural mutation on `NetDriverDefinitions`.

Release is requested on synchronous request rejection, asynchronous Create/Join failure, timeout recovery completion, Destroy completion, `TravelFailure`, owner subsystem deinitialization, and any operation rollback after acquisition. Connection/travel failure and session destruction may initially defer restoration while a driver remains active; the coordinator's ticker is the automatic completion mechanism. Its lifetime belongs to the Runtime module rather than a World subsystem so deferred cleanup can outlive a traveling or deinitializing World.

`ShutdownModule` first unregisters the coordinator ticker so unloaded code can never be called. It then performs one final restoration only when `GEngine` is still valid, the structural/current-value invariants match, and the all-World scan finds no active or pending driver. Otherwise it records the safest available Error and stops without mutating an active or externally changed definition; process exit is safer than a speculative restore.

The lease changes only effective in-memory engine state. It never writes project or engine files.

## Operation State Machine and Watchdogs

The existing shared template flow remains authoritative. Every public operation follows this order:

1. Allocate a diagnostic-only monotonic `AttemptSequence` and fresh `OperationId` at the public entry before any argument, environment, state, or lease check, then record the attempt. Even an immediate rejection therefore has a usable correlation ID, but it cannot disturb an operation already in flight.
2. Validate local arguments, provider environment, provider operation state, and the active gameplay provider. A rejection emits its structured event and matching existing Blueprint failure exactly once without modifying the active operation's generation, timer, delegate, pending data, or completion flag.
3. Acquire or validate the NetDriver lease when Create or Join may lead to network travel.
4. Only after every preflight succeeds, increment `ActiveGeneration`, associate it with this attempt's `OperationId`, store pending data, transition to the submitted operation state, and bind exactly one provider-specific completion delegate.
5. Arm the generation-bound watchdog before calling the OSS interface.
6. Submit the OSS request. After the call returns, handle its return value only if the same generation is still current; this protects against an implementation that invoked its completion delegate synchronously.
7. If submission returns `false` while the generation is current, immediately cancel the timer, clear the delegate, sanitize pending data, request lease rollback where applicable, return to Idle, emit `RequestRejected`, and broadcast the existing Blueprint failure exactly once.
8. On a normal callback, validate the generation before touching state. Cancel the watchdog, clear the matching delegate, update session/search data, perform any approved travel, transition to the appropriate terminal state, and broadcast the existing completion exactly once.

Each provider state gains:

- Monotonic `AttemptSequence`, current active `OperationId`, and separately monotonic `ActiveGeneration`.
- Operation start time.
- Watchdog timer handle.
- `bCompletionBroadcast`, so a timeout and late callback cannot notify Blueprint twice.
- `bAwaitingOriginalCompletion` for non-cancelable Create, Join, and Destroy requests, plus the separate Find-cancellation state and recovery-destroy attempt count.
- The existing operation enum, pending action, session interface, results, and delegate handles, extended with `Recovering`.

At the timeout threshold, the subsystem first validates the generation, emits `FU.Operation.Timeout` with elapsed time and suggested checks, and broadcasts the matching existing failure result once. A timeout does **not** prove that the underlying OSS request stopped:

- Find has a cancellation API. FU transitions to `Recovering`, binds the generation-bound cancel-completion delegate, calls `CancelFindSessions`, and retains both delegates until either the original Find callback or a successful cancellation callback proves the request terminal. That terminal path clears both delegate handles exactly once. A synchronous cancellation rejection or failed cancellation remains Recovering and waits for the original callback.
- Create, Join, and Destroy have no general cancellation contract. FU transitions to `Recovering`, sets `bAwaitingOriginalCompletion=true`, keeps the original delegate and interface alive, blocks new operations for that provider, and blocks any gameplay provider switch. `GetSessionState()==NoSession` alone never clears that flag or opens the gate. A timed-out Create/Join callback that later succeeds must not start travel; it is reconciled and the unexpected named session is destroyed through a separate generation-bound recovery step.
- A late callback for the timed-out generation updates recovery state and diagnostics but never broadcasts the public completion a second time. It can never be mistaken for a newer operation because no newer operation is admitted while that generation is recovering.

The recovery reconciler reads `GetSessionState(NAME_GameSession)` only after a terminal callback/cancellation outcome has cleared the relevant outstanding-request flag; it never force-resets an unknown request:

- For timed-out non-cancelable operations, no state value including `NoSession` permits cleanup before the original completion delegate arrives. `TryRecoverProvider` reports that fact but cannot cancel or discard the request.
- After the original callback (or successful Find cancellation) has made the request terminal, `NoSession`, no outstanding Find cancellation, and no active/pending World driver permit delegate cleanup, lease release, and transition to Idle.
- A transient `Creating`, `Starting`, or `Destroying` state remains Recovering and is polled at a bounded interval.
- A stable named session left by a timed-out Create, Join, **or Destroy** starts exactly one automatic recovery `DestroySession` after the original request is proven terminal (and no longer `Destroying`) and its callback/delegate has been reconciled. A late successful Destroy waits for the post-callback safe `NoSession` path; a late failed Destroy retains the live session, provider, and lease in Recovering until this controlled recovery Destroy is issued. Idle is reached only after recovery destroy success or a later state check proves `NoSession`.
- If the one automatic recovery destroy fails and the stable session remains, FU stays Recovering and does not spin an unbounded retry loop. Each explicit `TryRecoverProvider` call may request at most one further recovery destroy when no destroy is in flight; the result and remaining state are diagnosed.
- A lost session interface or permanently indeterminate backend remains Recovering, emits periodic rate-limited guidance, and requires interface restoration, GameInstance teardown, or process restart; it never opens the gate merely because local time elapsed.

`TryRecoverProvider(Provider)` exposes the same conservative reconciler to Blueprint. It returns `true` only when the provider is now Idle, emits a diagnostic explaining its decision, and never discards an in-flight request or active session.

Deinitialization cancels timers and removes callbacks owned by the dying subsystem so no UObject is called afterward, records any indeterminate operation, and requests lease release. The Runtime module's process-global lease ticker completes deferred restoration only after the all-World safety checks pass.

## Structured Diagnostics

### Blueprint types

`FFU_OnlineDiagnosticEvent` is Blueprint-visible and contains:

- UTC timestamp and per-subsystem sequence number.
- `FGuid OperationId`.
- Provider, operation, phase, and severity enums.
- Stable machine-readable `FName Code`.
- Provider operation state and effective subsystem/driver names.
- World name and PIE instance identifier when available.
- Concise message, likely cause, and suggested action.
- An array of already-sanitized diagnostic fields.

Diagnostics use stable codes such as `FU.Provider.SubsystemUnavailable`, `FU.SteamSockets.ModuleUnavailable`, `FU.Find.RequestRejected`, `FU.Join.ResolveAddressFailed`, and `FU.Operation.Timeout`. Blueprint UI should branch on enums or codes, not localized text.

### Single dispatch path

All runtime events go through one shared dispatcher. Provider-aware call sites use a templated helper so subsystem name, required driver, and provider label always come from the same compile-time trait as the session operation.

The dispatcher performs these actions in order on the game thread:

1. Build and sanitize the event.
2. Append it to a bounded ring history, removing the oldest entries beyond the configured limit.
3. Emit to the public `LogFUOnlineSession` category at the matching verbosity when that sink is enabled.
4. Display a concise colored message in the runtime viewport overlay when that sink is enabled, a viewport exists, and severity passes its configured threshold.
5. Broadcast `OnOnlineDiagnosticEvent` to Blueprint.

History insertion and the Blueprint broadcast are mandatory for every operation event. The log and overlay are configurable presentation sinks: logging defaults to all severities, while Warning and Error are shown in the overlay by default. Verbose and Info remain available through the log, Blueprint, history, and an explicit overlay threshold opt-in. A server or commandlet with no local viewport simply skips presentation without losing the event.

No call site may pass a raw password, ticket, token, authorization value, or secret to the dispatcher. Diagnostic field construction distinguishes public and sensitive values; sensitive values are replaced with `<redacted>` before an event enters any sink or history.

### Blueprint API

The existing `UFU_OnlineSessionSubsystem` adds this fixed public contract:

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FFU_OnOnlineDiagnosticEvent,
    const FFU_OnlineDiagnosticEvent&, Event);

UPROPERTY(BlueprintAssignable, Category="FU Online Session|Diagnostics")
FFU_OnOnlineDiagnosticEvent OnOnlineDiagnosticEvent;

UFUNCTION(BlueprintPure, Category="FU Online Session|Diagnostics")
TArray<FFU_OnlineDiagnosticEvent> GetDiagnosticHistory() const;

UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
void ClearDiagnosticHistory();

UFUNCTION(BlueprintPure, Category="FU Online Session|Diagnostics")
FString BuildDiagnosticReport() const;

UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
bool SaveDiagnosticReport(FString& OutSavedPath, FString& OutError);

UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
FFU_OnlineProviderStatus RunProviderDiagnostics(EFU_OnlineProvider Provider);

UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
bool TryRecoverProvider(EFU_OnlineProvider Provider);
```

`GetDiagnosticHistory` returns an oldest-to-newest snapshot. `ClearDiagnosticHistory` clears only retained events and never operational state. `RunProviderDiagnostics` is synchronous and read-only with respect to OSS/session state, returns the same status schema used by the existing provider checks, and also emits one correlated diagnostic event. `TryRecoverProvider` has only the conservative recovery semantics defined above.

`BuildDiagnosticReport` includes plugin version, UE version, platform/build configuration, effective non-secret OnlineSubsystem values, provider readiness, and the bounded history. `SaveDiagnosticReport` writes only beneath `Saved/Logs/FUOnlineSession`; Blueprint cannot supply an arbitrary path.

`SaveDiagnosticReport` clears both output strings on entry. On success it returns `true`, fills `OutSavedPath`, and leaves `OutError` empty. On failure it returns `false`, leaves `OutSavedPath` empty, fills `OutError` with a sanitized actionable reason, and emits one non-recursive save-failure event.

Runtime initialization and every operation preflight emit environment snapshots. Create, Find, Join, Destroy, delegate cleanup, timeout, ClientTravel, network failure, and travel failure emit correlated events. Search diagnostics include raw and filtered counts but no private lobby metadata.

Packaged on-screen diagnostics do not use `UKismetSystemLibrary::PrintString`, `GEngine->AddOnScreenDebugMessage`, or other development-only debug-screen contracts. A small asset-free Slate widget is attached through `UGameViewportClient::AddViewportWidgetContent`, keeps a bounded queue of expiring severity-colored rows, and is removed when its owning viewport/subsystem is torn down. This keeps Blueprint history and client viewport presentation available in Development and Shipping without editing or depending on project UMG assets.

Calls to the public `UE_LOG` category remain present in all configurations, but actual Shipping log compilation and persistence follow the consuming target's rules. The plugin does not alter TargetRules or force `bUseLoggingInShipping`; Blueprint history/report saving and the viewport overlay provide the supported Shipping investigation path when ordinary log files are unavailable.

## Error Handling

- Every existing early-return path emits a diagnostic before its current failure delegate.
- Provider readiness continues to distinguish World, subsystem, Session interface, Identity, login, SteamSockets module/enabled/socket-subsystem, driver definition, driver class, active-driver, and lease-owner failures.
- Steam AppID 480 is a Warning, not an Error. Invalid AppID is an Error and prevents Steam operations.
- A successful `JoinSession` still means ClientTravel started, not that network travel completed. Network and travel failures remain separate events and also enter diagnostic history.
- File migration never attempts publication unless the readable original, byte-exact Saved backup, same-directory temporary file, same-directory rollback-backup path, and reread validation prerequisites all succeed. A `ReplaceFileW` failure is classified by target hash rather than assumed safe: unchanged, published-with-warning, or manual-recovery-required. The injected file-operation seam covers all three outcomes.
- Diagnostic-report save failure is itself returned to Blueprint and dispatched through the normal diagnostic pipeline, but it does not recursively attempt to save another report.
- Sensitive values are omitted at construction time rather than removed only during final string formatting.

## Compatibility

Existing session functions, provider enums, session result fields, and completion delegates retain their names and meanings. New diagnostics are additive. Existing Widget assets are not resaved or edited.

The obsolete `bAutoConfigureProject` setting is retained only as a deprecated migration field if removing it would create avoidable config churn; it no longer controls steady-state Engine configuration. New code and documentation do not instruct users to enable it.

The plugin README will document:

- Enable-one-plugin setup and transitive dependencies.
- NULL and Steam Blueprint flows in the same packaged build.
- The required destroy-and-leave-world sequence before provider switching.
- Development AppID 480 versus a real Shipping AppID, Steam launch/subscription checks, and the consuming project's `UE_PROJECT_STEAMSHIPPINGID` requirement when launching outside Steam.
- SteamSockets build compatibility boundaries.
- How to bind diagnostics and save a report.
- The advertised-password security limitation.

## Test-Driven Implementation

Every production behavior change starts with a failing test. Configuration-only artifacts are covered by contract tests before their final content is added.

### Runtime automation coverage

- Provider traits select the expected subsystem, NetDriver, NetConnection, create flags, search flags, and Steam project keyword.
- NULL traits never inherit Steam lobby or transport settings.
- Provider status maps every missing dependency, SteamSockets module/enabled/socket-subsystem condition, AppID-mode mismatch, and lease conflict to the correct status.
- NetDriver lease tests cover exact-one-definition enforcement; first-acquire rejection for every active `FNamedNetDriver` referencing the target and every non-null `PendingNetGame` (with and without a driver); complete-copy/two-field in-place mutation; explicit five-field equality; pointer/index/Num/ordered-name fingerprinting; idempotence; cross-GameInstance and provider-switch rejection; deferred restoration; exact full-definition restoration; poisoned third-party mutation handling; and safe Runtime-module shutdown. Tests assert that no path inserts or removes an array element.
- Diagnostic severity mapping, sequence and operation correlation, bounded history, report content, and sensitive-field redaction.
- Operation tests cover AttemptSequence/OperationId allocation before every preflight failure, ActiveGeneration allocation only after preflight, watchdog-before-submit ordering, a synchronous callback during submission, synchronous `false` cleanup, and exactly-once public completion. A second call rejected as Busy must leave the first call's generation/timer/delegate intact and allow that first call to complete normally.
- Timeout tests cover Find cancellation terminal rules, non-cancelable Create/Join/Destroy transitions to Recovering, `NoSession` before the original callback remaining blocked, late-success cleanup without travel, late-Destroy failure recovery, stale-generation rejection, refusal to admit a new operation while indeterminate, `TryRecoverProvider`, and Idle only after original completion plus backend/session/driver state is proven safe.
- Overlay presenter tests cover severity filtering, expiry, bounded rows, sanitization, absent-viewport behavior, and teardown without relying on development-only screen APIs.
- Existing request-validation and result-default tests remain green.

### Editor automation coverage

- Plugin `Engine.ini` contains the reversible NULL/Steam transport defaults, has no static `SteamDevAppId`, and does not clear `NetDriverDefinitions`.
- `DefaultFUOnlineSession.ini` contains the exact 30-second timeout, `[5, 300]` runtime clamp contract, development AppID, expected Shipping AppID, history, log, and overlay defaults.
- The plugin descriptor asserts exact `EngineVersion`, root supported platform, both module allow/architecture-deny lists, Runtime `PreDefault`, and every direct dependency.
- Startup-order coverage proves the tagged non-Shipping in-memory AppID layer precedes the first instrumented FU `IOnlineSubsystem::Get(STEAM)` request; detects a missing branch, duplicate tag, pre-existing Steam instance, add/remove failure, effective-value conflict, and provider `GetAppId()` mismatch; removes only its owned process-unique layer; and never enters runtime changes or flushes project/Saved Engine config. Shipping tests assert that no development dynamic layer is added and that a missing or mismatched expected real AppID blocks only Steam. An explicit test requires bootstrap failure when layer addition returns `false` even if readback already happens to equal 480.
- Legacy migration handles no marker, one valid block, malformed ordering, missing marker, duplicates, and byte-exact preservation of surrounding content, UTF-8 BOM, and CRLF/LF style.
- Migration write tests inject backup, temporary-write, flush, reread-validation, and `ReplaceFileW` outcomes. They cover API-false/target-original, API-false/target-transformed, and API-false/target-missing-or-third-hash; the first reports unchanged, the second reports published-with-warning, and the third retains every recovery artifact with `ManualRecoveryRequired`. No test treats a generic file move as atomic.
- Settings changes that require restart are classified correctly.

Source-text assertions should be replaced with behavior or configuration contract tests where practical. They may be retained only where Unreal APIs do not expose a test seam and the assertion protects a critical integration contract.

## Verification Matrix

1. Inspect Git status before and after work to ensure the existing Widget changes and incorrectly nested staged Build.cs are untouched.
2. Build `EvilClownParadiseEditor Win64 Development` with `FUOnlineSession` explicitly enabled for the command.
3. Run the complete `Automation RunTests FUOnlineSession` suite and require zero failures.
4. Run `BuildPlugin` for Win64 and inspect the packaged descriptor, Runtime/Editor binaries, and plugin configuration files.
5. Run Win64 Development and Win64 Shipping `BuildCookRun` with the plugin enabled.
6. Inspect staged receipts/files for `FUOnlineSession`, `OnlineSubsystem`, `OnlineSubsystemNull`, `OnlineSubsystemSteam`, `SteamSockets`, Steam redistributables, and the staged plugin config cache.
7. In Development, query or log effective packaged configuration to prove NULL is the default, Steam is enabled, the configured development AppID was applied before the first Steam lookup and matches the initialized provider's `GetAppId()`, the legacy OnlineSubsystemSteam SteamNetworking path is disabled, and FU selects SteamSockets explicitly only for Steam operations. In Shipping, prove no test AppID layer was injected and compare `GetAppId()` against the expected real AppID after valid Steam initialization.
8. In both Development and Shipping clients, force a safe warning/error and manually verify the asset-free viewport overlay, Blueprint event/history, report generation, and report save path. Verify the same event remains in history when the overlay is disabled or no viewport exists.
9. Launch two packaged instances in **both Development and Shipping** and verify NULL host, discovery, join, ClientTravel, destroy, deferred NetDriver restoration, provider switch, and timeout/failure reporting.
10. In Shipping, verify Steam preflight requires the expected real AppID, reports failed initialization/subscription with Steam launch and TargetRules prerequisites as actionable checks, selects the SteamSockets driver contract only when the module and named socket subsystem are ready, and returns precise diagnostics without affecting NULL.
11. When two distinct logged-in Steam environments are available, verify Steam lobby create, project-keyword discovery, join, SteamSockets travel, destroy, and negative diagnostics in a packaged Development build; repeat in a Shipping build launched through Steam or built with the consuming project's required `UE_PROJECT_STEAMSHIPPINGID`. Until that occurs, report it as an explicit manual acceptance gap. AppID 480 is never treated as proof that a real Shipping AppID/account entitlement works.

Passing Automation tests alone is not evidence that packaging or real provider networking works. Every completion claim must identify which matrix entries were run and which external acceptance items remain.

## Delivery Constraints

- Do not edit or delete existing user-owned Widget assets.
- Do not stage, unstage, or commit the unrelated incorrectly nested empty Build.cs unless the user separately authorizes it.
- Use focused commits and include only files belonging to this upgrade.
- Preserve UTF-8 source text and existing public Blueprint names.
- Do not modify UE installation files, engine `BaseEngine.ini`, or built-in plugin descriptors.
- Do not publish Steam credentials, real private AppIDs, tickets, or user identifiers in source, tests, diagnostics, or documentation.
