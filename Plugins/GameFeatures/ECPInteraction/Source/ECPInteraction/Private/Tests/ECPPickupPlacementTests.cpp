#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Actors/ECPMediumPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"

namespace ECPPlacementTests
{
bool CheckPlacement(FAutomationTestBase& Test, bool bMedium)
{
    Test.AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
        EAutomationExpectedErrorFlags::Contains, 0);
    for (const float Distance : {80.f, 150.f})
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        UClass* PlayerClass = LoadClass<AECPPlayerBase>(nullptr,
            TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
        UClass* ItemClass = LoadClass<AECPPickupItem>(nullptr, bMedium
            ? TEXT("/Game/Project/Interaction/Items/BP_Medium-sizeditems.BP_Medium-sizeditems_C")
            : TEXT("/Game/Project/Interaction/Items/BP_Smallitems.BP_Smallitems_C"));
        AECPPlayerBase* Player = PlayerClass ? World->SpawnActor<AECPPlayerBase>(PlayerClass) : nullptr;
        AECPPickupItem* Item = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        if (!Test.TestNotNull(TEXT("Placement player exists"), Player)
            || !Test.TestNotNull(TEXT("Placement item blueprint exists"), Item)
            || !Test.TestNotNull(TEXT("Placement controller exists"), Controller))
        {
            World->DestroyWorld(false);
            return false;
        }
        Player->NetworkDebugWidgetClass = nullptr;
        Controller->SetAsLocalPlayerController();
        Controller->Possess(Player);
        Player->DispatchBeginPlay();
        USceneComponent* Camera = Player->GetPickupInspectionComponent();
        Camera->SetWorldLocationAndRotation(FVector(230.f, 170.f, 250.f), FRotator(15.f, 35.f, 0.f));
        UPrimitiveComponent* Body = CastChecked<UPrimitiveComponent>(Item->GetRootComponent());
        Body->SetMassOverrideInKg(NAME_None, bMedium ? 120.f : 10.f);
        // 固定测试实例的可调范围，避免蓝图中日常调整的距离限制改变本次复现输入。
        Item->InspectionDistanceMin = 80.f;
        Item->InspectionDistanceMax = 150.f;
        Item->InspectionRelativeTransform.SetLocation(FVector(120.f, 15.f, -10.f));
        const FVector Scale(0.75f, 1.25f, 0.9f);
        Item->SetActorScale3D(Scale);
        Item->DispatchBeginPlay();
        Test.TestTrue(TEXT("Item enters pickup"), Item->TryEnterPickup(Player));
        Player->CompleteQueuedPickup(Item);
        const FVector BeforeWheel = Item->GetActorLocation();
        Player->RequestSetInspectionDistance(Item, Item->GetInspectionGeneration(),
            Item->GetNextInspectionDistanceSequence(), Distance);
        Test.TestEqual(TEXT("Wheel distance was accepted before placing"), Item->GetInspectionDistance(), Distance);
        FECPInspectionRotationSample Rotation;
        Rotation.Generation = Item->GetInspectionGeneration();
        Rotation.Session = 1;
        Rotation.Sequence = 1;
        Rotation.Rotation = FRotator(20.f, 55.f, 10.f);
        Rotation.bFinal = true;
        Player->RequestFinalizePickupRotation(Item, Rotation);
        if (bMedium)
        {
            Item->Tick(1.f / 60.f);
            const float DisplayedDistance = Camera->GetComponentTransform().InverseTransformPosition(
                Item->GetActorLocation()).X;
            Test.TestTrue(TEXT("Heavy item is still following when right-click arrives"),
                FMath::Abs(DisplayedDistance - Distance) > 1.f);
        }
        else
        {
            Test.TestTrue(TEXT("Small items preserve their real pose when a wheel target arrives"),
                Item->GetActorLocation().Equals(BeforeWheel, 0.001f));
        }
        const FTransform Before = Item->GetActorTransform();
        Player->RequestPlacePickup();
        Test.TestEqual(TEXT("Right-click changes item to world state"), Item->GetPickupState(), EECPPickupState::World);
        Test.TestNull(TEXT("Placement clears the inspected slot"), Player->GetInspectedPickup());
        Test.TestNull(TEXT("Placed item detaches from the camera"), Body->GetAttachParent());
        Test.TestTrue(TEXT("Right-click keeps the displayed world location"),
            Item->GetActorLocation().Equals(Before.GetLocation(), 0.001f));
        Test.TestTrue(TEXT("Right-click keeps the displayed world rotation"),
            Item->GetActorQuat().Equals(Before.GetRotation(), 0.001f));
        Test.TestTrue(TEXT("Placement preserves the item's scale"), Item->GetActorScale3D().Equals(Scale, 0.001f));
        Test.TestTrue(TEXT("Placement restores physics"), Body->IsSimulatingPhysics());
        World->DestroyWorld(false);
    }
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPSmallWheelPlacementTest,
    "ECP.Pickup.Placement.SmallPreservesWheelPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPSmallWheelPlacementTest::RunTest(const FString& Parameters)
{
    return ECPPlacementTests::CheckPlacement(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMediumWheelPlacementTest,
    "ECP.Pickup.Placement.MediumPreservesLaggedWheelPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMediumWheelPlacementTest::RunTest(const FString& Parameters)
{
    return ECPPlacementTests::CheckPlacement(*this, true);
}

#endif
