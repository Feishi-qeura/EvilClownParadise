// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/ECPMonsterBase.h"

#include "AI/ECPAIController.h"
#include "ECPCore.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AECPMonsterBase::AECPMonsterBase(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	// 感知阵营：决定其他感知者把本怪物视为敌人还是友方（见 ECPCore.h）
	TeamId = ECPTeam::Monster;

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
	AActor* Target = GetTarget();
	if (!IsValid(Target))
	{
		return false;
	}
	const AECPCharBase* TargetChar = Cast<AECPCharBase>(Target);
	return !TargetChar || !TargetChar->IsDead();
}

bool AECPMonsterBase::IsTargetInAttackRange() const
{
	AActor* Target = GetTarget();
	if (!IsValid(Target))
	{
		return false;
	}
	const float DistSq = FVector::DistSquared(GetActorLocation(), Target->GetActorLocation());
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
	// 攻击间隔是"节流"而不是"状态"：每次调用都自检，状态机不必再算冷却
	if (!HasAuthority() || !IsTargetInAttackRange())
	{
		return;
	}

	const float TimeNow = GetWorld()->GetTimeSeconds();
	if (TimeNow - LastAttackTime < AttackInterval)
	{
		return;
	}

	LastAttackTime = TimeNow;

	UGameplayStatics::ApplyDamage(GetTarget(), AttackDamage, GetController(), this, nullptr);
}
