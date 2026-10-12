#include "Characters/ECPPlayerBase.h"
#include "Actors/ECPPickupItem.h"
#include "Components/ECPPhysicalCarryComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Camera/CameraComponent.h"
#include "CoreGlobals.h"
#include "Data/ECPPlayerController.h"
#include "Engine/World.h"
#include "TimerManager.h"

AActor* AECPPlayerBase::GetScannedCarryTarget() const
{
	FHitResult Hit;
	return TracePickup(Hit) ? Hit.GetActor() : nullptr;
}

void AECPPlayerBase::RecalculateCarriedWeight()
{
	if (!HasAuthority()) return;
	// Reconcile accepted item state before summing; notifications can precede the RPC's slot assignment.
	auto Reconcile = [this](TObjectPtr<AECPPickupItem>& Slot)
	{
		AECPPickupItem* Item = Slot.Get();
		if (!IsValid(Item) || Item->IsActorBeingDestroyed() || !Item->IsOwnedBy(this)) { Slot = nullptr; return; }
		if (Item->GetPickupState() == EECPPickupState::Stored) { StoredPickups.AddUnique(Item); Slot = nullptr; }
	};
	Reconcile(InspectedPickup);
	Reconcile(EquippedPickup);
	StoredPickups.RemoveAll([this](AECPPickupItem* Item)
	{
		return !IsValid(Item) || Item->IsActorBeingDestroyed() || !Item->IsOwnedBy(this) || Item->GetPickupState() != EECPPickupState::Stored;
	});
	StoredPickupCount = StoredPickups.Num();
	TSet<const AECPPickupItem*> Counted;
	double Weight = 0.0;
	auto Count = [this, &Weight, &Counted](const AECPPickupItem* Item)
	{
		if (!IsValid(Item) || Item->IsActorBeingDestroyed() || !Item->IsOwnedBy(this) || Counted.Contains(Item)) return;
		Counted.Add(Item);
		Weight += Item->GetPickupMassKg();
	};
	Count(InspectedPickup);
	Count(EquippedPickup);
	for (AECPPickupItem* Item : StoredPickups) Count(Item);
	if (IsValid(CarriedTeammate) && !CarriedTeammate->IsActorBeingDestroyed() && CarriedTeammate->GetCarryHolder() == this)
	{
		Weight += CarriedTeammate->GetCarryMassKg();
	}
	SetCarriedWeightKg(static_cast<float>(FMath::Min(Weight, static_cast<double>(MAX_flt))));
	ForceNetUpdate();
}

void AECPPlayerBase::NotifyOwnedPickupChanged(AActor* Target)
{
	if (!HasAuthority()) return;
	if (Target == CarriedTeammate && (!IsValid(Target) || Target->IsActorBeingDestroyed()
		|| CarriedTeammate->GetCarryHolder() != this || !CarriedTeammate->IsRagdollActive())) ReleaseCarriedTeammate();
	if (Target && Target->IsActorBeingDestroyed() && PhysicalCarry->GetCarryTarget() == Target) PhysicalCarry->StopCarry();
	RecalculateCarriedWeight();
}

void AECPPlayerBase::HandlePhysicalCarryLost(AActor* Target)
{
	if (!HasAuthority()) return;
	if (CarriedTeammate) ReleaseCarriedTeammate();
	if (AECPPickupItem* Item = Cast<AECPPickupItem>(Target); IsValid(Item) && Item->IsOwnedBy(this))
	{
		Item->TryDrop(this, Item->GetActorTransform(), FVector::ZeroVector);
	}
	RecalculateCarriedWeight();
}

void AECPPlayerBase::ReleaseCarriedTeammate()
{
	if (!HasAuthority() || !CarriedTeammate) return;
	AECPCharBase* Target = CarriedTeammate;
	CarriedTeammate = nullptr;
	PhysicalCarry->StopCarry();
	Target->OnCarryTargetChanged.RemoveAll(this);
	Target->EndCarry(this);
	RecalculateCarriedWeight();
	ForceNetUpdate();
}

bool AECPPlayerBase::FindSafePickupRelease(AECPPickupItem* Item, FTransform& OutTransform) const
{
	UPrimitiveComponent* Body = IsValid(Item) ? Item->GetCarryBody() : nullptr;
	if (!Body || !GetWorld()) return false;
	const FVector Start = PlayerCamera ? PlayerCamera->GetComponentLocation() : GetActorLocation();
	const FVector Extent = Body->Bounds.BoxExtent.ComponentMax(FVector(1.f));
	// Bounds center may be offset from the mesh pivot. Candidate remains the actor origin, not this center.
	const FVector BoundsOffset = Body->Bounds.Origin - Item->GetActorLocation();
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPUnload), false, this);
	Query.AddIgnoredActor(Item);
	// Deterministic candidates avoid moving inventory into a wall or selecting a client-supplied slot.
	const float Spacing = FMath::Max(130.f, Extent.GetMax() * 2.f + 30.f);
	const FVector Offsets[] = {FVector(Spacing,0,0), FVector(0,Spacing,0), FVector(0,-Spacing,0), FVector(-Spacing,0,0), FVector(0,0,Spacing)};
	for (const FVector& Offset : Offsets)
	{
		const FVector Candidate = Start + FRotator(0, GetViewRotation().Yaw, 0).RotateVector(Offset);
		const FVector CandidateCenter = Candidate + BoundsOffset;
		FHitResult Hit;
		if (GetWorld()->SweepSingleByChannel(Hit, Start + BoundsOffset, CandidateCenter, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeBox(Extent), Query)
			|| GetWorld()->OverlapBlockingTestByChannel(CandidateCenter, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeBox(Extent), Query)) continue;
		OutTransform = FTransform(Item->GetActorQuat(), Candidate, Item->GetActorScale3D());
		return true;
	}
	return false;
}

bool AECPPlayerBase::TryDropLastStoredPickup()
{
	if (!HasAuthority() || IsDead() || IsRagdollActive() || GetHeldPickup()) return false;
	RecalculateCarriedWeight();
	if (StoredPickups.IsEmpty()) return false;
	AECPPickupItem* Item = StoredPickups.Last();
	FTransform Release;
	if (!FindSafePickupRelease(Item, Release) || !Item->DropFromInventory(this, Release, GetVelocity())) return false;
	StoredPickups.Remove(Item);
	Item->OnCarryTargetChanged.RemoveAll(this);
	RecalculateCarriedWeight();
	return true;
}

void AECPPlayerBase::QueueTeammateCarryRequest(AECPPlayerBase* Player)
{
	if (!HasAuthority() || !GetWorld() || !IsValid(Player) || !CanBeCarriedBy(Player)) return;
	if (PendingTeammateRequests.ContainsByPredicate([Player](const FTeammateCarryRequest& Request) { return Request.Player == Player; })) return;
	PendingTeammateRequests.Add({Player, GFrameCounter, NextTeammateArrival++});
	if (!bTeammateResolveScheduled)
	{
		bTeammateResolveScheduled = true;
		GetWorldTimerManager().SetTimerForNextTick(this, &ThisClass::ResolveTeammateCarryRequests);
	}
}

void AECPPlayerBase::ResolveTeammateCarryRequests()
{
	bTeammateResolveScheduled = false;
	TArray<FTeammateCarryRequest> Requests = MoveTemp(PendingTeammateRequests);
	PendingTeammateRequests.Reset();
	Requests.RemoveAll([this](const FTeammateCarryRequest& Request)
	{
		AECPPlayerBase* Player = Request.Player.Get();
		FHitResult Hit;
		return !IsValid(Player) || !Player->GetController() || Player->GetHeldPickup() || !CanBeCarriedBy(Player)
			|| !Player->TracePickup(Hit) || Hit.GetActor() != this;
	});
	Requests.Sort([](const FTeammateCarryRequest& A, const FTeammateCarryRequest& B)
	{
		auto Priority = [](const FTeammateCarryRequest& Request)
		{
			const AECPPlayerController* Controller = Cast<AECPPlayerController>(Request.Player->GetController());
			return Controller ? Controller->GetPickupPriority(Request.Frame, Request.Arrival)
				: FECPPickupPriority{Request.Frame, false, MAX_flt, MAX_int32, Request.Arrival};
		};
		return ECPPickupRules::IsHigherPriority(Priority(A), Priority(B));
	});
	for (const FTeammateCarryRequest& Request : Requests)
	{
		AECPPlayerBase* Player = Request.Player.Get();
		FHitResult Hit;
		if (!IsValid(Player) || !Player->TracePickup(Hit) || Hit.GetActor()!=this) continue;
		if (!TryBeginCarry(Player)) continue;
		Player->CarriedTeammate = this;
		OnCarryTargetChanged.AddUObject(Player, &ThisClass::NotifyOwnedPickupChanged);
		if (!Player->PhysicalCarry->StartCarry(this,&Hit)) { Player->ReleaseCarriedTeammate(); continue; }
		Player->SetFocusedPickup(nullptr);
		Player->ClearTeammateFocus();
		Player->RecalculateCarriedWeight();
		Player->ForceNetUpdate();
		break;
	}
}
