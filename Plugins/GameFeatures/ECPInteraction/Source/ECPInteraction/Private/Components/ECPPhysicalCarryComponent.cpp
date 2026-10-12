#include "Components/ECPPhysicalCarryComponent.h"
#include "Components/ECPCarryContactProjection.h"
#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

UECPPhysicalCarryComponent::UECPPhysicalCarryComponent()
{
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bInterpolateTarget = false;
	bSoftLinearConstraint = true;
	bSoftAngularConstraint = true;
}

bool UECPPhysicalCarryComponent::StartCarry(AActor* Target, const FHitResult* Hit)
{
	AECPPlayerBase* Holder = Cast<AECPPlayerBase>(GetOwner());
	IECPCarryTarget* Contract = Cast<IECPCarryTarget>(Target);
	if (!Holder || !Holder->HasAuthority() || !Contract || Contract->GetCarryHolder() != Holder) return false;
	UPrimitiveComponent* Body = Contract->GetCarryBody();
	FName Bone = Contract->GetCarryBone();
	if (Hit && !Bone.IsNone())
	{
		USkeletalMeshComponent* Skeletal=Cast<USkeletalMeshComponent>(Body);
		if (!Skeletal || Hit->GetActor()!=Target || Hit->GetComponent()!=Body || Hit->BoneName.IsNone()
			|| Hit->ImpactPoint.ContainsNaN() || Skeletal->GetBoneIndex(Hit->BoneName)==INDEX_NONE) return false;
		Bone=Hit->BoneName;
		// 命中无独立刚体的子骨骼时，使用最近的有效物理父骨骼，表面命中点保持不变。
		while (!Bone.IsNone() && !Skeletal->GetBodyInstance(Bone)) Bone=Skeletal->GetParentBone(Bone);
		if (Bone.IsNone()) return false;
	}
	if (!IsValid(Body) || !Body->IsSimulatingPhysics(Bone)) return false;
	if (Bone.IsNone()) Body->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
	if (CarryTarget == Target && GetGrabbedComponent() == Body) return true;
	if (IsValid(CarryTarget.Get())) return false;
	StopCarry();
	FTransform Grip=Body->GetBodyInstance(Bone)->GetUnrealWorldTransform();
	if (Hit && !Bone.IsNone()) Grip.SetLocation(Hit->ImpactPoint);
	const float Mass = FMath::Clamp(Contract->GetCarryMassKg(), 0.1f, 1000.f);
	SetLinearStiffness(Mass * 500.f);
	SetLinearDamping(Mass * 40.f);
	SetAngularStiffness(Mass * 400.f);
	SetAngularDamping(Mass * 30.f);
	// Start at the body's real position, never teleport into an obstructed camera goal.
	GrabComponentAtLocationWithRotation(Body, Bone, Grip.GetLocation(), Grip.Rotator());
	if (GetGrabbedComponent() != Body) return false;
	CarryTarget = Target;
	ContactBody = Body;
	ContactBone = Bone;
	if (const FBodyInstance* Instance = Body->GetBodyInstance(Bone)) bOriginalCCD = Instance->bUseCCD;
	Body->SetUseCCD(true, Bone);
	SmoothedTarget = Grip;
	GripRelativeRotation = Holder->GetViewRotation().Quaternion().Inverse() * Grip.GetRotation();
	LastHolderLocation = Holder->GetActorLocation();
	SuppressHolderContact(Body, Bone);
	if (Bone.IsNone())
	{
		SavedInteractionMovement=Holder->GetCharacterMovement();
		if (UCharacterMovementComponent* Movement=SavedInteractionMovement.Get())
		{
			bSavedPhysicsInteraction=Movement->bEnablePhysicsInteraction;
			// CMC 仍扫掠并阻挡自己的手持物，但不能用默认高强度推力对抗抓取约束。
			Movement->bEnablePhysicsInteraction=false;
		}
	}
	SetTargetLocationAndRotation(Grip.GetLocation(), Grip.Rotator());
	SetComponentTickEnabled(true);
	return true;
}

void UECPPhysicalCarryComponent::SuppressHolderContact(UPrimitiveComponent* Body, FName Bone)
{
	AECPPlayerBase* Holder = CastChecked<AECPPlayerBase>(GetOwner());
	UCapsuleComponent* Capsule = Holder->GetCapsuleComponent();
	bHolderAlreadyIgnored = Body->GetMoveIgnoreActors().Contains(Holder);
	bTargetAlreadyIgnored = Capsule->GetMoveIgnoreActors().Contains(CarryTarget.Get());
	Body->IgnoreActorWhenMoving(Holder, true);
	if (!Bone.IsNone()) Capsule->IgnoreActorWhenMoving(CarryTarget.Get(), true);
	TArray<FName> Bones = {Bone};
	if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Body))
	{
		if (UPhysicsAsset* Asset = Skeletal->GetPhysicsAsset())
		{
			Bones.Reset();
			for (USkeletalBodySetup* Setup : Asset->SkeletalBodySetups) if (Setup) Bones.Add(Setup->BoneName);
		}
	}
	// IgnoreActorWhenMoving affects sweeps only. Free joints disable exactly these physics contact pairs
	// without globally ignoring other pawns, walls or dynamic bodies, and exert no movement constraint.
	for (FName BodyBone : Bones)
	{
		if (!Body->GetBodyInstance(BodyBone)) continue;
		UPhysicsConstraintComponent* Filter = NewObject<UPhysicsConstraintComponent>(Holder);
		Filter->SetLinearXLimit(LCM_Free, 0.f);
		Filter->SetLinearYLimit(LCM_Free, 0.f);
		Filter->SetLinearZLimit(LCM_Free, 0.f);
		Filter->SetAngularSwing1Limit(ACM_Free, 0.f);
		Filter->SetAngularSwing2Limit(ACM_Free, 0.f);
		Filter->SetAngularTwistLimit(ACM_Free, 0.f);
		Filter->SetDisableCollision(true);
		Filter->RegisterComponent();
		Filter->SetConstrainedComponents(Capsule, NAME_None, Body, BodyBone);
		ContactFilters.Add(Filter);
	}
}

void UECPPhysicalCarryComponent::StopCarry()
{
	AActor* Target = CarryTarget.Get();
	CarryTarget.Reset();
	ReleaseComponent();
	for (UPhysicsConstraintComponent* Filter : ContactFilters)
	{
		if (IsValid(Filter))
		{
			Filter->BreakConstraint();
			Filter->DestroyComponent();
		}
	}
	ContactFilters.Reset();
	if (UPrimitiveComponent* Body = ContactBody.Get())
	{
		Body->SetUseCCD(bOriginalCCD, ContactBone);
		if (!bHolderAlreadyIgnored) Body->IgnoreActorWhenMoving(GetOwner(), false);
	}
	if (AECPPlayerBase* Holder = Cast<AECPPlayerBase>(GetOwner()); Holder && Target && !bTargetAlreadyIgnored)
	{
		Holder->GetCapsuleComponent()->IgnoreActorWhenMoving(Target, false);
	}
	ContactBody.Reset();
	if (UCharacterMovementComponent* Movement=SavedInteractionMovement.Get()) Movement->bEnablePhysicsInteraction=bSavedPhysicsInteraction;
	SavedInteractionMovement.Reset();
	SetComponentTickEnabled(false);
}

FTransform UECPPhysicalCarryComponent::GetCurrentGripTransform() const
{
	UPrimitiveComponent* Body=GetGrabbedComponent();
	const FBodyInstance* Instance=Body ? Body->GetBodyInstance(GrabbedBoneName) : nullptr;
	if (!Instance) return FTransform::Identity;
	FTransform Grip=Instance->GetUnrealWorldTransform();
	// PhysicsHandle 已保存表面点的刚体局部坐标；每帧用真实物理姿态还原，不能退回骨骼中心。
	Grip.SetLocation(Grip.TransformPosition(ConstraintLocalPosition));
	return Grip;
}

FTransform UECPPhysicalCarryComponent::LimitTarget(const FTransform& Desired) const
{
	UPrimitiveComponent* Body = GetGrabbedComponent();
	FTransform Safe = Desired;
	if (!Body || !GetWorld()) return Safe;
	const FVector GripLocation=GetCurrentGripTransform().GetLocation();
	if (GrabbedBoneName.IsNone())
	{
		const AECPPlayerBase* Holder=Cast<AECPPlayerBase>(GetOwner());
		const UCapsuleComponent* Capsule=Holder ? Holder->GetCapsuleComponent() : nullptr;
		if (Capsule)
		{
			// 禁用自身接触力用于避免推飞角色，不代表允许物体占据持有者空间。
			// 沿视线分离两个体积；模型缩放、旋转及偏心枢轴均参与，而不是固定相机距离。
			const FVector Forward=Holder->GetViewRotation().Vector().GetSafeNormal();
			const FBoxSphereBounds Bounds=Body->CalcBounds(FTransform(Desired.GetRotation(),Body->GetComponentLocation(),Body->GetComponentScale()));
			const FVector Center=Bounds.Origin+Safe.GetLocation()-GripLocation;
			const float BodySupport=FVector::DotProduct(Bounds.BoxExtent,Forward.GetAbs());
			const float Radius=Capsule->GetScaledCapsuleRadius();
			const float CapsuleSupport=Radius+(Capsule->GetScaledCapsuleHalfHeight()-Radius)*FMath::Abs(FVector::DotProduct(Forward,Capsule->GetUpVector()));
			const float Separation=FVector::DotProduct(Center-Capsule->GetComponentLocation(),Forward);
			if (Separation < BodySupport+CapsuleSupport+3.f) Safe.AddToTranslation(Forward*(BodySupport+CapsuleSupport+3.f-Separation));
		}
	}
	// The conservative world-space box includes item edges and ragdoll limbs; center-ray checks do not.
	const FVector Extent = Body->Bounds.BoxExtent.ComponentMax(FVector(1.f));
	const FVector DesiredDelta=Safe.GetLocation()-GripLocation;
	TArray<ECPCarryContacts::FPlaneConstraint,TInlineAllocator<16>> ContactPlanes;
	bool bChangedContacts=false;
	auto ConstrainTranslation=[&](bool bPawnObjects)
	{
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPCarryBlocking),false,GetOwner());
		Query.AddIgnoredActor(CarryTarget.Get());
		for (int32 Contact=0;Contact<8;++Contact)
		{
			const FVector Delta=Safe.GetLocation()-GripLocation;
			FHitResult Hit;
			const bool bHit=bPawnObjects
				? GetWorld()->SweepSingleByObjectType(Hit,Body->Bounds.Origin,Body->Bounds.Origin+Delta,FQuat::Identity,
					FCollisionObjectQueryParams(ECC_Pawn),FCollisionShape::MakeBox(Extent),Query)
				: GetWorld()->SweepSingleByChannel(Hit,Body->Bounds.Origin,Body->Bounds.Origin+Delta,FQuat::Identity,
					ECC_Visibility,FCollisionShape::MakeBox(Extent),Query);
			if (!bHit) break;
			// 保守包围盒可在真实几何尚未接触时初始重叠。离开或沿接触面移动必须允许，
			// 继续检查其他组件；不能一直把目标锁在当前点，靠重力落到脚边才能脱离。
			const FVector ToContact=Hit.bStartPenetrating ? FVector::ZeroVector : Delta*FMath::Max(0.f,Hit.Time-0.02f);
			const double Minimum=FMath::Min(0.0,FVector::DotProduct(ToContact,Hit.Normal));
			const int32 Existing=ContactPlanes.IndexOfByPredicate([&](const ECPCarryContacts::FPlaneConstraint& Plane) { return Plane.Normal.Equals(Hit.Normal,0.0001); });
			if (Existing==INDEX_NONE) { ContactPlanes.Add({Hit.Normal,Minimum}); bChangedContacts=true; }
			else if (Minimum>ContactPlanes[Existing].MinimumDot+0.0001) { ContactPlanes[Existing].MinimumDot=Minimum; bChangedContacts=true; }
			Safe.SetLocation(GripLocation+ECPCarryContacts::ProjectDelta(DesiredDelta,ContactPlanes));
			if (!Hit.GetComponent()) break;
			Query.AddIgnoredComponent(Hit.GetComponent());
		}
	};
	// Pawn 和 CharacterMesh 默认忽略 Visibility；角色阻挡必须按对象类型检测。
	FCollisionObjectQueryParams Pawns(ECC_Pawn);
	// 角色投影会改变原来的路径，可能遇到先前对角线未穿过的墙；两类障碍都要重查。
	for (int32 Pass=0;Pass<8;++Pass)
	{
		bChangedContacts=false;
		ConstrainTranslation(false); ConstrainTranslation(true);
		if (!bChangedContacts) break;
	}
	// 极端密集几何超过有界查询预算时保持当前点，不能提交未经检查的新路径。
	if (bChangedContacts) Safe.SetLocation(GripLocation);
	if (GrabbedBoneName.IsNone())
	{
		FComponentQueryParams ComponentQuery(SCENE_QUERY_STAT(ECPCarryRotation), GetOwner());
		ComponentQuery.AddIgnoredActor(CarryTarget.Get());
		FQuat AllowedRotation = Body->GetComponentQuat();
		// Incremental overlap checks constrain rotation too; translation sweep alone cannot do that.
		for (int32 Step = 1; Step <= 8; ++Step)
		{
			const FQuat Candidate = FQuat::Slerp(Body->GetComponentQuat(), Desired.GetRotation(), Step / 8.f);
			TArray<FOverlapResult> Overlaps;
			GetWorld()->ComponentOverlapMultiByChannel(Overlaps, Body, Safe.GetLocation(), Candidate,
				ECC_Visibility, ComponentQuery);
			if (Overlaps.ContainsByPredicate([](const FOverlapResult& Result) { return Result.bBlockingHit; })) break;
			Overlaps.Reset();
			GetWorld()->ComponentOverlapMulti(Overlaps,Body,Safe.GetLocation(),Candidate,ComponentQuery,Pawns);
			if (!Overlaps.IsEmpty()) break;
			AllowedRotation = Candidate;
		}
		Safe.SetRotation(AllowedRotation);
	}
	return Safe;
}

void UECPPhysicalCarryComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
	AECPPlayerBase* Holder = Cast<AECPPlayerBase>(GetOwner());
	AActor* Target = CarryTarget.Get();
	IECPCarryTarget* Contract = Cast<IECPCarryTarget>(Target);
	if (!Holder || !Holder->HasAuthority()) { StopCarry(); return; }
	if (!Target || !Contract || Contract->GetCarryHolder() != Holder || !GetGrabbedComponent()
		|| Holder->IsDead() || Holder->IsRagdollActive() || !Holder->GetController()
		|| FVector::DistSquared(Holder->GetActorLocation(), LastHolderLocation) > FMath::Square(500.f))
	{
		Holder->HandlePhysicalCarryLost(Target);
		StopCarry();
		return;
	}
	LastHolderLocation = Holder->GetActorLocation();
	FTransform Desired;
	float Response = 0.3f;
	if (const AECPPickupItem* Item = Cast<AECPPickupItem>(Target))
	{
		Desired = Item->GetInspectionTargetWorldTransform();
		Response = Item->GetPickupResponseSeconds();
	}
	else
	{
		UCameraComponent* Camera = Holder->GetPickupInspectionComponent();
		if (!Camera) { Holder->HandlePhysicalCarryLost(Target); StopCarry(); return; }
		const FQuat View = Holder->GetViewRotation().Quaternion();
		Desired = FTransform(View * GripRelativeRotation, Camera->GetComponentLocation() + View.RotateVector(FVector(120.f, 0.f, 0.f)));
	}
	const FVector BodyLocation=GetCurrentGripTransform().GetLocation();
	if (FVector::DistSquared(BodyLocation, Desired.GetLocation()) > FMath::Square(700.f))
	{
		Holder->HandlePhysicalCarryLost(Target);
		StopCarry();
		return;
	}
	const float Alpha = 1.f - FMath::Exp(-FMath::Max(0.f, DeltaTime) / Response);
	SmoothedTarget.Blend(SmoothedTarget, Desired, Alpha);
	SmoothedTarget = LimitTarget(SmoothedTarget);
	SetTargetLocationAndRotation(SmoothedTarget.GetLocation(), SmoothedTarget.Rotator());
	Super::TickComponent(DeltaTime, TickType, TickFunction);
}

void UECPPhysicalCarryComponent::OnUnregister()
{
	StopCarry();
	Super::OnUnregister();
}
