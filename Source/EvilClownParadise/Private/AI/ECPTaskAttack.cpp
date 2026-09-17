// Fill out your copyright notice in the Description page of Project Settings.

#include "AI/ECPTaskAttack.h"

#include "AI/ECPAIController.h"
#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"

EStateTreeRunStatus FECPTaskAttack::EnterState(FStateTreeExecutionContext& Context,
                                               const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AECPAIController* AIController = Monster ? Cast<AECPAIController>(Monster->GetController()) : nullptr;
	if (!Monster || !AIController)
	{
		return EStateTreeRunStatus::Failed;
	}

	Monster->SetMaxWalkSpeed(0.f);
	AIController->StopMovement();

	Monster->PerformAttack();

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FECPTaskAttack::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	if (!Monster)
	{
		return EStateTreeRunStatus::Failed;
	}
	Monster->PerformAttack();

	return EStateTreeRunStatus::Running;
}

void FECPTaskAttack::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		// 恢复到"警戒"速度而不是 0：这是中性兜底值，下一个状态会在自己的 EnterState 里覆盖。
		// 目的是让本状态即便被异常打断（目标消失 / 外部转场），怪物也不会永久停在原地。
		Monster->SetMaxWalkSpeed(Monster->ChaseSpeed);
	}
}
