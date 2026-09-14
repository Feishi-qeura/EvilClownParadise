# FUOnlineSession UE 5.8 Win64 Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade FUOnlineSession into a UE 5.8 Win64 plugin that ships a reversible NULL/LAN plus SteamSockets session environment, guarded runtime state, and correlated Blueprint-visible diagnostics.

**Architecture:** Configuration is supplied by an enabled-plugin Engine modification layer, while a non-Shipping, in-memory dynamic layer supplies the development Steam AppID without writing project Engine config. Compile-time provider traits remain the single Steam/NULL mapping; new private coordinators own global NetDriver leasing, SteamSockets readiness, operation recovery, diagnostics, and legacy-config migration.

**Tech Stack:** Unreal Engine 5.8.2 C++20/UBT, OnlineSubsystemNull, OnlineSubsystemSteam, SteamSockets, Slate/SlateCore, UE Automation Tests, Win32 `ReplaceFileW`/`FlushFileBuffers`.

**Spec:** `docs/superpowers/specs/2026-09-10-fu-online-session-ue58-upgrade-design.md`

## Global Constraints

- Target UE 5.8.2 and Win64 x64 only; descriptor root is `EngineVersion: "5.8.0"` and `SupportedTargetPlatforms: ["Win64"]`.
- Preserve every existing Blueprint function, enum ordinal, struct field, and delegate; new public surface is additive only.
- Keep `TFU_OnlineSessionProviderTraits<Steam/Lan>` as the sole source of provider-specific OSS, driver, connection, create, and search facts.
- Never modify engine files, `BaseEngine.ini`, existing Widget assets, or the unrelated staged nested Editor Build.cs path.
- Enabled plugin configuration must be reversible through UE 5.8 plugin config layers; it must not continuously rewrite project `DefaultEngine.ini`.
- Development AppID 480 may be injected only in-memory and only outside Shipping. Shipping Steam requires a real expected AppID plus the consuming game's Steam launch/subscription/TargetRules prerequisites.
- Every non-trivial new function and each changed safety branch receives a Chinese comment explaining purpose, invariant, and failure behavior.
- Every behavior change follows red-green-refactor: write one failing Automation test, run it to see the intended failure, write the smallest production change, then re-run the focused and full FU test suites.
- Never place passwords, auth tickets, Steam identifiers, or secrets into diagnostic history, reports, overlay rows, Blueprint fields, or logs.

## File Structure

| Path | Responsibility |
| --- | --- |
| `Plugins/FUOnlineSession/FUOnlineSession.uplugin` | UE/platform boundary, module load phase, explicit plugin dependencies. |
| `Plugins/FUOnlineSession/Config/Engine.ini` | Reversible NULL/SteamSockets transport defaults; no static test AppID. |
| `Plugins/FUOnlineSession/Config/DefaultFUOnlineSession.ini` | FU-owned developer settings defaults. |
| `Source/FUOnlineSession/Public/FUOnlineSessionModule.h` / `Private/FUOnlineSessionModule.cpp` | Public log category, PreDefault AppID bootstrap ownership, global coordinator shutdown. |
| `Private/FU_SteamAppIdBootstrap.h/.cpp` | In-memory Engine config layer lifecycle and boot result classification. |
| `Private/FU_SteamSocketsReadiness.h/.cpp` | SteamSockets module/socket subsystem readiness snapshot. |
| `Private/NetDriver/FU_OnlineSessionNetDriverLease.h/.cpp` | Process-wide, non-structural GameNetDriver lease and deferred restoration. |
| `Public/FU_OnlineDiagnosticTypes.h` | Additive Blueprint enums, event/field structs, and multicast delegate declaration. |
| `Private/Diagnostics/FU_OnlineSessionDiagnostics.h/.cpp` | Sanitization, history, reporting, file save, log/BP dispatch. |
| `Private/Diagnostics/SFU_OnlineDiagnosticOverlay.h/.cpp` | Asset-free Slate overlay and its testable row model. |
| `Public/FU_OnlineSessionSettings.h` / `Private/FU_OnlineSessionSettings.cpp` | Plugin-owned settings, validation, and restart semantics. |
| `Public/FU_OnlineSessionTypes.h` / `Private/FU_OnlineProviderStatusEvaluator.h` | Additive provider-status fields/codes and deterministic status evaluation. |
| `Public/FU_OnlineSessionSubsystem.h` / `Private/FU_OnlineSessionSubsystem.cpp` | Existing template session flow, operation state machine, watchdogs, diagnostics, and additive Blueprint APIs. |
| `Source/FUOnlineSessionEditor/Private/FUOnlineSessionLegacyConfigMigration.h/.cpp` | Pure marker analysis plus Win64 crash-aware legacy block migration. |
| `Source/FUOnlineSessionEditor/Private/FUOnlineSessionConfigManager.h/.cpp` | Thin editor entry that runs only legacy migration; no managed config generation. |
| `Source/FUOnlineSessionEditor/Private/FUOnlineSessionEditorModule.cpp` | One-time migration and restart-only Project Settings notifications. |
| `Private/Tests/FUOnlineSessionAutomationTests.cpp` and `FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp` | Runtime and editor behavior/contract tests. |
| `Config/DefaultEngine.ini`, `Plugins/FUOnlineSession/README.md` | Removal of historical FU block and end-user setup/diagnostic instructions. |

---

### Task 1: Lock the descriptor, plugin configuration, and settings contract

**Files:**

- Modify: `Plugins/FUOnlineSession/FUOnlineSession.uplugin`
- Create: `Plugins/FUOnlineSession/Config/Engine.ini`
- Create: `Plugins/FUOnlineSession/Config/DefaultFUOnlineSession.ini`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/FUOnlineSession.Build.cs`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSettings.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSettings.cpp`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp`

**Interfaces:**

- Consumes: UE 5.8 plugin modification layers and the existing `UFU_OnlineSessionSettings` config class.
- Produces: `UFU_OnlineSessionSettings::SteamDevAppId`, `ExpectedShippingSteamAppId`, `OperationTimeoutSeconds`, `DiagnosticHistoryLimit`, log/overlay settings; Runtime module `PreDefault` load phase; Win64-only descriptor contract.

- [ ] **Step 1: Write the failing descriptor/config contract tests**

  Name the break: “a package silently targets another platform, injects 480 in Shipping, or misses a required dependency.” Add tests that load the plugin descriptor and ini text from the project path, parse their values, and assert the hand-written expected values:

  ```cpp
  TestEqual(TEXT("Runtime loads before Default Steam consumers"), RuntimeModule.LoadingPhase, TEXT("PreDefault"));
  TestTrue(TEXT("Win64 only"), Descriptor.SupportedTargetPlatforms.Contains(TEXT("Win64")));
  TestFalse(TEXT("No static test AppID in Engine layer"), EngineIni.Contains(TEXT("SteamDevAppId=")));
  TestEqual(TEXT("Development AppID default"), Settings->SteamDevAppId, 480);
  TestEqual(TEXT("Shared timeout default"), Settings->OperationTimeoutSeconds, 30.0f);
  ```

- [ ] **Step 2: Run the focused editor test and verify RED**

  Run:

  ```powershell
  & 'F:\UNREAL\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\demo\EvilClownParadise\EvilClownParadise.uproject' -EnablePlugins=FUOnlineSession -unattended -NoSound -NullRHI '-ExecCmds=Automation RunTests FUOnlineSession.EditorConfig.Descriptor; Quit' -TestExit='Automation Test Queue Empty'
  ```

  Expected: the test fails because `EngineVersion`, platform restrictions, new settings, or plugin config files are absent.

- [ ] **Step 3: Implement only the descriptor/config/settings values required by the test**

  Set the descriptor fields exactly:

  ```json
  "EngineVersion": "5.8.0",
  "SupportedTargetPlatforms": ["Win64"],
  "LoadingPhase": "PreDefault",
  "PlatformAllowList": ["Win64"],
  "PlatformArchitectureDenyList": ["Win64:arm64"]
  ```

  Add `Slate`, `SlateCore`, `Sockets`, and private `SteamSockets` dependencies. Add the static Engine transport layer without `SteamDevAppId`:

  ```ini
  [OnlineSubsystem]
  DefaultPlatformService=Null

  [OnlineSubsystemSteam]
  bEnabled=true
  bUseSteamNetworking=false
  bAllowP2PPacketRelay=true
  ```

  Move settings to `Config=FUOnlineSession`, retain deprecated `bAutoConfigureProject`, retain `SteamDevAppId`, and append `ExpectedShippingSteamAppId`, `OperationTimeoutSeconds`, history limit, log/overlay enablement, severity, duration, and row-limit properties. Clamp timeout in a non-Blueprint validation helper to `5.0f..300.0f`.

- [ ] **Step 4: Re-run the focused test and then all existing editor configuration tests**

  Expected: descriptor/config contract passes; existing tests that intentionally describe the obsolete managed block may now fail and are updated only in Task 2.

- [ ] **Step 5: Commit the isolated configuration contract**

  ```powershell
  git add Plugins/FUOnlineSession/FUOnlineSession.uplugin Plugins/FUOnlineSession/Config Plugins/FUOnlineSession/Source/FUOnlineSession/FUOnlineSession.Build.cs Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSettings.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSettings.cpp Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp
  git commit -m "feat: add UE58 Win64 online session config contract"
  ```

### Task 2: Replace DefaultEngine.ini generation with crash-aware legacy migration

**Files:**

- Create: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/FUOnlineSessionLegacyConfigMigration.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/FUOnlineSessionLegacyConfigMigration.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/FUOnlineSessionConfigManager.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/FUOnlineSessionConfigManager.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/FUOnlineSessionEditorModule.cpp`
- Modify: `Config/DefaultEngine.ini`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp`

**Interfaces:**

- Consumes: marker bytes `; BEGIN FUONLINESESSION AUTO CONFIG` / `; END FUONLINESESSION AUTO CONFIG`.
- Produces:

  ```cpp
  enum class EFU_LegacyMigrationResult : uint8
  {
      NoManagedBlock, Published, PublishedWithWarning,
      InvalidMarkers, Failed, ManualRecoveryRequired
  };

  struct FFU_LegacyMigrationArtifacts
  {
      FString SavedBackupPath;
      FString RollbackBackupPath;
      FString TemporaryPath;
      FString Error;
  };

  struct FFU_LegacyTransformResult
  {
      EFU_LegacyMigrationResult Result;
      TArray<uint8> TransformedBytes;
      FString Error;
  };

  class FFU_LegacyConfigMigration final
  {
  public:
      static FFU_LegacyTransformResult Transform(const TArray<uint8>& OriginalBytes);
      static EFU_LegacyMigrationResult ClassifyReplaceOutcome(
          const FSHAHash& OriginalHash,
          const FSHAHash& TransformedHash,
          const TOptional<FSHAHash>& TargetHash);
  };
  ```

- [ ] **Step 1: Write failing pure transformation and publisher-classification tests**

  Name the breaks: “a malformed marker deletes arbitrary user config” and “a failed `ReplaceFileW` is incorrectly reported as unchanged.” Use literal UTF-8 BOM/CRLF bytes and a file-operation seam returning one of the three post-replace target byte fixtures:

  ```cpp
  TestEqual(TEXT("One marked block is removed"), Transform(ValidBytes).Result, EFU_LegacyMigrationResult::Published);
  TestEqual(TEXT("Broken marker never writes"), Transform(BeginOnlyBytes).Result, EFU_LegacyMigrationResult::InvalidMarkers);
  TestEqual(TEXT("False replace with original target"), Classify(OriginalHash), EFU_LegacyMigrationResult::Failed);
  TestEqual(TEXT("False replace with transformed target"), Classify(TransformedHash), EFU_LegacyMigrationResult::PublishedWithWarning);
  TestEqual(TEXT("False replace with unknown target"), Classify(UnknownHash), EFU_LegacyMigrationResult::ManualRecoveryRequired);
  ```

- [ ] **Step 2: Run the focused migration test and verify RED**

  Run `Automation RunTests FUOnlineSession.EditorConfig.LegacyMigration`. Expected: missing migration types or legacy writer behavior causes the named assertions to fail.

- [ ] **Step 3: Implement pure analysis first, then the Win64 publisher**

  The pure layer receives `TArray<uint8>`, finds exactly one ordered marker pair, removes precisely that byte range, and preserves the leading BOM and all remaining bytes. The publisher must:

  1. write a timestamped byte-exact Saved backup;
  2. write/flush/close a same-directory temp file;
  3. reread and validate transformed bytes;
  4. call `ReplaceFileW(Target, Temp, RollbackBackup, 0, nullptr, nullptr)`;
  5. on every return, hash the target and classify original/transformed/unknown;
  6. retain all artifacts and give paths when classification is unknown.

  `IFileManager::Move` is not a fallback for the publish operation. Keep file-operation injection private to the migration implementation so production classes do not gain test-only methods.

- [ ] **Step 4: Convert the editor module to migration-only behavior and remove the checked-in managed block**

  `StartupModule` invokes the migration once; settings changes call `SaveConfig()` and show “restart required”, never regenerate Engine config. Delete `BuildManagedConfigBlock` and `WriteManagedConfigBlock`, preserve private compatibility only if a current editor test needs a wrapper, and remove the legacy marked FU block from `Config/DefaultEngine.ini` through the same transformation fixture.

- [ ] **Step 5: Re-run migration/editor tests and commit**

  Expected: valid migration, invalid markers, all three `ReplaceFileW` outcomes, BOM/CRLF preservation, and restart notification behavior pass.

  ```powershell
  git add Config/DefaultEngine.ini Plugins/FUOnlineSession/Source/FUOnlineSessionEditor
  git commit -m "feat: migrate legacy online config safely"
  ```

### Task 3: Add deterministic AppID bootstrap and SteamSockets readiness checks

**Files:**

- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FUOnlineSessionModule.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FUOnlineSessionModule.cpp`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamAppIdBootstrap.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamAppIdBootstrap.cpp`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamSocketsReadiness.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamSocketsReadiness.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionTypes.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineProviderStatusEvaluator.h`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp`

**Interfaces:**

```cpp
enum class EFU_SteamBootstrapStatus : uint8
{
    Ready, ShippingNoInjection, InvalidDevelopmentAppId,
    SteamAlreadyInstantiated, BranchUnavailable, LayerAddFailed,
    LayerOwnershipConflict, EffectiveValueConflict, LayerRemoveFailed
};

struct FFU_SteamAppIdBootstrapInputs
{
    bool bShippingBuild = false;
    bool bSteamInstanceAlreadyExists = false;
    bool bEngineBranchAvailable = false;
    bool bLayerAdded = false;
    bool bOwnedTagResolvesToEngineBranch = false;
    int32 RequestedAppId = 0;
    int32 EffectiveAppId = 0;
};

struct FFU_SteamAppIdBootstrapTicket
{
    EFU_SteamBootstrapStatus Status = EFU_SteamBootstrapStatus::BranchUnavailable;
    FString SyntheticLayerFilename;
    bool bOwnsAddedLayer = false;
};

struct FFU_SteamSocketsReadiness
{
    bool bModuleAvailable = false;
    bool bModuleEnabled = false;
    bool bSocketSubsystemAvailable = false;
};
```

Append, never reorder, provider statuses for SteamSockets module unavailable/disabled/socket subsystem unavailable, AppID bootstrap invalid, and Shipping prerequisite missing.

- [ ] **Step 1: Write failing unit tests for decision boundaries**

  Name the breaks: “readback equals 480 even though the dynamic layer was never added” and “a loadable NetDriver class hides a disabled SteamSockets module.” Test the pure input/output decision helpers with hand-written snapshots:

  ```cpp
  TestEqual(TEXT("False Add is not bootstrap success"),
      FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, false, true, 480, 480 }),
      EFU_SteamBootstrapStatus::LayerAddFailed);
  TestEqual(TEXT("Disabled module blocks Steam"),
      FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, DisabledSocketsInputs),
      EFU_OnlineProviderStatusCode::SteamSocketsDisabled);
  ```

- [ ] **Step 2: Run the two focused runtime tests and verify RED**

  Run `Automation RunTests FUOnlineSession.AppIdBootstrap+FUOnlineSession.ProviderStatus.SteamSockets`. Expected: missing helper/status values fail at compile or assertion level.

- [ ] **Step 3: Implement bootstrap lifecycle and real SteamSockets probe**

  Define the public `LogFUOnlineSession` category in the Runtime module. Outside Shipping, use `FConfigBranch::AddDynamicLayerStringToHierarchy(SyntheticFilename, Contents, FuOwnershipTag, DynamicLayerPriority::ProjectPluginOverrides)` with a process-unique synthetic filename and FU tag; mark success only after branch/tag/readback checks. In Shipping, add no dynamic layer. `RemoveDynamicLayerFromHierarchy` accepts the filename rather than the tag, so shutdown removes only the exact stored synthetic filename owned by this module instance and checks the return value.

  Implement Steam readiness through `FModuleManager::LoadModulePtr<FSteamSocketsModule>(TEXT("SteamSockets"))`, then `IsSteamSocketsEnabled()`, plus `ISocketSubsystem::Get(STEAM_SOCKETS_SUBSYSTEM)`. Feed an injectable snapshot into the existing status evaluator. Use `IOnlineSubsystem::GetAppId()` after instantiation to compare development AppID outside Shipping or expected real AppID in Shipping; never emit the AppID as a secret, but include it in sanitized diagnostics as a numeric expectation.

- [ ] **Step 4: Re-run focused tests and verify the status evaluator mutation cases**

  Flip each readiness boolean in individual fixtures. Each flip must change `Ready` into its matching actionable status; LAN fixtures must remain `Ready` when SteamSockets is unavailable.

- [ ] **Step 5: Commit bootstrap/readiness work**

  ```powershell
  git add Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FUOnlineSessionModule.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FUOnlineSessionModule.cpp Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamAppIdBootstrap.* Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_SteamSocketsReadiness.* Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionTypes.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineProviderStatusEvaluator.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp
  git commit -m "feat: validate Steam bootstrap and SteamSockets readiness"
  ```

### Task 4: Build the structured diagnostics, Blueprint data, and Shipping overlay

**Files:**

- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineDiagnosticTypes.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Diagnostics/FU_OnlineSessionDiagnostics.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Diagnostics/FU_OnlineSessionDiagnostics.cpp`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Diagnostics/SFU_OnlineDiagnosticOverlay.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Diagnostics/SFU_OnlineDiagnosticOverlay.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp`

**Interfaces:**

```cpp
UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticSeverity : uint8 { Verbose, Info, Warning, Error };

UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticOperation : uint8
{
    Environment, CreateSession, FindSessions, JoinSession, DestroySession,
    NetDriverLease, AppIdBootstrap, LegacyConfigMigration, Recovery
};

UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticPhase : uint8
{
    Requested, Preflight, Submitted, Callback, Timeout, Recovery, Completed
};

USTRUCT(BlueprintType)
struct FFU_OnlineDiagnosticField
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FName Key;
    UPROPERTY(BlueprintReadOnly) FString Value;
};

USTRUCT(BlueprintType)
struct FFU_OnlineDiagnosticEvent
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FDateTime TimestampUtc;
    UPROPERTY(BlueprintReadOnly) FGuid OperationId;
    UPROPERTY(BlueprintReadOnly) EFU_OnlineProvider Provider = EFU_OnlineProvider::Lan;
    UPROPERTY(BlueprintReadOnly) EFU_OnlineDiagnosticOperation Operation = EFU_OnlineDiagnosticOperation::Environment;
    UPROPERTY(BlueprintReadOnly) EFU_OnlineDiagnosticPhase Phase = EFU_OnlineDiagnosticPhase::Requested;
    UPROPERTY(BlueprintReadOnly) EFU_OnlineDiagnosticSeverity Severity = EFU_OnlineDiagnosticSeverity::Info;
    UPROPERTY(BlueprintReadOnly) FString Code;
    UPROPERTY(BlueprintReadOnly) FString Status;
    UPROPERTY(BlueprintReadOnly) FString WorldName;
    UPROPERTY(BlueprintReadOnly) int32 PIEInstanceId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) FString Message;
    UPROPERTY(BlueprintReadOnly) FString Cause;
    UPROPERTY(BlueprintReadOnly) FString RecommendedAction;
    UPROPERTY(BlueprintReadOnly) TArray<FFU_OnlineDiagnosticField> Fields;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFU_OnOnlineDiagnosticEvent, const FFU_OnlineDiagnosticEvent&, Event);

struct FFU_OnlineDiagnosticDispatchConfig
{
    int32 HistoryLimit = 200;
    bool bEmitToLog = true;
    bool bEnableOverlay = true;
    EFU_OnlineDiagnosticSeverity MinimumOverlaySeverity = EFU_OnlineDiagnosticSeverity::Warning;
    float OverlayDurationSeconds = 8.0f;
    int32 OverlayRowLimit = 6;
};

struct FFU_OnlineDiagnosticOverlayRow
{
    FDateTime ExpiresAtUtc;
    EFU_OnlineDiagnosticSeverity Severity = EFU_OnlineDiagnosticSeverity::Info;
    FString Text;
};

class FFU_OnlineSessionDiagnostics final
{
public:
    explicit FFU_OnlineSessionDiagnostics(
        const FFU_OnlineDiagnosticDispatchConfig& InConfig,
        TFunction<void(const FFU_OnlineDiagnosticEvent&)> InBlueprintBroadcast);
    FFU_OnlineDiagnosticEvent Emit(const FFU_OnlineDiagnosticEvent& Candidate);
    TArray<FFU_OnlineDiagnosticEvent> GetHistory() const;
    void ClearHistory();
    FString BuildReport() const;
    bool SaveReport(FString& OutSavedPath, FString& OutError) const;
    static FFU_OnlineDiagnosticEvent Sanitize(const FFU_OnlineDiagnosticEvent& Candidate);
};

class FFU_OnlineDiagnosticOverlayModel final
{
public:
    void Add(const FFU_OnlineDiagnosticEvent& Event, const FFU_OnlineDiagnosticDispatchConfig& Config, FDateTime NowUtc);
    TArray<FFU_OnlineDiagnosticOverlayRow> GetVisibleRows(FDateTime NowUtc) const;
};

TArray<FFU_OnlineDiagnosticEvent> GetDiagnosticHistory() const;
void ClearDiagnosticHistory();
FString BuildDiagnosticReport() const;
bool SaveDiagnosticReport(FString& OutSavedPath, FString& OutError);
```

- [ ] **Step 1: Write a failing diagnostic-history/redaction test**

  Name the break: “an error contains a password in Blueprint history” and “history grows unbounded.” Construct literal events and assert both behavior outcomes:

  ```cpp
  Dispatcher.Emit(MakeEvent(TEXT("RoomPassword"), TEXT("secret")));
  TestEqual(TEXT("Sensitive values are redacted"), Dispatcher.GetHistory()[0].Fields[0].Value, TEXT("<redacted>"));
  TestEqual(TEXT("Oldest event rolls out"), Dispatcher.GetHistory(2).Num(), 2);
  ```

- [ ] **Step 2: Run the focused diagnostics test and verify RED**

  Run `Automation RunTests FUOnlineSession.Diagnostics.History`. Expected: missing dispatcher/types or an unbounded/raw-value result.

- [ ] **Step 3: Implement the one-way dispatch path and reporting contract**

  The dispatcher must sanitize before history, append bounded history, call `UE_LOG` only if enabled, feed overlay only when enabled/severity qualifies, and always broadcast to Blueprint. `SaveDiagnosticReport` clears outputs first, saves only below `Saved/Logs/FUOnlineSession`, and returns a sanitized failure without recursively emitting another save attempt.

  Add `OnOnlineDiagnosticEvent`, `GetDiagnosticHistory`, `ClearDiagnosticHistory`, `BuildDiagnosticReport`, and `SaveDiagnosticReport` as additive Blueprint members. Do not expose an arbitrary file path.

- [ ] **Step 4: Write and pass overlay-model tests, then attach the Slate widget**

  Name the break: “Shipping overlay depends on debug-screen APIs.” Test a real overlay row model with injected time: Warning creates a row, Info is filtered by default, expired rows disappear, and no viewport preserves history. Then attach `SFU_OnlineDiagnosticOverlay` through `UGameViewportClient::AddViewportWidgetContent` and remove the exact shared widget at teardown. Do not call `PrintString` or `GEngine->AddOnScreenDebugMessage`.

- [ ] **Step 5: Re-run diagnostics tests and commit**

  ```powershell
  git add Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineDiagnosticTypes.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Diagnostics Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp
  git commit -m "feat: add Blueprint online diagnostics and overlay"
  ```

### Task 5: Replace ad-hoc GameNetDriver mutation with a process-wide lease

**Files:**

- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/NetDriver/FU_OnlineSessionNetDriverLease.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/NetDriver/FU_OnlineSessionNetDriverLease.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineProviderStatusEvaluator.h`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp`

**Interfaces:**

```cpp
enum class EFU_NetDriverLeaseResult : uint8
{
    Acquired, AlreadyOwned, GameNetDriverMissing, GameNetDriverDuplicate,
    ActiveOrPendingDriver, OwnedByAnotherGameInstance, ProviderSwitchBlocked,
    ExternallyModified, NotGameThread
};

struct FFU_NetDriverDefinitionValue
{
    FName DefName;
    FString DriverClassName;
    FString DriverClassNameFallback;
    int32 MaxChannelsOverride = 0;
    bool bRunParallelConnectionTick = false;
};

struct FFU_NetDriverLeaseFingerprint
{
    const FNetDriverDefinition* DefinitionPointer = nullptr;
    int32 DefinitionIndex = INDEX_NONE;
    int32 DefinitionArrayNum = 0;
    TArray<FName> OrderedDefinitionNames;
    FFU_NetDriverDefinitionValue OriginalValue;
    FFU_NetDriverDefinitionValue InstalledValue;
};

struct FFU_NetDriverLeasePreflight
{
    bool bIsGameThread = false;
    int32 GameNetDriverDefinitionCount = 0;
    bool bHasLiveTargetNamedDriver = false;
    bool bHasPendingNetGame = false;
    bool bLeaseExists = false;
    bool bSameOwner = false;
    bool bSameProvider = false;
    bool bInstalledValueStillMatches = false;
};

class FFU_OnlineSessionNetDriverLease final
{
public:
    static EFU_NetDriverLeaseResult EvaluateAcquire(const FFU_NetDriverLeasePreflight& Snapshot);
    static EFU_NetDriverLeaseResult EvaluateRestore(
        const FFU_NetDriverLeaseFingerprint& Expected,
        const FFU_NetDriverLeaseFingerprint& Current,
        bool bAllWorldsClear);
    static EFU_NetDriverLeaseResult Acquire(UGameInstance& Owner, EFU_OnlineProvider Provider, FName DriverClass);
    static void RequestRelease(UGameInstance* Owner, const TCHAR* Reason);
    static void TickDeferredRelease();
    static void ShutdownModule();
};
```

- [ ] **Step 1: Write failing lease snapshot tests**

  Name the breaks: “a live `FNamedNetDriver` is mutated during travel” and “a third-party change is restored over.” Build literal `FNetDriverDefinition` fixtures and a usage snapshot. Assert:

  ```cpp
  TestEqual(TEXT("Any live target blocks first acquire"), EvaluateAcquire(OneGameDriverUse), EFU_NetDriverLeaseResult::ActiveOrPendingDriver);
  TestEqual(TEXT("Pending game with null driver blocks first acquire"), EvaluateAcquire(OnePendingGame), EFU_NetDriverLeaseResult::ActiveOrPendingDriver);
  TestTrue(TEXT("Only driver fields change"), Installed.MaxChannelsOverride == 77 && Installed.bRunParallelConnectionTick);
  TestEqual(TEXT("Fingerprint mismatch poisons"), EvaluateRestore(ChangedOrder), EFU_NetDriverLeaseResult::ExternallyModified);
  ```

- [ ] **Step 2: Run the focused lease tests and verify RED**

  Run `Automation RunTests FUOnlineSession.NetDriverLease`. Expected: no coordinator exists or current `FU_PrepareGameNetDriver` permits the unsafe mutation.

- [ ] **Step 3: Implement the coordinator with only in-place assignment**

  On a first acquire require exactly one `GameNetDriver`; scan every `GEngine->GetWorldContexts()` entry. Reject an `FNamedNetDriver` whose `NetDriverDef` equals the target or whose `UNetDriver::GetNetDriverDefinition()` is `GameNetDriver`; reject each non-null `PendingNetGame`, including a null pending driver. Snapshot target pointer/index, `Num`, ordered DefName fingerprint, all five definition fields, owner GameInstance, and installed value. Set only primary/fallback driver names in an in-place copy.

  Restore only after all-world scan is clear and pointer/index/Num/fingerprint/value invariants match. Any mismatch becomes poisoned for the process. A CoreTicker retry handles deferred release. `ShutdownModule` unregisters ticker first, then attempts a final safe restoration only.

- [ ] **Step 4: Replace `FU_PrepareGameNetDriver` with the lease API and pass focused tests**

  Keep provider class selection in traits. Map lease failure to the existing/additive readiness diagnostics, set `PreparedNetDriverProvider` only on `Acquired`/`AlreadyOwned`, and request release from synchronous rejection, Create/Join failure, Destroy completion, network/travel failure, recovery completion, and subsystem deinitialization.

- [ ] **Step 5: Commit the lease**

  ```powershell
  git add Plugins/FUOnlineSession/Source/FUOnlineSession/Private/NetDriver Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineProviderStatusEvaluator.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp
  git commit -m "feat: guard GameNetDriver with a process lease"
  ```

### Task 6: Add a testable operation gate, watchdog, and conservative recovery

**Files:**

- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineOperationStateMachine.h`
- Create: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineOperationStateMachine.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp`

**Interfaces:**

```cpp
enum class EFU_OperationKind : uint8 { Create, Find, Join, Destroy };
enum class EFU_OperationPhase : uint8 { Idle, Submitted, Recovering };

enum class EFU_OperationAction : uint8
{
    None, BroadcastFailure, ClearTimerAndDelegates, KeepOriginalDelegate,
    StartFindCancellation, StartRecoveryDestroy, RequestLeaseRelease
};

struct FFU_OperationTicket
{
    uint64 AttemptSequence = 0;
    uint64 Generation = 0;
    FGuid OperationId;
    EFU_OperationKind Kind = EFU_OperationKind::Create;
    bool bAccepted = false;
};

struct FFU_OperationState
{
    uint64 AttemptSequence = 0;
    uint64 ActiveGeneration = 0;
    FGuid ActiveOperationId;
    EFU_OperationKind ActiveKind = EFU_OperationKind::Create;
    EFU_OperationPhase Phase = EFU_OperationPhase::Idle;
    bool bCompletionBroadcast = false;
    bool bAwaitingOriginalCompletion = false;
    bool bFindCancellationOutstanding = false;
    uint8 RecoveryDestroyAttempts = 0;
};

class FFU_OnlineOperationStateMachine final
{
public:
    FFU_OperationTicket BeginAcceptedAttempt(EFU_OperationKind Kind);
    FFU_OperationTicket RecordRejectedAttempt(EFU_OperationKind Kind);
    EFU_OperationAction HandleSynchronousReject(uint64 Generation);
    EFU_OperationAction HandleTimeout(uint64 Generation);
    EFU_OperationAction HandleOriginalCompletion(uint64 Generation, bool bSucceeded, bool bSessionStillExists);
    bool CanFinishRecovery(bool bOriginalCallbackSeen, bool bNoSession) const;
    bool CanStartExplicitRecovery(bool bOriginalCallbackSeen, bool bNoSession, bool bNoLiveOrPendingDriver, bool bLeaseReleased) const;
    const FFU_OperationState& Get() const;
};
```

- [ ] **Step 1: Write failing state-machine tests**

  Name the breaks: “a rejected second call invalidates an active callback” and “`NoSession` after timeout falsely cancels an in-flight Create.” Use the pure state machine:

  ```cpp
  const auto First = Machine.BeginAcceptedAttempt(EFU_OperationKind::Create);
  Machine.RecordRejectedAttempt(EFU_OperationKind::Create);
  TestEqual(TEXT("Busy reject preserves active generation"), Machine.Get().ActiveGeneration, First.Generation);
  Machine.Timeout(First.Generation);
  TestFalse(TEXT("NoSession cannot end original Create"), Machine.CanFinishRecovery(false /* callback seen */, true /* no session */));
  ```

- [ ] **Step 2: Run `FUOnlineSession.OperationStateMachine` and verify RED**

  Expected: the helper does not exist or current subsystem behavior permits the invalid transition.

- [ ] **Step 3: Implement the pure gate before touching OnlineSubsystem delegates**

  `AttemptSequence`/`OperationId` are allocated at every public entry. `ActiveGeneration` changes only after preflight succeeds and a delegate is about to bind. A Busy rejection never touches the active timer, delegate, generation, pending data, or completion flag. The helper returns explicit actions for synchronous reject, normal callback, timeout, Find cancellation, and recovery eligibility.

- [ ] **Step 4: Integrate generation-captured delegates and timers into templated session flows**

  Bind Create/Find/Join/Destroy delegates and watchdogs with the active generation captured as an extra payload. Arm the timer before calling OSS. If the call returns `false` for the same generation, clear delegate/timer, clean pending state, request lease release, emit one rejection event, and broadcast one existing failure result.

  On Find timeout bind cancellation completion, call `CancelFindSessions`, and clear both Find/cancel handles only after original completion or successful cancellation. On Create/Join/Destroy timeout set `bAwaitingOriginalCompletion`; do not clear the original delegate and do not let `NoSession` alone reach Idle. Late Create/Join success never travels and starts a single recovery Destroy. Late Destroy failure remains Recovering and allows exactly one controlled recovery Destroy per explicit `TryRecoverProvider` call after the original callback is terminal.

- [ ] **Step 5: Add the additive Blueprint recovery entry and pass focused tests**

  Add and implement:

  ```cpp
  UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
  bool TryRecoverProvider(EFU_OnlineProvider Provider);
  ```

  It must return true only after original completion, no session, no active/pending driver, and lease release are safe. It emits a diagnostic for every decision and cannot force-cancel a non-cancelable OSS operation.

- [ ] **Step 6: Commit the operation lifecycle change**

  ```powershell
  git add Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineOperationStateMachine.* Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp
  git commit -m "feat: recover timed out online session operations safely"
  ```

### Task 7: Route all template operation outcomes through diagnostics and Blueprint APIs

**Files:**

- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp`
- Modify: `Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionTypes.h`
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp`

**Interfaces:**

```cpp
UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
FFU_OnlineProviderStatus RunProviderDiagnostics(EFU_OnlineProvider Provider);

UPROPERTY(BlueprintAssignable, Category="FU Online Session|Diagnostics")
FFU_OnOnlineDiagnosticEvent OnOnlineDiagnosticEvent;
```

- [ ] **Step 1: Write a failing operation-to-diagnostic correlation test**

  Name the break: “preflight rejects a Steam create without Blueprint-visible reason.” Exercise the real template gate through a test seam containing no subsystem and assert one history event whose operation is Create, provider is Steam, code is `FU.Provider.SubsystemUnavailable`, and `OperationId` is nonzero before the existing failure delegate fires.

- [ ] **Step 2: Run the focused test and verify RED**

  Run `Automation RunTests FUOnlineSession.Diagnostics.OperationCorrelation`. Expected: current early return only logs/broadcasts the legacy result and has no diagnostic event.

- [ ] **Step 3: Add a templated `FU_EmitDiagnostic<Provider>` boundary and use it on every path**

  Replace ad-hoc `LogTemp`/private raw logs in Create, Find, Join, Destroy, preflight, delegate cleanup, ClientTravel, NetworkFailure, TravelFailure, watchdog, and lease paths. Preserve all existing Blueprint completion delegates and call order: emit event first, then existing failure/success result. Pass room name only where useful; never pass password or connection tokens.

- [ ] **Step 4: Implement `RunProviderDiagnostics` and verify Blueprint-history semantics**

  It calls the existing status check synchronously, builds one environment event with the same `OperationId` allocation rules, and returns the status unchanged. Add tests that `ClearDiagnosticHistory` does not alter the active operation, and report-save failure returns sanitized output without adding a recursive save attempt.

- [ ] **Step 5: Run all Runtime Automation tests and commit**

  ```powershell
  git add Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionSubsystem.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp Plugins/FUOnlineSession/Source/FUOnlineSession/Public/FU_OnlineSessionTypes.h Plugins/FUOnlineSession/Source/FUOnlineSession/Private/Tests/FUOnlineSessionAutomationTests.cpp
  git commit -m "feat: correlate session outcomes with Blueprint diagnostics"
  ```

### Task 8: Document the installed-plugin experience and clean configuration artifacts

**Files:**

- Modify: `Plugins/FUOnlineSession/README.md`
- Modify: `Plugins/FUOnlineSession/FUOnlineSession.uplugin` only if Task 1 tests expose description/version documentation gaps
- Test: `Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp`

**Interfaces:**

- Consumes: all public Blueprint and settings contracts from Tasks 1–7.
- Produces: user-facing activation, provider flow, diagnostics, and Shipping prerequisite documentation.

- [ ] **Step 1: Write a failing documentation-adjacent behavior test only for a runtime contract**

  Name the break: “enabling the plugin still requires a managed DefaultEngine block.” Execute the editor migration/config validation against a temporary project config with no markers and assert `NoManagedBlock`, not a write. Do not grep README prose.

- [ ] **Step 2: Run the focused test and verify RED if a writer remains**

  Expected: it fails only if legacy `EnsureProjectConfiguration` still writes a block; otherwise record the test as already green because Task 2 implemented the behavior and do not invent a redundant test.

- [ ] **Step 3: Rewrite README from actual public behavior**

  Include: enable only FUOnlineSession and its declared dependencies; choose Steam or LAN Blueprint entry points from the same package; plugin enable/disable/restart config behavior; `SteamDevAppId=480` development use; `ExpectedShippingSteamAppId` validation; Steam launch/subscription and `UE_PROJECT_STEAMSHIPPINGID` when launching outside Steam; SteamSockets compatibility; `OnOnlineDiagnosticEvent`, history/report save; and room-password limitation.

- [ ] **Step 4: Run editor tests and commit documentation**

  ```powershell
  git add Plugins/FUOnlineSession/README.md Plugins/FUOnlineSession/Source/FUOnlineSessionEditor/Private/Tests/FUOnlineSessionEditorAutomationTests.cpp
  git commit -m "docs: explain FU online session setup and diagnostics"
  ```

### Task 9: Run compilation, Automation, package, and two-instance verification

**Files:**

- Modify only when a failing verification identifies a defect; add a targeted failing Automation test before any fix.
- Inspect: `Saved/Logs`, staged plugin/package receipts, generated plugin config cache, and git status.

**Interfaces:**

- Consumes: all completed Tasks 1–8.
- Produces: recorded evidence for each locally runnable matrix row and an explicit external Steam acceptance gap if accounts are unavailable.

- [ ] **Step 1: Build the Editor target**

  ```powershell
  & 'F:\UNREAL\UE_5.8\Engine\Build\BatchFiles\Build.bat' EvilClownParadiseEditor Win64 Development '-Project=D:\demo\EvilClownParadise\EvilClownParadise.uproject' -WaitMutex -EnablePlugins=FUOnlineSession
  ```

  Expected: Runtime, Editor, and Automation test translation units compile with no new warnings/errors.

- [ ] **Step 2: Run the full focused Automation suite**

  ```powershell
  & 'F:\UNREAL\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\demo\EvilClownParadise\EvilClownParadise.uproject' -EnablePlugins=FUOnlineSession -unattended -NoSound -NullRHI '-ExecCmds=Automation RunTests FUOnlineSession; Quit' -TestExit='Automation Test Queue Empty'
  ```

  Expected: zero failed FUOnlineSession tests. For a failure, write/reproduce a focused test first, then fix minimally.

- [ ] **Step 3: Build a distributable plugin and inspect its contents**

  ```powershell
  & 'F:\UNREAL\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat' BuildPlugin '-Plugin=D:\demo\EvilClownParadise\Plugins\FUOnlineSession\FUOnlineSession.uplugin' '-Package=D:\demo\EvilClownParadise\Saved\PluginPackage\FUOnlineSession' -TargetPlatforms=Win64
  ```

  Expected: package contains Win64 Runtime/Editor outputs and both plugin ini files, with no unsupported platform claim.

- [ ] **Step 4: Build/cook/stage Development and Shipping Win64 packages**

  ```powershell
  & 'F:\UNREAL\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat' BuildCookRun '-project=D:\demo\EvilClownParadise\EvilClownParadise.uproject' -noP4 -platform=Win64 -clientconfig=Development -build -cook -stage -pak -archive -EnablePlugins=FUOnlineSession
  & 'F:\UNREAL\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat' BuildCookRun '-project=D:\demo\EvilClownParadise\EvilClownParadise.uproject' -noP4 -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -archive -EnablePlugins=FUOnlineSession
  ```

  Expected: staged receipts include OnlineSubsystem, Null, Steam, SteamSockets, Steam redistributables, FU plugin binaries, and plugin config cache.

- [ ] **Step 5: Execute local manual matrix and record limits**

  Launch two Development then two Shipping instances for NULL host/find/join/travel/destroy/provider switch. In both configurations trigger one warning/error to verify overlay, Blueprint history, and report save. In a valid Steam environment verify `GetAppId`, SteamSockets readiness, lobby create/find/join/travel/destroy. If two distinct logged-in Steam accounts or the real Shipping AppID are unavailable, record Steam E2E as an explicit unrun external acceptance item rather than claiming it passed.

- [ ] **Step 6: Final review and focused commit**

  Run `git diff --check`, inspect `git status --short`, confirm the three user-owned Widget assets and unrelated staged nested Build.cs were not staged/modified by this task, and commit only FUOnlineSession, Config, docs, and relevant `.gitignore` paths.
