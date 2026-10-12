#if WITH_DEV_AUTOMATION_TESTS
#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Widgets/Text/STextBlock.h"

namespace ECPValueWidgetTests
{
struct FFixture
{
	UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
	AECPPlayerBase* Player=nullptr;
	APlayerController* Viewer=nullptr;
	AECPPickupItem* Item=nullptr;
	UWidgetComponent* Widget=nullptr;
	UCameraComponent* Camera=nullptr;
	FFixture()
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); World->InitializeActorsForPlay(FURL());
		Player=World->SpawnActor<AECPPlayerBase>();
		Camera=NewObject<UCameraComponent>(Player,TEXT("Camera")); Player->AddInstanceComponent(Camera);
		Camera->SetupAttachment(Player->GetRootComponent()); Camera->SetRelativeLocation(FVector(0,0,100)); Camera->RegisterComponent();
		USkeletalMeshComponent* Carry=NewObject<USkeletalMeshComponent>(Player,TEXT("SkeletalMesh")); Player->AddInstanceComponent(Carry);
		Carry->SetupAttachment(Camera); Carry->SetRelativeRotation(FRotationMatrix::MakeFromZ(FVector::ForwardVector).Rotator()); Carry->RegisterComponent();
		Viewer=World->SpawnActor<APlayerController>(); Viewer->SetAsLocalPlayerController(); Viewer->Possess(Player); Player->DispatchBeginPlay();
		Item=World->SpawnActorDeferred<AECPPickupItem>(AECPPickupItem::StaticClass(),FTransform(FVector(160,0,100)));
		UStaticMeshComponent* Body=NewObject<UStaticMeshComponent>(Item,TEXT("ValueTestBody")); Item->AddInstanceComponent(Body); Item->SetRootComponent(Body);
		Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"))); Body->SetCollisionProfileName(TEXT("PhysicsActor")); Body->RegisterComponent();
		Widget=NewObject<UWidgetComponent>(Item,TEXT("ValueWidget")); Item->AddInstanceComponent(Widget); Widget->SetupAttachment(Body);
		Widget->SetRelativeLocation(FVector(0,0,80)); Widget->RegisterComponent();
		Widget->SetSlateWidget(SNew(STextBlock).Text(FText::FromString(TEXT("100000"))));
		Item->FinishSpawning(FTransform(FVector(160,0,100))); Item->DispatchBeginPlay();
		Item->SetActorLocation(FVector(160,0,100),false,nullptr,ETeleportType::TeleportPhysics);
	}
	void Hold() { Item->TryEnterPickup(Player); Player->CompleteQueuedPickup(Item); Item->Tick(1.f/60.f); }
	~FFixture() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPValueWidgetRotationTest,"ECP.Pickup.ValueWidget.StableAnchorAndRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPValueWidgetRotationTest::RunTest(const FString& Parameters)
{
	ECPValueWidgetTests::FFixture Fixture;
	TestFalse(TEXT("World item does not advertise held value"),Fixture.Widget->IsVisible());
	Fixture.Hold();
	TestEqual(TEXT("Value widget uses stable screen rendering"),Fixture.Widget->GetWidgetSpace(),EWidgetSpace::Screen);
	const FVector Offset=Fixture.Widget->GetComponentLocation()-Fixture.Item->GetCarryBody()->Bounds.Origin;
	const FQuat Rotation=Fixture.Widget->GetComponentQuat();
	Fixture.Item->SetActorRotation(FRotator(60,90,25),ETeleportType::TeleportPhysics); Fixture.Item->Tick(1.f/60.f);
	TestTrue(TEXT("Price label does not orbit with item inspection rotation"),(Fixture.Widget->GetComponentLocation()-Fixture.Item->GetCarryBody()->Bounds.Origin).Equals(Offset,0.1f));
	TestTrue(TEXT("Price label does not inherit item rotation"),Fixture.Widget->GetComponentQuat().Equals(Rotation,0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPValueWidgetOcclusionTest,"ECP.Pickup.ValueWidget.OcclusionAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPValueWidgetOcclusionTest::RunTest(const FString& Parameters)
{
	ECPValueWidgetTests::FFixture Fixture; Fixture.Widget->SetWidgetSpace(EWidgetSpace::Screen); Fixture.Hold();
	TestTrue(TEXT("Unobstructed local viewer sees held value"),Fixture.Widget->IsVisible());
	AActor* Wall=Fixture.World->SpawnActor<AActor>(); UBoxComponent* Shape=NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(Shape); Wall->SetRootComponent(Shape); Shape->SetBoxExtent(FVector(5,200,250));
	Shape->SetCollisionProfileName(TEXT("BlockAll")); Shape->RegisterComponent(); Wall->SetActorLocation(FVector(80,0,100));
	Fixture.Item->Tick(1.f/60.f);
	TestFalse(TEXT("Screen value cannot reveal held item through a wall"),Fixture.Widget->IsVisible());
	TestFalse(TEXT("Actual Slate price hides in the same frame"),Fixture.Widget->GetSlateWidget()->GetVisibility().IsVisible());
	Fixture.Player->SetActorLocation(FVector(0,700,0),false,nullptr,ETeleportType::TeleportPhysics);
	const FRotator Aim=(Fixture.Item->GetActorLocation()-Fixture.Camera->GetComponentLocation()).Rotation();
	Fixture.Viewer->SetControlRotation(Aim); Fixture.Camera->SetWorldRotation(Aim); Fixture.Item->Tick(1.f/60.f);
	TestTrue(TEXT("Local viewer with different clear sight can see value"),Fixture.Widget->IsVisible());
	Fixture.Item->TryStore(Fixture.Player); Fixture.Item->Tick(1.f/60.f);
	TestFalse(TEXT("Stored item removes the price label immediately"),Fixture.Widget->IsVisible());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPValueSpectatorOwnershipTest,"ECP.Pickup.ValueWidget.UnresolvedOwnerIsNotSpectator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPValueSpectatorOwnershipTest::RunTest(const FString& Parameters)
{
	ECPValueWidgetTests::FFixture Fixture; Fixture.Hold();
	Fixture.Player->SetRole(ROLE_AutonomousProxy); Fixture.Viewer->UnPossess(); Fixture.Item->SetOwner(nullptr);
	ACameraActor* Camera=Fixture.World->SpawnActor<ACameraActor>(FVector(0,0,100),FRotator::ZeroRotator);
	Fixture.Viewer->SetViewTarget(Camera); Fixture.Viewer->UpdateCameraManager(0.f);
	Fixture.Widget->SetOnlyOwnerSee(true); Fixture.Item->Tick(1.f/60.f);
	TestFalse(TEXT("Unresolved owner does not make an unpossessed viewer the owner"),Fixture.Widget->IsVisible());
	Fixture.Player->SetRole(ROLE_Authority);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPValueOnlyHolderTest,"ECP.Pickup.OwnerView.PriceOnlyForActualHolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPValueOnlyHolderTest::RunTest(const FString& Parameters)
{
	ECPValueWidgetTests::FFixture Fixture; Fixture.Hold();
	TestTrue(TEXT("Actual holder sees value"),Fixture.Widget->IsVisible());
	Fixture.Player->SetRole(ROLE_AutonomousProxy); Fixture.Viewer->UnPossess();
	ACameraActor* Camera=Fixture.World->SpawnActor<ACameraActor>(FVector(0,0,100),FRotator::ZeroRotator);
	Fixture.Viewer->SetViewTarget(Camera); Fixture.Viewer->UpdateCameraManager(0.f); Fixture.Item->Tick(1.f/60.f);
	TestFalse(TEXT("Spectator with clear sight cannot see another character's price"),Fixture.Widget->IsVisible());
	TestFalse(TEXT("Rendered Slate price is private to holder"),Fixture.Widget->GetSlateWidget()->GetVisibility().IsVisible());
	Fixture.Player->SetRole(ROLE_Authority);
	return true;
}
#endif
