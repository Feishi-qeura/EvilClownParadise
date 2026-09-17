// Fill out your copyright notice in the Description page of Project Settings.


#include "Characters/ECPMonsterBase.h"

#include "Data/ECPAIController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AECPMonsterBase::AECPMonsterBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
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

bool AECPMonsterBase::HasTarget() const
{
	// 目标已死亡也算没有目标：玩家死亡不销毁 Actor，这里不判掉，状态机会一直卡在追击/攻击
	const AECPCharBase* TargetChar = Cast<AECPCharBase>(TargetActor);
	return IsValid(TargetActor) && !(TargetChar && TargetChar -> IsDead());
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
	if (!HasAuthority() || !HasTarget() || !IsTargetInAttackRange()){ return; }
	
	const float now = GetWorld()->GetTimeSeconds();
	if (now - LastAttackTime < AttackInterval){return;}
	
	LastAttackTime = now;
	
	UGameplayStatics::ApplyDamage(TargetActor, AttackDamage, GetController(), this, nullptr);
}