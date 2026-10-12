#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Actors/ECPMediumPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/ECPPhysicalCarryComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include "UObject/Script.h"

namespace ECPMediumTests
{
struct FFixture
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AECPPlayerBase* Player = nullptr;
    AECPPickupItem* Item = nullptr;
    UStaticMeshComponent* Mesh = nullptr;
    UCameraComponent* Camera = nullptr;

    explicit FFixture(float Mass, const TCHAR* MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube"))
    {
        UClass* PlayerClass = LoadClass<AECPPlayerBase>(nullptr,
            TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
        Player = PlayerClass ? World->SpawnActor<AECPPlayerBase>(PlayerClass) : nullptr;
        if (!Player) return;
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        Controller->SetAsLocalPlayerController();
        Player->NetworkDebugWidgetClass = nullptr;
        Controller->Possess(Player);
        Player->DispatchBeginPlay();
        Camera = Cast<UCameraComponent>(Player->GetPickupInspectionComponent());
        UClass* MediumClass = FindObject<UClass>(nullptr, TEXT("/Script/ECPInteraction.ECPMediumPickupItem"));
        if (!MediumClass) return;
        Item = World->SpawnActor<AECPPickupItem>(MediumClass);
        Mesh = NewObject<UStaticMeshComponent>(Item, TEXT("Cube"));
        Item->AddInstanceComponent(Mesh);
        Item->SetRootComponent(Mesh);
        Mesh->SetMobility(EComponentMobility::Movable);
        UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
#if WITH_EDITOR
        if (StaticMesh)
        {
            UStaticMesh* Meshes[] = {StaticMesh};
            FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
        }
#endif
        Mesh->SetStaticMesh(StaticMesh);
        Mesh->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
        Mesh->SetCollisionObjectType(ECC_WorldDynamic);
        Mesh->RegisterComponent();
        Mesh->SetMassOverrideInKg(NAME_None, Mass);
        Item->SetActorScale3D(FVector(0.4f, 0.6f, 0.5f));
        Item->DispatchBeginPlay();
    }
    ~FFixture() { World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumRestrictionsTest, "ECP.Pickup.Medium.RestrictionsAndRelease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumRestrictionsTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Fixture(80.f);
    if (!TestNotNull(TEXT("Medium pickup native class exists"), Fixture.Item)) return false;
    AECPPickupItem* Item = Fixture.Item;
    TestTrue(TEXT("Existing player has a real valid equipment socket"), Fixture.Player->GetPickupAttachmentComponent()
        && Fixture.Player->GetPickupAttachmentComponent()->DoesSocketExist(Fixture.Player->GetPickupAttachmentSocket()));
    const FVector Scale = Item->GetActorScale3D();
    TestEqual(TEXT("Legacy physics-only medium root also supports queries"), Fixture.Mesh->GetCollisionEnabled(), ECollisionEnabled::QueryAndPhysics);
    TestEqual(TEXT("Medium root uses the existing thing scan channel"), Fixture.Mesh->GetCollisionObjectType(), ECC_GameTraceChannel1);
    TestFalse(TEXT("World medium items do not tick"), Item->IsActorTickEnabled());
    TestTrue(TEXT("Medium item can enter inspection"), Item->TryEnterPickup(Fixture.Player));
    Fixture.Player->CompleteQueuedPickup(Item);
    TestTrue(TEXT("Only held medium items tick for smoothing"), Item->IsActorTickEnabled());
    const uint32 Revision = Item->GetStateRevision();
    TestFalse(TEXT("Authority rejects medium equipment"), Item->TryEquip(Fixture.Player));
    TestFalse(TEXT("Authority rejects medium inventory"), Item->TryStore(Fixture.Player));
    Fixture.Player->RequestStorePickup();
    Fixture.Player->RequestEquipPickup();
    TestEqual(TEXT("Rejected requests do not publish state transitions"), Item->GetStateRevision(), Revision);
    TestEqual(TEXT("Rejected requests keep the item in pickup"), Item->GetPickupState(), EECPPickupState::Pickup);
    TestEqual(TEXT("Rejected store does not increase inventory"), Fixture.Player->GetStoredPickupCount(), 0);
    TestTrue(TEXT("Active inspected slot remains owned"), Fixture.Player->GetInspectedPickup() == Item);
    TestTrue(TEXT("Medium item can still be released"), Item->TryDrop(Fixture.Player,
        FTransform(FVector(300.f, 0.f, 100.f)), FVector::ZeroVector));
    TestTrue(TEXT("Release restores physics"), Fixture.Mesh->IsSimulatingPhysics());
    TestFalse(TEXT("Release stops smoothing tick"), Item->IsActorTickEnabled());
    TestTrue(TEXT("Release preserves scale"), Item->GetActorScale3D().Equals(Scale, 0.001f));
    TestTrue(TEXT("Released item can be picked up again"), Item->TryEnterPickup(Fixture.Player));
    TestTrue(TEXT("Reentry restarts smoothing without stale state"), Item->IsActorTickEnabled());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumScanAndMassRangeTest, "ECP.Pickup.Medium.ScanSiblingAndMassRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumScanAndMassRangeTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Low(10.f), High(200.f);
    if (!TestNotNull(TEXT("Low mass fixture exists"), Low.Item) || !TestNotNull(TEXT("High mass fixture exists"), High.Item)) return false;
    TestEqual(TEXT("Medium physical mass has a 50kg minimum"), Low.Mesh->GetMass(), 50.f);
    TestEqual(TEXT("Medium physical mass has a 120kg maximum"), High.Mesh->GetMass(), 120.f);
    USkeletalMeshComponent* Carry = Low.Player->GetPickupAttachmentComponent();
    if (!TestNotNull(TEXT("Real weapon scan component"), Carry)) return false;
    Carry->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
    Carry->SetWorldRotation(FRotator::ZeroRotator);
    Low.Item->SetActorLocation(Carry->GetComponentLocation() + Carry->GetUpVector() * 150.f);
    TestTrue(TEXT("Player retains the existing small-item blueprint class setting"), Low.Player->PickupActorClass
        && !Low.Item->IsA(Low.Player->PickupActorClass));
    TestTrue(TEXT("The same player scan accepts the sibling medium class"), Low.Player->GetScannedPickup() == Low.Item);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumSceneMeshesTest, "ECP.Pickup.Medium.SceneMeshPhysicsAndWeights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumSceneMeshesTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    const TCHAR* Paths[] = {
        TEXT("/Engine/BasicShapes/Cube.Cube"),
        TEXT("/Game/Project/Interaction/Items/Meshes/Medium/SM_Medium_Cylinder.SM_Medium_Cylinder"),
        TEXT("/Game/Project/Interaction/Items/Meshes/Medium/SM_Medium_QuarterCylinder.SM_Medium_QuarterCylinder"),
        TEXT("/Game/Project/Interaction/Items/Meshes/Medium/SM_Medium_CircularBand.SM_Medium_CircularBand")};
    const float Masses[] = {80.f, 100.f, 120.f, 50.f};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Paths); ++Index)
    {
        ECPMediumTests::FFixture Fixture(Masses[Index], Paths[Index]);
        if (!TestNotNull(TEXT("Scene mesh medium fixture exists"), Fixture.Item)) return false;
        Fixture.Item->SetActorScale3D(FVector(1.25f));
        const FString Label = FString::Printf(TEXT("Scene mesh %d"), Index + 1);
        AddInfo(FString::Printf(TEXT("%s: simulated=%d physical mass=%.8f lag mass=%.8f"), *Label,
            Fixture.Mesh->IsSimulatingPhysics(), Fixture.Mesh->GetMass(), CastChecked<AECPMediumPickupItem>(Fixture.Item)->GetPickupMassKg()));
        if (UBodySetup* Setup = Fixture.Mesh->GetStaticMesh()->GetBodySetup())
        {
            AddInfo(FString::Printf(TEXT("Physics shapes=%d convex=%d created=%d failed=%d nonmirror=%d"),
                Setup->AggGeom.GetElementCount(), Setup->AggGeom.ConvexElems.Num(), Setup->bCreatedPhysicsMeshes,
                Setup->bFailedToCreatePhysicsMeshes, Setup->bGenerateNonMirroredCollision));
        }
        TestTrue(Label + TEXT(" supports physical simulation"), Fixture.Mesh->IsSimulatingPhysics());
        TestTrue(Label + TEXT(" has the configured physical mass"), FMath::IsNearlyEqual(Fixture.Mesh->GetMass(), Masses[Index], 0.01f));
        if (Index == 3)
        {
            FHitResult Hit;
            FCollisionQueryParams Params;
            Params.bTraceComplex = false;
            const FTransform Transform = Fixture.Mesh->GetComponentTransform();
            TestFalse(TEXT("Ring simple collision preserves the center opening"), Fixture.Mesh->LineTraceComponent(Hit,
                Transform.TransformPosition(FVector(0.f, 0.f, -100.f)),
                Transform.TransformPosition(FVector(0.f, 0.f, 100.f)), Params));
            TestTrue(TEXT("Ring simple collision supports the visible band"), Fixture.Mesh->LineTraceComponent(Hit,
                Transform.TransformPosition(FVector(90.f, 0.f, 0.f)),
                Transform.TransformPosition(FVector(50.f, 0.f, 0.f)), Params));
        }
        TestTrue(Label + TEXT(" enters pickup"), Fixture.Item->TryEnterPickup(Fixture.Player));
        TestTrue(Label + TEXT(" uses its real weight for lag"), FMath::IsNearlyEqual(
            CastChecked<AECPMediumPickupItem>(Fixture.Item)->GetPickupMassKg(), Masses[Index], 0.01f));
        TestTrue(Label + TEXT(" can be released"), Fixture.Item->TryDrop(Fixture.Player,
            FTransform(FVector(300.f, 0.f, 100.f)), FVector::ZeroVector));
        TestTrue(Label + TEXT(" restores simulation and scale"), Fixture.Mesh->IsSimulatingPhysics()
            && Fixture.Item->GetActorScale3D().Equals(FVector(1.25f), 0.001f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumConfiguredMassFallbackTest, "ECP.Pickup.Medium.ConfiguredMassWithoutPhysicsBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumConfiguredMassFallbackTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Fixture(100.f, TEXT("/FUOnlineSession/LevelPrototyping/Meshes/SM_Cylinder.SM_Cylinder"));
    if (!TestNotNull(TEXT("Mass fallback fixture exists"), Fixture.Item)) return false;
    TestTrue(TEXT("Configured mass survives an unavailable physical body"), FMath::IsNearlyEqual(
        CastChecked<AECPMediumPickupItem>(Fixture.Item)->GetPickupMassKg(), 100.f, 0.01f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumBlueprintIntegrationTest, "ECP.Pickup.Medium.BlueprintIntegration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumBlueprintIntegrationTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Fixture(80.f);
    UClass* BlueprintClass = LoadClass<AActor>(nullptr,
        TEXT("/Game/Project/Interaction/Items/BP_Medium-sizeditems.BP_Medium-sizeditems_C"));
    if (!TestTrue(TEXT("Real medium blueprint inherits the new native class"), BlueprintClass
        && BlueprintClass->IsChildOf(AECPMediumPickupItem::StaticClass()))) return false;
    AECPMediumPickupItem* Item = Fixture.World->SpawnActor<AECPMediumPickupItem>(BlueprintClass);
    if (!TestNotNull(TEXT("Real medium blueprint spawns"), Item)) return false;
    Item->DispatchBeginPlay();
    UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Item->GetRootComponent());
    if (!TestNotNull(TEXT("Existing Cube mesh remains the real root"), Mesh)) return false;
    Fixture.Item->SetActorLocation(FVector(10000.f));
    USkeletalMeshComponent* Carry = Fixture.Player->GetPickupAttachmentComponent();
    Carry->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
    Carry->SetWorldRotation(FRotator::ZeroRotator);
    Item->SetActorLocation(Carry->GetComponentLocation() + Carry->GetUpVector() * 150.f);
    TestTrue(TEXT("Actual medium blueprint can be scanned"), Fixture.Player->GetScannedPickup() == Item);
    TestTrue(TEXT("Actual medium blueprint preserves default 80kg"), FMath::IsNearlyEqual(Item->GetPickupMassKg(), 80.f));
    TestTrue(TEXT("Actual medium blueprint uses the existing focus outline"), Item->TryAcquireFocus(Fixture.Player));
    TestTrue(TEXT("The actual Cube stays visible and outlined"), Mesh->IsVisible() && Mesh->GetOverlayMaterial() != nullptr);
    Mesh->SetMassOverrideInKg(NAME_None, 120.f);
    TestTrue(TEXT("Actual medium blueprint can be picked up"), Item->TryEnterPickup(Fixture.Player));
    TestEqual(TEXT("Pickup captures updated root mass"), Item->GetPickupMassKg(), 120.f);
    TestFalse(TEXT("Actual medium blueprint rejects equipment"), Item->TryEquip(Fixture.Player));
    TestFalse(TEXT("Actual medium blueprint rejects storage"), Item->TryStore(Fixture.Player));
    TestTrue(TEXT("Actual medium blueprint can be dropped"), Item->TryDrop(Fixture.Player,
        FTransform(FVector(300.f, 0.f, 100.f)), FVector::ZeroVector));
    TestTrue(TEXT("Actual Cube resumes physics and scan queries"), Mesh->IsSimulatingPhysics()
        && Mesh->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumPredictionTest, "ECP.Pickup.Medium.DelayedConfirmationAndDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumPredictionTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Fixture(120.f);
    if (!TestNotNull(TEXT("Medium prediction fixture exists"), Fixture.Item)) return false;
    AECPPickupItem* Item = Fixture.Item;
    TestTrue(TEXT("Enter pickup for owner prediction"), Item->TryEnterPickup(Fixture.Player));
    Fixture.Player->SetRole(ROLE_AutonomousProxy);
    Item->SetRole(ROLE_SimulatedProxy);
    FECPInspectionRotationSample Sample;
    Sample.Generation = Item->GetInspectionGeneration();
    Sample.Session = 2;
    Sample.Sequence = 8;
    Sample.Rotation = FRotator(0.f, 90.f, 0.f);
    Item->PreviewInspectionRotation(Sample);
    Item->Tick(0.1f);
    const FQuat Display = Item->GetActorQuat();
    FStructProperty* Property = FindFProperty<FStructProperty>(AECPPickupItem::StaticClass(), TEXT("AttachmentState"));
    UFunction* Notify = Item->FindFunction(TEXT("OnRep_AttachmentState"));
    if (!TestNotNull(TEXT("Replicated item state"), Property) || !TestNotNull(TEXT("State notification"), Notify)) return false;
    FECPPickupNetState* State = Property->ContainerPtrToValuePtr<FECPPickupNetState>(Item);
    State->Inspection = Sample;
    State->Inspection.Sequence = 7;
    State->Inspection.Rotation = FRotator::ZeroRotator;
    {
        FEditorScriptExecutionGuard ScriptGuard;
        Item->ProcessEvent(Notify, nullptr);
    }
    TestTrue(TEXT("Delayed confirmation does not snap the smoothed display"), Item->GetActorQuat().Equals(Display, 0.001f));
    TestEqual(TEXT("Newer owner target survives delayed confirmation"), Item->GetInspectionRotation().Yaw, 90.0);
    const FVector BeforeDistance = Item->GetActorLocation();
    Item->PreviewInspectionDistance(Item->GetInspectionGeneration(), Item->GetNextInspectionDistanceSequence(), 150.f);
    TestTrue(TEXT("Wheel target does not immediately snap position"), Item->GetActorLocation().Equals(BeforeDistance, 0.001f));
    TestEqual(TEXT("Owner retains the latest wheel intent"), Item->GetInspectionDistance(), 150.f);
    TestTrue(TEXT("Owner input cannot teleport the real body"), Item->GetActorLocation().Equals(BeforeDistance, 0.001f));
    State->HeldTransform = FTransform(FRotator(0.f,30.f,0.f), FVector(200.f,80.f,70.f), Item->GetActorScale3D());
    for (int32 Frame = 0; Frame < 600; ++Frame) Item->Tick(1.f / 60.f);
    TestTrue(TEXT("Owner follows the actual server pose, not a predicted camera goal"),
        Item->GetActorTransform().Equals(State->HeldTransform, 0.01f));
    TestEqual(TEXT("Input rotation is independent of collision-limited server pose"), Item->GetInspectionRotation().Yaw, 90.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumMassLagTest, "ECP.Pickup.Medium.MassWeightedFollow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumMassLagTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Light(50.f), Heavy(120.f);
    if (!TestNotNull(TEXT("Light medium exists"), Light.Item) || !TestNotNull(TEXT("Heavy medium exists"), Heavy.Item)) return false;
    for (auto* Fixture : {&Light, &Heavy})
    {
        TestTrue(TEXT("Both weights enter pickup"), Fixture->Item->TryEnterPickup(Fixture->Player));
        FVector Initial;
        FRotator InitialRotation;
        Fixture->Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(Initial, InitialRotation);
        Fixture->Camera->AddWorldOffset(FVector(100.f, 0.f, 0.f));
        Fixture->Player->GetPhysicalCarryComponent()->TickComponent(1.f/60.f, LEVELTICK_All, nullptr);
        FVector Next;
        FRotator NextRotation;
        Fixture->Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(Next, NextRotation);
        TestTrue(TEXT("Grab target progresses without instantly snapping"), FVector::Distance(Next, Initial) > 0.0
            && FVector::Distance(Next, Initial) < FVector::Distance(Fixture->Item->GetInspectionTargetWorldTransform().GetLocation(), Initial));
    }
    FVector LightTarget, HeavyTarget;
    FRotator TargetRotation;
    Light.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(LightTarget, TargetRotation);
    Heavy.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(HeavyTarget, TargetRotation);
    const double LightError = FVector::Distance(LightTarget, Light.Item->GetInspectionTargetWorldTransform().GetLocation());
    const double HeavyError = FVector::Distance(HeavyTarget, Heavy.Item->GetInspectionTargetWorldTransform().GetLocation());
    TestTrue(TEXT("120kg has more lag than 50kg"), HeavyError > LightError + 1.0);
    const FQuat Before = Heavy.Item->GetActorQuat();
    FECPInspectionRotationSample Sample;
    Sample.Generation = Heavy.Item->GetInspectionGeneration();
    Sample.Session = 1;
    Sample.Sequence = 1;
    Sample.Rotation = FRotator(0.f, 90.f, 0.f);
    Sample.bFinal = true;
    TestTrue(TEXT("Final inspection rotation is accepted"), Heavy.Item->ApplyInspectionRotation(Heavy.Player, Sample));
    TestTrue(TEXT("An accepted final sample does not snap the displayed heavy item"), Heavy.Item->GetActorQuat().Equals(Before, 0.001f));
    Heavy.Player->GetPhysicalCarryComponent()->TickComponent(1.f/60.f, LEVELTICK_All, nullptr);
    Heavy.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(HeavyTarget, TargetRotation);
    TestFalse(TEXT("Constraint rotation starts converging after final sample"), TargetRotation.Quaternion().Equals(Before, 0.001f));
    for (int32 Frame = 0; Frame < 600; ++Frame) Heavy.Player->GetPhysicalCarryComponent()->TickComponent(1.f/60.f, LEVELTICK_All, nullptr);
    Heavy.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(HeavyTarget, TargetRotation);
    TestTrue(TEXT("Constraint keeps approaching the final rotation"), TargetRotation.Quaternion().Equals(
        Heavy.Item->GetInspectionTargetWorldTransform().GetRotation(), 0.001f));
    // 无 NetDriver 的原生测试世界不会懒初始化类的复制索引，直接查询前先执行引擎的初始化入口。
    Heavy.Item->GetClass()->SetUpRuntimeReplicationData();
    FFloatProperty* Mass = FindFProperty<FFloatProperty>(Heavy.Item->GetClass(), TEXT("AuthoritativeMassKg"));
    TestNotNull(TEXT("Mass is available for authoritative replication"), Mass);
    if (Mass)
    {
        TArray<FLifetimeProperty> Lifetime;
        static_cast<AActor*>(Heavy.Item)->GetLifetimeReplicatedProps(Lifetime);
        TestTrue(TEXT("Observers receive the same mass for smoothing"), Lifetime.ContainsByPredicate([Mass](const FLifetimeProperty& Property)
            { return Property.RepIndex == Mass->RepIndex && Property.Condition == COND_None; }));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumFrameRateTest, "ECP.Pickup.Medium.FrameRateIndependent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumFrameRateTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, 0);
    ECPMediumTests::FFixture Slow(80.f), Fast(80.f);
    if (!TestNotNull(TEXT("30fps medium exists"), Slow.Item) || !TestNotNull(TEXT("120fps medium exists"), Fast.Item)) return false;
    // This is a free-space response test. Start outside the holder's capsule; the old origin (0,0,0)
    // was already inside it and exercises a nonlinear collision correction rather than frame-rate response.
    for (auto* Fixture : {&Slow,&Fast})
    {
        Fixture->Item->SetActorLocation(Fixture->Camera->GetComponentLocation()
            + Fixture->Player->GetViewRotation().Vector()*120.f,false,nullptr,ETeleportType::TeleportPhysics);
    }
    Slow.Item->TryEnterPickup(Slow.Player);
    Fast.Item->TryEnterPickup(Fast.Player);
    Slow.Camera->AddWorldOffset(FVector(100.f, 0.f, 0.f));
    Fast.Camera->AddWorldOffset(FVector(100.f, 0.f, 0.f));
    for (int32 Frame = 0; Frame < 30; ++Frame) Slow.Player->GetPhysicalCarryComponent()->TickComponent(1.f/30.f, LEVELTICK_All, nullptr);
    for (int32 Frame = 0; Frame < 120; ++Frame) Fast.Player->GetPhysicalCarryComponent()->TickComponent(1.f/120.f, LEVELTICK_All, nullptr);
    FVector SlowTarget, FastTarget;
    FRotator TargetRotation;
    Slow.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(SlowTarget, TargetRotation);
    Fast.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(FastTarget, TargetRotation);
    TestTrue(TEXT("Equal elapsed time has equal follow response across frame rates"),
        SlowTarget.Equals(FastTarget, 0.01f));
    return true;
}
#endif
