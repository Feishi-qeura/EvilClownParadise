// Fill out your copyright notice in the Description page of Project Settings.


#include "AI/ECPTaskChase.h"
#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"
#include "Data/ECPAIController.h"

EStateTreeRunStatus FECPTaskChase::EnterState(FStateTreeExecutionContext& Context,
                                             const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AECPAIController* AIController = Monster ? Cast<AECPAIController>(Monster -> GetController()) : nullptr	;
	if (!Monster || !AIController)
	{
		return EStateTreeRunStatus::Failed;
	}
	
	Monster -> SetMaxWalkSpeed(Monster -> ChaseSpeed);
	
	// 追到攻击范围的一半就停，保证停下时已经在攻击圈内（真实停止距离 = 本值 + 胶囊半径）
	AIController -> MoveToActor(Monster -> GetTarget(), Monster -> AttackRange * 0.5f);
	
	return EStateTreeRunStatus::Running;
}

void FECPTaskChase::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		if (AECPAIController* AIController = Cast<AECPAIController>(Monster -> GetController()))
		{
			AIController -> StopMovement();
		}
	}
}
