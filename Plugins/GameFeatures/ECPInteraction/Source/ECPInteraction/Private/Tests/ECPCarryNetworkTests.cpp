#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/ECPPhysicalCarryComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Components/BoxComponent.h"
#include "Components/WidgetComponent.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

// These commands run only in the automation process, use real NetDrivers, and never save editor assets/settings.
class FECPCarryNetworkCommand : public IAutomationLatentCommand
{
public:
	FECPCarryNetworkCommand(FAutomationTestBase* InTest, ULevelEditorPlaySettings* InSettings, AGameModeBase* InModeCDO, TSubclassOf<APawn> InOldPawn, bool bInImpaired)
		: Test(InTest), Settings(InSettings), ModeCDO(InModeCDO), OldPawn(InOldPawn), Started(FPlatformTime::Seconds()), StageStarted(Started), bImpaired(bInImpaired)
	{
		EPlayNetMode Mode; InSettings->GetPlayNetMode(Mode); bListen=Mode==PIE_ListenServer;
	}

	virtual bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - Started > 60.0) { Test->AddError(FString::Printf(TEXT("Real carry network test timed out at stage %d"), Stage)); return Finish(); }
		UWorld* Server = nullptr;
		TArray<UWorld*> Clients;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || World->WorldType != EWorldType::PIE) continue;
			if (World->GetNetMode() == (bListen ? NM_ListenServer : NM_DedicatedServer)) Server = World;
			else if (World->GetNetMode() == NM_Client) Clients.Add(World);
		}
		if (!Server || Clients.Num() < (bListen?1:2)) return false;
		if (Stage == 0)
		{
			TArray<AECPPlayerBase*> Players;
			for (TActorIterator<AECPPlayerBase> It(Server); It; ++It) if (It->GetController()) Players.Add(*It);
			if (Players.Num() != 2) return false;
			if (!Players[0]->GetCharacterMovement() || !Players[1]->GetCharacterMovement())
			{
				Test->AddError(FString::Printf(TEXT("Network fixture spawned movement-less pawn classes: %s / %s"),
					*Players[0]->GetClass()->GetName(),*Players[1]->GetClass()->GetName()));
				return Finish();
			}
			Source = Players[0]; Target = Players[1];
			if (bListen && !Source->IsLocallyControlled()) { Source=Players[1]; Target=Players[0]; }
			if (bImpaired)
			{
				FPacketSimulationSettings Packets;
				Packets.PktLag=80;
				Packets.PktLoss=5;
				Server->GetNetDriver()->SetPacketSimulationSettings(Packets);
				for (UWorld* World : Clients) World->GetNetDriver()->SetPacketSimulationSettings(Packets);
			}
			FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Floor = Server->SpawnActor<AStaticMeshActor>(FVector(0,0,-50), FRotator::ZeroRotator, Spawn);
			Floor->SetReplicates(true);
			Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
			Floor->GetStaticMeshComponent()->SetIsReplicated(true);
			Floor->SetActorScale3D(FVector(30,30,1));
			Source->SetActorLocation(FVector(0,0,96), false, nullptr, ETeleportType::TeleportPhysics);
			// Keep the standing second pawn out of the held item's contact space until the rescue scenario.
			Target->SetActorLocation(FVector(600,0,96), false, nullptr, ETeleportType::TeleportPhysics);
			// Restore actual CMC walking after empty Entry-map spawn; the replicated floor is real collision geometry.
			Source->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			Target->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			Source->GetController()->SetControlRotation(FRotator::ZeroRotator);
			CastChecked<APlayerController>(Source->GetController())->ClientSetRotation(FRotator::ZeroRotator);
			Advance(Now); return false;
		}
		if (!Source.IsValid() || !Target.IsValid()) { Test->AddError(TEXT("PIE pawns disappeared")); return Finish(); }
		AECPPlayerBase* Owner = bListen ? Source.Get() : nullptr;
		for (UWorld* World : Clients)
		{
			AECPPlayerBase* Copy = Replica(Source.Get(), World);
			if (!Copy) return false;
			if (Copy->IsLocallyControlled()) Owner = Copy;
		}
		if (!Owner) return false;
		if (Stage == 1)
		{
			if (Now - StageStarted < 2.0 || Source->IsMovementLocked() || Target->IsMovementLocked()) return false;
			UClass* Class = LoadClass<AECPPickupItem>(nullptr, TEXT("/Game/Project/Interaction/Items/BP_Smallitems.BP_Smallitems_C"));
			const FVector Position = Source->GetPickupInspectionComponent()->GetComponentLocation() + FVector(120,0,0);
			Item = Class ? Server->SpawnActor<AECPPickupItem>(Class, Position, FRotator::ZeroRotator) : nullptr;
			if (!Item.IsValid()) { Test->AddError(TEXT("Network item failed to spawn")); return Finish(); }
			Item->GetCarryBody()->SetMassOverrideInKg(NAME_None, 100.f);
			Item->RefreshPickupMass();
			if (!Item->TryEnterPickup(Source.Get())) { Test->AddError(TEXT("Server physics pickup rejected")); return Finish(); }
			Source->CompleteQueuedPickup(Item.Get());
			Advance(Now); return false;
		}
		if (Stage == 2)
		{
			if (!Owner->GetInspectedPickup() || !FMath::IsNearlyEqual(Owner->GetCarriedWeightKg(),100.f) || Now - StageStarted < 1.0) return false;
			if (!bInitialReplicationChecked)
			{
				for (UWorld* World : Clients)
				{
					AECPPickupItem* Copy = Replica(Item.Get(), World);
					if (!Copy || !Copy->GetPickupOwner()) return false;
					Test->TestFalse(TEXT("Real client held body does not simulate a second physics world"), Copy->GetCarryBody()->IsSimulatingPhysics());
					Test->TestTrue(TEXT("Stationary clients follow the server physical pose"), FVector::Dist(Copy->GetActorLocation(), Item->GetActorLocation()) < 20.f);
					Test->TestEqual(TEXT("Load replicates to both owner and observer"), Replica(Source.Get(),World)->GetCarriedWeightKg(),100.f);
				}
				bInitialReplicationChecked=true;
			}
			if (!CheckOwnerMotion(Owner,Now)) return false;
			if (!CheckPriceViews(Server,Clients,Now)) return false;
			Queue(Owner, [](AECPPlayerBase* Player) { Player->RequestStorePickup(); });
			Advance(Now); return false;
		}
		if (Stage == 3)
		{
			if (Source->GetStoredPickupCount()!=1 || Owner->GetStoredPickupCount()!=1) return false;
			for (const FPriceView& Price : PriceViews)
			{
				if (Price.Widget.IsValid() && CastChecked<AECPPickupItem>(Price.Widget->GetOwner())->GetPickupState()!=EECPPickupState::Stored) return false;
				if (Price.Widget.IsValid()) Test->TestFalse(TEXT("Storage hides each local price label"),Price.Widget->IsVisible());
			}
			Test->TestEqual(TEXT("Real owner RPC preserves stored load"), Source->GetCarriedWeightKg(),100.f);
			Queue(Owner, [](AECPPlayerBase* Player) { Player->RequestDropPickup(); });
			Advance(Now); return false;
		}
		if (Stage == 4)
		{
			if (Source->GetStoredPickupCount()!=0 || Owner->GetStoredPickupCount()!=0 || Source->GetCarriedWeightKg()!=0.f) return false;
			Item->SetActorLocation(FVector(3000,0,200), false, nullptr, ETeleportType::TeleportPhysics);
			// Establish the rescue inside the real 250cm pickup range; no widened gameplay range for tests.
			Target->SetActorLocation(Source->GetActorLocation() + FVector(130,0,0), false, nullptr, ETeleportType::TeleportPhysics);
			Target->StartRagdoll();
			Advance(Now); return false;
		}
		if (Stage == 5)
		{
			if (!Target->IsRagdollActive() || Now - StageStarted < 1.0) return false;
			for (UWorld* World : Clients)
			{
				AECPPlayerBase* Copy = Replica(Target.Get(),World);
				if (!Copy || !Copy->IsRagdollActive()) return false;
				Test->TestFalse(TEXT("Real clients do not simulate the server ragdoll"),Copy->GetMesh()->IsSimulatingPhysics(Copy->RagdollPelvisBone));
			}
			if (Target->GetMesh()->GetBoneIndex(TEXT("LowerLeg_R"))==INDEX_NONE) { Test->AddError(TEXT("Rescue fixture has no physical leg")); return Finish(); }
			OwnerOriginalOverlay=Replica(Target.Get(),Owner->GetWorld())->GetMesh()->GetOverlayMaterial();
			const FVector Direction = (Target->GetMesh()->GetSocketLocation(TEXT("LowerLeg_R"))
				- Source->GetPickupAttachmentComponent()->GetComponentLocation()).GetSafeNormal();
			const FRotator Aim = Direction.Rotation();
			Source->GetController()->SetControlRotation(Aim);
			CastChecked<APlayerController>(Source->GetController())->ClientSetRotation(Aim);
			Advance(Now); return false;
		}
		if (Stage == 6)
		{
			if (Now - StageStarted < 1.0) return false;
			// The carry-mesh ray origin moves with pitch. A one-shot angle from the old origin can miss a
			// settled pelvis; calibrate the fixture until both real worlds actually aim at the target.
			FHitResult LimbHit;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPNetworkLimb),false,Source.Get());
			FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_Pawn); Objects.AddObjectTypesToQuery(ECC_PhysicsBody); Objects.AddObjectTypesToQuery(ECC_GameTraceChannel1);
			const FVector RayStart=Source->GetPickupAttachmentComponent()->GetComponentLocation();
			Server->LineTraceSingleByObjectType(LimbHit,RayStart,RayStart+Source->GetPickupAttachmentComponent()->GetUpVector()*Source->PickupDistance,Objects,Query);
			if (Source->GetScannedCarryTarget()!=Target.Get() || Owner->GetScannedCarryTarget()!=Replica(Target.Get(),Owner->GetWorld())
				|| LimbHit.GetActor()!=Target.Get() || LimbHit.BoneName.IsNone() || LimbHit.BoneName==Target->RagdollPelvisBone)
			{
				if (Now - LastAimUpdate > 0.2)
				{
					LastAimUpdate=Now;
					const FRotator Aim=(Target->GetMesh()->GetSocketLocation(TEXT("LowerLeg_R"))
						- Source->GetPickupAttachmentComponent()->GetComponentLocation()).Rotation();
					Source->GetController()->SetControlRotation(Aim);
					CastChecked<APlayerController>(Source->GetController())->ClientSetRotation(Aim);
				}
				return false;
			}
			// A fresh trace can align between local 0.1s focus ticks; wait for the actual visible state
			// before pressing F. Failure to converge still hits this command's real-time timeout.
			if (!Owner->TeammateOutlineMaterial || Replica(Target.Get(),Owner->GetWorld())->GetMesh()->GetOverlayMaterial()!=Owner->TeammateOutlineMaterial) return false;
			Test->TestTrue(TEXT("Real owner sees skeletal outline before F"),Replica(Target.Get(),Owner->GetWorld())->GetMesh()->GetOverlayMaterial()==Owner->TeammateOutlineMaterial);
			Test->AddInfo(FString::Printf(TEXT("Carry aim localUp=%s serverHit=%s clientHit=%s serverStart=%s serverBone=%s clientStart=%s clientBone=%s"),
				*Source->GetPickupAttachmentComponent()->GetRelativeRotation().Quaternion().RotateVector(FVector::UpVector).ToString(),
				*GetNameSafe(Source->GetScannedCarryTarget()), *GetNameSafe(Owner->GetScannedCarryTarget()),
				*Source->GetPickupAttachmentComponent()->GetComponentLocation().ToString(), *Target->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone).ToString(),
				*Owner->GetPickupAttachmentComponent()->GetComponentLocation().ToString(), *Replica(Target.Get(),Owner->GetWorld())->GetMesh()->GetSocketLocation(Target->RagdollPelvisBone).ToString()));
			Queue(Owner, [](AECPPlayerBase* Player) { Player->RequestPickup(); });
			Advance(Now); return false;
		}
		if (Stage == 7)
		{
			if (Source->GetCarriedTeammate()==Target.Get()) bObservedServerCarry=true;
			if (bObservedServerCarry && !Source->GetCarriedTeammate())
			{
				Test->AddError(TEXT("Accepted teammate carry was unexpectedly lost before release")); return Finish();
			}
			if (Now - StageStarted > 3.0 && !bDiagnosed)
			{
				bDiagnosed=true;
				Test->AddInfo(FString::Printf(TEXT("Carry post-F serverHeld=%s ownerHeld=%s targetHolder=%s serverHit=%s clientHit=%s targetSim=%d"),
					*GetNameSafe(Source->GetCarriedTeammate()), *GetNameSafe(Owner->GetCarriedTeammate()), *GetNameSafe(Target->GetCarryHolder()),
					*GetNameSafe(Source->GetScannedCarryTarget()), *GetNameSafe(Owner->GetScannedCarryTarget()), Target->GetMesh()->IsSimulatingPhysics(Target->RagdollPelvisBone)));
			}
			if (!bObservedServerCarry && Now-StageStarted>1.0)
			{
				Test->AddInfo(TEXT("F was not accepted after network delay; re-aim and retry against the fresh server limb pose."));
				Stage=6; StageStarted=Now-1.1; return false;
			}
			if (Source->GetCarriedTeammate()!=Target.Get() || Owner->GetCarriedTeammate()!=Replica(Target.Get(),Owner->GetWorld())) return false;
			if (!bLateRequested) { bLateRequested=true; GEditor->RequestLateJoin(); return false; }
			if (Clients.Num()!=(bListen?2:3)) return false;
			for (UWorld* World : Clients)
			{
				AECPPlayerBase* CarrierCopy=Replica(Source.Get(),World);
				AECPPlayerBase* TargetCopy=Replica(Target.Get(),World);
				if (!CarrierCopy || !TargetCopy || CarrierCopy->GetCarriedTeammate()!=TargetCopy
					|| TargetCopy->GetCarryHolder()!=CarrierCopy || CarrierCopy->GetCarriedWeightKg()!=80.f) return false;
			}
			Test->TestEqual(TEXT("Real remote F RPC adds teammate body weight"),Source->GetCarriedWeightKg(),80.f);
			const FName GrabBone=Source->GetPhysicalCarryComponent()->GrabbedBoneName;
			Test->TestTrue(TEXT("Server grabs a hit limb rather than the pelvis"),!GrabBone.IsNone() && GrabBone!=Target->RagdollPelvisBone);
			Test->TestTrue(TEXT("Owner outline restores after accepted grab"),Replica(Target.Get(),Owner->GetWorld())->GetMesh()->GetOverlayMaterial()==OwnerOriginalOverlay);
			Test->TestFalse(TEXT("Real network carried target cannot recover"),Target->TryRecoverFromRagdoll());
			for (UWorld* World : Clients) Test->TestEqual(TEXT("Teammate load reaches all observers"),Replica(Source.Get(),World)->GetCarriedWeightKg(),80.f);
			Queue(Owner, [](AECPPlayerBase* Player) { Player->RequestPlacePickup(); });
			Advance(Now); return false;
		}
		if (Stage == 8)
		{
			if (Source->GetCarriedTeammate() || Owner->GetCarriedTeammate() || Source->GetCarriedWeightKg()!=0.f || Target->GetCarryHolder()) return false;
			Test->AddInfo(FString::Printf(TEXT("Dedicated PIE server + two clients + late join passed actual pose, store/unload and teammate F/RMB RPCs; lag=%dms loss=%d%%."),bImpaired?80:0,bImpaired?5:0));
			return Finish();
		}
		return false;
	}
private:
	bool CheckOwnerMotion(AECPPlayerBase* Owner,double Now)
	{
		if (bMotionComplete) return true;
		if (MotionStarted==0.0) { MotionStarted=Now; MotionPreviousTime=Now; Owner->SetRunning(false); }
		const double Elapsed=Now-MotionStarted;
		if (Elapsed>3.0 && !bMotionRunning) { bMotionRunning=true; Owner->SetRunning(true); }
		if (Elapsed<6.0) Owner->AddMovementInput(FVector::YAxisVector,1.f);
		AECPPickupItem* Copy=Replica(Item.Get(),Owner->GetWorld());
		UStaticMeshComponent* Visual=nullptr;
		if (!Owner->HasAuthority())
		{
			TInlineComponentArray<UStaticMeshComponent*> Meshes(Copy);
			for (UStaticMeshComponent* Mesh : Meshes) if (Mesh->GetFName()==TEXT("OwnerDisplayMesh")) { Visual=Mesh; break; }
			if (!Visual || Visual->GetCollisionEnabled()!=ECollisionEnabled::NoCollision) return false;
			FStructProperty* StateProperty=FindFProperty<FStructProperty>(Copy->GetClass(),TEXT("AttachmentState"));
			const FECPPickupNetState* State=StateProperty->ContainerPtrToValuePtr<FECPPickupNetState>(Copy);
			MotionRootMaximumError=FMath::Max(MotionRootMaximumError,FVector::Dist(Copy->GetActorLocation(),State->HeldTransform.GetLocation()));
		}
		const FVector Relative=Owner->GetPickupInspectionComponent()->GetComponentTransform().InverseTransformPosition(Visual ? Visual->GetComponentLocation() : Copy->GetActorLocation());
		const double SampleTime=Now-MotionPreviousTime;
		if (SampleTime>0.0001 && ((Elapsed>2.0 && Elapsed<2.8) || (Elapsed>5.0 && Elapsed<5.8)))
		{
			const double Step=(Relative-MotionPreviousRelative).Size()/(SampleTime*60.0);
			if (Elapsed<3.0) WalkMaximumStep=FMath::Max(WalkMaximumStep,Step); else RunMaximumStep=FMath::Max(RunMaximumStep,Step);
			++MotionSamples;
		}
		MotionPreviousRelative=Relative; MotionPreviousTime=Now;
		if (Elapsed<6.5) return false;
		Owner->SetRunning(false); bMotionComplete=true;
		Test->TestTrue(TEXT("Real owner actually walked and ran"),MotionSamples>10 && Owner->GetActorLocation().Y>200.f);
		if (!Owner->HasAuthority()) Test->TestTrue(TEXT("Owning client collision root remains at replicated server pose"),MotionRootMaximumError<0.1);
		Test->AddInfo(FString::Printf(TEXT("Owner steady camera-relative steps: walk=%.3fcm run=%.3fcm samples=%d"),WalkMaximumStep,RunMaximumStep,MotionSamples));
		Test->TestTrue(TEXT("Walking owner display has no snapshot-rate pulses"),WalkMaximumStep<1.0);
		Test->TestTrue(TEXT("Running owner display has no snapshot-rate pulses"),RunMaximumStep<1.0);
		return true;
	}
	struct FPriceView
	{
		TWeakObjectPtr<APlayerController> Viewer;
		TWeakObjectPtr<AActor> PreviousView,Camera,Wall;
		TWeakObjectPtr<UWidgetComponent> Widget;
		bool bExpected=false;
	};
	bool CheckPriceViews(UWorld* Server,const TArray<UWorld*>& Clients,double Now)
	{
		if (!bPriceViewsPrepared)
		{
			TArray<UWorld*> Worlds=Clients; if (bListen) Worlds.Add(Server);
			for (int32 Index=0;Index<Worlds.Num();++Index)
			{
				UWorld* World=Worlds[Index]; AECPPickupItem* Copy=Replica(Item.Get(),World);
				TInlineComponentArray<UWidgetComponent*> Widgets(Copy);
				if (Widgets.IsEmpty()) { Test->AddError(TEXT("Real BP_Smallitems has no configured value widget")); bPriceViewsPrepared=true; return true; }
				APlayerController* Viewer=nullptr;
				for (FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It)
					if (APlayerController* Candidate=It->Get();Candidate && Candidate->IsLocalController()) { Viewer=Candidate; break; }
				if (!Viewer) { Test->AddError(TEXT("Price view has no local player")); bPriceViewsPrepared=true; return true; }
				FPriceView& Price=PriceViews.AddDefaulted_GetRef(); Price.Viewer=Viewer; Price.PreviousView=Viewer->GetViewTarget(); Price.Widget=Widgets[0]; Price.bExpected=Viewer->GetPawn()==Copy->GetPickupOwner();
				const FVector Center=Item->GetCarryBody()->Bounds.Origin;
				const FVector Eye=Center+FVector(0,200,0); // Both views are clear; only the actual holder may see the price.
				Price.Camera=World->SpawnActor<ACameraActor>(Eye,(Center-Eye).Rotation()); Viewer->SetViewTarget(Price.Camera.Get());
				AActor* Wall=World->SpawnActor<AActor>(); Price.Wall=Wall;
				UBoxComponent* Shape=NewObject<UBoxComponent>(Wall); Wall->AddInstanceComponent(Shape); Wall->SetRootComponent(Shape);
				Shape->SetBoxExtent(FVector(5,70,500)); Shape->SetCollisionProfileName(TEXT("BlockAll")); Shape->RegisterComponent(); Wall->SetActorLocation(Center-FVector(100,0,0));
			}
			bPriceViewsPrepared=true; PriceViewsStarted=Now; return false;
		}
		if (Now-PriceViewsStarted<0.3) return false;
		for (const FPriceView& Price : PriceViews)
		{
			const bool bActual=Price.Widget->IsVisible();
			if (bActual!=Price.bExpected && Now-PriceViewsStarted<2.0) return false;
			Test->TestEqual(TEXT("Held price is visible only to the actual owning player"),bActual,Price.bExpected);
			Test->TestEqual(TEXT("Actual Blueprint price widget class is preserved"),Price.Widget->GetWidgetClass()->GetName(),FString(TEXT("BPUMG_Itemvalue_C")));
			Test->TestEqual(TEXT("Real price uses Screen presentation"),Price.Widget->GetWidgetSpace(),EWidgetSpace::Screen);
			if (UUserWidget* User=Price.Widget->GetUserWidgetObject()) Test->TestEqual(TEXT("Rendered price matches local visibility"),User->IsVisible(),Price.bExpected);
			Price.Viewer->SetViewTarget(Price.PreviousView.Get()); Price.Camera->Destroy(); Price.Wall->Destroy();
		}
		return true;
	}
	void Advance(double Now) { ++Stage; StageStarted=Now; }
	bool Finish() { if (ModeCDO) ModeCDO->DefaultPawnClass=OldPawn; GEditor->RequestEndPlayMap(); return true; }
	template<typename T> T* Replica(T* Authority,UWorld* Client)
	{
		if (!Authority || !Authority->GetWorld()->GetNetDriver() || !Client->GetNetDriver()) return nullptr;
		if (Authority->GetWorld()==Client) return Authority;
		const FNetworkGUID Guid = Authority->GetWorld()->GetNetDriver()->GuidCache->GetNetGUID(Authority);
		if (!Guid.IsValid()) return nullptr;
		return Cast<T>(Client->GetNetDriver()->GuidCache->GetObjectFromNetGUID(Guid,true));
	}
	static void Queue(AECPPlayerBase* Owner,TFunction<void(AECPPlayerBase*)> Request)
	{
		Owner->GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(Owner,[Owner,Request=MoveTemp(Request)]() { Request(Owner); }));
	}
	FAutomationTestBase* Test;
	UMaterialInterface* OwnerOriginalOverlay=nullptr;
	TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
	AGameModeBase* ModeCDO;
	TSubclassOf<APawn> OldPawn;
	TWeakObjectPtr<AECPPlayerBase> Source,Target;
	TWeakObjectPtr<AECPPickupItem> Item;
	double Started,StageStarted;
	double LastAimUpdate=0;
	int32 Stage=0;
	bool bDiagnosed=false;
	bool bImpaired=false;
	bool bLateRequested=false;
	bool bObservedServerCarry=false;
	bool bListen=false;
	bool bPriceViewsPrepared=false;
	bool bInitialReplicationChecked=false;
	double PriceViewsStarted=0.0;
	TArray<FPriceView> PriceViews;
	double MotionStarted=0.0,MotionPreviousTime=0.0,WalkMaximumStep=0.0,RunMaximumStep=0.0,MotionRootMaximumError=0.0;
	FVector MotionPreviousRelative=FVector::ZeroVector;
	int32 MotionSamples=0;
	bool bMotionRunning=false,bMotionComplete=false;
};

class FECPCarryWaitEndCommand : public IAutomationLatentCommand
{
public:
	virtual bool Update() override
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
			if (Context.WorldType==EWorldType::PIE) return false;
		return true;
	}
};

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FECPCarryNetworkTest,"ECP.Network.Carry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FECPCarryNetworkTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
	Names.Add(TEXT("DedicatedServerTwoClientsAndLateJoin")); Commands.Add(TEXT("normal"));
	Names.Add(TEXT("Lag80msLoss5PercentAndLateJoin")); Commands.Add(TEXT("lag"));
	Names.Add(TEXT("ListenHostHeldPoseAndLateJoin")); Commands.Add(TEXT("listen"));
}
bool FECPCarryNetworkTest::RunTest(const FString& Parameters)
{
	if (!GEditor || GEditor->PlayWorld) { AddError(TEXT("Carry network automation requires an idle private editor process.")); return false; }
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),EAutomationExpectedErrorFlags::Contains,-1);
	ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(GetMutableDefault<ULevelEditorPlaySettings>(),GetTransientPackage());
	Settings->SetPlayNetMode(Parameters==TEXT("listen") ? PIE_ListenServer : PIE_Client);
	Settings->SetRunUnderOneProcess(true);
	Settings->SetPlayNumberOfClients(2);
	Settings->SetServerPort(17879);
	Settings->NetworkEmulationSettings.bIsNetworkEmulationEnabled=false;
	FRequestPlaySessionParams Params;
	Params.EditorPlaySettings=Settings;
	Params.GlobalMapOverride=TEXT("/Engine/Maps/Entry");
	Params.GameModeOverride=LoadClass<AGameModeBase>(nullptr,TEXT("/Game/Project/Interaction/Framework/GM_InGame.GM_InGame_C"));
	if (!Params.GameModeOverride) { AddError(TEXT("Current game mode failed to load")); return false; }
	AGameModeBase* ModeCDO=Params.GameModeOverride->GetDefaultObject<AGameModeBase>();
	const TSubclassOf<APawn> OldPawn=ModeCDO->DefaultPawnClass;
	// GM's legacy BP_Player has missing CMC data in this dirty migration. Use the existing supported player
	// blueprint in this private process only, then restore the CDO; do not save or rewrite any asset.
	ModeCDO->DefaultPawnClass=LoadClass<APawn>(nullptr,TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	Params.bAllowOnlineSubsystem=false;
	GEditor->RequestPlaySession(Params);
	AddCommand(new FECPCarryNetworkCommand(this,Settings,ModeCDO,OldPawn,Parameters==TEXT("lag")));
	AddCommand(new FECPCarryWaitEndCommand());
	return true;
}
#endif
