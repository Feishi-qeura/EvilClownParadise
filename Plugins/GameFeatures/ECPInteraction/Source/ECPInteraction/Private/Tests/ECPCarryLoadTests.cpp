#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/ECPPhysicalCarryComponent.h"
#include "Components/ECPCarryContactProjection.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/Engine.h"
#include "CoreGlobals.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "UObject/Script.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Materials/Material.h"
#include <limits>

namespace ECPCarryTests
{
struct FFixture
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	AECPPlayerBase* Player = nullptr;
	AECPPickupItem* Item = nullptr;
	UStaticMeshComponent* Body = nullptr;
	uint64 InitialFrame = GFrameCounter;
	uint64 Frames = 0;

	FFixture()
	{
		if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		Player = World->SpawnActor<AECPPlayerBase>();
		UCameraComponent* Camera = NewObject<UCameraComponent>(Player, TEXT("Camera"));
		Player->AddInstanceComponent(Camera);
		Camera->SetupAttachment(Player->GetRootComponent());
		Camera->SetRelativeLocation(FVector(0,0,100));
		Camera->RegisterComponent();
		USkeletalMeshComponent* Carry = NewObject<USkeletalMeshComponent>(Player, TEXT("SkeletalMesh"));
		Player->AddInstanceComponent(Carry);
		Carry->SetupAttachment(Camera);
		Carry->SetRelativeRotation(FRotationMatrix::MakeFromZ(FVector::ForwardVector).Rotator());
		Carry->RegisterComponent();
		APlayerController* Controller = World->SpawnActor<APlayerController>();
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Player);
		Player->DispatchBeginPlay();
		Item = AddItem(100.f);
		Body = CastChecked<UStaticMeshComponent>(Item->GetRootComponent());
		World->BeginPlay();
		World->GetWorldSettings()->NotifyBeginPlay();
	}
	AECPPickupItem* AddItem(float Mass)
	{
		AECPPickupItem* Added = World->SpawnActorDeferred<AECPPickupItem>(AECPPickupItem::StaticClass(), FTransform(FVector(160.f,0.f,100.f)));
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Added, TEXT("CarryTestBody"));
		Added->AddInstanceComponent(Mesh);
		Added->SetRootComponent(Mesh);
		Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Mesh->SetCollisionResponseToAllChannels(ECR_Block);
		Mesh->SetCollisionObjectType(ECC_GameTraceChannel1);
		Mesh->RegisterComponent();
		Mesh->SetMassOverrideInKg(NAME_None, Mass);
		Added->FinishSpawning(FTransform(FVector(160.f,0.f,100.f)));
		if (!Added->HasActorBegunPlay()) Added->DispatchBeginPlay();
		// A dynamically registered deferred root can still have a pending kinematic transform when enabled.
		// Match its actual Chaos pose to the fixture spawn pose before testing physics, not just its render transform.
		Added->SetActorLocation(FVector(160.f,0.f,100.f), false, nullptr, ETeleportType::TeleportPhysics);
		return Added;
	}
	void Step()
	{
		TGuardValue<uint64> Guard(GFrameCounter, InitialFrame + ++Frames);
		World->Tick(LEVELTICK_All, 1.f/60.f);
	}
	~FFixture()
	{
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
		if (GEngine) GEngine->DestroyWorldContext(World);
	}
};
}

// Regresses the reported failure: disabling collisions/teleporting on pickup must fail this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryCollisionTest, "ECP.Pickup.Load.HeldCollisionAndNoTeleport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryCollisionTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	const FVector Start = Fixture.Item->GetActorLocation();
	TestTrue(TEXT("Authority accepts a valid pickup"), Fixture.Item->TryEnterPickup(Fixture.Player));
	TestTrue(TEXT("Held root keeps physics"), Fixture.Body->IsSimulatingPhysics());
	TestEqual(TEXT("Held root keeps query and physical collision"), Fixture.Body->GetCollisionEnabled(), ECollisionEnabled::QueryAndPhysics);
	TestTrue(TEXT("Pickup does not teleport the object through obstacles"), Fixture.Item->GetActorLocation().Equals(Start, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryLoadAttributeTest, "ECP.Pickup.Load.CharacterAttributes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryLoadAttributeTest::RunTest(const FString& Parameters)
{
	AECPPlayerBase* Player = NewObject<AECPPlayerBase>();
	FFloatProperty* Capacity = FindFProperty<FFloatProperty>(Player->GetClass(), TEXT("MaxCarryWeightKg"));
	FFloatProperty* BodyMass = FindFProperty<FFloatProperty>(Player->GetClass(), TEXT("BodyWeightKg"));
	if (TestNotNull(TEXT("Role exposes an expandable load capacity"), Capacity))
	{
		TestEqual(TEXT("Capacity defaults to 200kg"), Capacity->GetPropertyValue_InContainer(Player), 200.f);
	}
	if (TestNotNull(TEXT("Role exposes body mass for teammate carrying"), BodyMass))
	{
		TestEqual(TEXT("Body mass defaults to 80kg"), BodyMass->GetPropertyValue_InContainer(Player), 80.f);
	}
	TestNotNull(TEXT("Role exposes current carried weight"), Player->FindFunction(TEXT("GetCarriedWeightKg")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryRagdollContractTest, "ECP.Pickup.Load.RagdollContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryRagdollContractTest::RunTest(const FString& Parameters)
{
	UClass* Contract = FindObject<UClass>(nullptr, TEXT("/Script/ECPFoundation.ECPCarryTarget"));
	if (TestNotNull(TEXT("Shared carry contract belongs to Foundation"), Contract))
	{
		TestTrue(TEXT("Characters implement the shared carry target"), AECPPlayerBase::StaticClass()->ImplementsInterface(Contract));
		TestTrue(TEXT("Small and medium items share the carry target"), AECPPickupItem::StaticClass()->ImplementsInterface(Contract));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPLoadMovementTest, "ECP.Pickup.Load.MovementAndCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPLoadMovementTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	AECPPlayerBase* Player = Fixture.Player;
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	const float Jump = Movement->JumpZVelocity;
	Player->SetCarriedWeightKg(100.f);
	TestEqual(TEXT("100kg halves walking"), Movement->MaxWalkSpeed, Player->WalkSpeed * 0.5f);
	TestEqual(TEXT("100kg halves jump velocity"), Movement->JumpZVelocity, Jump * 0.5f);
	Player->SetRunning(true);
	TestEqual(TEXT("Running cannot bypass load"), Movement->MaxWalkSpeed, Player->RunSpeed * 0.5f);
	Player->SetCarriedWeightKg(150.f);
	TestEqual(TEXT("150kg leaves one quarter speed"), Movement->MaxWalkSpeed, Player->RunSpeed * 0.25f);
	TestEqual(TEXT("Crouching also uses load"), Movement->MaxWalkSpeedCrouched, Player->CrouchSpeed * 0.25f);
	Player->SetCarriedWeightKg(199.f);
	TestFalse(TEXT("199kg is still below capacity"), Player->IsOverloaded());
	Player->SetCarriedWeightKg(200.f);
	TestTrue(TEXT("Exactly 200kg blocks movement"), Player->IsOverloaded());
	TestEqual(TEXT("Overload has zero walking speed"), Movement->MaxWalkSpeed, 0.f);
	Player->Jump();
	TestFalse(TEXT("Overload rejects new jump intent"), Player->bPressedJump);
	Movement->Velocity = FVector(30.f, 20.f, -120.f);
	Player->SetCarriedWeightKg(250.f);
	TestEqual(TEXT("Overload preserves falling velocity"), Movement->Velocity.Z, -120.0);
	Player->SetCarriedWeightKg(-1.f);
	Player->SetCarriedWeightKg(std::numeric_limits<float>::quiet_NaN());
	TestEqual(TEXT("Invalid load does not overwrite authority"), Player->GetCarriedWeightKg(), 250.f);
	Player->SetMaxCarryWeightKg(300.f);
	TestFalse(TEXT("Expanded capacity restores movement"), Player->IsOverloaded());
	TestTrue(TEXT("Expanded capacity uses same base speed"), FMath::IsNearlyEqual(Movement->MaxWalkSpeed, Player->RunSpeed / 6.f, 0.001f));
	Player->SetMaxCarryWeightKg(0.f);
	TestEqual(TEXT("Invalid capacity is rejected"), Player->MaxCarryWeightKg, 300.f);
	Player->SetCarriedWeightKg(0.f);
	TestEqual(TEXT("Unloading restores run speed without compounded multipliers"), Movement->MaxWalkSpeed, Player->RunSpeed);
	TestEqual(TEXT("Unloading restores base jump"), Movement->JumpZVelocity, Jump);
	Player->SetRole(ROLE_AutonomousProxy);
	Player->SetCarriedWeightKg(100.f);
	Player->SetMaxCarryWeightKg(100.f);
	TestEqual(TEXT("Client cannot change load"), Player->GetCarriedWeightKg(), 0.f);
	TestEqual(TEXT("Client cannot change capacity"), Player->MaxCarryWeightKg, 300.f);
	Player->SetRole(ROLE_Authority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPLoadInventoryTest, "ECP.Pickup.Load.InventoryConservationAndUnloading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPLoadInventoryTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	AECPPickupItem* Last = nullptr;
	for (int32 Index=0; Index<3; ++Index)
	{
		Last = Index == 0 ? Fixture.Item : Fixture.AddItem(100.f);
		TestTrue(TEXT("Pickup remains allowed above capacity"), Last->TryEnterPickup(Fixture.Player));
		Fixture.Player->CompleteQueuedPickup(Last);
		TestEqual(TEXT("Held weight is included"), Fixture.Player->GetCarriedWeightKg(), 100.f * (Index+1));
		Fixture.Player->RequestStorePickup();
		TestEqual(TEXT("Storage preserves total weight"), Fixture.Player->GetCarriedWeightKg(), 100.f * (Index+1));
		TestEqual(TEXT("Storage preserves confirmed physical mass"), Last->GetPickupMassKg(), 100.f);
	}
	TestTrue(TEXT("300kg inventory is overloaded"), Fixture.Player->IsOverloaded());
	Fixture.Player->RequestDropPickup();
	TestEqual(TEXT("Empty hand G unloads the latest stored object"), Last->GetPickupState(), EECPPickupState::World);
	TestEqual(TEXT("Unloading removes exactly its weight"), Fixture.Player->GetCarriedWeightKg(), 200.f);
	Fixture.Player->RequestDropPickup();
	TestEqual(TEXT("A second unload restores walking"), Fixture.Player->GetCarriedWeightKg(), 100.f);
	Fixture.Item->Destroy();
	TestEqual(TEXT("Destroying a stored item removes its weight"), Fixture.Player->GetCarriedWeightKg(), 0.f);
	TestEqual(TEXT("Destroyed entries do not remain in inventory"), Fixture.Player->GetStoredPickupCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryWallTest, "ECP.Pickup.Load.PhysicsStopsAtWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryWallTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	AActor* Wall = Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Shape = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(Shape);
	Wall->SetRootComponent(Shape);
	Shape->SetBoxExtent(FVector(5.f, 150.f, 150.f));
	Shape->SetCollisionProfileName(TEXT("BlockAll"));
	Shape->SetCollisionObjectType(ECC_WorldStatic);
	Shape->RegisterComponent();
	Wall->SetActorLocation(FVector(100.f, 0.f, 100.f));
	TestTrue(TEXT("Physical pickup begins at the object"), Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	for (int32 Frame=0; Frame<120; ++Frame) Fixture.Step();
	FVector HandleLocation;
	FRotator HandleRotation;
	Fixture.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(HandleLocation, HandleRotation);
	AddInfo(FString::Printf(TEXT("Wall result body=%s extent=%s handle=%s holding=%d"), *Fixture.Body->GetComponentLocation().ToString(),
		*Fixture.Body->Bounds.BoxExtent.ToString(), *HandleLocation.ToString(), Fixture.Player->GetPhysicalCarryComponent()->GetCarryTarget()!=nullptr));
	TestTrue(TEXT("A held body cannot cross the solid wall to its camera goal"), Fixture.Body->GetComponentLocation().X >= 153.f);
	TestTrue(TEXT("World collision stays enabled while holding"), Fixture.Body->IsSimulatingPhysics());
	Fixture.Player->RequestPlacePickup();
	TestEqual(TEXT("Right click releases the actual physics position"), Fixture.Item->GetPickupState(), EECPPickupState::World);
	TestTrue(TEXT("Release does not move it through the wall"), Fixture.Item->GetActorLocation().X >= 153.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryTeammateTest, "ECP.Pickup.Load.TeammatePickupAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryTeammateTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, -1);
	ECPCarryTests::FFixture Fixture;
	Fixture.Item->SetActorLocation(FVector(10000.f,0.f,0.f), false, nullptr, ETeleportType::TeleportPhysics);
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	AActor* Floor = Fixture.World->SpawnActor<AActor>();
	UBoxComponent* FloorShape = NewObject<UBoxComponent>(Floor);
	Floor->AddInstanceComponent(FloorShape);
	Floor->SetRootComponent(FloorShape);
	FloorShape->SetBoxExtent(FVector(3000.f,3000.f,50.f));
	FloorShape->SetCollisionProfileName(TEXT("BlockAll"));
	FloorShape->RegisterComponent();
	Floor->SetActorLocation(FVector(0,0,-50), false, nullptr, ETeleportType::TeleportPhysics);
	UClass* Class = LoadClass<AECPPlayerBase>(nullptr, TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AECPPlayerBase* Target = Class ? Fixture.World->SpawnActor<AECPPlayerBase>(Class, FVector(180,0,96), FRotator::ZeroRotator, Spawn) : nullptr;
	if (!TestNotNull(TEXT("Real teammate blueprint spawns"), Target)) return false;
	TestFalse(TEXT("Standing teammates cannot be picked up"), Target->CanBeCarriedBy(Fixture.Player));
	for (int32 Frame=0; Frame<120; ++Frame) Fixture.Step();
	Target->StartRagdoll();
	for (int32 Frame=0; Frame<90; ++Frame) Fixture.Step();
	TestTrue(TEXT("Target entered ragdoll"), Target->IsRagdollActive());
	const FVector Direction = (Target->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone)
		- Fixture.Player->GetPickupInspectionComponent()->GetComponentLocation()).GetSafeNormal();
	Fixture.Player->GetController()->SetControlRotation(Direction.Rotation());
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldRotation(FRotationMatrix::MakeFromZ(Direction).Rotator());
	Fixture.Player->RequestPickup();
	for (int32 Frame=0; Frame<3; ++Frame) Fixture.Step();
	if (!TestTrue(TEXT("F uses the real server scan and queue to carry the teammate"), Fixture.Player->GetCarriedTeammate() == Target)) return false;
	TestTrue(TEXT("Authority records the unique carrier"), Target->GetCarryHolder() == Fixture.Player);
	TestEqual(TEXT("Body mass contributes to total load"), Fixture.Player->GetCarriedWeightKg(), 80.f);
	TestFalse(TEXT("A carried teammate cannot recover"), Target->TryRecoverFromRagdoll());
	// First let the initial grab reach its goal; its start pose is farther away than the hand goal.
	for (int32 Frame=0; Frame<120; ++Frame) Fixture.Step();
	const FVector CarriedStart = Target->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone);
	Fixture.Player->SetActorLocation(Fixture.Player->GetActorLocation() + FVector(60,0,0), false, nullptr, ETeleportType::TeleportPhysics);
	for (int32 Frame=0; Frame<120; ++Frame) Fixture.Step();
	TestTrue(TEXT("Physical teammate follows the carrier across floor contacts"),
		Target->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone).X > CarriedStart.X + 30.f);
	TestFalse(TEXT("No self-carry or cycles"), Target->TryBeginCarry(Target));
	Fixture.Player->RequestStorePickup();
	Fixture.Player->RequestEquipPickup();
	TestTrue(TEXT("Equipment/storage do not consume a teammate"), Fixture.Player->GetCarriedTeammate() == Target);
	Target->SetBodyWeightKg(120.f);
	TestEqual(TEXT("Changing body mass refreshes carrier load"), Fixture.Player->GetCarriedWeightKg(), 120.f);
	Fixture.Player->RequestDropPickup();
	TestNull(TEXT("G releases the teammate"), Target->GetCarryHolder());
	TestNull(TEXT("G clears the carrier slot"), Fixture.Player->GetCarriedTeammate());
	TestEqual(TEXT("Release removes body weight"), Fixture.Player->GetCarriedWeightKg(), 0.f);
	TestTrue(TEXT("Released teammate remains a ragdoll"), Target->IsRagdollActive());
	for (int32 Frame=0; Frame<180; ++Frame) Fixture.Step();
	TestTrue(TEXT("After release and settling the original recovery checks can succeed"), Target->TryRecoverFromRagdoll());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCleanupBlockedPoseTest, "ECP.Pickup.Load.Cleanup.BlockedPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCleanupBlockedPoseTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	TestTrue(TEXT("Enter physical pickup"), Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	const FTransform Actual = Fixture.Item->GetActorTransform();
	Fixture.Player->DropAllOwnedPickups();
	TestTrue(TEXT("Ragdoll/death cleanup preserves actual blocked physical pose"), Fixture.Item->GetActorTransform().Equals(Actual, 0.01f));
	TestEqual(TEXT("Cleanup removes load"), Fixture.Player->GetCarriedWeightKg(), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCleanupOffsetPivotTest, "ECP.Pickup.Load.Cleanup.OffsetPivotRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCleanupOffsetPivotTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, -1);
	ECPCarryTests::FFixture Fixture;
	Fixture.Item->SetActorLocation(FVector(10000,0,0), false, nullptr, ETeleportType::TeleportPhysics);
	UClass* Class = LoadClass<AECPPickupItem>(nullptr, TEXT("/Game/Project/Interaction/Items/BP_Smallitems.BP_Smallitems_C"));
	AECPPickupItem* OffsetItem = Class ? Fixture.World->SpawnActor<AECPPickupItem>(Class) : nullptr;
	if (!TestNotNull(TEXT("Load real offset-pivot small item"), OffsetItem)) return false;
	UPrimitiveComponent* Body = OffsetItem->GetCarryBody();
	FVector Offset = Body->Bounds.Origin - OffsetItem->GetActorLocation();
	if (!TestTrue(TEXT("Fixture actually has an off-center pivot"), Offset.Size() > 1.f)) return false;
	OffsetItem->SetActorRotation(FQuat::FindBetweenNormals(Offset.GetSafeNormal(), FVector::ForwardVector) * OffsetItem->GetActorQuat(), ETeleportType::TeleportPhysics);
	Body->UpdateBounds();
	Offset = Body->Bounds.Origin - OffsetItem->GetActorLocation();
	TestTrue(TEXT("Offset item enters pickup"), OffsetItem->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(OffsetItem);
	Fixture.Player->RequestStorePickup();
	const float Spacing = FMath::Max(130.f, Body->Bounds.BoxExtent.GetMax()*2.f + 30.f);
	AActor* Wall = Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Shape = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(Shape);
	Wall->SetRootComponent(Shape);
	Shape->SetBoxExtent(FVector(1.f,300.f,300.f));
	Shape->SetCollisionProfileName(TEXT("BlockAll"));
	Shape->RegisterComponent();
	Wall->SetActorLocation(Fixture.Player->GetPickupInspectionComponent()->GetComponentLocation()
		+ FVector(Spacing + Body->Bounds.BoxExtent.X + Offset.X*0.5f,0,0), false, nullptr, ETeleportType::TeleportPhysics);
	Fixture.Player->RequestDropPickup();
	TestEqual(TEXT("An alternative safe release remains available"), OffsetItem->GetPickupState(), EECPPickupState::World);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPOffsetRelease), false, Fixture.Player);
	Query.AddIgnoredActor(OffsetItem);
	TestFalse(TEXT("Actual offset bounds do not overlap the wall after G"), Fixture.World->OverlapBlockingTestByChannel(
		Body->Bounds.Origin, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeBox(Body->Bounds.BoxExtent), Query));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCleanupMultipleInventoryTest, "ECP.Pickup.Load.Cleanup.MultipleInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCleanupMultipleInventoryTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	AECPPickupItem* Second = Fixture.AddItem(80.f);
	for (AECPPickupItem* Item : {Fixture.Item, Second})
	{
		TestTrue(TEXT("Pickup for cleanup fixture"), Item->TryEnterPickup(Fixture.Player));
		Fixture.Player->CompleteQueuedPickup(Item);
		Fixture.Player->RequestStorePickup();
	}
	TestEqual(TEXT("Two items are stored before cleanup"), Fixture.Player->GetStoredPickupCount(), 2);
	Fixture.Player->DropAllOwnedPickups();
	TestEqual(TEXT("First stored item is released"), Fixture.Item->GetPickupState(), EECPPickupState::World);
	TestEqual(TEXT("Second stored item is released despite synchronous notifications"), Second->GetPickupState(), EECPPickupState::World);
	TestEqual(TEXT("Cleanup empties inventory"), Fixture.Player->GetStoredPickupCount(), 0);
	TestEqual(TEXT("Cleanup empties load"), Fixture.Player->GetCarriedWeightKg(), 0.f);
	Fixture.Player->DropAllOwnedPickups();
	TestEqual(TEXT("Repeated cleanup stays idempotent"), Fixture.Player->GetCarriedWeightKg(), 0.f);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryHolderClearanceTest, "ECP.Pickup.Regression.LargeItemAvoidsHolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryHolderClearanceTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	Fixture.Item->SetActorScale3D(FVector(3.f));
	Fixture.Item->SetActorLocation(FVector(330,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	TestTrue(TEXT("Large object enters physical pickup"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	for(int32 Frame=0;Frame<150;++Frame) Fixture.Step();
	const float NearEdge=Fixture.Body->Bounds.Origin.X-Fixture.Body->Bounds.BoxExtent.X;
	TestTrue(TEXT("Held object cannot occupy the holder capsule despite disabled self-contact forces"),
		NearEdge >= Fixture.Player->GetCapsuleComponent()->GetScaledCapsuleRadius()-2.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryOtherCharacterTest, "ECP.Pickup.Regression.BlocksVisibilityIgnoringCharacter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryOtherCharacterTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AECPPlayerBase* Other=Fixture.World->SpawnActor<AECPPlayerBase>(FVector(130,0,100),FRotator::ZeroRotator,Spawn);
	Other->GetCharacterMovement()->DisableMovement();
	Other->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility,ECR_Ignore);
	Fixture.Body->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);
	Fixture.Item->SetActorLocation(FVector(250,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	TestTrue(TEXT("Valid nonpenetrating pickup starts beyond the other character"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	for(int32 Frame=0;Frame<150;++Frame) Fixture.Step();
	const float NearEdge=Fixture.Body->Bounds.Origin.X-Fixture.Body->Bounds.BoxExtent.X;
	TestTrue(TEXT("Pawn ignoring Visibility still blocks held object geometry"),
		NearEdge >= Other->GetActorLocation().X+Other->GetCapsuleComponent()->GetScaledCapsuleRadius()-2.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryReplicaCorrectionTest, "ECP.Pickup.Regression.AuthorityPoseCannotStickAtClientWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryReplicaCorrectionTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	TestTrue(TEXT("Establish valid held lifecycle"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	AActor* Wall=Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Shape=NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(Shape); Wall->SetRootComponent(Shape);
	Shape->SetBoxExtent(FVector(5,500,500)); Shape->SetCollisionProfileName(TEXT("BlockAll")); Shape->RegisterComponent();
	Wall->SetActorLocation(FVector(100,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	Fixture.Player->SetRole(ROLE_AutonomousProxy);
	Fixture.Item->SetRole(ROLE_SimulatedProxy);
	FStructProperty* Property=FindFProperty<FStructProperty>(Fixture.Item->GetClass(),TEXT("AttachmentState"));
	FECPPickupNetState* State=Property->ContainerPtrToValuePtr<FECPPickupNetState>(Fixture.Item);
	State->HeldTransform=FTransform(FRotator(0,55,0),FVector(320,0,100));
	++State->Revision;
	Fixture.Body->SetSimulatePhysics(false);
	Fixture.Item->SetActorLocation(FVector(0,0,100));
	{
		FEditorScriptExecutionGuard Guard;
		Fixture.Item->ProcessEvent(Fixture.Item->FindFunction(TEXT("OnRep_AttachmentState")),nullptr);
	}
	for(int32 Frame=0;Frame<120;++Frame) Fixture.Item->Tick(1.f/60.f);
	TestTrue(TEXT("A stale client pose across a wall converges to actual authority position"),Fixture.Item->GetActorLocation().Equals(State->HeldTransform.GetLocation(),0.1f));
	TestTrue(TEXT("Actual authority rotation also converges"),Fixture.Item->GetActorQuat().Equals(State->HeldTransform.GetRotation(),0.001f));
	Fixture.Player->SetRole(ROLE_Authority); Fixture.Item->SetRole(ROLE_Authority);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryOwnerPredictionTest,"ECP.Pickup.Regression.ClientHolderMovementIgnoreRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryOwnerPredictionTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	TestTrue(TEXT("Create authority held state"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	Fixture.Player->GetPhysicalCarryComponent()->StopCarry();
	Fixture.Player->SetRole(ROLE_AutonomousProxy); Fixture.Item->SetRole(ROLE_SimulatedProxy);
	FStructProperty* Property=FindFProperty<FStructProperty>(Fixture.Item->GetClass(),TEXT("AttachmentState"));
	FECPPickupNetState* State=Property->ContainerPtrToValuePtr<FECPPickupNetState>(Fixture.Item);
	++State->Revision;
	UFunction* Notify=Fixture.Item->FindFunction(TEXT("OnRep_AttachmentState"));
	{
		FEditorScriptExecutionGuard Guard;
		Fixture.Item->ProcessEvent(Notify,nullptr);
	}
	TestFalse(TEXT("Owning client retains movement blocking against its held object"),Fixture.Player->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(Fixture.Item));
	State->State=EECPPickupState::World; State->Holder=nullptr; ++State->Revision;
	{
		FEditorScriptExecutionGuard Guard;
		Fixture.Item->ProcessEvent(Notify,nullptr);
	}
	TestFalse(TEXT("Release restores the owner's normal collision queries"),Fixture.Player->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(Fixture.Item));
	Fixture.Player->SetRole(ROLE_Authority); Fixture.Item->SetRole(ROLE_Authority);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryPinnedHolderTest,"ECP.Pickup.Regression.HolderCannotWalkThroughPinnedItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryPinnedHolderTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	TestTrue(TEXT("Claim the physical object"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	AActor* Wall=Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Shape=NewObject<UBoxComponent>(Wall); Wall->AddInstanceComponent(Shape); Wall->SetRootComponent(Shape);
	Shape->SetBoxExtent(FVector(5,500,500)); Shape->SetCollisionProfileName(TEXT("BlockAll")); Shape->RegisterComponent();
	Wall->SetActorLocation(FVector(225,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	FHitResult Hit;
	Fixture.Player->GetCapsuleComponent()->MoveComponent(FVector(180,0,0),FQuat::Identity,true,&Hit);
	const float Front=Fixture.Player->GetActorLocation().X+Fixture.Player->GetCapsuleComponent()->GetScaledCapsuleRadius();
	TestTrue(TEXT("Holder movement stops before its held body even with a wall behind it"),Front <= Fixture.Body->Bounds.Origin.X-Fixture.Body->Bounds.BoxExtent.X+1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryPawnEscapeTest,"ECP.Pickup.Regression.EscapeInitialPawnBoundsOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryPawnEscapeTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture;
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	Fixture.Item->SetActorScale3D(FVector(3.f,0.2f,0.2f));
	Fixture.Item->SetActorRotation(FRotator(0,45,0),ETeleportType::TeleportPhysics);
	Fixture.Item->SetActorLocation(FVector(360,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AECPPlayerBase* Other=Fixture.World->SpawnActor<AECPPlayerBase>(FVector(250,100,100),FRotator::ZeroRotator,Spawn);
	Other->GetCharacterMovement()->DisableMovement();
	TestTrue(TEXT("Rotated physical bar can be held"),Fixture.Item->TryEnterPickup(Fixture.Player));
	Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	Fixture.Player->GetPickupInspectionComponent()->SetWorldLocation(FVector(400,-200,100));
	for(int32 Frame=0;Frame<150;++Frame) Fixture.Step();
	TestTrue(TEXT("A conservative initial Pawn overlap must permit motion away"),Fixture.Item->GetActorLocation().Y < -80.f);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryLimbHitTest,"ECP.Pickup.RagdollFocus.GrabsRayHitLimb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryLimbHitTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, -1);
	ECPCarryTests::FFixture Fixture;
	Fixture.Item->SetActorLocation(FVector(10000,0,0),false,nullptr,ETeleportType::TeleportPhysics);
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	UClass* Class=LoadClass<AECPPlayerBase>(nullptr,TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AECPPlayerBase* Target=Class ? Fixture.World->SpawnActor<AECPPlayerBase>(Class,FVector(180,0,130),FRotator::ZeroRotator,Spawn) : nullptr;
	if (!TestNotNull(TEXT("Real teammate"),Target)) return false;
	Target->StartRagdoll(); Target->GetMesh()->SetEnableGravity(false);
	FName LimbBone;
	for (USkeletalBodySetup* Setup : Target->GetMesh()->GetPhysicsAsset()->SkeletalBodySetups)
	{
		const FString Name=Setup->BoneName.ToString().ToLower();
		if (Name.Contains(TEXT("lowerleg")) || Name.Contains(TEXT("calf"))) { LimbBone=Setup->BoneName; break; }
	}
	if (!TestFalse(TEXT("Fixture has a physical leg body"),LimbBone.IsNone())) return false;
	AddInfo(FString::Printf(TEXT("Aim at physical limb %s"),*LimbBone.ToString()));
	const FVector Limb=Target->GetMesh()->GetSocketLocation(LimbBone);
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldLocation(Limb-FVector(80,0,0));
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldRotation(FRotationMatrix::MakeFromZ(FVector::ForwardVector).Rotator());
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPLimbTest),false,Fixture.Player);
	FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_PhysicsBody); Objects.AddObjectTypesToQuery(ECC_Pawn);
	Fixture.World->LineTraceSingleByObjectType(Hit,Limb-FVector(80,0,0),Limb+FVector(80,0,0),Objects,Query);
	AddInfo(FString::Printf(TEXT("Limb ray position=%s hit=%s bone=%s"),*Limb.ToString(),*GetNameSafe(Hit.GetActor()),*Hit.BoneName.ToString()));
	if (!TestTrue(TEXT("Ray hits a limb rather than the pelvis"),Hit.GetActor()==Target && !Hit.BoneName.IsNone() && Hit.BoneName!=Target->RagdollPelvisBone)) return false;
	TestTrue(TEXT("Direct claim for surface anchor check"),Target->TryBeginCarry(Fixture.Player));
	TestTrue(TEXT("Grab accepts the real skeletal hit"),Fixture.Player->GetPhysicalCarryComponent()->StartCarry(Target,&Hit));
	FVector GripPosition; FRotator GripRotation;
	Fixture.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(GripPosition,GripRotation);
	TestTrue(TEXT("Initial handle anchor is the ray surface point"),GripPosition.Equals(Hit.ImpactPoint,0.1f));
	Fixture.Player->GetPhysicalCarryComponent()->StopCarry(); Target->EndCarry(Fixture.Player);
	Fixture.Player->RequestPickup();
	for(int32 Frame=0;Frame<3;++Frame) Fixture.Step();
	TestTrue(TEXT("Real F request carries the teammate"),Fixture.Player->GetCarriedTeammate()==Target);
	TestEqual(TEXT("Physics handle grabs the ray-hit skeletal body"),Fixture.Player->GetPhysicalCarryComponent()->GrabbedBoneName,Hit.BoneName);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryRagdollOutlineTest,"ECP.Pickup.RagdollFocus.OutlineAndRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryRagdollOutlineTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"), EAutomationExpectedErrorFlags::Contains, -1);
	ECPCarryTests::FFixture Fixture;
	Fixture.Item->SetActorLocation(FVector(10000,0,0),false,nullptr,ETeleportType::TeleportPhysics);
	Fixture.Player->GetCharacterMovement()->DisableMovement();
	UClass* Class=LoadClass<AECPPlayerBase>(nullptr,TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AECPPlayerBase* Target=Class ? Fixture.World->SpawnActor<AECPPlayerBase>(Class,FVector(180,0,130),FRotator::ZeroRotator,Spawn) : nullptr;
	if (!TestNotNull(TEXT("Real teammate"),Target)) return false;
	Target->StartRagdoll(); Target->GetMesh()->SetEnableGravity(false);
	UMaterialInterface* Original=Target->GetMesh()->GetOverlayMaterial();
	const FVector Pelvis=Target->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone);
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldLocation(Pelvis-FVector(80,0,0));
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldRotation(FRotationMatrix::MakeFromZ(FVector::ForwardVector).Rotator());
	for(int32 Frame=0;Frame<12;++Frame) Fixture.Step();
	TestTrue(TEXT("A locally aimed eligible ragdoll gets outline overlay"),Target->GetMesh()->GetOverlayMaterial()!=Original && Target->GetMesh()->GetOverlayMaterial()!=nullptr);
	Fixture.Player->GetPickupAttachmentComponent()->SetWorldRotation(FRotationMatrix::MakeFromZ(-FVector::ForwardVector).Rotator());
	for(int32 Frame=0;Frame<12;++Frame) Fixture.Step();
	TestTrue(TEXT("Looking away restores the original overlay"),Target->GetMesh()->GetOverlayMaterial()==Original);
	Target->GetMesh()->SetSimulatePhysics(false);
	Target->SetRole(ROLE_SimulatedProxy);
	TestTrue(TEXT("Client eligibility uses replicated ragdoll state despite no local simulation"),Target->CanBeCarriedBy(Fixture.Player));
	Target->SetRole(ROLE_Authority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryOutlineMaterialTest,"ECP.Pickup.RagdollFocus.MaterialSupportsSkeletalMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryOutlineMaterialTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Outline=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Project/Interaction/Materials/Outline/MI_RagdollOutline.MI_RagdollOutline"));
	if (!TestNotNull(TEXT("Existing outline material"),Outline)) return false;
	TestTrue(TEXT("Outline is cooked for skeletal mesh usage"),Outline->GetMaterial()->bUsedWithSkeletalMesh);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryPawnContactEscapeTest,"ECP.Pickup.Contact.EscapeWithoutFalling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryPawnContactEscapeTest::RunTest(const FString& Parameters)
{
	for (float Scale : {0.3f,1.5f})
	{
		ECPCarryTests::FFixture Fixture;
		Fixture.Player->GetCharacterMovement()->DisableMovement();
		Fixture.Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Fixture.Item->SetActorScale3D(FVector(Scale));
		Fixture.Body->SetMassOverrideInKg(NAME_None,Scale<1.f ? 10.f : 80.f); Fixture.Item->RefreshPickupMass();
		FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AECPPlayerBase* Other=Fixture.World->SpawnActor<AECPPlayerBase>(FVector(250,0,100),FRotator::ZeroRotator,Spawn);
		Other->GetCharacterMovement()->DisableMovement();
		const float Diagonal=(Other->GetCapsuleComponent()->GetScaledCapsuleRadius()+50.f*Scale+2.f)/FMath::Sqrt(2.f);
		const FVector Start=Other->GetActorLocation()+FVector(Diagonal,Diagonal,0);
		Fixture.Item->SetActorLocation(Start,false,nullptr,ETeleportType::TeleportPhysics);
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPContactEscape),false,Fixture.Player); Query.AddIgnoredActor(Fixture.Item);
		FHitResult Hit;
		Fixture.World->SweepSingleByObjectType(Hit,Fixture.Body->Bounds.Origin,Fixture.Body->Bounds.Origin+FVector(100,40,0),
			FQuat::Identity,FCollisionObjectQueryParams(ECC_Pawn),FCollisionShape::MakeBox(Fixture.Body->Bounds.BoxExtent),Query);
		TestTrue(TEXT("Conservative bounds begin inside the character query volume"),Hit.bStartPenetrating && Hit.GetActor()==Other);
		TestTrue(TEXT("Claim a held sphere"),Fixture.Item->TryEnterPickup(Fixture.Player)); Fixture.Player->CompleteQueuedPickup(Fixture.Item);
		Fixture.Player->GetPickupInspectionComponent()->SetWorldLocation(Start+FVector(80,80,0));
		for(int32 Frame=0;Frame<40;++Frame) Fixture.Step();
		AddInfo(FString::Printf(TEXT("Escape scale=%.1f start=%s final=%s"),Scale,*Start.ToString(),*Fixture.Item->GetActorLocation().ToString()));
		TestTrue(TEXT("Moving away from character recovers before falling to its feet"),Fixture.Item->GetActorLocation().X>Start.X+30.f);
		TestTrue(TEXT("Held item keeps height while escaping"),Fixture.Item->GetActorLocation().Z>Start.Z-15.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryPawnContactHeightTest,"ECP.Pickup.Contact.KeepsHeightAndReturns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryPawnContactHeightTest::RunTest(const FString& Parameters)
{
	for (float Scale : {0.3f,1.5f})
	{
		ECPCarryTests::FFixture Fixture;
		Fixture.Player->GetCharacterMovement()->DisableMovement();
		Fixture.Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Fixture.Item->SetActorScale3D(FVector(Scale));
		Fixture.Body->SetMassOverrideInKg(NAME_None,Scale<1.f ? 10.f : 80.f); Fixture.Item->RefreshPickupMass();
		FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AECPPlayerBase* Other=Fixture.World->SpawnActor<AECPPlayerBase>(FVector(400,0,100),FRotator::ZeroRotator,Spawn);
		Other->GetCharacterMovement()->DisableMovement();
		const float Edge=Other->GetActorLocation().X-Other->GetCapsuleComponent()->GetScaledCapsuleRadius()-50.f*Scale;
		Fixture.Item->SetActorLocation(FVector(Edge-2.f,0,100),false,nullptr,ETeleportType::TeleportPhysics);
		TestTrue(TEXT("Claim sphere in front of character"),Fixture.Item->TryEnterPickup(Fixture.Player)); Fixture.Player->CompleteQueuedPickup(Fixture.Item);
		Fixture.Player->GetPickupInspectionComponent()->SetWorldLocation(FVector(420,0,150));
		for(int32 Frame=0;Frame<60;++Frame) Fixture.Step();
		AddInfo(FString::Printf(TEXT("Contact scale=%.1f final=%s"),Scale,*Fixture.Item->GetActorLocation().ToString()));
		TestTrue(TEXT("Blocked item still follows vertical goal rather than sliding down"),Fixture.Item->GetActorLocation().Z>120.f);
		TestTrue(TEXT("Character continues blocking the physical item"),Fixture.Item->GetActorLocation().X<=Edge+3.f);
		Other->SetActorLocation(FVector(400,500,100),false,nullptr,ETeleportType::TeleportPhysics);
		for(int32 Frame=0;Frame<120;++Frame) Fixture.Step();
		TestTrue(TEXT("Removing obstruction restores inspection pose"),FVector::Dist(Fixture.Item->GetActorLocation(),Fixture.Item->GetInspectionTargetWorldTransform().GetLocation())<10.f);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryCompoundContactTest,"ECP.Pickup.Contact.PreservesAllBlockers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryCompoundContactTest::RunTest(const FString& Parameters)
{
	const TArray<ECPCarryContacts::FPlaneConstraint> Planes={{FVector(1,0,0),0.0},{FVector(-1,-1,0).GetSafeNormal(),0.0}};
	const FVector Result=ECPCarryContacts::ProjectDelta(FVector(-10,20,30),Planes);
	TestTrue(TEXT("Later contact cannot push the goal into an earlier blocker"),Result.X>=-0.001);
	TestTrue(TEXT("Goal also remains outside the second blocker"),FVector::DotProduct(Result,Planes[1].Normal)>=-0.001);
	TestTrue(TEXT("Corner blocking preserves the free vertical follow direction"),FMath::IsNearlyEqual(Result.Z,30.0,0.001));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPCarryNewWallAfterPawnTest,"ECP.Pickup.Contact.RechecksWallAfterPawnProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPCarryNewWallAfterPawnTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture; Fixture.Player->GetCharacterMovement()->DisableMovement();
	Fixture.Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	Fixture.Item->SetActorScale3D(FVector(0.3f)); Fixture.Item->SetActorLocation(FVector(200,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	auto Box=[&](FVector Location,FVector Extent,bool bPawn)
	{
		AActor* Actor=Fixture.World->SpawnActor<AActor>(); UBoxComponent* Shape=NewObject<UBoxComponent>(Actor);
		Actor->AddInstanceComponent(Shape); Actor->SetRootComponent(Shape); Shape->SetBoxExtent(Extent); Shape->SetCollisionProfileName(TEXT("BlockAll"));
		if (bPawn) { Shape->SetCollisionObjectType(ECC_Pawn); Shape->SetCollisionResponseToChannel(ECC_Visibility,ECR_Ignore); }
		Shape->RegisterComponent(); Actor->SetActorLocation(Location); return Actor;
	};
	Box(FVector(200,30,100),FVector(200,5,500),true);
	Box(FVector(250,0,100),FVector(5,2,50),false);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPProjectionNewWall),false,Fixture.Player); Query.AddIgnoredActor(Fixture.Item);
	FHitResult Hit;
	TestFalse(TEXT("Original diagonal motion misses the small world wall"),Fixture.World->SweepSingleByChannel(Hit,Fixture.Body->Bounds.Origin,
		Fixture.Body->Bounds.Origin+FVector(100,100,0),FQuat::Identity,ECC_Visibility,FCollisionShape::MakeBox(Fixture.Body->Bounds.BoxExtent),Query));
	TestTrue(TEXT("Claim test body"),Fixture.Item->TryEnterPickup(Fixture.Player)); Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	Fixture.Player->GetPickupInspectionComponent()->SetWorldLocation(FVector(180,100,100));
	Fixture.Player->GetPhysicalCarryComponent()->TickComponent(10.f,LEVELTICK_All,nullptr);
	FVector Target; FRotator Rotation; Fixture.Player->GetPhysicalCarryComponent()->GetTargetLocationAndRotation(Target,Rotation);
	TestTrue(TEXT("Pawn clips the sideways component"),Target.Y<=10.f);
	TestTrue(TEXT("Projected path must discover and stop at the new wall"),Target.X<=230.f);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPOwnerMovingFrameTest,"ECP.Pickup.OwnerView.ContinuousWalkAndRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPOwnerMovingFrameTest::RunTest(const FString& Parameters)
{
	for (float Speed : {300.f,450.f})
	{
		ECPCarryTests::FFixture Fixture; Fixture.Player->GetCharacterMovement()->DisableMovement();
		Fixture.Item->SetActorScale3D(FVector(0.2f));
		TestTrue(TEXT("Establish owned pickup"),Fixture.Item->TryEnterPickup(Fixture.Player)); Fixture.Player->CompleteQueuedPickup(Fixture.Item);
		Fixture.Player->GetPhysicalCarryComponent()->StopCarry(); Fixture.Body->SetSimulatePhysics(false);
		Fixture.Player->SetRole(ROLE_AutonomousProxy); Fixture.Item->SetRole(ROLE_SimulatedProxy);
		FStructProperty* StateProperty=FindFProperty<FStructProperty>(Fixture.Item->GetClass(),TEXT("AttachmentState"));
		FECPPickupNetState* State=StateProperty->ContainerPtrToValuePtr<FECPPickupNetState>(Fixture.Item);
		Fixture.Item->SetActorLocation(FVector(80,0,100));
		double PreviousRelative=80.0,MaximumStep=0.0,MaximumRootError=0.0;
		for(int32 Frame=0;Frame<180;++Frame)
		{
			Fixture.Player->SetActorLocation(FVector(Frame*Speed/60.f,0,0),false,nullptr,ETeleportType::TeleportPhysics);
			if (Frame%3==0) // 20Hz server poses; the owning camera moves at 60Hz.
			{
				const FVector ServerCamera(FMath::Max(0,Frame-3)*Speed/60.f,0,100);
				State->HeldTransform=FTransform(FQuat::Identity,ServerCamera+FVector(80,0,0),FVector(0.2f));
				if (FStructProperty* Reference=FindFProperty<FStructProperty>(FECPPickupNetState::StaticStruct(),TEXT("HeldReference")))
					*Reference->ContainerPtrToValuePtr<FTransform>(State)=FTransform(ServerCamera);
				if (FBoolProperty* Valid=FindFProperty<FBoolProperty>(FECPPickupNetState::StaticStruct(),TEXT("bHasHeldReference"))) Valid->SetPropertyValue_InContainer(State,true);
			}
			Fixture.Item->Tick(1.f/60.f);
			UStaticMeshComponent* Visual=nullptr;
			TInlineComponentArray<UStaticMeshComponent*> Meshes(Fixture.Item);
			for (UStaticMeshComponent* Mesh : Meshes) if (Mesh->GetFName()==TEXT("OwnerDisplayMesh")) { Visual=Mesh; break; }
			if (!Visual) { AddError(TEXT("Locally held item has no owner visual mesh")); break; }
			MaximumRootError=FMath::Max(MaximumRootError,FVector::Dist(Fixture.Item->GetActorLocation(),State->HeldTransform.GetLocation()));
			const double Relative=Visual->GetComponentLocation().X-Fixture.Player->GetPickupInspectionComponent()->GetComponentLocation().X;
			if (Frame>60) MaximumStep=FMath::Max(MaximumStep,FMath::Abs(Relative-PreviousRelative));
			PreviousRelative=Relative;
		}
		AddInfo(FString::Printf(TEXT("Owner motion speed=%.0f max relative step=%.3fcm"),Speed,MaximumStep));
		TestTrue(TEXT("Steady walk/run does not pulse at the server snapshot rate"),MaximumStep<0.5);
		TestTrue(TEXT("Owner collision root stays at the server physical pose"),MaximumRootError<0.01);
		Fixture.Player->SetRole(ROLE_Authority); Fixture.Item->SetRole(ROLE_Authority);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPOwnerRotationContactTest,"ECP.Pickup.OwnerView.RotationRespectsWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPOwnerRotationContactTest::RunTest(const FString& Parameters)
{
	ECPCarryTests::FFixture Fixture; Fixture.Player->GetCharacterMovement()->DisableMovement();
	TestTrue(TEXT("Establish long item pickup"),Fixture.Item->TryEnterPickup(Fixture.Player)); Fixture.Player->CompleteQueuedPickup(Fixture.Item);
	Fixture.Player->GetPhysicalCarryComponent()->StopCarry(); Fixture.Body->SetSimulatePhysics(false);
	Fixture.Player->SetActorLocation(FVector(100,0,0),false,nullptr,ETeleportType::TeleportPhysics);
	Fixture.Item->SetActorScale3D(FVector(2.f,0.2f,0.2f));
	Fixture.Item->SetActorLocation(FVector(100,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	Fixture.Player->SetRole(ROLE_AutonomousProxy); Fixture.Item->SetRole(ROLE_SimulatedProxy);
	FStructProperty* StateProperty=FindFProperty<FStructProperty>(Fixture.Item->GetClass(),TEXT("AttachmentState"));
	FECPPickupNetState* State=StateProperty->ContainerPtrToValuePtr<FECPPickupNetState>(Fixture.Item);
	State->HeldTransform=Fixture.Item->GetActorTransform();
	State->HeldReference=FTransform(FRotator(0,-90,0),Fixture.Player->GetPickupInspectionComponent()->GetComponentLocation(),FVector::OneVector);
	State->bHasHeldReference=true;
	Fixture.Item->Tick(1.f/60.f);
	UStaticMeshComponent* Visual=nullptr;
	TInlineComponentArray<UStaticMeshComponent*> Meshes(Fixture.Item);
	for (UStaticMeshComponent* Mesh : Meshes) if (Mesh->GetFName()==TEXT("OwnerDisplayMesh")) { Visual=Mesh; break; }
	if (!TestNotNull(TEXT("Owner has separate visual mesh"),Visual)) return false;
	TestTrue(TEXT("Free local view may smoothly turn the visual"),FMath::Abs(Visual->GetComponentRotation().Yaw)>80.f);
	AActor* Wall=Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Shape=NewObject<UBoxComponent>(Wall); Wall->AddInstanceComponent(Shape); Wall->SetRootComponent(Shape);
	Shape->SetBoxExtent(FVector(5,5,100)); Shape->SetCollisionProfileName(TEXT("BlockAll")); Shape->RegisterComponent();
	Wall->SetActorLocation(FVector(100,70,100));
	Fixture.Item->Tick(1.f/60.f);
	TestTrue(TEXT("Local rotation does not enter a wall that the server pose clears"),FMath::Abs(Visual->GetComponentRotation().Yaw)<5.f);
	TestTrue(TEXT("Collision root is still authoritative"),Fixture.Item->GetActorTransform().Equals(State->HeldTransform,0.01f));
	Fixture.Player->SetRole(ROLE_Authority); Fixture.Item->SetRole(ROLE_Authority);
	return true;
}
#endif
