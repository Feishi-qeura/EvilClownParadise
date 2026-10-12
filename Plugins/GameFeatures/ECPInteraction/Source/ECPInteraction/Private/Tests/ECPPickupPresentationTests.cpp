#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"

namespace ECPPickupPresentationTests
{
struct FFixture
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AECPPlayerBase* Player = nullptr;
    AECPPickupItem* Item = nullptr;

    FFixture()
    {
        Player = World->SpawnActor<AECPPlayerBase>();
        UCameraComponent* Camera = NewObject<UCameraComponent>(Player, TEXT("Camera"));
        Player->AddInstanceComponent(Camera);
        Camera->SetupAttachment(Player->GetRootComponent());
        Camera->RegisterComponent();
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        Controller->SetAsLocalPlayerController();
        Player->NetworkDebugWidgetClass = nullptr;
        Controller->Possess(Player);
        Player->DispatchBeginPlay();
        UClass* ItemClass = LoadClass<AECPPickupItem>(nullptr,
            TEXT("/Game/Project/Interaction/Items/BP_Smallitems.BP_Smallitems_C"));
        Item = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
        if (Item) Item->DispatchBeginPlay();
    }

    ~FFixture() { World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupMeshPresentationTest,
    "ECP.Pickup.Presentation.MeshRootAndFocus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupMeshPresentationTest::RunTest(const FString& Parameters)
{
    ECPPickupPresentationTests::FFixture Fixture;
    AECPPickupItem* Item = Fixture.Item;
    if (!TestNotNull(TEXT("Real small item blueprint loads"), Item)) return false;
    UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Item->GetRootComponent());
    if (!TestNotNull(TEXT("The replaced mesh is the physics root"), Mesh)) return false;
    TestTrue(TEXT("Mesh root can initialize pickup without a collision-component name"), Item->CanBePickedUp());
    TestTrue(TEXT("World item simulates physics on the mesh root"), Mesh->IsSimulatingPhysics());
    TestFalse(TEXT("Small items cannot be stepped onto"), Mesh->CanCharacterStepUp(Fixture.Player));
    FHitResult Floor;
    Floor.bBlockingHit = true;
    Floor.ImpactNormal = FVector::UpVector;
    Floor.Component = Mesh;
    TestFalse(TEXT("Even a flat item top cannot become a walking floor"),
        Fixture.Player->GetCharacterMovement()->IsWalkable(Floor));
    TestEqual(TEXT("Players can still push small items"), Mesh->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);

    UMaterialInterface* OriginalOverlay = Mesh->GetOverlayMaterial();
    const bool bOriginalForceDisableNanite = Mesh->IsForceDisableNanite();
    const uint32 OriginalRevision = Item->GetStateRevision();
    TestTrue(TEXT("A player can focus the item"), Item->TryAcquireFocus(Fixture.Player));
    TestNotNull(TEXT("Focused mesh has an outline overlay"), Mesh->GetOverlayMaterial());
    TestTrue(TEXT("Nanite meshes use the supported overlay render path while focused"), Mesh->IsForceDisableNanite());
    TestEqual(TEXT("Focus never restarts the physical lifecycle"), Item->GetStateRevision(), OriginalRevision);
    TestTrue(TEXT("Changing the outline render path keeps physics active"), Mesh->IsSimulatingPhysics());
    TestTrue(TEXT("Focusing never hides the actual item mesh"), Mesh->IsVisible());
    Item->ReleaseFocus(Fixture.Player);
    TestTrue(TEXT("Unfocus restores the original overlay"), Mesh->GetOverlayMaterial() == OriginalOverlay);
    TestEqual(TEXT("Unfocus restores the original Nanite override"), Mesh->IsForceDisableNanite(), bOriginalForceDisableNanite);
    TestTrue(TEXT("An unfocused item remains visible"), Mesh->IsVisible());

    Item->TryAcquireFocus(Fixture.Player);
    TestTrue(TEXT("Mesh item can enter inspection"), Item->TryEnterPickup(Fixture.Player));
    TestTrue(TEXT("Pickup clears the outline"), Mesh->GetOverlayMaterial() == OriginalOverlay);
    TestTrue(TEXT("Held roots retain server physics and collision"), Mesh->IsSimulatingPhysics()
        && Mesh->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
    TestTrue(TEXT("Inspected item can be released"), Item->TryDrop(Fixture.Player,
        FTransform(FVector(300.f, 0.f, 100.f)), FVector::ZeroVector));
    TestTrue(TEXT("Released mesh resumes physics"), Mesh->IsSimulatingPhysics());
    TestFalse(TEXT("Release does not make the item a walking floor"),
        Fixture.Player->GetCharacterMovement()->IsWalkable(Floor));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupInventoryPresentationTest,
    "ECP.Pickup.Presentation.InventoryCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupInventoryPresentationTest::RunTest(const FString& Parameters)
{
    ECPPickupPresentationTests::FFixture Fixture;
    AECPPlayerBase* Player = Fixture.Player;
    AECPPickupItem* Item = Fixture.Item;
    if (!TestNotNull(TEXT("Real item loads for inventory test"), Item)) return false;
    FIntProperty* Count = FindFProperty<FIntProperty>(Player->GetClass(), TEXT("StoredPickupCount"));
    if (!TestNotNull(TEXT("Owning clients receive an authoritative inventory count"), Count)) return false;
    TArray<FLifetimeProperty> Lifetime;
    static_cast<AActor*>(Player)->GetLifetimeReplicatedProps(Lifetime);
    const FLifetimeProperty* CountLifetime = Lifetime.FindByPredicate([Count](const FLifetimeProperty& Property)
    {
        return Property.RepIndex == Count->RepIndex;
    });
    TestTrue(TEXT("Inventory count is replicated only to its owner"),
        CountLifetime && CountLifetime->Condition == COND_OwnerOnly);
    TestTrue(TEXT("Real item enters pickup"), Item->TryEnterPickup(Player));
    Player->CompleteQueuedPickup(Item);
    TestTrue(TEXT("The inspection getter reflects the real owned state"), Player->GetInspectedPickup() == Item);
    Player->RequestStorePickup();
    TestEqual(TEXT("Stored transition updates the displayed count"), Player->GetStoredPickupCount(), 1);
    TestNull(TEXT("Store clears the inspected slot"), Player->GetInspectedPickup());
    TestEqual(TEXT("The item is actually in the stored state"), Item->GetPickupState(), EECPPickupState::Stored);
    Player->DropAllOwnedPickups();
    TestEqual(TEXT("Dropping inventory clears the displayed count"), Player->GetStoredPickupCount(), 0);
    TestEqual(TEXT("Dropped inventory returns to the world"), Item->GetPickupState(), EECPPickupState::World);
    return true;
}

#endif
