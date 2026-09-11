// Fill out your copyright notice in the Description page of Project Settings.


#include "Characters/ECPMonsterBase.h"

#include "Data/ECPAIController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AECPMonsterBase::AECPMonsterBase()
{
	AIControllerClass = AECPAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
}

void AECPMonsterBase::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		PatrolOrigin = GetActorLocation();
	}
}

bool AECPMonsterBase::IsTargetInAttackRange() const
{
	if (!HasTarget())
	{
		return false;
	}
	const float DistSq = FVector::DistSquared(GetActorLocation(), GetTarget() -> GetActorLocation());
	return DistSq <= FMath::Square(AttackRange);
}

void AECPMonsterBase::SetMaxWalkSpeed(float NewSpeed)
{
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = NewSpeed;
	}
	
}

void AECPMonsterBase::PerformAttack()
{
	if (!HasAuthority() || !HasTarget() || IsTargetInAttackRange()){return;}
	
	const float now = GetWorld()->GetTimeSeconds();
	if (now - LastAttackTime < AttackInterval){return;}
	
	LastAttackTime = now;
	
	UGameplayStatics::ApplyDamage(TargetActor, AttackDamage, GetController(), this, nullptr);
}