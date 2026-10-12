#include "Characters/ECPCharBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

float AECPCharBase::GetLoadMultiplier() const
{
	const float Capacity = FMath::IsFinite(MaxCarryWeightKg) && MaxCarryWeightKg > 0.f ? MaxCarryWeightKg : 200.f;
	const float Weight = FMath::IsFinite(CarriedWeightKg) ? FMath::Max(0.f, CarriedWeightKg) : Capacity;
	return FMath::Clamp(1.f - Weight / Capacity, 0.f, 1.f);
}

void AECPCharBase::SetCarriedWeightKg(float WeightKg)
{
	if (!HasAuthority() || !FMath::IsFinite(WeightKg) || WeightKg < 0.f || CarriedWeightKg == WeightKg) return;
	CarriedWeightKg = WeightKg;
	OnRep_Load();
	OnCarryTargetChanged.Broadcast(this);
	ForceNetUpdate();
}

void AECPCharBase::SetMaxCarryWeightKg(float CapacityKg)
{
	if (!HasAuthority() || !FMath::IsFinite(CapacityKg) || CapacityKg <= 0.f) return;
	MaxCarryWeightKg = CapacityKg;
	OnRep_Load();
	ForceNetUpdate();
}

void AECPCharBase::SetBodyWeightKg(float WeightKg)
{
	if (!HasAuthority() || !FMath::IsFinite(WeightKg) || WeightKg <= 0.f) return;
	BodyWeightKg = WeightKg;
	OnRep_BodyWeight();
	ForceNetUpdate();
}

void AECPCharBase::OnRep_Load()
{
	RefreshLoadMovement();
}

void AECPCharBase::OnRep_BodyWeight()
{
	OnCarryTargetChanged.Broadcast(this);
}

void AECPCharBase::RefreshLoadMovement()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (!Movement) return;
	const float Multiplier = GetLoadMultiplier();
	SavedWalkSpeed = FMath::Max(0.f, bRunningRequested ? RunSpeed : WalkSpeed) * Multiplier;
	SavedCrouchSpeed = FMath::Max(0.f, CrouchSpeed) * Multiplier;
	Movement->MaxWalkSpeed = IsMovementLocked() ? 0.f : SavedWalkSpeed;
	Movement->MaxWalkSpeedCrouched = IsMovementLocked() ? 0.f : SavedCrouchSpeed;
	Movement->JumpZVelocity = BaseJumpZVelocity * Multiplier;
	Movement->MaxAcceleration = IsMovementLocked() || IsOverloaded() ? 0.f : BaseMaxAcceleration;
	if (IsOverloaded())
	{
		// Do not StopMovementImmediately here: its zeroed vertical velocity would stop gravity in mid-air.
		Movement->Velocity.X = 0.f;
		Movement->Velocity.Y = 0.f;
		ConsumeMovementInputVector();
		StopJumping();
	}
}

UPrimitiveComponent* AECPCharBase::GetCarryBody() const
{
	return GetMesh();
}

float AECPCharBase::GetCarryMassKg() const
{
	const float BodyMass = FMath::IsFinite(BodyWeightKg) && BodyWeightKg > 0.f ? BodyWeightKg : 80.f;
	return BodyMass + FMath::Max(0.f, GetCarriedWeightKg());
}

bool AECPCharBase::CanBeCarriedBy(const APawn* Holder) const
{
	const AECPCharBase* Character = Cast<AECPCharBase>(Holder);
	// Occupancy and phase are rechecked on the server, preventing carry chains and recovery races.
	return Character && Character != this && !Character->IsDead() && !Character->IsMovementLocked()
		&& !IsValid(Character->CarryHolder) && !IsValid(CarryHolder) && !IsActorBeingDestroyed()
		&& bRagdollActive && MotionState.Phase == EECPMotionPhase::Ragdoll
		&& GetCharacterTeamId() != ECPTeam::Neutral && GetCharacterTeamId() == Character->GetCharacterTeamId()
		// 客户端布娃娃只显示服务器姿态，不能要求本地模拟才能显示可拾取提示。
		&& GetMesh() && (!HasAuthority() || GetMesh()->IsSimulatingPhysics(RagdollPelvisBone));
}

bool AECPCharBase::TryBeginCarry(APawn* Holder)
{
	if (!HasAuthority() || !CanBeCarriedBy(Holder)) return false;
	CarryHolder = Holder;
	Holder->OnDestroyed.AddUniqueDynamic(this, &ThisClass::OnCarryHolderDestroyed);
	OnCarryTargetChanged.Broadcast(this);
	ForceNetUpdate();
	return true;
}

void AECPCharBase::EndCarry(APawn* Holder)
{
	if (!HasAuthority() || !Holder || CarryHolder != Holder) return;
	Holder->OnDestroyed.RemoveDynamic(this, &ThisClass::OnCarryHolderDestroyed);
	CarryHolder = nullptr;
	OnCarryTargetChanged.Broadcast(this);
	ForceNetUpdate();
}

void AECPCharBase::OnCarryHolderDestroyed(AActor* DestroyedActor)
{
	EndCarry(Cast<APawn>(DestroyedActor));
}
