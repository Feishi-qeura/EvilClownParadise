# Networked Pickup Inventory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a server-authoritative four-state pickup system with camera inspection, equipment, physics throwing, temporary inventory, deterministic same-frame contention, and ragdoll/death drops.

**Architecture:** `AECPPickupItem` owns and replicates the authoritative item state while `AECPPlayerBase` validates player intent and owns the temporary inventory index. `AECPGameMode` assigns stable join priority, and `AECPPlayerController` supplies local input/presentation without trusting client targets, force, or final transforms.

**Tech Stack:** Unreal Engine 5.8 C++, replicated actors and RPCs, Enhanced Input plus fixed `FKey` bindings, Character Movement, Chaos physics, Unreal Automation Tests.

**Spec:** `docs/superpowers/specs/2026-09-21-networked-pickup-inventory-design.md`

## Global Constraints

- The server is authoritative for targets, transitions, ownership, rotation limits, throw direction, force, inventory, and drops.
- States are exactly `World`, `Pickup`, `Equipped`, and `Stored`; inventory is unlimited, in-match only, and has no UI.
- Listen Server host wins same-server-frame contention; other same-frame requests use ascending server join order; cross-frame winners cannot be preempted.
- Entering ragdoll is the temporary death/drop trigger; formal `OnDied` calls the same idempotent release path.
- Existing `F` Enhanced Input remains; `2`, `G`, `V`, left mouse, and right mouse use direct controller bindings.
- Every new complex branch, network boundary, null guard, state transition, and Oasis/Unreal API call receives an intent/reason comment above it.
- Preserve all existing uncommitted Blueprint, map, config, plugin, and source changes. Do not commit overlapping dirty source files as if they were created by this task.

## Review Focus

- Repeated or stale transition requests must not change an item after its revision/state/owner has moved on; Task 1 pins transition legality and Task 3 pins owner checks.
- Duplicate same-frame requests and invalid requesters must not create two winners; Tasks 1 and 3 test stable ordering and queue de-duplication.
- Missing camera, root primitive, or equipment socket must leave the previous state intact; Task 3 adds actor-state rollback tests.
- Maliciously large rotation deltas must be clamped and non-owners ignored; Tasks 1 and 4 test both cases.
- Ragdoll plus death or destruction in the same frame must drop each item once; Task 4 adds an idempotence test and Task 6 verifies PIE behavior.

---

### Task 1: Pure pickup state and arbitration rules

**Files:**
- Create: `Source/EvilClownParadise/Public/Actors/ECPPickupTypes.h`
- Create: `Source/EvilClownParadise/Private/Actors/ECPPickupTypes.cpp`
- Create: `Source/EvilClownParadise/Private/Tests/ECPPickupTypesTests.cpp`

**Interfaces:**
- Consumes: Unreal `FMath`, `FRotator`, and Automation Test framework.
- Produces: `EECPPickupState`, `FECPPickupPriority`, `ECPPickupRules::CanTransition`, `ECPPickupRules::IsHigherPriority`, `ECPPickupRules::ClampRotationDelta`, and `ECPPickupRules::ExpectedVelocityDelta`.

- [ ] **Step 1: Write failing rule tests**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupTransitionTest, "ECP.Pickup.Rules.Transitions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupPriorityTest, "ECP.Pickup.Rules.Priority", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupRotationClampTest, "ECP.Pickup.Rules.RotationClamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupMassResponseTest, "ECP.Pickup.Rules.MassResponse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

The transition test asserts only the seven design transitions. The priority test asserts host over remote, lower join order over higher, and lower arrival order as a final stable tie-breaker. Rotation asserts every axis is within `MaxDegrees`. Mass response asserts `ExpectedVelocityDelta(Impulse, 10)` is lower than for mass `1`.

- [ ] **Step 2: Build once and confirm tests fail to compile because the rule API does not exist**

Run the project Editor target through the UE 5.8 `Build.bat` associated with `EvilClownParadise.uproject`.

Expected: compiler errors naming `EECPPickupState` and `ECPPickupRules`.

- [ ] **Step 3: Add the minimal public rule API with reason comments**

```cpp
UENUM(BlueprintType)
enum class EECPPickupState : uint8 { World, Pickup, Equipped, Stored };

struct FECPPickupPriority
{
    bool bListenServerHost = false;
    int32 JoinOrder = MAX_int32;
    uint32 ArrivalOrder = MAX_uint32;
};

namespace ECPPickupRules
{
    EVILCLOWNPARADISE_API bool CanTransition(EECPPickupState From, EECPPickupState To);
    EVILCLOWNPARADISE_API bool IsHigherPriority(const FECPPickupPriority& Left, const FECPPickupPriority& Right);
    EVILCLOWNPARADISE_API FRotator ClampRotationDelta(const FRotator& Delta, float MaxDegrees);
    EVILCLOWNPARADISE_API float ExpectedVelocityDelta(float ImpulseStrength, float MassKg);
}
```

- [ ] **Step 4: Run `ECP.Pickup.Rules` and confirm all four tests pass**

Run `UnrealEditor-Cmd.exe EvilClownParadise.uproject -unattended -nop4 -NullRHI -ExecCmds="Automation RunTests ECP.Pickup.Rules; Quit" -TestExit="Automation Test Queue Empty" -log` from the resolved UE 5.8 installation.

Expected: four passing tests and process exit code `0`.

- [ ] **Step 5: Record a clean task checkpoint without absorbing unrelated index entries**

Run `git diff --check -- Source/EvilClownParadise/Public/Actors/ECPPickupTypes.h Source/EvilClownParadise/Private/Actors/ECPPickupTypes.cpp Source/EvilClownParadise/Private/Tests/ECPPickupTypesTests.cpp` and retain the diff for final review. Commit only these newly created paths if the existing staged index can remain untouched.

### Task 2: Stable server join priority

**Files:**
- Modify: `Source/EvilClownParadise/Public/Data/ECPGameMode.h`
- Modify: `Source/EvilClownParadise/Private/Data/ECPGameMode.cpp`
- Modify: `Source/EvilClownParadise/Public/Data/ECPPlayerController.h`
- Modify: `Source/EvilClownParadise/Private/Data/ECPPlayerController.cpp`

**Interfaces:**
- Consumes: `AGameModeBase::PostLogin`, `APlayerController::IsLocalController`, and net mode.
- Produces: `AECPPlayerController::SetPickupJoinOrder`, `GetPickupJoinOrder`, `IsPickupListenServerHost`, and `GetPickupPriority(uint32 ArrivalOrder)`.

- [ ] **Step 1: Extend priority tests with controller-independent host/dedicated cases**

```cpp
TestTrue(TEXT("Listen host wins"), ECPPickupRules::IsHigherPriority({true, 99, 9}, {false, 0, 0}));
TestTrue(TEXT("Dedicated uses join order"), ECPPickupRules::IsHigherPriority({false, 1, 9}, {false, 2, 0}));
```

- [ ] **Step 2: Add controller priority storage**

```cpp
void SetPickupJoinOrder(int32 InJoinOrder, bool bInListenServerHost);
int32 GetPickupJoinOrder() const { return PickupJoinOrder; }
bool IsPickupListenServerHost() const { return bPickupListenServerHost; }
FECPPickupPriority GetPickupPriority(uint32 ArrivalOrder) const;
```

Keep these values server-only: clients never arbitrate and do not need another replicated field.

- [ ] **Step 3: Assign values in `PostLogin`**

```cpp
void AECPGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);
    if (AECPPlayerController* ECPController = Cast<AECPPlayerController>(NewPlayer))
    {
        const bool bHost = GetNetMode() == NM_ListenServer && ECPController->IsLocalController();
        ECPController->SetPickupJoinOrder(NextPickupJoinOrder++, bHost);
    }
}
```

- [ ] **Step 4: Rebuild and rerun `ECP.Pickup.Rules.Priority`**

Expected: compile succeeds and both Listen Server and dedicated-server ordering assertions pass.

- [ ] **Step 5: Check the focused diff**

Run `git diff --check -- Source/EvilClownParadise/Public/Data/ECPGameMode.h Source/EvilClownParadise/Private/Data/ECPGameMode.cpp Source/EvilClownParadise/Public/Data/ECPPlayerController.h Source/EvilClownParadise/Private/Data/ECPPlayerController.cpp`.

### Task 3: Item-owned replicated four-state machine

**Files:**
- Modify: `Source/EvilClownParadise/Public/Actors/ECPPickupItem.h`
- Modify: `Source/EvilClownParadise/Private/Actors/ECPPickupItem.cpp`
- Create: `Source/EvilClownParadise/Private/Tests/ECPPickupItemStateTests.cpp`

**Interfaces:**
- Consumes: Task 1 rule API and Task 2 controller priority.
- Produces: `QueuePickupRequest`, `ResolvePickupRequests`, `TryEnterPickup`, `TryEquip`, `TryStore`, `TryDrop`, `ApplyInspectionRotation`, `DropFromInventory`, `IsOwnedBy`, and `GetPickupState`.

- [ ] **Step 1: Add failing state/queue tests**

Create automation coverage that spawns a test world, `AECPPickupItem`, and lightweight player/controller instances. Assert duplicate requester entries collapse to one; an invalid/destroyed requester is skipped; a non-owner cannot transition; missing camera/socket leaves state and revision unchanged; and an item can produce only one winner.

```cpp
TestEqual(TEXT("Initial state"), Item->GetPickupState(), EECPPickupState::World);
TestFalse(TEXT("Non-owner cannot store"), Item->TryStore(OtherPlayer));
TestEqual(TEXT("Rejected transition keeps revision"), Item->GetStateRevision(), InitialRevision);
```

- [ ] **Step 2: Replace `bHeld` with the replicated state enum and explicit holder**

```cpp
USTRUCT()
struct FECPPickupNetState
{
    GENERATED_BODY()
    UPROPERTY() uint32 Revision = 0;
    UPROPERTY() EECPPickupState State = EECPPickupState::World;
    UPROPERTY() TObjectPtr<AECPPlayerBase> Holder = nullptr;
    UPROPERTY() FRotator InspectionRotation = FRotator::ZeroRotator;
    UPROPERTY() FTransform ReleaseTransform = FTransform::Identity;
    UPROPERTY() FVector ReleaseVelocity = FVector::ZeroVector;
};
```

Keep focus fields separate so highlighting cannot confer ownership. Preserve revision-aware retry behavior already present in `ApplyAttachmentState`.

- [ ] **Step 3: Implement same-frame request collection**

`QueuePickupRequest` validates authority and `World`, de-duplicates the player weak pointer, records a monotonically increasing arrival number, and schedules exactly one `SetTimerForNextTick`. `ResolvePickupRequests` revalidates every player through `Player->CanServerPickUpItem(this)`, sorts by `GetPickupPriority`, transitions only the winner, clears the queue, and never preempts a non-`World` item.

- [ ] **Step 4: Implement state presentation transactionally**

Before changing the net state, resolve the required camera/root/socket. On success increment `Revision`, update owner/relevancy/dormancy, and invoke the RepNotify locally. `Pickup` attaches to camera with `InspectionOffset`; `Equipped` attaches to `HitScanSocket2`; `Stored` hides and disables every primitive; `World` detaches, restores cached collision/replication defaults, wakes physics, and applies release velocity only for a real held-to-world transition.

- [ ] **Step 5: Implement clamped owner-only inspection rotation and throw/drop**

`ApplyInspectionRotation` rejects non-authority, non-owner, and non-`Pickup`; clamp with Task 1. `TryDrop` uses the server-provided release transform and optional impulse, calls `AddImpulse` exactly once on the root primitive, and does not divide by mass manually.

- [ ] **Step 6: Run item tests and rule tests**

Expected: rejected transitions retain revision; one winner; duplicate and destroyed candidates do not win; missing attachment dependencies do not mutate; all rule tests remain green.

- [ ] **Step 7: Check the focused diff**

Run `git diff --check -- Source/EvilClownParadise/Public/Actors/ECPPickupItem.h Source/EvilClownParadise/Private/Actors/ECPPickupItem.cpp Source/EvilClownParadise/Private/Tests/ECPPickupItemStateTests.cpp`.

### Task 4: Player intent, inventory, movement lock, and drop lifecycle

**Files:**
- Modify: `Source/EvilClownParadise/Public/Characters/ECPPlayerBase.h`
- Modify: `Source/EvilClownParadise/Private/Characters/ECPPlayerBase.cpp`
- Create: `Source/EvilClownParadise/Private/Tests/ECPPickupPlayerTests.cpp`

**Interfaces:**
- Consumes: Task 3 item transitions.
- Produces: `CanServerPickUpItem`, `CompleteQueuedPickup`, `RequestEquipPickup`, `RequestStorePickup`, `RequestDropPickup`, `RequestRotatePickup`, `IsInspectingPickup`, `GetInspectedPickup`, and `DropAllOwnedPickups`.

- [ ] **Step 1: Add failing player lifecycle tests**

Test that a player cannot queue while dead/movement-locked/already holding; only the inspected or equipped owner can issue actions; rotation from a non-owner is ignored; storing adds exactly once; and two consecutive `DropAllOwnedPickups` calls release each item once.

```cpp
Player->DropAllOwnedPickups();
const uint32 RevisionAfterFirstDrop = Item->GetStateRevision();
Player->DropAllOwnedPickups();
TestEqual(TEXT("Drop is idempotent"), Item->GetStateRevision(), RevisionAfterFirstDrop);
```

- [ ] **Step 2: Change `TryPickupOnServer` from immediate pickup to queueing**

The server repeats `TracePickup`, casts the hit, and calls `Item->QueuePickupRequest(this)`. `CompleteQueuedPickup` is callable only by the authoritative item resolver and sets `InspectedPickup` after the item transition succeeds.

- [ ] **Step 3: Add reliable action RPCs and an unreliable rotation RPC**

```cpp
UFUNCTION(Server, Reliable) void ServerRequestEquipPickup();
UFUNCTION(Server, Reliable) void ServerRequestStorePickup();
UFUNCTION(Server, Reliable) void ServerRequestDropPickup();
UFUNCTION(Server, Unreliable) void ServerRequestRotatePickup(FRotator Delta);
```

Every implementation re-checks state and ownership. The drop RPC derives direction from `GetViewRotation()` and uses configurable `ThrowStrength`; no client direction or force parameter exists.

- [ ] **Step 4: Maintain server inventory and replicated active references**

Replicate `InspectedPickup` and `EquippedPickup` with RepNotify for owner presentation. Keep the stored collection server-only, de-duplicate on insert, prune invalid references on access, and remove entries when an item leaves `Stored`.

- [ ] **Step 5: Lock and restore movement safely**

On entering inspection, cache movement mode/custom mode once and set `MOVE_None` on the server. On every exit path, restore the cached mode if valid; otherwise restore walking. The owning client gates move/look immediately from the replicated inspected reference.

- [ ] **Step 6: Add ragdoll/death/destruction release hooks**

Override `Tick` to detect the authoritative false-to-true edge of `IsRagdollActive()`, override `HandleDied_Implementation`, and retain `EndPlay` cleanup. All three call idempotent `DropAllOwnedPickups`, which wakes stored items and scatters them within configurable `InventoryDropRadius` around the player.

- [ ] **Step 7: Run player, item, and rule tests**

Expected: all owner validation, inventory de-duplication, rotation rejection, and double-drop assertions pass.

- [ ] **Step 8: Check the focused diff**

Run `git diff --check -- Source/EvilClownParadise/Public/Characters/ECPPlayerBase.h Source/EvilClownParadise/Private/Characters/ECPPlayerBase.cpp Source/EvilClownParadise/Private/Tests/ECPPickupPlayerTests.cpp`.

### Task 5: Controller keys, cursor mode, and drag prediction

**Files:**
- Modify: `Source/EvilClownParadise/Public/Data/ECPPlayerController.h`
- Modify: `Source/EvilClownParadise/Private/Data/ECPPlayerController.cpp`

**Interfaces:**
- Consumes: Task 4 player request methods and `IsInspectingPickup`.
- Produces: `RefreshPickupInputMode`, fixed-key handlers, drag state, and throttled rotation submission.

- [ ] **Step 1: Bind explicit keys without modifying asset files**

```cpp
InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ThisClass::EquipPickup);
InputComponent->BindKey(EKeys::G, IE_Pressed, this, &ThisClass::DropPickup);
InputComponent->BindKey(EKeys::V, IE_Pressed, this, &ThisClass::StorePickup);
InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ThisClass::BeginRotatePickup);
InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &ThisClass::EndRotatePickup);
InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ThisClass::PlacePickup);
```

- [ ] **Step 2: Gate existing move/look and action handlers**

When the current pawn is inspecting, `Move`, `Look`, crouch, jump, run, and ragdoll input return before mutating the pawn. `2`, `V`, right mouse, and drag remain enabled; `G` operates only for equipped items.

- [ ] **Step 3: Implement cursor/input-mode restoration**

`RefreshPickupInputMode` records previous cursor visibility once, uses `FInputModeGameAndUI` with no permanent viewport lock while inspecting, and restores `FInputModeGameOnly` plus saved cursor visibility on exit, pawn switch, unpossess, and `EndPlay`.

- [ ] **Step 4: Sample left-drag in `PlayerTick`**

While left drag and inspection are active, call `GetInputMouseDelta`, convert by configurable `PickupRotationSensitivity`, apply local predicted rotation to the inspected item, accumulate delta, and send at no more than configurable `PickupRotationSendRate` (default 20 Hz). Stop and clear accumulation as soon as state/owner changes.

- [ ] **Step 5: Build and run the complete automation prefix**

Run `Automation RunTests ECP.Pickup` headlessly.

Expected: all tests pass; no compile warning indicates hidden overloaded RPCs, missing generated headers, or unsafe direct input bindings.

- [ ] **Step 6: Check the focused diff**

Run `git diff --check -- Source/EvilClownParadise/Public/Data/ECPPlayerController.h Source/EvilClownParadise/Private/Data/ECPPlayerController.cpp`.

### Task 6: Full build and multiplayer acceptance

**Files:**
- Verify: all files from Tasks 1-5
- Do not modify: `.uasset`, `.umap`, `Config`, or `Plugins/Developer/RiderLink` files unless compilation proves an in-scope dependency and the owner explicitly approves it.

**Interfaces:**
- Consumes: complete pickup feature.
- Produces: build/test evidence and a user-ready manual PIE checklist.

- [ ] **Step 1: Run whitespace and conflict-marker checks**

Run `git diff --check` for only the C++ and documentation paths in this plan, then search those paths for `<<<<<<<`, `=======`, and `>>>>>>>`.

- [ ] **Step 2: Build `EvilClownParadiseEditor Win64 Development`**

Expected: exit code `0`. Fix only errors caused by the touched pickup files; report unrelated existing build failures separately with their first error.

- [ ] **Step 3: Run all `ECP.Pickup` automation tests headlessly**

Expected: every rule, item, and player test passes with exit code `0`.

- [ ] **Step 4: Run Listen Server plus two-client PIE acceptance**

Verify: remote client `F` pickup; inspection cursor and movement/look lock; left drag rotation visible on peers; right-click place; `2` equip; `G` throw with light item accelerating more than heavy item; `V` store from both active states; `X` ragdoll explosion; formal death explosion; disconnect cleanup; same-frame host victory; same-frame remote lower join order victory; cross-frame first arrival remains owner.

- [ ] **Step 5: Review only the task diff and hand off testing**

List every changed source file, automation results, build command/result, untested editor-only steps, and any required Blueprint default adjustments. Do not stage or commit the owner's unrelated existing changes.
