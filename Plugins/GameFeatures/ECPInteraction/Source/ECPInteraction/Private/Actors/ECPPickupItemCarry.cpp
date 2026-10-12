#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"

void AECPPickupItem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (GetPickupState() != EECPPickupState::Pickup) { UpdateOwnerDisplayMesh(nullptr); return; }
	if (HasAuthority())
	{
		CaptureHeldPose();
	}
	else if (GetCarryBody() && GetWorld())
	{
		FTransform Display;
		AECPPlayerBase* Holder=AttachmentState.Holder;
		UCameraComponent* Camera=IsValid(Holder) ? Holder->GetPickupInspectionComponent() : nullptr;
		if (Camera && Holder->IsLocallyControlled() && AttachmentState.bHasHeldReference)
		{
			const FTransform Relative=AttachmentState.HeldTransform.GetRelativeTransform(AttachmentState.HeldReference);
			if (!bOwnerRelativeInitialized || OwnerDisplayRevision!=AttachmentState.Revision || PresentedLocalHolder.Get()!=Holder
				|| FVector::DistSquared(OwnerRelativeDisplay.GetLocation(),Relative.GetLocation())>FMath::Square(150.f))
			{
				OwnerRelativeDisplay=Relative; bOwnerRelativeInitialized=true; OwnerDisplayRevision=AttachmentState.Revision; PresentedLocalHolder=Holder;
			}
			else OwnerRelativeDisplay.Blend(OwnerRelativeDisplay,Relative,1.f-FMath::Exp(-FMath::Max(0.f,DeltaSeconds)*20.f));
			const FTransform LocalReference(Holder->GetViewRotation(),Camera->GetComponentLocation(),FVector::OneVector);
			Display=LimitOwnerDisplayPrediction(OwnerRelativeDisplay*LocalReference);
		}
		else
		{
			bOwnerRelativeInitialized=false; PresentedLocalHolder.Reset();
			const bool bLargeCorrection=FVector::DistSquared(GetActorLocation(),AttachmentState.HeldTransform.GetLocation())>FMath::Square(150.f);
			Display.Blend(GetActorTransform(),AttachmentState.HeldTransform,bLargeCorrection ? 1.f : 1.f-FMath::Exp(-FMath::Max(0.f,DeltaSeconds)*20.f));
		}
		// 碰撞根组件始终使用服务器姿态；仅本机拾取者看到逐帧的渲染副本。
		if (bOwnerRelativeInitialized)
		{
			SetActorTransform(AttachmentState.HeldTransform);
			UpdateOwnerDisplayMesh(&Display);
		}
		else
		{
			UpdateOwnerDisplayMesh(nullptr);
			SetActorTransform(Display);
		}
	}
	UpdateValueWidgets();
}

void AECPPickupItem::UpdateOwnerDisplayMesh(const FTransform* Display)
{
	UStaticMeshComponent* Body=Cast<UStaticMeshComponent>(GetCarryBody());
	if (!Display || !Body)
	{
		if (bOwnerDisplayActive && Body) Body->SetVisibility(bRootVisibleBeforeOwnerDisplay,false);
		if (OwnerDisplayMesh) OwnerDisplayMesh->SetVisibility(false,false);
		bOwnerDisplayActive=false;
		return;
	}
	if (!OwnerDisplayMesh)
	{
		OwnerDisplayMesh=NewObject<UStaticMeshComponent>(this,TEXT("OwnerDisplayMesh"));
		OwnerDisplayMesh->SetMobility(EComponentMobility::Movable);
		OwnerDisplayMesh->SetupAttachment(Body);
		OwnerDisplayMesh->SetAbsolute(true,true,true);
		OwnerDisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		OwnerDisplayMesh->SetSimulatePhysics(false);
		OwnerDisplayMesh->SetIsReplicated(false);
		OwnerDisplayMesh->SetStaticMesh(Body->GetStaticMesh());
		for (int32 Index=0;Index<Body->GetNumMaterials();++Index) OwnerDisplayMesh->SetMaterial(Index,Body->GetMaterial(Index));
		OwnerDisplayMesh->SetOverlayMaterial(Body->GetOverlayMaterial());
		OwnerDisplayMesh->SetRenderCustomDepth(Body->bRenderCustomDepth);
		OwnerDisplayMesh->SetCustomDepthStencilValue(Body->CustomDepthStencilValue);
		OwnerDisplayMesh->SetCastShadow(Body->CastShadow);
		AddInstanceComponent(OwnerDisplayMesh);
		OwnerDisplayMesh->RegisterComponent();
	}
	if (!bOwnerDisplayActive)
	{
		bRootVisibleBeforeOwnerDisplay=Body->IsVisible();
		Body->SetVisibility(false,false);
		OwnerDisplayMesh->SetVisibility(true,false);
		bOwnerDisplayActive=true;
	}
	OwnerDisplayMesh->SetWorldTransform(*Display);
}

void AECPPickupItem::CaptureHeldPose()
{
	AttachmentState.HeldTransform=GetActorTransform();
	AECPPlayerBase* Holder=AttachmentState.Holder;
	UCameraComponent* Camera=IsValid(Holder) ? Holder->GetPickupInspectionComponent() : nullptr;
	AttachmentState.bHasHeldReference=Camera!=nullptr;
	if (Camera) AttachmentState.HeldReference=FTransform(Holder->GetViewRotation(),Camera->GetComponentLocation(),FVector::OneVector);
}

FTransform AECPPickupItem::LimitOwnerDisplayPrediction(const FTransform& Predicted) const
{
	FTransform Display=Predicted;
	const UPrimitiveComponent* Body=GetCarryBody();
	if (!Body || !GetWorld()) return AttachmentState.HeldTransform;
	const FVector Delta=Predicted.GetLocation()-AttachmentState.HeldTransform.GetLocation();
	const FBoxSphereBounds Bounds=Body->CalcBounds(AttachmentState.HeldTransform);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPOwnerDisplayPrediction),false,this); Query.AddIgnoredActor(AttachmentState.Holder);
	FHitResult Hit;
	float Allowed=1.f;
	// 只检查从最新权威姿态出发的本地补偿段；不能从旧客户端位置扫掠而把权威校正卡住。
	if (!Delta.IsNearlyZero())
	{
		if (GetWorld()->SweepSingleByChannel(Hit,Bounds.Origin,Bounds.Origin+Delta,FQuat::Identity,ECC_Visibility,
			FCollisionShape::MakeBox(Bounds.BoxExtent.ComponentMax(FVector(1.f))),Query)) Allowed=Hit.bStartPenetrating ? 0.f : FMath::Max(0.f,Hit.Time-0.02f);
		if (GetWorld()->SweepSingleByObjectType(Hit,Bounds.Origin,Bounds.Origin+Delta,FQuat::Identity,FCollisionObjectQueryParams(ECC_Pawn),
			FCollisionShape::MakeBox(Bounds.BoxExtent.ComponentMax(FVector(1.f))),Query)) Allowed=FMath::Min(Allowed,Hit.bStartPenetrating ? 0.f : FMath::Max(0.f,Hit.Time-0.02f));
	}
	Display.SetLocation(AttachmentState.HeldTransform.GetLocation()+Delta*Allowed);
	if (!Display.GetRotation().Equals(AttachmentState.HeldTransform.GetRotation(),1.e-3f))
	{
		auto Overlaps=[&](const FTransform& Pose)
		{
			const FBoxSphereBounds Box=Body->CalcBounds(Pose);
			const FCollisionShape Shape=FCollisionShape::MakeBox(Box.BoxExtent.ComponentMax(FVector(1.f)));
			return GetWorld()->OverlapAnyTestByChannel(Box.Origin,FQuat::Identity,ECC_Visibility,Shape,Query)
				|| GetWorld()->OverlapAnyTestByObjectType(Box.Origin,FQuat::Identity,FCollisionObjectQueryParams(ECC_Pawn),Shape,Query);
		};
		// 原地转动同样可能扩大物体的占用空间；若新姿态碰撞，保留权威角度。
		if (Overlaps(Display) && !Overlaps(AttachmentState.HeldTransform))
		{
			Display.SetRotation(AttachmentState.HeldTransform.GetRotation());
			if (Overlaps(Display)) Display=AttachmentState.HeldTransform;
		}
	}
	return Display;
}

void AECPPickupItem::RefreshPickupMass()
{
	UPrimitiveComponent* Body = GetCarryBody();
	if (!HasAuthority() || !Body) return;
	const FBodyInstance* Instance = Body->GetBodyInstance();
	const float Configured = Instance && Instance->bOverrideMass ? Instance->GetMassOverride() : DefaultMassKg;
	const float Physical = Body->GetMass();
	const float Fallback = FMath::IsFinite(Configured) && Configured > 0.f ? Configured : 10.f;
	const float Minimum = FMath::IsFinite(MinimumMassKg) ? FMath::Max(0.1f, MinimumMassKg) : 0.1f;
	const float Maximum = FMath::IsFinite(MaximumMassKg) ? FMath::Max(Minimum, MaximumMassKg) : 100000.f;
	const float Mass = FMath::Clamp(FMath::IsFinite(Physical) && Physical > 0.f ? Physical : Fallback, Minimum, Maximum);
	const bool bChanged = AuthoritativeMassKg != Mass;
	AuthoritativeMassKg = Mass;
	OnRep_Mass();
	if (bChanged)
	{
		OnCarryTargetChanged.Broadcast(this);
		FlushNetDormancy();
		ForceNetUpdate();
	}
}

void AECPPickupItem::OnRep_Mass()
{
	if (UPrimitiveComponent* Body = GetCarryBody()) Body->SetMassOverrideInKg(NAME_None, AuthoritativeMassKg, true);
}

float AECPPickupItem::GetPickupResponseSeconds() const
{
	const float Light = FMath::IsFinite(LagResponseAt50Kg) ? FMath::Max(0.01f, LagResponseAt50Kg) : 0.12f;
	const float Heavy = FMath::IsFinite(LagResponseAt120Kg) ? FMath::Max(Light, LagResponseAt120Kg) : 0.45f;
	return FMath::Lerp(Light, Heavy, FMath::Clamp((GetPickupMassKg() - 50.f) / 70.f, 0.f, 1.f));
}

FTransform AECPPickupItem::GetInspectionTargetWorldTransform() const
{
	AECPPlayerBase* Holder = AttachmentState.Holder;
	const USceneComponent* Camera = IsValid(Holder) ? Holder->GetPickupInspectionComponent() : nullptr;
	if (!Camera) return GetActorTransform();
	// Remote camera components may not have ticked when RPCs arrive. Use the authoritative view rotation.
	FTransform CameraTransform = Camera->GetComponentTransform();
	CameraTransform.SetRotation(Holder->GetViewRotation().Quaternion());
	return BuildInspectionRelativeTransform(Camera, AttachmentState.Inspection.Rotation) * CameraTransform;
}

UPrimitiveComponent* AECPPickupItem::GetCarryBody() const { return Cast<UPrimitiveComponent>(GetRootComponent()); }
APawn* AECPPickupItem::GetCarryHolder() const { return AttachmentState.Holder.Get(); }
bool AECPPickupItem::CanBeCarriedBy(const APawn* Holder) const { return CanBePickedUpBy(Cast<AECPPlayerBase>(Holder)); }
bool AECPPickupItem::TryBeginCarry(APawn* Holder) { return TryEnterPickup(Cast<AECPPlayerBase>(Holder)); }
void AECPPickupItem::EndCarry(APawn* Holder) { ReleaseFrom(Cast<AECPPlayerBase>(Holder)); }
